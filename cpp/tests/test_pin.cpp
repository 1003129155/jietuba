#include "save_dialog_answerer.h"
#include "ocr/ocr_manager.h"
#include "canvas/items/background_item.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/drawing_items.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/items/text_item.h"
#include "canvas/items/watermark_item.h"
#include "canvas/scene.h"
#include "canvas/smart_edit_controller.h"
#include "tools/controller.h"
#include "tools/number.h"
#include "translation/translation_dialog.h"
#include "translation/translation_manager.h"
#include "ocr/ocr_line_geometry.h"
#include "pin/ocr_text_layout.h"
#include "pin/pin_border_overlay.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_click_through_bar.h"
#include "pin/pin_image_transform.h"
#include "pin/pin_context_menu.h"
#include "pin/pin_hover.h"
#include "pin/pin_manager.h"
#include "pin/pin_ocr_manager.h"
#include "pin/pin_shortcut.h"
#include "pin/pin_text_content.h"
#include "pin/pin_text_mode.h"
#include "pin/pin_text_selection.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include "core/clipboard_utils.h"
#include "core/word_boundaries.h"
#include "settings/tool_settings.h"
#include "canvas/undo.h"
#include "ui/toolbar_layout.h"
#include "ui/toolbar_layout_dialog.h"
#include "ui/arrow_settings_panel.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/watermark_settings_panel.h"
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QJsonArray>
#include <QFileDialog>
#include <QImageReader>
#include <QLineEdit>
#include <QFileInfo>
#include <QFontDatabase>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextList>
#include <QTextTable>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <atomic>
#include <cmath>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jietuba;
namespace tl = jietuba::toolbar_layout;

namespace {
PinImageTransform transform_state(int rotation, bool flip_h, bool flip_v) {
    PinImageTransform transform;
    for (int turned = 0; turned < rotation; turned += 90) transform.rotate_cw();
    if (flip_h) transform.flip_horizontal();
    if (flip_v) transform.flip_vertical();
    return transform;
}

bool close_to(double a, double b, double eps = 1e-6) { return std::abs(a - b) <= eps; }

QImage solid(const QSize &size) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(0xFF336699);
    return image;
}

template <typename T>
QList<T *> items_of(QGraphicsScene *scene) {
    QList<T *> found;
    for (auto *item : scene->items())
        if (auto *typed = dynamic_cast<T *>(item)) found << typed;
    return found;
}

// Black with a white top-left pixel, to follow where that corner goes.
QImage marked(int w = 8, int h = 4) {
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(0xFF000000);
    image.setPixel(0, 0, 0xFFFFFFFF);
    return image;
}

void flush_deletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }

// Mouse events go to the view's viewport, through the same event filter as real input. A pin
// closed by an earlier event (a double click bound to closing) gets no more.
void send_mouse(PinWindow *pin, QEvent::Type type, const QPoint &local, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    if (!pin->view) return;
    auto *viewport = pin->view->viewport();
    QMouseEvent event(type, QPointF(local), QPointF(viewport->mapToGlobal(local)), button, buttons, modifiers);
    QApplication::sendEvent(viewport, &event);
}

void send_mouse_global(PinWindow *pin, QEvent::Type type, const QPoint &global, Qt::MouseButton button, Qt::MouseButtons buttons) {
    auto *viewport = pin->view->viewport();
    QMouseEvent event(type, QPointF(viewport->mapFromGlobal(global)), QPointF(global), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(viewport, &event);
}

// For a pin's child other than the view's viewport, such as the text page or the text's viewport.
void send_mouse_to(QWidget *widget, QEvent::Type type, const QPoint &local, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(local), QPointF(widget->mapToGlobal(local)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void drag_on(QWidget *widget, const QPoint &start, const QPoint &end) {
    send_mouse_to(widget, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    send_mouse_to(widget, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
    send_mouse_to(widget, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
}

void drag(PinWindow *pin, const QPoint &start, const QPoint &end, Qt::MouseButton button = Qt::LeftButton) {
    send_mouse(pin, QEvent::MouseButtonPress, start, button, button);
    send_mouse(pin, QEvent::MouseMove, end, Qt::NoButton, button);
    send_mouse(pin, QEvent::MouseButtonRelease, end, button, Qt::NoButton);
}

void click(PinWindow *pin, const QPoint &local, Qt::MouseButton button, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    send_mouse(pin, QEvent::MouseButtonPress, local, button, button, modifiers);
    send_mouse(pin, QEvent::MouseButtonRelease, local, button, Qt::NoButton, modifiers);
}

// The order Windows delivers: press, release, double click, release.
void double_click(PinWindow *pin, const QPoint &local, Qt::MouseButton button, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    click(pin, local, button, modifiers);
    send_mouse(pin, QEvent::MouseButtonDblClick, local, button, button, modifiers);
    send_mouse(pin, QEvent::MouseButtonRelease, local, button, Qt::NoButton, modifiers);
}

// Through the window system like a real wheel: synthesized wheel events never reach parents.
void wheel(PinWindow *pin, int delta, Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool horizontal = false, int times = 1) {
    for (int i = 0; i < times; ++i)
        QTest::wheelEvent(pin->windowHandle(), QPointF(pin->rect().center()), horizontal ? QPoint(delta, 0) : QPoint(0, delta), QPoint(), modifiers);
}

// The top-left hint shared by zoom, opacity and lock messages; empty when hidden.
QString hint_text(PinWindow *pin) {
    for (auto *label : pin->findChildren<QLabel *>(Qt::FindDirectChildrenOnly))
        if (label->isVisible()) return label->text();
    return {};
}

void hide_hint(PinWindow *pin) {
    for (auto *label : pin->findChildren<QLabel *>(Qt::FindDirectChildrenOnly)) label->hide();
}

PinClickThroughBar *click_through_bar(PinWindow *pin) { return pin->findChild<PinClickThroughBar *>(); }

QLabel *move_handle(PinClickThroughBar *bar) {
    for (auto *label : bar->findChildren<QLabel *>())
        if (label->cursor().shape() == Qt::SizeAllCursor) return label;
    return nullptr;
}

LONG_PTR ex_style(QWidget *widget) { return GetWindowLongPtrW(reinterpret_cast<HWND>(widget->winId()), GWL_EXSTYLE); }

const QString LOCKED_HINT = "Locked, right-click to unlock";

QVector<QPointF> box(double x, double y, double w, double h) { return {QPointF(x, y), QPointF(x + w, y), QPointF(x + w, y + h), QPointF(x, y + h)}; }

OcrResult ocr_result(const QList<OcrLine> &lines, int code = 100) {
    OcrResult result;
    result.code = code;
    result.data = QVector<OcrLine>(lines.begin(), lines.end());
    return result;
}

// The regions a widget repaints.
struct PaintRecorder : QObject {
    QRegion painted;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint) painted += static_cast<QPaintEvent *>(event)->region();
        return false;
    }
};

// Gives a pin recognized text, as a finished recognition would: recognizing turns selection on.
PinTextSelection *give_text(PinWindow *pin, const QList<OcrLine> &lines) {
    pin->ocr_manager()->load_text(OcrTextLayout(ocr_result(lines)));
    pin->ocr_manager()->set_text_selection_enabled(true);
    return pin->text_selection();
}

// An OCR engine of the test's own, so recognition on demand runs where no real engine is installed.
// It answers on the recognition thread.
struct StandInEngine {
    explicit StandInEngine(std::function<OcrResult(const QImage &)> recognize) { set_recognizer_for_test(std::move(recognize)); }
    ~StandInEngine() { set_recognizer_for_test({}); }
};

struct RecordingTranslator : SelectionTranslator {
    QStringList texts;
    void translate_selection(const QString &text) override { texts << text; }
};

// Starts, centres and ends of character spans, flattened, for comparing with expected values.
QVector<double> span_points(const QVector<OcrCharSpan> &spans) {
    QVector<double> points;
    for (const auto &span : spans) points << span.start << span.center << span.end;
    return points;
}

bool all_close(const QVector<double> &got, const QVector<double> &want, double eps = 1e-6) {
    if (got.size() != want.size()) return false;
    for (qsizetype i = 0; i < got.size(); ++i)
        if (std::abs(got[i] - want[i]) > eps) return false;
    return true;
}

QString points_text(const QVector<double> &points) {
    QStringList parts;
    for (const double p : points) parts << QString::number(p, 'g', 6);
    return parts.join(", ");
}

// Ends a single-shot timer the way Qt does: stopped, then timeout.
void expire(QTimer *timer) {
    QVERIFY(timer && timer->isActive());
    timer->stop();
    QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection);
}

QTimer *pin_timer(PinWindow *pin, int interval) {
    for (auto *timer : pin->findChildren<QTimer *>(Qt::FindDirectChildrenOnly))
        if (timer->interval() == interval) return timer;
    return nullptr;
}

bool toolbar_shown(PinWindow *pin) { return pin->toolbar && pin->toolbar->isVisible(); }

// Opens the pin's real context menu with open(), lists it (submenu entries indented,
// separators as "-", switches led by "[x] " or "[ ] "), triggers the entry whose text starts
// with trigger, then closes it.
QStringList run_menu(const std::function<void()> &open, const QString &trigger = {}) {
    QStringList entries;
    // Local timers: a menu that never opens must not leave a callback behind.
    QTimer reader;
    reader.setSingleShot(true);
    QObject::connect(&reader, &QTimer::timeout, [&entries, trigger] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        QAction *chosen = nullptr;
        for (auto *action : menu->actions()) {
            if (action->isSeparator()) {
                entries << "-";
                continue;
            }
            entries << (action->isCheckable() ? (action->isChecked() ? "[x] " : "[ ] ") + action->text() : action->text());
            if (!trigger.isEmpty() && action->text().startsWith(trigger)) chosen = action;
            if (auto *sub = action->menu()) {
                for (auto *item : sub->actions()) {
                    const auto text = item->isSeparator() ? QString("  -") : "  " + item->text();
                    entries << text;
                    if (!trigger.isEmpty() && !item->isSeparator() && text.startsWith(trigger)) chosen = item;
                }
            }
        }
        menu->close();
        if (chosen) chosen->trigger();
    });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, [] {
        if (auto *popup = QApplication::activePopupWidget()) popup->close();
    });
    reader.start(0);
    guard.start(3000);
    open();
    return entries;
}

QStringList pin_menu(PinWindow *pin, const QString &trigger = {}) {
    return run_menu([pin] { pin->show_context_menu(pin->mapToGlobal(QPoint(10, 10))); }, trigger);
}

void press_key(PinWindow *pin, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { QTest::keyClick(pin, key, modifiers); }

QString clipboard_text() { return QApplication::clipboard()->text(); }

// The format of the first run of text holding part.
QTextCharFormat format_of(const QTextDocument *document, const QString &part) {
    for (auto block = document->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().text().contains(part)) return it.fragment().charFormat();
    return {};
}

// The default pin layout with keys folded into "...".
QJsonArray folded_layout(const QStringList &keys) {
    QJsonArray layout;
    for (const auto &key : tl::PIN.order) layout.append(QJsonArray{key, keys.contains(key) ? tl::MORE : tl::PIN.default_mode(key)});
    return layout;
}

QString mode_in(const QJsonArray &layout, const QString &key) {
    for (const auto &entry : layout)
        if (entry.toArray()[0].toString() == key) return entry.toArray()[1].toString();
    return {};
}

}

class PinTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
    QList<QPointer<PinWindow>> pins_;

    PinWindow *make_pin(const QSize &size = QSize(200, 120), const QPoint &pos = QPoint(100, 100), const QVariantMap &settings = {}) {
        for (auto it = settings.begin(); it != settings.end(); ++it) config_->set_app_setting(it.key(), it.value());
        auto *pin = new PinWindow(solid(size), pos, config_);
        pins_.append(pin);
        return pin;
    }

    PinWindow *make_pin(const QVariantMap &settings) { return make_pin(QSize(200, 120), QPoint(100, 100), settings); }

    PinWindow *make_text_pin(const QString &text, const QString &html = {}, bool formatted = true) {
        auto *pin = PinManager::instance()->create_text_pin(std::make_unique<PinTextContent>(text, html, formatted), QPoint(100, 100), config_);
        pins_.append(pin);
        return pin;
    }

    // A text's page as a worker thread lays it out, as the pin does when its formatting switches. The
    // offscreen platform's fonts there lack synthetic bold, so the UI thread's page can differ.
    static QSize worker_page_size(const QString &text, const QString &html, bool formatted) {
        QSize size;
        QObject context;
        PinTextContent::build_async(text, html, formatted, &context, [&size](std::unique_ptr<PinTextContent> content) { size = content->size(); });
        return QTest::qWaitFor([&size] { return size.isValid(); }, 5000) ? size : QSize();
    }

    static bool same_pixels(const QImage &a, const QImage &b) {
        return a.convertToFormat(QImage::Format_ARGB32_Premultiplied) == b.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    bool desktop() const { return QGuiApplication::platformName() == "windows"; }

    // The offscreen platform has no font directory; the font file is loaded once per process,
    // so a repeated run (check.ps1 -Mode Leak) does not add it again. Empty when the system has none.
    static QString yahei_family() {
        static const int id = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/msyh.ttc"));
        return id < 0 ? QString() : QFontDatabase::applicationFontFamilies(id).value(0);
    }

    // Where a line's character is shown, for clicking it.
    static QPoint char_point(PinWindow *pin, const OcrLayoutLine &line, int index) {
        return pin->view->mapFromScene(QPointF(line.bounds.left() + line.centers[index], line.bounds.center().y()));
    }

    // The order Windows delivers: a double click, then a press soon after at the same place.
    static void triple_click(PinWindow *pin, const QPoint &local) {
        double_click(pin, local, Qt::LeftButton);
        click(pin, local, Qt::LeftButton);
    }
    static QString arial_family() {
        static const int id = QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf"));
        return id < 0 ? QString() : QFontDatabase::applicationFontFamilies(id).value(0);
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        // The settings singleton reads a throwaway file, never the registry. It binds once per
        // process, so a repeated run (check.ps1 -Mode Leak) keeps the first file.
        if (!settings_) settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        QCOMPARE(&config_->settings(), settings_);
    }

    void init() {
        settings_->clear();
        // Save dialogs create the save folder; keep it out of the user's real one.
        config_->set_screenshot_save_path(dir_.filePath("captures"));
        // Recognition would load an OCR engine 300 ms after every pin.
        config_->set_ocr_enabled(false);
    }

    void cleanup() {
        for (const auto &pin : pins_)
            if (pin) pin->close_window();
        pins_.clear();
        PinManager::instance()->close_all();
        flush_deletes();
    }

    // ---- PinImageTransform: rotation and flip state ----

    void rotationStateCyclesAndResets() {
        PinImageTransform transform;
        QCOMPARE(transform.rotation(), 0);
        QVERIFY(!transform.flip_h() && !transform.flip_v() && !transform.has_transform());
        for (int want : {90, 180, 270, 0}) {
            transform.rotate_cw();
            QCOMPARE(transform.rotation(), want);
        }
        for (int want : {270, 180, 90, 0}) {
            transform.rotate_ccw();
            QCOMPARE(transform.rotation(), want);
        }
        transform.rotate_cw();
        transform.rotate_ccw();
        QVERIFY(!transform.has_transform());

        QVERIFY(!transform.is_rotated_90_or_270());
        for (bool want : {true, false, true}) {
            transform.rotate_cw();
            QCOMPARE(transform.is_rotated_90_or_270(), want);
        }
        transform.reset();

        transform.flip_horizontal();
        QVERIFY(transform.flip_h() && !transform.flip_v());
        // A flip alone counts as a transform: export and view must follow it.
        QVERIFY(transform.has_transform());
        transform.flip_vertical();
        QVERIFY(transform.flip_h() && transform.flip_v());
        transform.flip_horizontal();
        QVERIFY(!transform.flip_h() && transform.flip_v());

        transform.rotate_cw();
        transform.reset();
        QCOMPARE(transform.rotation(), 0);
        QVERIFY(!transform.flip_h() && !transform.flip_v() && !transform.has_transform());
    }

    void displaySizeSwapsOnlyForQuarterTurns() {
        PinImageTransform transform;
        const QSize size(200, 100);
        QCOMPARE(transform.display_size(size), size);
        for (const QSize &want : {QSize(100, 200), QSize(200, 100), QSize(100, 200), QSize(200, 100)}) {
            transform.rotate_cw();
            QCOMPARE(transform.display_size(size), want);
        }
        transform.flip_horizontal();
        transform.flip_vertical();
        QCOMPARE(transform.display_size(size), size);
    }

    void transformImageMovesPixelsAndKeepsDpr() {
        const auto image = marked();
        QCOMPARE(transform_state(0, false, false).transform_image(image).cacheKey(), image.cacheKey());
        auto out = transform_state(90, false, false).transform_image(marked(8, 4));
        QCOMPARE(out.size(), QSize(4, 8));
        QCOMPARE(out.pixel(out.width() - 1, 0), 0xFFFFFFFFu);
        out = transform_state(180, false, false).transform_image(marked(8, 4));
        QCOMPARE(out.size(), QSize(8, 4));
        QCOMPARE(out.pixel(7, 3), 0xFFFFFFFFu);
        out = transform_state(0, true, false).transform_image(marked(8, 4));
        QCOMPARE(out.pixel(out.width() - 1, 0), 0xFFFFFFFFu);
        out = transform_state(0, false, true).transform_image(marked(8, 4));
        QCOMPARE(out.pixel(0, out.height() - 1), 0xFFFFFFFFu);
        // Losing the ratio would save a high-DPI pin at twice its size.
        auto hidpi = marked();
        hidpi.setDevicePixelRatio(2.0);
        QCOMPARE(transform_state(90, false, false).transform_image(hidpi).devicePixelRatio(), 2.0);
    }

    // ---- Window: initial size ----

    void oversizedImagesShrinkToFitTheScreen() {
        const auto area = QApplication::primaryScreen()->geometry();
        auto *pin = make_pin(QSize(area.width() * 2, area.height() * 3), QPoint(0, 0));
        const auto geo = pin->geometry();
        QVERIFY(std::abs(geo.height() - area.height() * AUTO_SHRINK_RATIO) <= 1);
        QVERIFY(close_to(double(geo.width()) / geo.height(), 2.0 * area.width() / (3.0 * area.height()), 0.01));
        QVERIFY(std::abs(geo.center().x() - area.center().x()) <= 1);
        QVERIFY(std::abs(geo.center().y() - area.center().y()) <= 1);
        QVERIFY(close_to(pin->scale_factor, double(geo.height()) / (area.height() * 3), 0.001));

        auto *fits = make_pin(QSize(area.width() - 10, area.height() - 10), QPoint(5, 5));
        QCOMPARE(fits->geometry().topLeft(), QPoint(5, 5));
        QCOMPARE(fits->scale_factor, 1.0);

        auto *kept = make_pin(QSize(area.width() * 2, 100), QPoint(0, 0), {{"pin_auto_shrink", false}});
        QCOMPARE(kept->width(), area.width() * 2);
        QCOMPARE(kept->scale_factor, 1.0);
    }

    void defaultOpacityIsClampedToTheFloor() {
        QVERIFY(close_to(make_pin({{"pin_default_opacity", 0.6}})->windowOpacity(), 0.6, 0.01));
        // A hand-edited 0.05 must not make the pin all but invisible.
        QVERIFY(close_to(make_pin({{"pin_default_opacity", 0.05}})->windowOpacity(), MIN_OPACITY, 0.01));
    }

    // ---- Window: wheel zoom ----

    void wheelZoomStepsLinearlyFromTheOriginalSize() {
        auto *pin = make_pin(QSize(400, 300), QPoint(10, 20));
        wheel(pin, 120);
        QCOMPARE(pin->scale_factor, 1.1);
        QCOMPARE(pin->size(), QSize(440, 330));
        QCOMPARE(hint_text(pin), QString("110%"));
        // Points of the original size, not compounding percentages.
        wheel(pin, 120, Qt::NoModifier, false, 2);
        QCOMPARE(pin->scale_factor, 1.3);
        QCOMPARE(pin->size(), QSize(520, 390));
        wheel(pin, -120, Qt::NoModifier, false, 3);
        QCOMPARE(pin->scale_factor, 1.0);
        QCOMPARE(pin->size(), QSize(400, 300));
        // The top-left corner is the default anchor.
        QCOMPARE(pin->pos(), QPoint(10, 20));
    }

    void wheelZoomIsClampedBetweenMinSideAndFourTimes() {
        auto *pin = make_pin(QSize(400, 300), QPoint(10, 20));
        wheel(pin, 120, Qt::NoModifier, false, 40);
        QCOMPARE(pin->scale_factor, MAX_SCALE);
        QCOMPARE(pin->size(), QSize(1600, 1200));
        wheel(pin, -120, Qt::NoModifier, false, 60);
        // The shorter side stops at 50 pixels.
        QVERIFY(close_to(pin->scale_factor, 50.0 / 300));
        QCOMPARE(pin->height(), MIN_SIDE);
        QCOMPARE(pin->width(), 67);

        // The minimum follows the image shape.
        auto *wide = make_pin(QSize(1000, 100), QPoint(10, 20), {{"pin_auto_shrink", false}});
        wheel(wide, -120, Qt::NoModifier, false, 20);
        QCOMPARE(wide->scale_factor, 0.5);
        QCOMPARE(wide->size(), QSize(500, 50));
    }

    void zoomStepFollowsTheSetting() {
        auto *pin = make_pin(QSize(400, 300), QPoint(10, 20), {{"pin_zoom_step", 25}});
        wheel(pin, 120);
        QCOMPARE(pin->scale_factor, 1.25);
        QCOMPARE(pin->size(), QSize(500, 375));
    }

    void fineZoomArrivesHorizontalAndSnapsBackToOneHundred() {
        // Windows delivers Alt+wheel as horizontal scrolling.
        auto *pin = make_pin(QSize(400, 300), QPoint(10, 20));
        wheel(pin, 120, Qt::AltModifier, true, 3);
        QCOMPARE(pin->scale_factor, 1.03);
        QCOMPARE(hint_text(pin), QString("103%"));
        wheel(pin, -120, Qt::AltModifier, true, 3);
        QCOMPARE(pin->scale_factor, 1.0);
        QCOMPARE(pin->size(), QSize(400, 300));

        auto *custom = make_pin(QSize(400, 300), QPoint(10, 20), {{"pin_fine_zoom_step", 5}});
        wheel(custom, -120, Qt::AltModifier, true);
        QCOMPARE(custom->scale_factor, 0.95);
    }

    void configuredZoomAnchorStaysPut() {
        const QList<QPair<QString, QPointF>> anchors = {
            {"top_right", {1, 0}}, {"bottom_left", {0, 1}}, {"bottom_right", {1, 1}}, {"center", {0.5, 0.5}}, {"unknown", {0, 0}}};
        for (const auto &[name, fraction] : anchors) {
            auto *pin = make_pin(QSize(400, 300), QPoint(100, 100), {{"pin_zoom_anchor", name}});
            const auto before = pin->geometry();
            const QPointF fixed(before.x() + fraction.x() * before.width(), before.y() + fraction.y() * before.height());
            wheel(pin, 120);
            const auto after = pin->geometry();
            const QPointF now(after.x() + fraction.x() * after.width(), after.y() + fraction.y() * after.height());
            QVERIFY2(std::abs(now.x() - fixed.x()) <= 1 && std::abs(now.y() - fixed.y()) <= 1, qPrintable(name));
            pin->close_window();
        }
    }

    void mouseAnchoredZoomKeepsThePointUnderTheCursor() {
        auto *pin = make_pin({{"pin_zoom_anchor", "mouse"}});
        const auto cursor = pin->mapToGlobal(QPoint(150, 30));
        QCursor::setPos(cursor);
        QCOMPARE(QCursor::pos(), cursor);
        wheel(pin, 120);
        const auto local = pin->mapFromGlobal(cursor);
        QVERIFY(close_to(double(local.x()) / pin->width(), 150.0 / 200, 0.01));
        QVERIFY(close_to(double(local.y()) / pin->height(), 30.0 / 120, 0.01));
    }

    void rotatedPinsZoomFromTheirTurnedSize() {
        auto *pin = make_pin(QSize(400, 300), QPoint(100, 100));
        pin->rotate_image_cw();
        QCOMPARE(pin->size(), QSize(300, 400));
        wheel(pin, 120);
        QCOMPARE(pin->size(), QSize(330, 440));
        // Reset keeps the rotation and the top-left corner.
        const auto top_left = pin->pos();
        pin->reset_to_original_size();
        QCOMPARE(pin->size(), QSize(300, 400));
        QCOMPARE(pin->pos(), top_left);
        QCOMPARE(pin->scale_factor, 1.0);
        pin->reset_image_transform();
        QCOMPARE(pin->size(), QSize(400, 300));
    }

    // ---- Window: opacity ----

    void ctrlWheelStepsOpacityWithinItsBounds() {
        auto *pin = make_pin();
        const auto geometry = pin->geometry();
        wheel(pin, -120, Qt::ControlModifier);
        QVERIFY(close_to(pin->windowOpacity(), 0.95, 0.005));
        QCOMPARE(hint_text(pin), QString::fromUtf8("α 95%"));
        wheel(pin, -120, Qt::ControlModifier, false, 3);
        QVERIFY2(close_to(pin->windowOpacity(), 0.80, 0.005), qPrintable(QString::number(pin->windowOpacity())));
        // Rounded per step, so 0.9 never shows as 89%.
        wheel(pin, 120, Qt::ControlModifier, false, 2);
        QVERIFY2(close_to(pin->windowOpacity(), 0.90, 0.005), qPrintable(QString::number(pin->windowOpacity())));
        QCOMPARE(hint_text(pin), QString::fromUtf8("α 90%"));
        wheel(pin, 120, Qt::ControlModifier, false, 5);
        QVERIFY(close_to(pin->windowOpacity(), 1.0, 0.005));
        // Opacity never touches the scale.
        QCOMPARE(pin->scale_factor, 1.0);
        QCOMPARE(pin->geometry(), geometry);

        auto *faint = make_pin({{"pin_default_opacity", 0.15}});
        wheel(faint, -120, Qt::ControlModifier, false, 5);
        QVERIFY(close_to(faint->windowOpacity(), MIN_OPACITY, 0.005));

        config_->set_app_setting("pin_default_opacity", 1.0);
        auto *coarse = make_pin({{"pin_opacity_step", 10}});
        wheel(coarse, -120, Qt::ControlModifier, false, 2);
        QVERIFY(close_to(coarse->windowOpacity(), 0.80, 0.005));
    }

    void thumbnailModeLeavesTheWheelAlone() {
        auto *pin = make_pin();
        QCursor::setPos(0, 0);
        pin->toggle_thumbnail_mode();
        const auto geometry = pin->geometry();
        wheel(pin, 120);
        wheel(pin, -120, Qt::ControlModifier);
        QCOMPARE(pin->geometry(), geometry);
        QVERIFY(close_to(pin->windowOpacity(), 1.0, 0.005));
    }

    // Leaving the thumbnail puts the pin back where it was, whatever the size and wherever the pointer was;
    // a dragged thumbnail moves the restored pin by as much.
    void thumbnailRoundTripKeepsThePinInPlace_data() {
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("where");
        for (const auto &size : {QSize(500, 350), QSize(451, 301)})
            for (const QString where : {"centre", "near_corner", "outside"})
                QTest::newRow(qPrintable(QString("%1x%2 %3").arg(size.width()).arg(size.height()).arg(where))) << size << where;
    }
    void thumbnailRoundTripKeepsThePinInPlace() {
        QFETCH(QSize, size);
        QFETCH(QString, where);
        auto *pin = make_pin(size, QPoint(300, 200));
        const auto before = pin->geometry();
        QCursor::setPos(where == "centre" ? before.center() : where == "near_corner" ? before.topLeft() + QPoint(5, 5)
                                                                                     : before.bottomRight() + QPoint(200, 200));
        pin->toggle_thumbnail_mode();
        QVERIFY(pin->thumbnail_mode());
        pin->toggle_thumbnail_mode();
        QCOMPARE(pin->geometry(), before);
    }
    void draggedThumbnailMovesTheRestoredPin() {
        auto *pin = make_pin(QSize(500, 350), QPoint(300, 200));
        const auto before = pin->geometry();
        QCursor::setPos(before.center());
        pin->toggle_thumbnail_mode();
        pin->move(pin->pos() + QPoint(150, -40));
        pin->toggle_thumbnail_mode();
        QCOMPARE(pin->geometry(), before.translated(150, -40));
    }

    // ---- Window: dragging and edge resizing ----

    void draggingFollowsThePointerFromTheOriginalAnchor() {
        auto *pin = make_pin();
        const auto start = pin->pos();
        const auto grab = pin->view->viewport()->mapToGlobal(QPoint(100, 60));
        send_mouse_global(pin, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
        QVERIFY(pin->is_dragging());
        QCOMPARE(pin->cursor().shape(), Qt::ClosedHandCursor);
        send_mouse_global(pin, QEvent::MouseMove, grab + QPoint(30, -10), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(pin->pos(), start + QPoint(30, -10));
        send_mouse_global(pin, QEvent::MouseMove, grab + QPoint(10, 20), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(pin->pos(), start + QPoint(10, 20));
        send_mouse_global(pin, QEvent::MouseButtonRelease, grab + QPoint(10, 20), Qt::LeftButton, Qt::NoButton);
        QVERIFY(!pin->is_dragging());
        QCOMPARE(pin->cursor().shape(), Qt::ArrowCursor);
        QCOMPARE(pin->size(), QSize(200, 120));
        // Moving without a drag does nothing.
        pin->update_window_drag(grab + QPoint(300, 300));
        QCOMPARE(pin->pos(), start + QPoint(10, 20));
    }

    void edgesShowResizeCursors() {
        auto *pin = make_pin();
        auto *viewport = pin->view->viewport();
        send_mouse(pin, QEvent::MouseMove, QPoint(199, 60), Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::SizeHorCursor);
        send_mouse(pin, QEvent::MouseMove, QPoint(199, 119), Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::SizeFDiagCursor);
        send_mouse(pin, QEvent::MouseMove, QPoint(0, 119), Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::SizeBDiagCursor);
        send_mouse(pin, QEvent::MouseMove, QPoint(100, 0), Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::SizeVerCursor);
        send_mouse(pin, QEvent::MouseMove, QPoint(100, 60), Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::ArrowCursor);
    }

    void draggingAnEdgeKeepsTheRatioAndTheOppositeEdge() {
        auto *pin = make_pin();
        const auto start = pin->geometry();
        drag(pin, QPoint(199, 60), QPoint(299, 60));
        QCOMPARE(pin->geometry().topLeft(), start.topLeft());
        QCOMPARE(pin->size(), QSize(300, 180));
        QCOMPARE(pin->scale_factor, 1.5);

        auto *left = make_pin();
        const int right = left->geometry().right();
        drag(left, QPoint(0, 60), QPoint(100, 60));
        QCOMPARE(left->geometry().right(), right);
        QCOMPARE(left->size(), QSize(100, 60));

        // A corner follows the direction that changes more.
        auto *corner = make_pin();
        drag(corner, QPoint(199, 119), QPoint(299, 125));
        QCOMPARE(corner->size(), QSize(300, 180));

        auto *shrink = make_pin();
        const auto pos = shrink->pos();
        drag(shrink, QPoint(199, 60), QPoint(150, 60));
        QCOMPARE(shrink->pos(), pos);
        QVERIFY(shrink->width() < 200);
    }

    void edgeResizeCanBeTurnedOffOrBlocked() {
        auto *off = make_pin({{"pin_border_resize", false}});
        drag(off, QPoint(199, 60), QPoint(299, 60));
        QCOMPARE(off->width(), 200);
        config_->set_app_setting("pin_border_resize", true);

        auto *locked = make_pin();
        locked->set_locked(true);
        drag(locked, QPoint(199, 60), QPoint(299, 60));
        QCOMPARE(locked->width(), 200);

        auto *thumbnail = make_pin();
        QCursor::setPos(0, 0);
        thumbnail->toggle_thumbnail_mode();
        const auto size = thumbnail->size();
        drag(thumbnail, QPoint(size.width() - 1, size.height() / 2), QPoint(size.width() + 60, size.height() / 2));
        QCOMPARE(thumbnail->size(), size);

        auto *allowed = make_pin({{"pin_lock_block_move", false}});
        allowed->set_locked(true);
        drag(allowed, QPoint(199, 60), QPoint(299, 60));
        QCOMPARE(allowed->width(), 300);
    }

    // ---- Window: lock ----

    void lockingShowsTheLockColourEvenWithoutABorder_data() {
        QTest::addColumn<bool>("border");
        QTest::newRow("border") << true;
        QTest::newRow("no border") << false;
    }

    void lockingShowsTheLockColourEvenWithoutABorder() {
        QFETCH(bool, border);
        auto *pin = make_pin({{"pin_auto_border", border}});
        pin->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(pin));
        pin->set_locked(true);
        QVERIFY(pin->locked());
        QVERIFY(pin->border_overlay && !pin->border_overlay->isHidden());
        QCOMPARE(pin->border_overlay->border_color, LOCKED_BORDER_COLOR);
        QCOMPARE(hint_text(pin), LOCKED_HINT);
        pin->set_locked(false);
        QCOMPARE(hint_text(pin), QString("Unlocked"));
        if (border) {
            QVERIFY(!pin->border_overlay->isHidden());
            QCOMPARE(pin->border_overlay->border_color, pin->border_color);
        } else {
            QVERIFY(pin->border_overlay->isHidden());
        }
    }

    void togglingTheBorderWhileLockedKeepsTheLockFrame() {
        auto *pin = make_pin();
        pin->set_locked(true);
        pin->toggle_border_effect();
        QVERIFY(!pin->border_enabled);
        QVERIFY(!pin->border_overlay->isHidden());
        pin->toggle_lock();
        QVERIFY(!pin->locked());
        QVERIFY(pin->border_overlay->isHidden());
    }

    // The theme colour marks the pin that takes keys; the others turn grey.
    void theBorderMarksThePinWithKeyboardFocus() {
        auto *first = make_pin(QSize(200, 120), QPoint(100, 100), {{"pin_auto_border", true}});
        auto *second = make_pin(QSize(200, 120), QPoint(400, 100));
        const auto colour = [](PinWindow *pin) { return pin->border_overlay->border_color; };
        first->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(first));
        QTRY_COMPARE(colour(first), first->border_color);
        QCOMPARE(colour(second), UNFOCUSED_BORDER_COLOR);
        second->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(second));
        QTRY_COMPARE(colour(first), UNFOCUSED_BORDER_COLOR);
        QCOMPARE(colour(second), second->border_color);
        // Its own toolbar keeps the pin marked.
        second->toggle_toolbar();
        second->toolbar->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(second->toolbar));
        QCOMPARE(colour(second), second->border_color);
        // The lock colour holds with or without focus.
        first->set_locked(true);
        QCOMPARE(colour(first), LOCKED_BORDER_COLOR);
        first->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(first));
        QTRY_COMPARE(colour(second), UNFOCUSED_BORDER_COLOR);
        QCOMPARE(colour(first), LOCKED_BORDER_COLOR);
        first->set_locked(false);
        QCOMPARE(colour(first), first->border_color);
        QWidget elsewhere;
        elsewhere.show();
        elsewhere.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&elsewhere));
        QTRY_COMPARE(colour(first), UNFOCUSED_BORDER_COLOR);
        QCOMPARE(colour(second), UNFOCUSED_BORDER_COLOR);
    }

    // The overlay is translucent: a full update would repaint the whole image beneath it on every focus change.
    void aBorderColourChangeRepaintsOnlyTheRing() {
        QWidget host;
        host.resize(400, 300);
        auto *below = new QWidget(&host);
        below->setGeometry(host.rect());
        PaintRecorder recorder;
        below->installEventFilter(&recorder);
        auto *overlay = new PinBorderOverlay(&host, 0, QColor(Qt::blue));
        overlay->setGeometry(host.rect());
        host.show();
        QVERIFY(QTest::qWaitForWindowExposed(&host));
        QTRY_VERIFY(!recorder.painted.isEmpty());
        recorder.painted = {};
        overlay->set_border_color(QColor(Qt::red));
        QTRY_VERIFY(recorder.painted.contains(QPoint(1, 150)));
        QVERIFY(!recorder.painted.intersects(QRect(10, 10, 380, 280)));
        // An unchanged colour repaints nothing.
        recorder.painted = {};
        overlay->set_border_color(QColor(Qt::red));
        QTest::qWait(50);
        QVERIFY(recorder.painted.isEmpty());
    }

    void aLockedPinDoesNotMoveOrZoom() {
        auto *pin = make_pin();
        pin->set_locked(true);
        hide_hint(pin);
        const auto start = pin->pos();
        const auto grab = pin->view->viewport()->mapToGlobal(QPoint(100, 60));
        send_mouse_global(pin, QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
        // A click (below the drag distance) stays quiet.
        send_mouse_global(pin, QEvent::MouseMove, grab + QPoint(1, 0), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(hint_text(pin), QString());
        QVERIFY(pin->is_dragging());
        // A real drag hints once: the drag ends with the hint.
        send_mouse_global(pin, QEvent::MouseMove, grab + QPoint(150, 130), Qt::NoButton, Qt::LeftButton);
        QCOMPARE(hint_text(pin), LOCKED_HINT);
        QVERIFY(!pin->is_dragging());
        send_mouse_global(pin, QEvent::MouseButtonRelease, grab + QPoint(150, 130), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(pin->pos(), start);

        wheel(pin, 120, Qt::NoModifier, false, 3);
        wheel(pin, 120, Qt::AltModifier, true);
        QCOMPARE(pin->scale_factor, 1.0);
        QCOMPARE(pin->size(), QSize(200, 120));
        // Opacity still works.
        wheel(pin, -120, Qt::ControlModifier);
        QVERIFY(close_to(pin->windowOpacity(), 0.95, 0.005));
    }

    void mouseGesturesRunAndAreBlockedWhenLocked_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("close") << "close";
        QTest::newRow("reset") << "reset";
        QTest::newRow("thumbnail") << "thumbnail";
    }

    void mouseGesturesRunAndAreBlockedWhenLocked() {
        QFETCH(QString, action);
        if (action != "thumbnail") config_->set_app_setting("mouse_pin_" + action, "middle");
        const auto gesture = [&](PinWindow *pin) {
            if (action == "thumbnail") double_click(pin, QPoint(100, 60), Qt::LeftButton, Qt::ShiftModifier);
            else click(pin, QPoint(100, 60), Qt::MiddleButton);
        };
        QCursor::setPos(0, 0);

        auto *locked = make_pin();
        wheel(locked, 120);
        locked->set_locked(true);
        const auto geometry = locked->geometry();
        gesture(locked);
        QVERIFY(!locked->is_closed());
        QVERIFY(!locked->thumbnail_mode());
        QCOMPARE(locked->geometry(), geometry);
        QCOMPARE(hint_text(locked), LOCKED_HINT);

        auto *pin = make_pin();
        wheel(pin, 120);
        gesture(pin);
        if (action == "close") {
            QVERIFY(pin->is_closed());
        } else if (action == "reset") {
            QCOMPARE(pin->size(), QSize(200, 120));
            QCOMPARE(pin->scale_factor, 1.0);
        } else {
            QVERIFY(pin->thumbnail_mode());
            // The release after the double click is swallowed, not a new click.
            QVERIFY(!pin->is_dragging());
        }
    }

    void closeButtonAndKeysRespectTheLockButTheMenuCloses() {
        auto *pin = make_pin();
        pin->set_locked(true);
        pin->close_button()->click();
        pin->close_unless_locked();
        QVERIFY(!pin->is_closed());
        // The context menu's Close calls this directly.
        pin->close_window();
        QVERIFY(pin->is_closed());

        auto *unlocked = make_pin();
        unlocked->close_button()->click();
        QVERIFY(unlocked->is_closed());

        auto *mouse_allowed = make_pin({{"pin_lock_block_click_close", false}});
        mouse_allowed->set_locked(true);
        mouse_allowed->close_unless_locked("key_close");
        QVERIFY(!mouse_allowed->is_closed());
        mouse_allowed->close_button()->click();
        QVERIFY(mouse_allowed->is_closed());

        config_->set_app_setting("pin_lock_block_click_close", true);
        auto *key_allowed = make_pin({{"pin_lock_block_key_close", false}});
        key_allowed->set_locked(true);
        key_allowed->close_button()->click();
        QVERIFY(!key_allowed->is_closed());
        key_allowed->close_unless_locked("key_close");
        QVERIFY(key_allowed->is_closed());
    }

    void aLockedPinMovesAndZoomsWhenMovingIsAllowed() {
        auto *pin = make_pin({{"pin_lock_block_move", false}});
        pin->set_locked(true);
        const auto start = pin->pos();
        pin->start_window_drag(QPoint(250, 250));
        pin->update_window_drag(QPoint(300, 270));
        pin->end_window_drag();
        QCOMPARE(pin->pos(), start + QPoint(50, 20));
        wheel(pin, 120);
        QCOMPARE(pin->scale_factor, 1.1);
    }

    void lockHintsCanBeTurnedOff() {
        auto *pin = make_pin({{"pin_lock_hint", false}});
        pin->set_locked(true);
        QCOMPARE(hint_text(pin), QString());
        pin->start_window_drag(QPoint(250, 250));
        pin->update_window_drag(QPoint(400, 400));
        QCOMPARE(hint_text(pin), QString());
        QCOMPARE(pin->border_overlay->border_color, LOCKED_BORDER_COLOR);
    }

    // ---- Window: click-through ----

    void clickThroughLowersOpacityAndShowsTheBar() {
        const double through = config_->app_defaults().value("pin_click_through_opacity").toDouble();
        auto *pin = make_pin();
        pin->set_click_through(true);
        QVERIFY(pin->click_through());
        QVERIFY(close_to(pin->windowOpacity(), through, 0.01));
        auto *bar = click_through_bar(pin);
        QVERIFY(bar && bar->isVisible());
        QCOMPARE(bar->findChild<QSlider *>()->value(), qRound(through * 100));
        QCOMPARE(hint_text(pin), QString("Click-through on"));
        pin->set_click_through(false);
        QVERIFY(close_to(pin->windowOpacity(), 1.0, 0.01));
        QVERIFY(!bar->isVisible());
        QCOMPARE(hint_text(pin), QString("Click-through off"));

        // An already faint pin is not made more opaque.
        pin->set_click_through(true);
        bar->findChild<QSlider *>()->setValue(30);
        pin->set_click_through(false);
        wheel(pin, -120, Qt::ControlModifier, false, 15);
        const double faint = pin->windowOpacity();
        QVERIFY(faint < 0.3);
        pin->set_click_through(true);
        QVERIFY(close_to(pin->windowOpacity(), faint, 0.01));
        pin->set_click_through(false);
        QVERIFY(close_to(pin->windowOpacity(), faint, 0.01));
    }

    void theSliderOpacityIsRememberedAndSettingsApplyNextTime() {
        auto *pin = make_pin();
        pin->set_click_through(true);
        auto *slider = click_through_bar(pin)->findChild<QSlider *>();
        slider->setValue(30);
        QVERIFY(close_to(pin->windowOpacity(), 0.3, 0.01));
        pin->set_click_through(false);
        pin->set_click_through(true);
        QVERIFY(close_to(pin->windowOpacity(), 0.3, 0.01));
        pin->set_click_through(false);

        config_->set_app_setting("pin_click_through_opacity", 0.4);
        pin->refresh_appearance();
        pin->set_click_through(true);
        QVERIFY(close_to(pin->windowOpacity(), 0.4, 0.01));
        QCOMPARE(slider->value(), 40);
    }

    void theExitButtonLeavesClickThrough() {
        auto *pin = make_pin();
        pin->set_click_through(true);
        auto *bar = click_through_bar(pin);
        QPushButton *exit = bar->findChild<QPushButton *>();
        QVERIFY(exit);
        exit->click();
        QVERIFY(!pin->click_through());
        QVERIFY(!bar->isVisible());
    }

    void theBarSitsAboveThePinAndFollowsIt() {
        auto *pin = make_pin(QSize(200, 120), QPoint(200, 200));
        pin->set_click_through(true);
        auto *bar = click_through_bar(pin);
        QVERIFY(bar->geometry().bottom() < pin->frameGeometry().top());
        QCOMPARE(bar->x(), pin->frameGeometry().left());
        pin->move(260, 240);
        QVERIFY(bar->geometry().bottom() < pin->frameGeometry().top());
        QCOMPARE(bar->x(), pin->frameGeometry().left());
        // No room above: the bar drops below.
        pin->move(pin->x(), pin->screen()->geometry().top());
        QVERIFY(bar->y() > pin->frameGeometry().bottom());
        // It hides and returns with the pin.
        pin->hide();
        QVERIFY(!bar->isVisible());
        pin->show();
        QVERIFY(bar->isVisible());
    }

    void theHandleMovesThePinUnlessLocked() {
        auto *pin = make_pin(QSize(200, 120), QPoint(200, 200));
        pin->set_click_through(true);
        auto *handle = move_handle(click_through_bar(pin));
        QVERIFY(handle);
        const auto drag_handle = [&](int dx, int dy) {
            QTest::mousePress(handle, Qt::LeftButton, {}, QPoint(5, 5));
            QTest::mouseMove(handle, QPoint(5 + dx, 5 + dy));
            QTest::mouseRelease(handle, Qt::LeftButton, {}, QPoint(5 + dx, 5 + dy));
        };
        const auto start = pin->pos();
        drag_handle(50, 20);
        QCOMPARE(pin->pos(), start + QPoint(50, 20));
        QCOMPARE(click_through_bar(pin)->x(), pin->frameGeometry().left());
        pin->set_locked(true);
        const auto moved = pin->pos();
        drag_handle(60, 60);
        QCOMPARE(pin->pos(), moved);
    }

    void closingAPinInClickThroughRemovesTheBar() {
        auto *pin = make_pin();
        pin->set_click_through(true);
        QPointer<PinClickThroughBar> bar = click_through_bar(pin);
        pin->close_window();
        flush_deletes();
        QVERIFY(!bar);
    }

    // The real window style; offscreen windows have no handle to inspect.
    void clickThroughStyleSurvivesStayOnTopToggles() {
        if (!desktop()) QSKIP("needs -platform windows");
        auto *pin = make_pin();
        QVERIFY(!(ex_style(pin) & WS_EX_TRANSPARENT));
        pin->set_click_through(true);
        QVERIFY(ex_style(pin) & WS_EX_TRANSPARENT);
        // Changing window flags recreates the style; showing applies it again.
        pin->toggle_stay_on_top();
        QVERIFY(!(pin->windowFlags() & Qt::WindowStaysOnTopHint));
        QVERIFY(ex_style(pin) & WS_EX_TRANSPARENT);
        QVERIFY(click_through_bar(pin)->isVisible());
        pin->set_click_through(false);
        QVERIFY(!(ex_style(pin) & WS_EX_TRANSPARENT));
    }

    // ---- Thumbnail mode ----

    void thumbnailCentresOnTheCursorAndRoundTrips() {
        auto *pin = make_pin(QSize(400, 300), QPoint(100, 100));
        const auto cursor = pin->mapToGlobal(QPoint(200, 150));
        QCursor::setPos(cursor);
        pin->toggle_thumbnail_mode();
        QVERIFY(pin->thumbnail_mode());
        QCOMPARE(pin->size(), QSize(100, 100));
        QVERIFY((pin->geometry().center() - cursor).manhattanLength() <= 2);
        pin->toggle_thumbnail_mode();
        QVERIFY(!pin->thumbnail_mode());
        QCOMPARE(pin->size(), QSize(400, 300));
        // The viewed point stays where the thumbnail showed it.
        QVERIFY((pin->mapToGlobal(QPoint(200, 150)) - cursor).manhattanLength() <= 2);
    }

    void thumbnailFallsBackToTheImageCentre() {
        auto *pin = make_pin(QSize(400, 300), QPoint(100, 100));
        const auto centre = pin->geometry().center();
        QCursor::setPos(0, 0);
        pin->toggle_thumbnail_mode();
        QVERIFY((pin->geometry().center() - centre).manhattanLength() <= 2);
        // A corner cursor clamps the view inside the image; leaving restores the size.
        pin->toggle_thumbnail_mode();
        QCursor::setPos(pin->mapToGlobal(QPoint(2, 2)));
        pin->toggle_thumbnail_mode();
        const auto top_left = pin->view->mapToScene(QPoint(0, 0));
        QVERIFY(top_left.x() >= -0.5 && top_left.y() >= -0.5);
        pin->toggle_thumbnail_mode();
        QCOMPARE(pin->size(), QSize(400, 300));
    }

    void rightDragShowsTheSelectedRegion() {
        auto *pin = make_pin(QSize(400, 300), QPoint(100, 100));
        const auto origin = pin->view->viewport()->mapToGlobal(QPoint(20, 20));
        drag(pin, QPoint(20, 20), QPoint(120, 80), Qt::RightButton);
        QVERIFY(pin->thumbnail_mode());
        // Both corners are inclusive, so the window may be a pixel or two larger.
        QVERIFY((pin->pos() - origin).manhattanLength() <= 2);
        QVERIFY(std::abs(pin->width() - 100) <= 2 && std::abs(pin->height() - 60) <= 2);
        pin->toggle_thumbnail_mode();
        QCOMPARE(pin->size(), QSize(400, 300));

        auto *locked = make_pin(QSize(400, 300), QPoint(100, 100));
        locked->set_locked(true);
        drag(locked, QPoint(20, 20), QPoint(120, 80), Qt::RightButton);
        QVERIFY(!locked->thumbnail_mode());
        QCOMPARE(hint_text(locked), LOCKED_HINT);
    }

    // ---- PinManager ----

    // A new pin takes the keys; under a capture that stays open they remain the capture's. Offscreen
    // activates every window it shows, while Windows shows a tool window without activating it.
    void aNewPinTakesKeyboardFocus() {
        if (!desktop()) QSKIP("needs -platform windows");
        auto *manager = PinManager::instance();
        auto *pin = manager->create_pin(solid(QSize(80, 60)), QPoint(10, 20), config_);
        QVERIFY(QTest::qWaitForWindowActive(pin));
        QTRY_COMPARE(pin->border_overlay->border_color, pin->border_color);
        QWidget capture;
        capture.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        capture.setGeometry(300, 300, 300, 200);
        capture.show();
        capture.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&capture));
        manager->begin_capture(&capture);
        auto *under_capture = manager->create_pin(solid(QSize(80, 60)), QPoint(350, 350), config_);
        QTest::qWait(100);
        manager->end_capture();
        QVERIFY(capture.isActiveWindow());
        QCOMPARE(under_capture->border_overlay->border_color, UNFOCUSED_BORDER_COLOR);
    }

    void managerTracksCreatedPins() {
        auto *manager = PinManager::instance();
        QCOMPARE(PinManager::get_instance(), manager);
        QCOMPARE(get_pin_manager(), manager);
        QVERIFY(!manager->has_pins());
        QList<PinWindow *> created;
        connect(manager, &PinManager::pin_created, this, [&](PinWindow *pin) { created.append(pin); });
        auto *first = manager->create_pin(solid(QSize(80, 60)), QPoint(10, 20), config_);
        auto *second = manager->create_pin(solid(QSize(80, 60)), QPoint(200, 20), config_);
        disconnect(manager, &PinManager::pin_created, this, nullptr);
        QCOMPARE(created, (QList<PinWindow *>{first, second}));
        QCOMPARE(first->pos(), QPoint(10, 20));
        QCOMPARE(manager->count(), 2);
        // A copy: changing it leaves the manager alone.
        auto copy = manager->get_all_pins();
        copy.clear();
        QCOMPARE(manager->count(), 2);

        manager->hide_all();
        QVERIFY(!first->isVisible() && !second->isVisible());
        manager->show_all();
        QVERIFY(first->isVisible() && second->isVisible());

        manager->remove_pin(second);
        QCOMPARE(manager->get_all_pins(), QList<PinWindow *>{first});
        QVERIFY(!second->is_closed());
        manager->remove_pin(second);
        QCOMPARE(manager->count(), 1);
        pins_.append(second);
    }

    void managerAnnouncesClosesOnce() {
        auto *manager = PinManager::instance();
        QSignalSpy closed(manager, &PinManager::pin_closed);
        QSignalSpy all_closed(manager, &PinManager::all_pins_closed);
        auto *first = manager->create_pin(solid(QSize(80, 60)), QPoint(10, 20), config_);
        auto *second = manager->create_pin(solid(QSize(80, 60)), QPoint(200, 20), config_);
        first->close_window();
        QCOMPARE(closed.count(), 1);
        QCOMPARE(all_closed.count(), 0);
        QCOMPARE(manager->get_all_pins(), QList<PinWindow *>{second});
        // A second close of the same pin is ignored.
        first->close_window();
        QCOMPARE(closed.count(), 1);
        second->close_window();
        QCOMPARE(closed.count(), 2);
        QCOMPARE(all_closed.count(), 1);
        QVERIFY(!manager->has_pins());
    }

    void closeAllClosesEveryPinAndAnnouncesOnce() {
        auto *manager = PinManager::instance();
        QSignalSpy all_closed(manager, &PinManager::all_pins_closed);
        manager->close_all();
        QCOMPARE(all_closed.count(), 0);
        QList<QPointer<PinWindow>> created;
        for (int i = 0; i < 3; ++i) created.append(manager->create_pin(solid(QSize(80, 60)), QPoint(10 + 90 * i, 20), config_));
        manager->close_all();
        QVERIFY(!manager->has_pins());
        QCOMPARE(all_closed.count(), 1);
        for (const auto &pin : created) QVERIFY(!pin || pin->is_closed());
        flush_deletes();
        for (const auto &pin : created) QVERIFY(!pin);
    }

    void thumbnailBatchTogglesOnlyPinsInTheWrongState() {
        auto *manager = PinManager::instance();
        QCursor::setPos(0, 0);
        auto *first = manager->create_pin(solid(QSize(200, 120)), QPoint(10, 20), config_);
        auto *second = manager->create_pin(solid(QSize(200, 120)), QPoint(300, 20), config_);
        first->toggle_thumbnail_mode();
        manager->set_all_thumbnail_mode(true);
        QVERIFY(first->thumbnail_mode() && second->thumbnail_mode());
        manager->set_all_thumbnail_mode(true);
        QVERIFY(first->thumbnail_mode() && second->thumbnail_mode());
        second->toggle_thumbnail_mode();
        manager->set_all_thumbnail_mode(false);
        QVERIFY(!first->thumbnail_mode() && !second->thumbnail_mode());
        QCOMPARE(first->size(), QSize(200, 120));
    }

    void pinsMoveToTheCentreOfTheirScreen() {
        auto *manager = PinManager::instance();
        auto *pin = manager->create_pin(solid(QSize(200, 100)), QPoint(10, 20), config_);
        manager->move_all_to_screen_center();
        const auto area = pin->screen()->availableGeometry();
        QCOMPARE(pin->pos(), QPoint(area.x() + (area.width() - 200) / 2, area.y() + (area.height() - 100) / 2));
        manager->refresh_all_appearance();
        QVERIFY(!pin->is_closed());
    }

    void saveAllNumbersFilesAndCountsFailures() {
        auto *manager = PinManager::instance();
        QCOMPARE(manager->save_all_to_directory(dir_.filePath("none")), std::make_pair(0, 0));
        auto *first = manager->create_pin(solid(QSize(80, 60)), QPoint(10, 20), config_);
        manager->create_pin(solid(QSize(40, 30)), QPoint(200, 20), config_);
        const auto target = dir_.filePath("nested/pins");
        QCOMPARE(manager->save_all_to_directory(target), std::make_pair(2, 0));
        QCOMPARE(QImage(QDir(target).filePath("pins_001.png")).size(), QSize(80, 60));
        QCOMPARE(QImage(QDir(target).filePath("pins_002.png")).size(), QSize(40, 30));

        // Editing pauses around the save and resumes with the same tool.
        QVERIFY(first->canvas->activate_tool("pen"));
        QVERIFY(first->canvas->is_editing());
        const auto custom = dir_.filePath("custom");
        QDir().mkpath(QDir(custom).filePath("shot_002.png"));
        QCOMPARE(manager->save_all_to_directory(custom, "shot"), std::make_pair(1, 1));
        QVERIFY(QFileInfo::exists(QDir(custom).filePath("shot_001.png")));
        QVERIFY(first->canvas->is_editing());
    }

    void aCaptureLeavesThePinsAsTheyAre() {
        auto *manager = PinManager::instance();
        auto *pin = manager->create_pin(solid(QSize(80, 60)), QPoint(10, 20), config_);
        QWidget capture;
        QVERIFY(!manager->capture_open());
        manager->begin_capture(&capture);
        QVERIFY(manager->capture_open());
        manager->end_capture();
        QVERIFY(!manager->capture_open());
        QVERIFY(pin->windowFlags() & Qt::WindowStaysOnTopHint);
    }

    // The pins keep their band and their order under a capture; one pinned during it goes right below
    // the capture window, still topmost, so nothing has to be put back afterwards.
    void aPinMadeDuringACaptureGoesRightBelowIt() {
        if (!desktop()) QSKIP("needs -platform windows");
        auto *manager = PinManager::instance();
        auto *lower = manager->create_pin(solid(QSize(80, 60)), QPoint(10, 20), config_);
        auto *upper = manager->create_pin(solid(QSize(80, 60)), QPoint(30, 40), config_);
        auto *plain = manager->create_pin(solid(QSize(80, 60)), QPoint(200, 20), config_);
        plain->toggle_stay_on_top();
        const auto hwnd = [](QWidget *window) { return reinterpret_cast<HWND>(window->winId()); };
        // Higher in the z-order: reached first walking down from the top.
        const auto above = [](HWND upper_window, HWND lower_window) {
            for (HWND window = GetWindow(upper_window, GW_HWNDNEXT); window; window = GetWindow(window, GW_HWNDNEXT))
                if (window == lower_window) return true;
            return false;
        };
        QVERIFY(above(hwnd(upper), hwnd(lower)));
        QWidget capture;
        capture.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        capture.setGeometry(300, 300, 300, 200);
        capture.show();
        capture.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&capture));
        manager->begin_capture(&capture);
        QVERIFY(ex_style(lower) & WS_EX_TOPMOST);
        QVERIFY(above(hwnd(&capture), hwnd(upper)));
        auto *late = manager->create_pin(solid(QSize(80, 60)), QPoint(400, 20), config_);
        QVERIFY(ex_style(late) & WS_EX_TOPMOST);
        QCOMPARE(GetWindow(hwnd(&capture), GW_HWNDNEXT), hwnd(late));
        manager->end_capture();
        QVERIFY(above(hwnd(upper), hwnd(lower)));
        QVERIFY(above(hwnd(late), hwnd(upper)));
        QVERIFY(!(ex_style(plain) & WS_EX_TOPMOST));
    }

    // ---- Hover controls ----

    void autoToolbarFollowsTheHoverSession() {
        auto *pin = make_pin({{"pin_auto_toolbar", true}});
        auto &hover = *pin->hover_controls;
        auto *session = pin_timer(pin, PinHoverControls::TOOLBAR_GRACE_MS);
        QVERIFY(session);
        hover.set_pin_hovered(true);
        QVERIFY(toolbar_shown(pin));
        // Leaving keeps it through the grace period, then hides it.
        hover.set_pin_hovered(false);
        QVERIFY(toolbar_shown(pin));
        expire(session);
        QVERIFY(!toolbar_shown(pin));
        // Moving onto the toolbar keeps it.
        hover.set_pin_hovered(true);
        hover.set_pin_hovered(false);
        hover.set_toolbar_hovered(true);
        QVERIFY(!session->isActive());
        QVERIFY(toolbar_shown(pin));
        hover.set_toolbar_hovered(false);
        expire(session);
        QVERIFY(!toolbar_shown(pin));
    }

    void aManualCloseLastsForThisHoverOnly() {
        auto *pin = make_pin({{"pin_auto_toolbar", true}});
        auto &hover = *pin->hover_controls;
        auto *session = pin_timer(pin, PinHoverControls::TOOLBAR_GRACE_MS);
        hover.set_pin_hovered(true);
        hover.toggle_toolbar();
        QVERIFY(!toolbar_shown(pin));
        // Mouse moves do not bring it back.
        for (int i = 0; i < 3; ++i) hover.set_pin_hovered(true);
        hover.sync();
        QVERIFY(!toolbar_shown(pin));
        // Coming back within the grace period keeps the choice.
        hover.set_pin_hovered(false);
        hover.set_pin_hovered(true);
        QVERIFY(!toolbar_shown(pin));
        // After the session ends the next hover shows it again.
        hover.set_pin_hovered(false);
        expire(session);
        hover.set_pin_hovered(true);
        QVERIFY(toolbar_shown(pin));
        hover.toggle_toolbar();
        hover.toggle_toolbar();
        QVERIFY(toolbar_shown(pin));
    }

    void aToolbarToggledWithoutHoverStillEnds() {
        auto *pin = make_pin({{"pin_auto_toolbar", true}});
        pin->toggle_toolbar();
        QVERIFY(toolbar_shown(pin));
        expire(pin_timer(pin, PinHoverControls::TOOLBAR_GRACE_MS));
        QVERIFY(!toolbar_shown(pin));
    }

    void manualModeKeepsTheChoice() {
        auto *pin = make_pin({{"pin_auto_toolbar", false}});
        auto &hover = *pin->hover_controls;
        auto *session = pin_timer(pin, PinHoverControls::TOOLBAR_GRACE_MS);
        hover.set_pin_hovered(true);
        QVERIFY(!toolbar_shown(pin));
        hover.toggle_toolbar();
        hover.set_pin_hovered(false);
        expire(session);
        QVERIFY(toolbar_shown(pin));
        hover.set_pin_hovered(true);
        hover.toggle_toolbar();
        hover.set_pin_hovered(false);
        expire(session);
        hover.set_pin_hovered(true);
        QVERIFY(!toolbar_shown(pin));
    }

    void editingKeepsTheToolbarPastTheGracePeriod() {
        auto *pin = make_pin({{"pin_auto_toolbar", true}});
        auto &hover = *pin->hover_controls;
        auto *session = pin_timer(pin, PinHoverControls::TOOLBAR_GRACE_MS);
        hover.set_pin_hovered(true);
        QVERIFY(pin->canvas->activate_tool("pen"));
        QVERIFY(pin->is_editing);
        hover.set_pin_hovered(false);
        expire(session);
        QVERIFY(toolbar_shown(pin));
        // Leaving the tool hides the toolbar after the grace period.
        pin->canvas->deactivate_tool();
        QVERIFY(toolbar_shown(pin));
        expire(session);
        QVERIFY(!toolbar_shown(pin));
    }

    void anOpenMenuCountsAsHovering() {
        auto *pin = make_pin({{"pin_auto_toolbar", true}});
        auto &hover = *pin->hover_controls;
        auto *session = pin_timer(pin, PinHoverControls::TOOLBAR_GRACE_MS);
        hover.set_pin_hovered(true);
        hover.set_menu_open(true);
        hover.set_pin_hovered(false);
        QVERIFY(!session->isActive());
        QVERIFY(toolbar_shown(pin));
        // The pin got Leave when the menu opened; closing it over the pin keeps hovering.
        QCursor::setPos(pin->mapToGlobal(QPoint(100, 60)));
        auto *under = QApplication::widgetAt(QCursor::pos());
        QVERIFY(under && under->window() == pin);
        hover.set_menu_open(false);
        QVERIFY(!session->isActive());
        QVERIFY(toolbar_shown(pin));
        // Leave onto the pin's own child is still hovering.
        hover.recheck_pin_hovered();
        QVERIFY(!session->isActive());
        // Closing it away from the pin starts the grace period.
        hover.set_menu_open(true);
        QCursor::setPos(pin->mapToGlobal(QPoint(-50, -50)));
        hover.set_menu_open(false);
        expire(session);
        QVERIFY(!toolbar_shown(pin));
    }

    void thumbnailAndClickThroughHideEverything() {
        auto *pin = make_pin({{"pin_auto_toolbar", false}, {"pin_hover_buttons", true}});
        auto &hover = *pin->hover_controls;
        hover.set_pin_hovered(true);
        hover.toggle_toolbar();
        QVERIFY(toolbar_shown(pin));
        QCursor::setPos(0, 0);
        pin->toggle_thumbnail_mode();
        QVERIFY(!toolbar_shown(pin));
        hover.toggle_toolbar();
        QVERIFY(!toolbar_shown(pin));
        pin->toggle_thumbnail_mode();
        QVERIFY(toolbar_shown(pin));

        // Click-through hides toolbar and buttons even while editing.
        QVERIFY(pin->canvas->activate_tool("pen"));
        QVERIFY(pin->close_button()->isVisible());
        pin->set_click_through(true);
        QVERIFY(!toolbar_shown(pin));
        QVERIFY(!pin->close_button()->isVisible() && !pin->toolbar_toggle_button()->isVisible());
        hover.toggle_toolbar();
        QVERIFY(!toolbar_shown(pin));
        pin->set_click_through(false);
        hover.set_pin_hovered(true);
        QVERIFY(pin->close_button()->isVisible());
    }

    void hoverButtonsFollowTheSettingAndMode_data() {
        QTest::addColumn<bool>("auto_toolbar");
        QTest::addColumn<bool>("thumbnail");
        QTest::addColumn<bool>("toolbar_button");
        QTest::newRow("manual") << false << false << true;
        QTest::newRow("auto") << true << false << false;
        QTest::newRow("thumbnail") << false << true << false;
    }

    void hoverButtonsFollowTheSettingAndMode() {
        QFETCH(bool, auto_toolbar);
        QFETCH(bool, thumbnail);
        QFETCH(bool, toolbar_button);
        auto *pin = make_pin({{"pin_auto_toolbar", auto_toolbar}, {"pin_hover_buttons", true}});
        if (thumbnail) {
            QCursor::setPos(0, 0);
            pin->toggle_thumbnail_mode();
        }
        pin->hover_controls->set_pin_hovered(true);
        QVERIFY(pin->close_button()->isVisible());
        QCOMPARE(pin->toolbar_toggle_button()->isVisible(), toolbar_button);
        // The close button lingers briefly after leaving.
        pin->hover_controls->set_pin_hovered(false);
        QVERIFY(pin->close_button()->isVisible());
        expire(pin_timer(pin, PinHoverControls::BUTTONS_GRACE_MS));
        QVERIFY(!pin->close_button()->isVisible());
    }

    void hoverButtonsAreOffByDefault() {
        auto *pin = make_pin();
        pin->hover_controls->set_pin_hovered(true);
        QVERIFY(!pin->close_button()->isVisible());
        QVERIFY(!pin->toolbar_toggle_button()->isVisible());
        QVERIFY(pin->border_overlay && !pin->border_overlay->isHidden());
    }

    // ---- Context menu ----

    void theMenuOffersEveryEntryWithStateAndShortcuts() {
        auto *pin = make_pin();
        const QStringList want = {
            "Copy (CTRL+C)", "Save as", "Translate (Text Recognition)", "Reset size", "Image transform",
            "  Rotate right", "  Rotate left", "  Flip horizontal", "  Flip vertical", "  -", "  Reset transform", "-",
            "[ ] Toolbar (SPACE)", "[ ] Hide annotations (H)", "[x] Always on top", "[ ] Mouse click-through",
            "[ ] Lock (L)", "[x] Border", "[ ] Text selection (P)",
            "[ ] Text mode (T)", "[ ] Thumbnail mode (R)", "-", "Close (ESC)"};
        QCOMPARE(pin_menu(pin), want);
    }

    void hiddenAnnotationsLeaveTheImageShownAndCopied() {
        auto *pin = make_pin(QSize(60, 40));
        PinShortcutController::instance()->reload_bindings();
        QCursor::setPos(pin->mapToGlobal(QPoint(30, 20)));
        auto *mark = pin->canvas->scene->addRect(QRectF(0, 0, 60, 40), Qt::NoPen, QBrush(Qt::red));
        mark->setZValue(10);
        const auto center_of = [pin] { return pin->get_current_image().pixelColor(30, 20); };
        const auto shown_center = [pin] { return pin->view->grab().toImage().pixelColor(30, 20); };
        QCOMPARE(center_of(), QColor(Qt::red));
        QCOMPARE(shown_center(), QColor(Qt::red));
        // H hides them on the pin and in what it copies; the annotation itself is untouched.
        press_key(pin, Qt::Key_H);
        QVERIFY(pin->annotations_hidden());
        QCOMPARE(hint_text(pin), QString("Annotations hidden"));
        QCOMPARE(center_of(), QColor(0x33, 0x66, 0x99));
        QCOMPARE(shown_center(), QColor(0x33, 0x66, 0x99));
        QVERIFY(mark->isVisible() && mark->scene());
        QVERIFY(pin_menu(pin).contains("[x] Hide annotations (H)"));
        press_key(pin, Qt::Key_H);
        QVERIFY(!pin->annotations_hidden());
        QCOMPARE(center_of(), QColor(Qt::red));
        // Drawing shows them again.
        pin->toggle_annotations_hidden();
        QVERIFY(pin->canvas->activate_tool("pen"));
        QVERIFY(!pin->annotations_hidden());
        QCOMPARE(shown_center(), QColor(Qt::red));
    }

    void exportsAreTheOriginalWhateverTheZoom() {
        QImage checker(120, 100, QImage::Format_RGB32);
        for (int y = 0; y < checker.height(); ++y)
            for (int x = 0; x < checker.width(); ++x) checker.setPixel(x, y, (x + y) % 2 ? 0xFF000000 : 0xFFFFFFFF);
        auto *pin = new PinWindow(checker, QPoint(100, 100), config_);
        pins_.append(pin);
        auto *background = pin->canvas->scene->background;
        const auto exported = [pin] { return pin->get_current_image().convertToFormat(QImage::Format_RGB32); };
        QCOMPARE(exported(), checker);
        // Shrunk, the screen shows a smaller copy; copying and saving still get the original.
        wheel(pin, -120, Qt::NoModifier, false, 5);
        QVERIFY(pin->scale_factor < 1.0);
        QTRY_COMPARE(background->pixmap().width(), pin->width());
        QCOMPARE(exported(), checker);
        pin->set_annotations_hidden(true);
        QCOMPARE(exported(), checker);
        // What the screen shows is left as it was.
        QCOMPARE(background->pixmap().width(), pin->width());
    }

    // Each notch asks for the sharp image at once, so it shows while the wheel still turns.
    void zoomingSharpensWhileTheWheelStillTurns() {
        auto *pin = make_pin(QSize(300, 200));
        auto *background = pin->canvas->scene->background;
        wheel(pin, -120, Qt::NoModifier, false, 2);
        bool sharp = false;
        // A notch every 30 ms between 70% and 80%, never back at the original's size.
        for (int notch = 0; notch < 60 && !sharp; ++notch) {
            wheel(pin, notch % 2 ? 120 : -120);
            QTest::qWait(30);
            sharp = background->pixmap().size() == pin->size();
        }
        QVERIFY(sharp);
        QVERIFY(pin->scale_factor < 1.0);
    }

    // An image scaled for an earlier size never replaces what is shown since.
    void anOutdatedScaledImageIsDropped() {
        auto *pin = make_pin(QSize(300, 200));
        auto *background = pin->canvas->scene->background;
        wheel(pin, -120, Qt::NoModifier, false, 3);
        pin->reset_to_original_size();
        QTest::qWait(100);
        QCOMPARE(background->pixmap().cacheKey(), pin->base_pixmap().cacheKey());
        // Several notches at once: the last size is shown, and stays.
        wheel(pin, -120, Qt::NoModifier, false, 3);
        QTRY_COMPARE(background->pixmap().size(), pin->size());
        QTest::qWait(100);
        QCOMPARE(background->pixmap().size(), pin->size());
    }

    // The pin and its background share the image's pixels, and a text's page becomes a pixmap without
    // its pixels being copied.
    void aPinKeepsOneCopyOfItsImage() {
        auto *pin = make_pin(QSize(300, 200));
        QCOMPARE(pin->canvas->scene->background->pixmap().cacheKey(), pin->base_pixmap().cacheKey());
        arial_family();
        const PinTextContent content("shared, not copied", {}, false);
        const QImage page = content.render(content.size());
        QCOMPARE(QPixmap::fromImage(page).toImage().constBits(), page.constBits());
    }

    void aTextPinIsDrawnAgainForExportAndMosaic() {
        arial_family();
        auto *pin = make_text_pin(QString("exported at its own size\n").repeated(4).trimmed());
        auto *content = pin->text_content();
        auto *background = pin->canvas->scene->background;
        // No page is kept: mosaic samples one drawn from the text when asked.
        QVERIFY(content->take_page().isNull());
        QVERIFY(same_pixels(background->image(), content->render(pin->orig_size())));
        wheel(pin, -120, Qt::NoModifier, false, 3);
        QVERIFY(pin->scale_factor < 1.0);
        QTRY_COMPARE(background->pixmap().deviceIndependentSize(), QSizeF(pin->size()));
        QVERIFY(same_pixels(pin->get_current_image(), content->render(pin->orig_size(), pin->devicePixelRatioF())));
    }

    // The image's own entries go; Esc leaves the text first, so Close shows no key.
    void aTextModeMenuDropsTheImageEntries() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        pin->toggle_text_mode();
        const QStringList want = {"Copy (CTRL+C)", "Save as", "Translate (Text Recognition)", "-", "[x] Always on top",
                                  "[ ] Mouse click-through", "[ ] Lock (L)", "[x] Border",
                                  "[x] Text mode (T)", "Edit text (ENTER)", "[ ] Thumbnail mode (R)", "-", "Close"};
        QCOMPARE(pin_menu(pin), want);
    }

    void aThumbnailMenuKeepsOnlySevenEntries() {
        auto *pin = make_pin();
        QCursor::setPos(0, 0);
        pin->toggle_thumbnail_mode();
        const QStringList want = {"Copy (CTRL+C)", "Save as", "-", "[x] Always on top", "[ ] Mouse click-through",
                                  "[ ] Lock (L)", "[x] Thumbnail mode (R)", "-", "Close (ESC)"};
        QCOMPARE(pin_menu(pin), want);
    }

    void theMenuReportsLockAndWhetherEscCloses() {
        auto *pin = make_pin();
        pin->toggle_lock();
        const auto entries = pin_menu(pin);
        QVERIFY(entries.contains("[x] Lock (L)"));
        // Esc cannot close a locked pin, so the hint is dropped.
        QCOMPARE(entries.last(), QString("Close"));
        pin->toggle_lock();
        QVERIFY(pin->canvas->activate_tool("pen"));
        QCOMPARE(pin_menu(pin).last(), QString("Close"));
    }

    void shortcutHintsTrackSettingsAndUnbinding() {
        auto *pin = make_pin();
        const auto label = [&](const QString &prefix) {
            for (const auto &entry : pin_menu(pin))
                if (entry.startsWith(prefix)) return entry.section('\t', 0, 0);
            return QString();
        };
        config_->set_inapp_shortcut("inapp_pin_reset_size", "f2");
        QCOMPARE(label("Reset size"), QString("Reset size (F2)"));
        config_->set_inapp_shortcut("inapp_copy_pin", "ctrl+f2");
        QCOMPARE(label("Copy"), QString("Copy (CTRL+F2)"));
        // Cleared, mouse and reserved bindings show nothing.
        for (const QString binding : {"", "mousemiddle", "ctrl+mousemiddle", "escape", "ctrl+"}) {
            config_->set_inapp_shortcut("inapp_copy_pin", binding);
            QVERIFY2(label("Copy") == "Copy", qPrintable(binding));
        }
        QCOMPARE(pin_shortcut_display("inapp_nonexistent"), QString());
    }

    void menuEntriesReachTheirHandlers() {
        auto *pin = make_pin(QSize(200, 120), QPoint(100, 100), {{"pin_auto_toolbar", false}});
        pin_menu(pin, "Lock");
        QVERIFY(pin->locked());
        pin_menu(pin, "Lock");
        QVERIFY(!pin->locked());
        pin_menu(pin, "Border");
        QVERIFY(!pin->border_enabled);
        pin_menu(pin, "Always on top");
        QVERIFY(!(pin->windowFlags() & Qt::WindowStaysOnTopHint));
        pin_menu(pin, "Toolbar");
        QVERIFY(toolbar_shown(pin));
        pin_menu(pin, "  Rotate right");
        QCOMPARE(pin->size(), QSize(120, 200));
        pin_menu(pin, "  Rotate left");
        QCOMPARE(pin->size(), QSize(200, 120));
        pin_menu(pin, "  Flip horizontal");
        pin_menu(pin, "  Flip vertical");
        QImage flipped = pin->get_current_image();
        QCOMPARE(flipped.size(), QSize(200, 120));
        pin_menu(pin, "  Reset transform");
        wheel(pin, 120);
        pin_menu(pin, "Reset size");
        QCOMPARE(pin->size(), QSize(200, 120));
        pin_menu(pin, "Mouse click-through");
        QVERIFY(pin->click_through());
        pin->set_click_through(false);
        QCursor::setPos(0, 0);
        pin_menu(pin, "Thumbnail mode");
        QVERIFY(pin->thumbnail_mode());
        pin_menu(pin, "Thumbnail mode");
        QVERIFY(!pin->thumbnail_mode());
        // The menu's Close is explicit and works on a locked pin.
        pin->set_locked(true);
        pin_menu(pin, "Close");
        QVERIFY(pin->is_closed());
    }

    void aRightClickOpensTheMenu() {
        auto *pin = make_pin();
        const auto entries = run_menu([pin] { click(pin, QPoint(100, 60), Qt::RightButton); });
        QCOMPARE(entries.value(0), QString("Copy (CTRL+C)"));
        // A right drag selects a region instead and opens no menu.
        QVERIFY(run_menu([pin] { drag(pin, QPoint(20, 20), QPoint(120, 80), Qt::RightButton); }).isEmpty());
        QVERIFY(pin->thumbnail_mode());
    }

    // ---- Keyboard shortcuts: the focused pin, else the pin under the pointer ----

    void normalKeysActOnThePinUnderThePointer() {
        auto *pin = make_pin();
        QCursor::setPos(pin->mapToGlobal(QPoint(100, 60)));
        PinShortcutController::instance()->reload_bindings();
        QCOMPARE(PinShortcutController::instance()->find_pin_under_cursor(), pin);
        press_key(pin, Qt::Key_L);
        QVERIFY(pin->locked());
        // Locked: R and Esc only hint.
        press_key(pin, Qt::Key_R);
        QVERIFY(!pin->thumbnail_mode());
        press_key(pin, Qt::Key_Escape);
        QVERIFY(!pin->is_closed());
        QCOMPARE(hint_text(pin), LOCKED_HINT);
        // The modifiers must match exactly.
        press_key(pin, Qt::Key_L, Qt::ControlModifier);
        QVERIFY(pin->locked());
        press_key(pin, Qt::Key_L);
        QVERIFY(!pin->locked());
        // + and - zoom like the wheel.
        press_key(pin, Qt::Key_Equal);
        QCOMPARE(pin->scale_factor, 1.1);
        press_key(pin, Qt::Key_Minus);
        QCOMPARE(pin->scale_factor, 1.0);
        press_key(pin, Qt::Key_Space);
        QVERIFY(toolbar_shown(pin));
        press_key(pin, Qt::Key_Space);
        QVERIFY(!toolbar_shown(pin));
        press_key(pin, Qt::Key_R);
        QVERIFY(pin->thumbnail_mode());
        press_key(pin, Qt::Key_R);
        QVERIFY(!pin->thumbnail_mode());
        press_key(pin, Qt::Key_Escape);
        QVERIFY(pin->is_closed());
    }

    void resetSizeKeyRespectsLockAndThumbnail() {
        config_->set_inapp_shortcut("inapp_pin_reset_size", "f2");
        PinShortcutController::instance()->reload_bindings();
        auto *pin = make_pin();
        QCursor::setPos(pin->mapToGlobal(QPoint(100, 60)));
        wheel(pin, 120);
        pin->set_locked(true);
        press_key(pin, Qt::Key_F2);
        QCOMPARE(pin->scale_factor, 1.1);
        pin->set_locked(false);
        press_key(pin, Qt::Key_F2);
        QCOMPARE(pin->size(), QSize(200, 120));
        QCursor::setPos(pin->mapToGlobal(QPoint(50, 50)));
        pin->toggle_thumbnail_mode();
        const auto thumbnail = pin->size();
        QCursor::setPos(pin->geometry().center());
        press_key(pin, Qt::Key_F2);
        QCOMPARE(pin->size(), thumbnail);
    }

    // The pointer lookup, used by the translation hotkey, finds the pin really under the pointer.
    void thePointerLookupSkipsPinsNotReallyUnderIt() {
        auto *lower = make_pin(QSize(200, 120), QPoint(100, 100));
        auto *upper = make_pin(QSize(200, 120), QPoint(150, 130));
        auto *controller = PinShortcutController::instance();
        QCursor::setPos(500, 500);
        QCOMPARE(controller->find_pin_under_cursor(), nullptr);
        // The newest pin is on top where they overlap.
        QCursor::setPos(200, 180);
        QCOMPARE(controller->find_pin_under_cursor(), upper);
        // Click-through pins take no shortcuts.
        upper->set_click_through(true);
        QCOMPARE(controller->find_pin_under_cursor(), lower);
        press_key(upper, Qt::Key_Escape);
        QVERIFY(!upper->is_closed());
        upper->set_click_through(false);
        // During a capture the pins are covered by it.
        QWidget capture;
        PinManager::instance()->begin_capture(&capture);
        QCOMPARE(controller->find_pin_under_cursor(), nullptr);
        PinManager::instance()->end_capture();
        // Closed pins are dropped.
        upper->close_window();
        QCOMPARE(controller->find_pin_under_cursor(), lower);
        controller->register_pin(lower);
        controller->unregister_pin(upper);
        QCOMPARE(controller->find_pin_under_cursor(), lower);
    }

    // Keys go to the pin they are typed into, as its border shows, wherever the pointer is.
    void keysGoToTheFocusedPinWhereverThePointerIs() {
        auto *focused = make_pin(QSize(200, 120), QPoint(100, 100));
        auto *other = make_pin(QSize(200, 120), QPoint(400, 100));
        auto *controller = PinShortcutController::instance();
        config_->set_inapp_shortcut("inapp_thumbnail", "mousemiddle");
        controller->reload_bindings();
        focused->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(focused));
        QCOMPARE(controller->focused_pin(), focused);
        QCursor::setPos(other->mapToGlobal(QPoint(100, 60)));
        press_key(focused, Qt::Key_L);
        QVERIFY(focused->locked() && !other->locked());
        QCursor::setPos(700, 500);
        press_key(focused, Qt::Key_L);
        QVERIFY(!focused->locked());
        // A mouse binding acts on the pin it is pressed on.
        QCursor::setPos(other->mapToGlobal(QPoint(100, 60)));
        click(other, QPoint(100, 60), Qt::MiddleButton);
        QVERIFY(other->thumbnail_mode() && !focused->thumbnail_mode());
        other->toggle_thumbnail_mode();
        // The pin's toolbar holds its focus; a click-through pin takes no keys.
        focused->toggle_toolbar();
        focused->toolbar->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(focused->toolbar));
        QCOMPARE(controller->focused_pin(), focused);
        focused->set_click_through(true);
        QCOMPARE(controller->focused_pin(), nullptr);
        focused->set_click_through(false);
        // Keys typed into another window leave the pins alone, the one under the pointer too.
        QWidget elsewhere;
        elsewhere.show();
        elsewhere.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&elsewhere));
        QCOMPARE(controller->focused_pin(), nullptr);
        QCursor::setPos(other->mapToGlobal(QPoint(100, 60)));
        QTest::keyClick(&elsewhere, Qt::Key_L);
        QTest::keyClick(&elsewhere, Qt::Key_Escape);
        QVERIFY(!other->locked() && !focused->locked());
        QVERIFY(!other->is_closed() && !focused->is_closed());
    }

    void editModeKeysLeaveTheToolBeforeClosing() {
        auto *pin = make_pin();
        QCursor::setPos(pin->mapToGlobal(QPoint(150, 100)));
        PinShortcutController::instance()->reload_bindings();
        auto *selection = give_text(pin, {{box(10, 10, 100, 20), "hello", 0.9}});
        drag(pin, QPoint(12, 20), QPoint(108, 20));
        QCOMPARE(selection->selected_text(), QString("hello"));
        QVERIFY(pin->canvas->activate_tool("pen"));
        // The text selection is cleared first.
        press_key(pin, Qt::Key_Escape);
        QVERIFY(!selection->has_selection());
        QVERIFY(pin->canvas->is_editing());
        // Then Esc leaves editing, never closing the pin.
        press_key(pin, Qt::Key_Escape);
        QVERIFY(!pin->canvas->is_editing());
        QVERIFY(!pin->is_closed());
        // L while editing may be typed text: no lock.
        QVERIFY(pin->canvas->activate_tool("pen"));
        press_key(pin, Qt::Key_L);
        QVERIFY(!pin->locked());
        press_key(pin, Qt::Key_Escape);
        press_key(pin, Qt::Key_Escape);
        QVERIFY(pin->is_closed());
    }

    // A modal dialog over the pin (watermark text, toolbar layout) owns the keyboard even with the pointer on the
    // pin: otherwise the first Esc leaves editing and the second closes the pin. The middle button still reaches it.
    void aModalDialogOwnsTheKeyboard() {
        auto *pin = make_pin();
        QCursor::setPos(pin->mapToGlobal(QPoint(150, 100)));
        config_->set_inapp_shortcut("inapp_copy_pin", "mousemiddle");
        PinShortcutController::instance()->reload_bindings();
        QDialog dialog;
        const auto activate = [&dialog](Qt::WindowModality modality) {
            dialog.hide();
            dialog.setWindowModality(modality);
            dialog.show();
            dialog.activateWindow();
            return QTest::qWaitForWindowActive(&dialog);
        };
        QVERIFY(activate(Qt::WindowModal));
        QVERIFY(pin->canvas->activate_tool("pen"));
        press_key(pin, Qt::Key_Escape);
        QVERIFY(pin->canvas->is_editing());
        pin->canvas->deactivate_tool();
        press_key(pin, Qt::Key_Escape);
        QVERIFY(!pin->is_closed());
        QVERIFY(OpenClipboard(nullptr));
        EmptyClipboard();
        CloseClipboard();
        click(pin, QPoint(150, 100), Qt::MiddleButton);
        QVERIFY(wait_for_clipboard_writes(10.0));
        QVERIFY(IsClipboardFormatAvailable(CF_DIB));
        // A non-modal active window leaves the keys to the pin.
        QVERIFY(activate(Qt::NonModal));
        press_key(pin, Qt::Key_Escape);
        QVERIFY(pin->is_closed());
    }

    // The context menu opens under the pointer, on the pin, without taking activation: Esc belongs to the menu.
    // Closing the pin from inside the menu's loop used to delete the menu as its child while it ran.
    void escapeOnTheContextMenuClosesOnlyTheMenu() {
        auto *pin = make_pin();
        QCursor::setPos(pin->mapToGlobal(QPoint(150, 100)));
        PinShortcutController::instance()->reload_bindings();
        bool menu_seen = false;
        QTimer escape;
        escape.setSingleShot(true);
        QObject::connect(&escape, &QTimer::timeout, [&menu_seen] {
            auto *menu = QApplication::activePopupWidget();
            menu_seen = menu != nullptr;
            if (menu) QTest::keyClick(menu, Qt::Key_Escape);
        });
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, [] {
            if (auto *popup = QApplication::activePopupWidget()) popup->close();
        });
        escape.start(0);
        guard.start(3000);
        pin->show_context_menu(pin->mapToGlobal(QPoint(150, 100)));
        QVERIFY(menu_seen);
        QVERIFY(!QApplication::activePopupWidget());
        QVERIFY(!pin->is_closed());
        // Its Close entry still closes the pin.
        pin_menu(pin, "Close");
        QVERIFY(pin->is_closed());
    }

    void copyKeyPrefersTheSelectedText() {
        auto *pin = make_pin();
        QCursor::setPos(pin->mapToGlobal(QPoint(150, 100)));
        PinShortcutController::instance()->reload_bindings();
        // Eleven characters of 10 px from x = 10: dragging from x = 70 passes the centres of "world".
        give_text(pin, {{box(10, 10, 110, 20), "hello world", 0.9}});
        drag(pin, QPoint(70, 20), QPoint(125, 20));
        QApplication::clipboard()->clear();
        press_key(pin, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard_text(), QString("world"));
        // Editing does not change that.
        QVERIFY(pin->canvas->activate_tool("pen"));
        QApplication::clipboard()->clear();
        press_key(pin, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard_text(), QString("world"));
    }

    // Text selected, then the pointer moved off the pin or onto another: Ctrl+C still copies that text.
    void copyTakesTheFocusedPinsSelectionWithThePointerOff() {
        auto *pin = make_pin(QSize(200, 120), QPoint(100, 100));
        auto *other = make_pin(QSize(200, 120), QPoint(400, 100));
        PinShortcutController::instance()->reload_bindings();
        auto *selection = give_text(pin, {{box(10, 10, 100, 20), "hello", 0.9}});
        drag(pin, QPoint(12, 20), QPoint(108, 20));
        QCOMPARE(selection->selected_text(), QString("hello"));
        pin->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(pin));
        for (const auto &pos : {other->mapToGlobal(QPoint(100, 60)), QPoint(700, 500)}) {
            QCursor::setPos(pos);
            QApplication::clipboard()->clear();
            press_key(pin, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(clipboard_text(), QString("hello"));
        }
    }

    void escapeClearsTheTextSelectionBeforeClosing() {
        auto *pin = make_pin();
        QCursor::setPos(pin->mapToGlobal(QPoint(150, 100)));
        PinShortcutController::instance()->reload_bindings();
        auto *selection = give_text(pin, {{box(10, 10, 100, 20), "hello", 0.9}});
        drag(pin, QPoint(12, 20), QPoint(108, 20));
        QVERIFY(selection->has_selection());
        press_key(pin, Qt::Key_Escape);
        QVERIFY(!selection->has_selection());
        QVERIFY(!pin->is_closed());
        press_key(pin, Qt::Key_Escape);
        QVERIFY(pin->is_closed());
    }

    // ---- Recognized text: line geometry and character positions ----

    void linesTakeTheirFrameFromTheCorners() {
        const auto across = line_frame(box(10, 20, 100, 30), false);
        QVERIFY(across.valid());
        QCOMPARE(across.origin, QPointF(10, 20));
        QCOMPARE(across.along, QPointF(1, 0));
        QCOMPARE(across.across, QPointF(0, 1));
        QCOMPARE(across.length, 100.0);
        QCOMPARE(across.thickness, 30.0);
        // Vertical text reads from the top-right corner down.
        const auto down = line_frame(box(10, 20, 30, 100), true);
        QCOMPARE(down.origin, QPointF(40, 20));
        QCOMPARE(down.along, QPointF(0, 1));
        QCOMPARE(down.across, QPointF(-1, 0));
        QCOMPARE(down.length, 100.0);
        QCOMPARE(down.thickness, 30.0);
        QCOMPARE(down.to_source().map(QPointF(50, 15)), QPointF(25, 70));
        // A tilted line: along its top edge, 100 long and 10 thick.
        const auto tilted = line_frame({QPointF(0, 0), QPointF(80, 60), QPointF(74, 68), QPointF(-6, 8)}, false);
        QCOMPARE(tilted.along, QPointF(0.8, 0.6));
        QCOMPARE(tilted.length, 100.0);
        QCOMPARE(tilted.thickness, 10.0);
        // A pixel of lean along a line is the engine's noise: the frame stands upright on the
        // corners' bounding box.
        const auto leaning = line_frame({QPointF(43, 61), QPointF(156, 60), QPointF(155, 81), QPointF(43, 80)}, false);
        QCOMPARE(leaning.origin, QPointF(43, 60));
        QCOMPARE(leaning.along, QPointF(1, 0));
        QCOMPARE(leaning.across, QPointF(0, 1));
        QCOMPARE(leaning.length, 113.0);
        QCOMPARE(leaning.thickness, 21.0);
        // Mirrored, it reads leftwards from the right edge.
        const auto mirrored = line_frame({QPointF(100, 0), QPointF(0, 1), QPointF(0, 21), QPointF(100, 20)}, false);
        QCOMPARE(mirrored.origin, QPointF(100, 0));
        QCOMPARE(mirrored.along, QPointF(-1, 0));
        QCOMPARE(mirrored.across, QPointF(0, 1));
        QCOMPARE(mirrored.length, 100.0);
        // Too few corners, or corners on one line, give no frame.
        QVERIFY(!line_frame({QPointF(0, 0), QPointF(1, 0), QPointF(2, 0)}, false).valid());
        QVERIFY(!line_frame({QPointF(0, 0), QPointF(10, 0), QPointF(20, 0), QPointF(30, 0)}, false).valid());
    }

    void estimatedPositionsWeighWideCharactersDouble() {
        const auto check = [](const QString &text, const QVector<double> &want) {
            const auto got = span_points(estimate_char_spans(text));
            QVERIFY2(all_close(got, want), qPrintable(text + ": " + points_text(got)));
        };
        check("abcd", {0, 0.125, 0.25, 0.25, 0.375, 0.5, 0.5, 0.625, 0.75, 0.75, 0.875, 1});
        check(QString::fromUtf8("中a"), {0, 1.0 / 3, 2.0 / 3, 2.0 / 3, 5.0 / 6, 1});
        check(QString::fromUtf8("，a"), {0, 1.0 / 3, 2.0 / 3, 2.0 / 3, 5.0 / 6, 1});
        // Half-width punctuation next to a wide character is read as the full-width mark.
        check(QString::fromUtf8("中,a"), {0, 0.2, 0.4, 0.4, 0.6, 0.8, 0.8, 0.9, 1});
        check("a,b", {0, 1.0 / 6, 1.0 / 3, 1.0 / 3, 0.5, 2.0 / 3, 2.0 / 3, 5.0 / 6, 1});
        // Characters outside the BMP count once.
        QCOMPARE(estimate_char_spans(QString::fromUtf8("a😀b")).size(), 3);
        QVERIFY(estimate_char_spans("").isEmpty());
    }

    void wordBoxesGiveCharacterPositions() {
        const auto frame = line_frame(box(0, 0, 200, 20), false);
        const auto check = [&](const QString &text, const QVector<OcrWord> &words, const QVector<double> &want_pixels) {
            QVector<double> want;
            for (const double x : want_pixels) want << x / 200;
            const auto got = span_points(spans_from_words(text, frame, words));
            QVERIFY2(all_close(got, want), qPrintable(text + ": " + points_text(got)));
        };
        // Chinese: every character is a word of its own, so each keeps its box, moved a tenth of
        // the 20 px line height along the line, where the engine's ink lies.
        check(QString::fromUtf8("中文"), {{QString::fromUtf8("中"), box(0, 0, 20, 20)}, {QString::fromUtf8("文"), box(25, 0, 20, 20)}}, {2, 12, 22, 27, 37, 47});
        // Read down, the boxes stay where they are.
        const auto column = line_frame(box(0, 0, 20, 200), true);
        const auto down = span_points(spans_from_words(QString::fromUtf8("中文"), column, {{QString::fromUtf8("中"), box(0, 0, 20, 20)}, {QString::fromUtf8("文"), box(0, 25, 20, 20)}}));
        QVERIFY2(all_close(down, {0, 0.05, 0.1, 0.125, 0.175, 0.225}), qPrintable(points_text(down)));
        // Words as wide as their letter counts read as a fixed-width face: the letters split
        // evenly, and the space fills the gap between the words.
        const QVector<OcrWord> latin{{"ab", box(0, 0, 40, 20)}, {"cd", box(60, 0, 40, 20)}};
        check("ab cd", latin, {0, 10, 20, 20, 30, 40, 40, 50, 60, 60, 70, 80, 80, 90, 100});
        check("ill mom", {{"ill", box(0, 0, 30, 20)}, {"mom", box(40, 0, 30, 20)}},
              {0, 5, 10, 10, 15, 20, 20, 25, 30, 30, 35, 40, 40, 45, 50, 50, 55, 60, 60, 65, 70});
        // Extents that follow a proportional face split by its widths: at 10 px a half em, "ill"
        // (0.46 each) is 13.8 px and "mom" (1.69, 1.14, 1.69) 45.2 px.
        check("ill mom", {{"ill", box(0, 0, 13.8, 20)}, {"mom", box(30, 0, 45.2, 20)}},
              {0, 2.3, 4.6, 4.6, 6.9, 9.2, 9.2, 11.5, 13.8, 13.8, 21.9, 30, 30, 38.45, 46.9, 46.9, 52.6, 58.3, 58.3, 66.75, 75.2});
        // A half-width comma after a Chinese character takes a full width (2 against 1.10 per digit).
        check(QString::fromUtf8("中,12"), {{QString::fromUtf8("中"), box(0, 0, 20, 20)}, {",12", box(20, 0, 42, 20)}}, {2, 12, 22, 22, 32, 42, 42, 47.5, 53, 53, 58.5, 64});
        // Spaces before the first word and after the last reach the line's ends.
        check(" 12 ", {{"12", box(50, 0, 40, 20)}}, {0, 25, 50, 50, 60, 70, 70, 80, 90, 90, 145, 200});
        // Words out of step with the text give nothing, so the line falls back to estimates.
        QVERIFY(spans_from_words("abc", frame, {{"abd", box(0, 0, 30, 20)}}).isEmpty());
        QVERIFY(spans_from_words("ab x", frame, latin).isEmpty());
        QVERIFY(spans_from_words("ab cd e", frame, latin).isEmpty());
        QVERIFY(spans_from_words("ab cd", OcrLineFrame(), latin).isEmpty());
    }

    void wordsTellWhichWayALineReads() {
        QVERIFY(words_run_vertically(box(0, 0, 20, 200), {{QString::fromUtf8("一"), box(0, 0, 20, 20)}, {QString::fromUtf8("二"), box(0, 30, 20, 20)}}));
        QVERIFY(!words_run_vertically(box(0, 0, 200, 20), {{"ab", box(0, 0, 40, 20)}, {"cd", box(60, 0, 40, 20)}}));
        // With one word the line's shape decides.
        QVERIFY(words_run_vertically(box(0, 0, 20, 100), {{"x", box(0, 0, 20, 20)}}));
        QVERIFY(!words_run_vertically(box(0, 0, 100, 20), {{"x", box(0, 0, 20, 20)}}));
    }

    // ---- Recognized text: layout and selection ----

    void aLayoutKeepsLinesWithTextAndCorners() {
        const OcrTextLayout layout(ocr_result({{box(0, 0, 50, 10), "hello", 0.9}, {box(0, 20, 60, 10), "world", 0.8}}));
        QCOMPARE(layout.lines().size(), size_t(2));
        QCOMPARE(layout.lines()[1].text, QString("world"));
        QCOMPARE(layout.bounds(), QRectF(0, 0, 60, 30));
        for (const auto &result : {OcrResult{}, ocr_result({{box(0, 0, 1, 1), "x", 1}}, 101), ocr_result({}), ocr_result({{box(0, 0, 10, 10), "", 0.9}, {{}, "ok", 0.9}})})
            QVERIFY(OcrTextLayout(result).empty());
        // Degenerate corners fall back to their bounding box.
        const OcrTextLayout squashed(ocr_result({{{QPointF(5, 5), QPointF(5, 5), QPointF(25, 5), QPointF(25, 15)}, "ok", 1}}));
        QCOMPARE(squashed.lines().size(), size_t(1));
        QCOMPARE(squashed.lines()[0].bounds, QRectF(5, 5, 20, 10));
        // The engine's positions are used when there is one per character; otherwise they are estimated.
        OcrLine given{box(0, 0, 100, 20), "abc", 1};
        given.chars = {{0, 0.1f, 0.2f}, {0.2f, 0.5f, 0.6f}, {0.6f, 0.9f, 1}};
        OcrLine short_of_one{box(0, 30, 100, 20), "abcd", 1};
        short_of_one.chars = given.chars;
        const OcrTextLayout mixed(ocr_result({given, short_of_one}));
        QVERIFY(!mixed.lines()[0].estimated);
        QVERIFY(all_close(QVector<double>(mixed.lines()[0].centers.begin(), mixed.lines()[0].centers.end()), {10, 50, 90}, 1e-4));
        QVERIFY(mixed.lines()[1].estimated);
        QCOMPARE(mixed.lines()[1].centers.size(), size_t(4));
    }

    // A recognition rendered at a screen's pixel ratio comes back in the pin's own image pixels.
    void recognizedPixelsMapToThePinsImage() {
        const OcrTextLayout layout(ocr_result({{box(20, 40, 200, 40), "hi", 1}}), QTransform::fromScale(0.5, 0.5));
        QCOMPARE(layout.bounds(), QRectF(10, 20, 100, 20));
        QCOMPARE(layout.line_at(QPointF(60, 30)), std::optional<int>(0));
    }

    void dragsSelectByCharacterCentres() {
        // Ten characters of 10 px from x = 10: centres at 15, 25, ... 105.
        OcrTextLayout layout(ocr_result({{box(10, 10, 100, 20), "abcdefghij", 1}}));
        const auto selected = [&](const QPointF &from, const QPointF &to) {
            layout.select_between(from, to);
            return layout.selected_text();
        };
        QCOMPARE(selected(QPointF(20, 20), QPointF(60, 20)), QString("bcde"));
        // Backwards reads the same.
        QCOMPARE(selected(QPointF(60, 20), QPointF(20, 20)), QString("bcde"));
        // A character counts once the drag passes its centre.
        QCOMPARE(selected(QPointF(14, 20), QPointF(16, 20)), QString("a"));
        QCOMPARE(selected(QPointF(16, 20), QPointF(24, 20)), QString());
        QVERIFY(!layout.has_selection());
        // Past the line's ends the run stops there.
        QCOMPARE(selected(QPointF(0, 20), QPointF(500, 20)), QString("abcdefghij"));
        layout.select_line(0);
        QCOMPARE(layout.selected_text(), QString("abcdefghij"));
        QVERIFY(layout.selection(0) == std::make_optional(std::make_pair(0, 10)));
        QCOMPARE(layout.selection_bounds(), QRectF(10, 10, 100, 20));
        layout.clear_selection();
        QVERIFY(!layout.has_selection());
        QCOMPARE(layout.selected_text(), QString());
        QVERIFY(layout.selection_bounds().isNull());
    }

    void dragsAcrossLinesKeepShortLinesAndStayInTheirColumn() {
        // A paragraph whose middle line is short, and a second column beside it.
        OcrTextLayout layout(ocr_result({
            {box(10, 10, 100, 20), "abcdefghij", 1},
            {box(10, 40, 30, 20), "klm", 1},
            {box(10, 70, 100, 20), "nopqrstuvw", 1},
            {box(300, 10, 100, 20), "ABCDEFGHIJ", 1},
            {box(300, 40, 100, 20), "KLMNOPQRST", 1},
        }));
        layout.select_between(QPointF(62, 20), QPointF(52, 80));
        QCOMPARE(layout.selected_text(), QString("fghij\nklm\nnopq"));
        layout.select_between(QPointF(306, 20), QPointF(344, 50));
        QCOMPARE(layout.selected_text(), QString("BCDEFGHIJ\nKLMN"));
        // Lines side by side on one row join with a space.
        OcrTextLayout row(ocr_result({{box(0, 0, 50, 20), "hello", 1}, {box(60, 0, 50, 20), "world", 1}}));
        row.select_between(QPointF(1, 10), QPointF(109, 10));
        QCOMPARE(row.selected_text(), QString("hello world"));
    }

    void closeLinesGiveEachPointOneLine() {
        // Paragraph lines boxed 17 px thick and 3 px apart, closer than the slack.
        OcrTextLayout layout(ocr_result({{box(10, 0, 100, 17), "abcdefghij", 1}, {box(10, 20, 100, 17), "klmnopqrst", 1}}));
        layout.select_between(QPointF(20, 16), QPointF(60, 16), 5);
        QCOMPARE(layout.selected_text(), QString("bcde"));
        layout.select_between(QPointF(20, 21), QPointF(60, 21), 5);
        QCOMPARE(layout.selected_text(), QString("lmno"));
        QCOMPARE(layout.line_at(QPointF(50, 16), 5), std::optional<int>(0));
        QCOMPARE(layout.line_at(QPointF(50, 21), 5), std::optional<int>(1));
        // Between the lines the nearer one takes the point.
        QCOMPARE(layout.line_at(QPointF(50, 18), 5), std::optional<int>(0));
        QCOMPARE(layout.line_at(QPointF(50, 19.5), 5), std::optional<int>(1));
        layout.select_between(QPointF(20, 16), QPointF(60, 21), 5);
        QCOMPARE(layout.selected_text(), QString("bcdefghij\nklmno"));
    }

    void rotatedAndVerticalLinesSelectAlongTheirOwnDirection() {
        // A line tilted by 30 degrees, 100 long and 20 thick, and a vertical one reading down.
        const QPointF along(std::cos(M_PI / 6), std::sin(M_PI / 6)), across(-along.y(), along.x());
        const QPointF origin(100, 100);
        const QVector<QPointF> tilted{origin, origin + 100 * along, origin + 100 * along + 20 * across, origin + 20 * across};
        OcrLine vertical{box(300, 0, 20, 100), QString::fromUtf8("一二三四五"), 1};
        vertical.vertical = true;
        OcrTextLayout layout(ocr_result({{tilted, "abcdefghij", 1}, vertical}));
        const auto at = [&](int line, double x, double y) { return layout.lines()[line].frame.to_source().map(QPointF(x, y)); };
        QCOMPARE(layout.line_at(at(0, 50, 10)), std::optional<int>(0));
        QVERIFY(!layout.line_at(at(0, 50, 40)));
        layout.select_between(at(0, 22, 10), at(0, 58, 10));
        QCOMPARE(layout.selected_text(), QString("cdef"));
        // Five characters of 20 px from the top: the drag from y = 25 to 75 passes three centres.
        layout.select_between(QPointF(310, 25), QPointF(310, 75));
        QCOMPARE(layout.selected_text(), QString::fromUtf8("二三四"));
    }

    void textJoinsByLineInReadingOrder() {
        OcrTextLayout stacked(ocr_result({{box(0, 0, 50, 20), "hello", 1}, {box(0, 60, 50, 20), "world", 1}}));
        stacked.select_between(QPointF(1, 10), QPointF(49, 70));
        QCOMPARE(stacked.selected_text(), QString("hello\nworld"));
        stacked.select_all();
        QCOMPARE(stacked.selected_text(), QString("hello\nworld"));
        // All text keeps reading order: same-line blocks with spaces.
        const OcrTextLayout blocks(ocr_result({{box(0, 0, 30, 20), "one", 1}, {box(40, 2, 30, 20), "two", 1}, {box(0, 60, 50, 20), "three", 1}}));
        QCOMPARE(blocks.all_text("\n"), QString("one two\nthree"));
        QCOMPARE(blocks.all_text(" | "), QString("one two | three"));
        QCOMPARE(OcrTextLayout(ocr_result({{box(0, 0, 30, 20), "single", 1}})).all_text(), QString("single"));
        QCOMPARE(OcrTextLayout().all_text(), QString());
        // Recognized on a pin turned a quarter: the lines stand side by side in the image and
        // still join by how they were read.
        const QTransform quarter(0, 1, -1, 0, 80, 0);
        OcrTextLayout turned(ocr_result({{box(0, 0, 30, 20), "one", 1}, {box(40, 2, 30, 20), "two", 1}, {box(0, 60, 50, 20), "three", 1}}), quarter);
        QCOMPARE(turned.all_text("\n"), QString("one two\nthree"));
        turned.select_all();
        QCOMPARE(turned.selected_text(), QString("one two\nthree"));
    }

    void wordsSplitAsTextEditorsPickThem() {
        const auto words = [](const QString &text) {
            const auto bounds = word_boundaries(text);
            QStringList pieces;
            for (int i = 0; i + 1 < bounds.size(); ++i) pieces.append(text.mid(bounds[i], bounds[i + 1] - bounds[i]));
            return pieces;
        };
        // With the system's ICU, Chinese by its words, not character by character.
        QCOMPARE(words(QString::fromUtf8("今天天气很好，我们去公园散步。")),
                 QString::fromUtf8("今天|天气|很好|，|我们|去|公园|散步|。").split('|'));
        QCOMPARE(words("Hello world, it's fine"), QStringList({"Hello", " ", "world", ",", " ", "it's", " ", "fine"}));
        QCOMPARE(words(QString::fromUtf8("用Qt的QTextEdit测试abc中文")), QString::fromUtf8("用|Qt|的|QTextEdit|测试|abc|中文").split('|'));
        QCOMPARE(word_boundaries(QString()), QList<int>({0}));
    }

    void withoutTheSystemsIcuWordsSplitAtSpacesAndPunctuation() {
        const auto words = [](const QString &text) {
            const auto bounds = punctuation_word_boundaries(text);
            QStringList pieces;
            for (int i = 0; i + 1 < bounds.size(); ++i) pieces.append(text.mid(bounds[i], bounds[i + 1] - bounds[i]));
            return pieces;
        };
        // Chinese runs up to its punctuation; English as with ICU.
        QCOMPARE(words(QString::fromUtf8("今天天气很好，我们去公园散步。")), QString::fromUtf8("今天天气很好|，|我们去公园散步|。").split('|'));
        QCOMPARE(words("Hello world, it's fine-tuned 3.14"), QStringList({"Hello", " ", "world", ",", " ", "it's", " ", "fine", "-", "tuned", " ", "3.14"}));
        // Mixed scripts are not cut where they change.
        QCOMPARE(words(QString::fromUtf8("用Qt写的QTextEdit测试，abc中文")), QString::fromUtf8("用Qt写的QTextEdit测试|，|abc中文").split('|'));
        // Full-width punctuation parts them as half-width does.
        QCOMPARE(words(QString::fromUtf8("《书名》（注释）【标签】…“引号”！")), QString::fromUtf8("《|书名|》|（|注释|）|【|标签|】|…|“|引号|”|！").split('|'));
        QCOMPARE(punctuation_word_boundaries(QString()), QList<int>({0}));
    }

    void aDoubleClickSelectsAWordAndATripleClickItsLine() {
        auto *pin = make_pin(QSize(400, 140));
        const QString chinese = QString::fromUtf8("今天天气很好，我们去公园散步。"), mixed = QString::fromUtf8("用Qt的QTextEdit测试abc中文");
        auto *selection = give_text(pin, {{box(10, 10, 300, 20), chinese, 0.9}, {box(10, 50, 300, 20), "Hello world, it's fine", 0.9}, {box(10, 90, 300, 20), mixed, 0.9}});
        const auto &lines = selection->layout().lines();
        const auto double_click_on = [&](int line, int index) {
            double_click(pin, char_point(pin, lines[line], index), Qt::LeftButton);
            return selection->selected_text();
        };
        QCOMPARE(double_click_on(0, chinese.indexOf(QString::fromUtf8("园"))), QString::fromUtf8("公园"));
        QVERIFY(selection->copy_selected_text());
        QCOMPARE(clipboard_text(), QString::fromUtf8("公园"));
        QCOMPARE(double_click_on(0, 0), QString::fromUtf8("今天"));
        QCOMPARE(double_click_on(1, 7), QString("world"));
        QCOMPARE(double_click_on(1, 13), QString("it's"));
        QCOMPARE(double_click_on(2, mixed.indexOf("Edit")), QString("QTextEdit"));
        QCOMPARE(double_click_on(2, mixed.indexOf(QString::fromUtf8("试"))), QString::fromUtf8("测试"));
        // Recognized text has no paragraphs: a third click takes the line.
        triple_click(pin, char_point(pin, lines[2], 1));
        QCOMPARE(selection->selected_text(), mixed);
        QVERIFY(selection->copy_selected_text());
        QCOMPARE(clipboard_text(), mixed);
        // Only a press soon after the double click is a third one.
        double_click(pin, char_point(pin, lines[1], 1), Qt::LeftButton);
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        click(pin, char_point(pin, lines[1], 1), Qt::LeftButton);
        QVERIFY(!selection->has_selection());
    }

    // ---- Recognized text on a pin ----

    void aLoadedResultReplacesTheText() {
        auto *pin = make_pin();
        QVERIFY(!pin->text_selection());
        auto *selection = give_text(pin, {{box(10, 10, 80, 20), "loaded", 0.9}, {box(10, 40, 80, 20), "second", 0.9}});
        QVERIFY(selection && selection->has_text());
        QVERIFY(pin->ocr_has_result());
        QCOMPARE(selection->layout().all_text("\n"), QString("loaded\nsecond"));
        give_text(pin, {});
        QVERIFY(!selection->has_text());
    }

    // ---- Text mode ----

    void textModeKeepsEditsAndUndoAcrossSwitches() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "first", 0.9}, {box(10, 70, 80, 20), "second", 0.9}});
        pin->toggle_text_mode();
        QVERIFY(pin->text_mode());
        auto *editor = pin->text_mode_controller()->editor();
        QVERIFY(editor && editor->isVisible());
        QCOMPARE(editor->toPlainText(), QString("first\nsecond"));
        editor->moveCursor(QTextCursor::End);
        editor->insertPlainText("!");
        pin->toggle_text_mode();
        QVERIFY(!pin->text_mode());
        QVERIFY(!editor->isVisible());
        pin->toggle_text_mode();
        QCOMPARE(pin->text_mode_controller()->editor(), editor);
        QCOMPARE(editor->toPlainText(), QString("first\nsecond!"));
        editor->undo();
        QCOMPARE(editor->toPlainText(), QString("first\nsecond"));
        // Copy takes the selection, or everything without one.
        QCOMPARE(pin->text_mode_controller()->text_to_copy(), QString("first\nsecond"));
        auto cursor = editor->textCursor();
        cursor.setPosition(0);
        cursor.setPosition(5, QTextCursor::KeepAnchor);
        editor->setTextCursor(cursor);
        QCOMPARE(pin->text_mode_controller()->text_to_copy(), QString("first"));
    }

    void textModeGivesWayToThumbnailAndClickThrough() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        pin->toggle_text_mode();
        pin->toggle_thumbnail_mode();
        QVERIFY(pin->thumbnail_mode());
        QVERIFY(!pin->text_mode());
        // Not entered from a thumbnail.
        pin->toggle_text_mode();
        QVERIFY(!pin->text_mode());
        pin->toggle_thumbnail_mode();
        pin->toggle_text_mode();
        QVERIFY(pin->text_mode());
        pin->set_click_through(true);
        QVERIFY(!pin->text_mode());
        pin->set_click_through(false);
    }

    void aPinTooShortForItsTextGrowsAndComesBack() {
        auto *pin = make_pin(QSize(300, 24), QPoint(100, 100));
        give_text(pin, {{box(4, 2, 200, 20), "one line", 0.9}});
        const auto before = pin->geometry();
        pin->toggle_text_mode();
        QVERIFY(pin->height() > before.height());
        QCOMPARE(pin->width(), before.width());
        QCOMPARE(pin->pos(), before.topLeft());
        // Moved meanwhile, it comes back to its own size where it was moved to.
        pin->move(pin->pos() + QPoint(30, 40));
        pin->toggle_text_mode();
        QCOMPARE(pin->geometry(), QRect(before.topLeft() + QPoint(30, 40), before.size()));
    }

    void textModeReadsUntilEnterAndEscStepsBackOneLayer() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        PinShortcutController::instance()->reload_bindings();
        QCursor::setPos(pin->mapToGlobal(QPoint(100, 60)));
        press_key(pin, Qt::Key_T);
        auto *mode = pin->text_mode_controller();
        auto *editor = mode->editor();
        QVERIFY(pin->text_mode() && !mode->editing());
        QVERIFY(editor->isReadOnly());
        QCOMPARE(editor->focusPolicy(), Qt::NoFocus);
        QCOMPARE(hint_text(pin), QString());
        // Reading, the pin's own keys still work.
        press_key(pin, Qt::Key_L);
        QVERIFY(pin->locked());
        press_key(pin, Qt::Key_L);
        QVERIFY(!pin->locked());
        press_key(pin, Qt::Key_Return);
        QVERIFY(mode->editing());
        QVERIFY(!editor->isReadOnly());
        QTRY_COMPARE(QApplication::focusWidget(), editor);
        QCOMPARE(hint_text(pin), QString("Press Esc to stop editing"));
        // Editing, plain keys are typed at the end instead.
        QTest::keyClick(editor, Qt::Key_L);
        QTest::keyClick(editor, Qt::Key_T);
        QCOMPARE(editor->toPlainText(), QString("hellolt"));
        QVERIFY(!pin->locked() && pin->text_mode());
        // Each Esc steps back one layer: editing, text, then the pin itself.
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(pin->text_mode() && !mode->editing());
        QVERIFY(editor->isReadOnly());
        QVERIFY(QApplication::focusWidget() != editor);
        press_key(pin, Qt::Key_Escape);
        QVERIFY(!pin->text_mode());
        QVERIFY(!pin->is_closed());
        press_key(pin, Qt::Key_Escape);
        QVERIFY(pin->is_closed());
    }

    void awayFromEveryPinTheEditorStopsEditingOnEsc() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        pin->toggle_text_mode();
        pin->edit_text();
        auto *editor = pin->text_mode_controller()->editor();
        QTRY_COMPARE(QApplication::focusWidget(), editor);
        QCursor::setPos(0, 0);
        QTest::keyClick(editor, Qt::Key_Escape);
        QVERIFY(pin->text_mode() && !pin->text_mode_controller()->editing());
    }

    void editingStopsWhenAnotherWindowTakesTheKeyboard() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        pin->toggle_text_mode();
        auto *mode = pin->text_mode_controller();
        pin->edit_text();
        // The pin's own menu keeps it editing.
        QFocusEvent popup(QEvent::FocusOut, Qt::PopupFocusReason);
        QApplication::sendEvent(mode->editor(), &popup);
        QVERIFY(mode->editing());
        QFocusEvent away(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
        QApplication::sendEvent(mode->editor(), &away);
        QVERIFY(!mode->editing());
        QVERIFY(pin->text_mode());
    }

    void textLeftWhileEditingTakesItsKeysAgainOnReturn() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        PinShortcutController::instance()->reload_bindings();
        auto *mode = pin->text_mode_controller();
        // Keys go where the system sends them: the focus widget of the active window.
        const auto type = [](Qt::Key key) {
            auto *focus = QApplication::focusWidget();
            QVERIFY(focus);
            QTest::keyClick(focus, key);
        };
        pin->toggle_text_mode();
        QVERIFY(QTest::qWaitForWindowActive(pin));
        type(Qt::Key_Return);
        QVERIFY(mode->editing());
        QWidget elsewhere;
        elsewhere.show();
        elsewhere.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&elsewhere));
        QVERIFY(!mode->editing());
        pin->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(pin));
        QVERIFY(QApplication::focusWidget() != mode->editor());
        type(Qt::Key_Return);
        QVERIFY(mode->editing());
        type(Qt::Key_Escape);
        QVERIFY(!mode->editing());
        type(Qt::Key_Escape);
        QVERIFY(!pin->text_mode());
        type(Qt::Key_Escape);
        QVERIFY(pin->is_closed());
    }

    void readingSelectsByDoubleClickAndCopiesTheTextsOwnSelection() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "first", 0.9}, {box(10, 70, 80, 20), "second", 0.9}});
        // A selection on the image's text layer, which text mode hides.
        QKeyEvent select_all(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
        QVERIFY(pin->text_selection()->key_press(&select_all));
        PinShortcutController::instance()->reload_bindings();
        QCursor::setPos(pin->mapToGlobal(QPoint(100, 60)));
        pin->toggle_text_mode();
        auto *editor = pin->text_mode_controller()->editor();
        auto cursor = editor->textCursor();
        cursor.setPosition(QString("first\nsec").size());
        const QPoint inside_second = editor->cursorRect(cursor).center();
        QTest::mouseDClick(editor->viewport(), Qt::LeftButton, {}, inside_second);
        QCOMPARE(editor->textCursor().selectedText(), QString("second"));
        QVERIFY(!pin->text_mode_controller()->editing());
        press_key(pin, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard_text(), QString("second"));
        // Enter keeps the selection, so typing replaces it.
        press_key(pin, Qt::Key_Return);
        QTest::keyClicks(editor, "2");
        QCOMPARE(editor->toPlainText(), QString("first\n2"));
    }

    void editingKeepsTheSizeAndLongTextScrolls() {
        auto *pin = make_pin(QSize(300, 60), QPoint(100, 100));
        give_text(pin, {{box(4, 2, 200, 20), "one line", 0.9}});
        pin->toggle_text_mode();
        const auto reading = pin->geometry();
        auto *editor = pin->text_mode_controller()->editor();
        pin->edit_text();
        for (int i = 0; i < 8; ++i) editor->insertPlainText("\nmore");
        QCOMPARE(pin->geometry(), reading);
        QTRY_VERIFY(editor->verticalScrollBar()->isVisible());
        pin->toggle_text_mode();
        QCOMPARE(pin->geometry(), QRect(100, 100, 300, 60));
    }

    void textModeEdgesResizeWidthAndHeightApart() {
        auto *pin = make_pin(QSize(200, 120), QPoint(100, 100));
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        pin->toggle_text_mode();
        auto *panel = pin->text_mode_controller()->panel();
        send_mouse_to(panel, QEvent::MouseMove, QPoint(199, 60), Qt::NoButton, Qt::NoButton);
        QCOMPARE(panel->cursor().shape(), Qt::SizeHorCursor);
        // Unlike an image, the right edge widens the pin alone.
        drag_on(panel, QPoint(199, 60), QPoint(259, 60));
        QCOMPARE(pin->geometry(), QRect(100, 100, 260, 120));
        // The bottom-left corner moves the left edge and the bottom; the right edge and the top stay.
        drag_on(panel, QPoint(0, 119), QPoint(-30, 159));
        QCOMPARE(pin->geometry(), QRect(70, 100, 290, 160));
        // Inside the margin, past the edge band, the page moves the pin again.
        send_mouse_to(panel, QEvent::MouseMove, QPoint(100, 7), Qt::NoButton, Qt::NoButton);
        QCOMPARE(panel->cursor().shape(), Qt::SizeAllCursor);
        // It stops at the smallest pin size.
        drag_on(panel, QPoint(289, 60), QPoint(-1000, 60));
        QCOMPARE(pin->geometry(), QRect(70, 100, MIN_SIDE, 160));
        // The image comes back at its own size.
        pin->toggle_text_mode();
        QCOMPARE(pin->size(), QSize(200, 120));
        // Locked, the edges do nothing.
        pin->toggle_text_mode();
        const auto size = pin->size();
        pin->set_locked(true);
        drag_on(panel, QPoint(size.width() - 1, 60), QPoint(size.width() + 60, 60));
        QCOMPARE(pin->size(), size);
    }

    void readingDragsThePinOffTheTextAndSelectsOnIt() {
        // Glyphs need a font to have any width.
        arial_family();
        auto *pin = make_pin(QSize(200, 120), QPoint(100, 100));
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        pin->toggle_text_mode();
        auto *editor = pin->text_mode_controller()->editor();
        auto *viewport = editor->viewport();
        auto cursor = editor->textCursor();
        cursor.setPosition(1);
        const QPoint on_text = editor->cursorRect(cursor).center();
        const QPoint blank(viewport->width() / 2, viewport->height() - 4);
        send_mouse_to(viewport, QEvent::MouseMove, blank, Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::SizeAllCursor);
        send_mouse_to(viewport, QEvent::MouseMove, on_text, Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::IBeamCursor);
        drag_on(viewport, blank, blank + QPoint(40, -20));
        QCOMPARE(pin->pos(), QPoint(140, 80));
        QVERIFY(!editor->textCursor().hasSelection());
        // On the text it selects and the pin stays.
        drag_on(viewport, on_text, on_text + QPoint(20, 0));
        QCOMPARE(pin->pos(), QPoint(140, 80));
        QVERIFY(editor->textCursor().hasSelection());
        // Editing, the blank places the caret instead.
        pin->edit_text();
        drag_on(viewport, blank, blank + QPoint(40, -20));
        QCOMPARE(pin->pos(), QPoint(140, 80));
    }

    void editTextIsOfferedInTheTextModeMenu() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        pin->toggle_text_mode();
        pin_menu(pin, "Edit text");
        QTRY_VERIFY(pin->text_mode_controller()->editing());
        // Still editing once the menu's activation changes have settled.
        QTest::qWait(50);
        QVERIFY(pin->text_mode_controller()->editing());
        // A cleared binding shows no key and no hint.
        config_->set_inapp_shortcut("inapp_pin_text_edit", "");
        auto restore = qScopeGuard([this] { config_->set_inapp_shortcut("inapp_pin_text_edit", "enter"); });
        pin->toggle_text_mode();
        hide_hint(pin);
        pin->toggle_text_mode();
        QCOMPARE(hint_text(pin), QString());
        QVERIFY(pin_menu(pin).contains("Edit text"));
    }

    // ---- Text pins ----

    void aTextPinPageIsAsWideAsItsLongestLineUpTo800() {
        // Glyphs need a font to have any width.
        arial_family();
        const QSize one = PinTextContent("short", {}, true).size();
        const QSize two = PinTextContent("short\nshort", {}, true).size();
        QCOMPARE(two.width(), one.width());
        QVERIFY(two.height() > one.height());
        // A longer line wraps at 800 pixels of text, plus 6 on each side.
        const QSize wrapped = PinTextContent(QString("word ").repeated(400), {}, true).size();
        QCOMPARE(wrapped.width(), 812);
        QVERIFY(wrapped.height() > 2 * one.height());
        // The pin is its page, shrunk to the screen as an image would be.
        auto *pin = make_text_pin("short");
        QCOMPARE(pin->orig_size(), one);
        QCOMPARE(pin->size(), one);
        auto *tall = make_text_pin(QString("line\n").repeated(500));
        QVERIFY(tall->scale_factor < 1.0);
        QVERIFY(tall->screen()->geometry().contains(tall->geometry()));
    }

    void aTextPinIsAPinOfItsText() {
        arial_family();
        auto *pin = make_text_pin("first line\nsecond");
        QVERIFY(pin->text_only() && !pin->text_mode());
        PinShortcutController::instance()->reload_bindings();
        QCursor::setPos(pin->mapToGlobal(pin->rect().center()));
        // Nothing to recognize: the text is selectable from the start.
        QVERIFY(pin->text_selection_enabled() && pin->ocr_has_result());
        QCOMPARE(pin->text_selection()->layout().all_text(), QString("first line\nsecond"));
        // It is text already; R folds it to a thumbnail as any pin.
        press_key(pin, Qt::Key_T);
        QVERIFY(!pin->text_mode());
        press_key(pin, Qt::Key_R);
        QVERIFY(pin->thumbnail_mode());
        press_key(pin, Qt::Key_R);
        QVERIFY(!pin->thumbnail_mode());
        const QStringList want = {"Copy text (CTRL+C)", "Copy plain text", "Copy image", "Save as", "Translate", "Reset size", "-", "[ ] Toolbar (SPACE)",
                                  "[ ] Hide annotations (H)", "[x] Always on top", "[ ] Mouse click-through", "[ ] Lock (L)", "[x] Border",
                                  "[x] Text selection (P)", "[ ] Thumbnail mode (R)", "-", "Close (ESC)"};
        QCOMPARE(pin_menu(pin), want);
        // Copied as text, plain text or the page as shown.
        pin->copy_to_clipboard();
        QCOMPARE(clipboard_text(), QString("first line\nsecond"));
        QApplication::clipboard()->clear();
        pin->copy_plain_text();
        QCOMPARE(clipboard_text(), QString("first line\nsecond"));
        // The image goes to the system clipboard, which a test leaves alone; this is what it gets.
        QCOMPARE(pin->get_current_image().size(), pin->orig_size() * pin->devicePixelRatioF());
        // Esc closes it.
        press_key(pin, Qt::Key_Escape);
        QVERIFY(pin->is_closed());
    }

    void aTextPinIsDrawnAfreshAtEachZoom() {
        arial_family();
        auto *pin = make_text_pin("sharp at any size");
        auto *background = pin->canvas->scene->background;
        const auto shown = [pin] { return pin->canvas->scene->background->pixmap(); };
        // Drawn for the screen at 100%, not the page's own pixels, on a worker thread.
        QTRY_COMPARE(shown().deviceIndependentSize(), QSizeF(pin->size()));
        QVERIFY(same_pixels(shown().toImage(), pin->text_content()->render(pin->size(), pin->devicePixelRatioF())));
        wheel(pin, 120, Qt::NoModifier, false, 4);
        QVERIFY(pin->scale_factor > 1.0);
        QTRY_COMPARE(shown().deviceIndependentSize(), QSizeF(pin->size()));
        QVERIFY(same_pixels(shown().toImage(), pin->text_content()->render(pin->size(), pin->devicePixelRatioF())));
        // The scene keeps the page's own size, so annotations stay where they were drawn.
        QCOMPARE(pin->canvas->scene->sceneRect().size(), QSizeF(pin->orig_size()));
        QCOMPARE(background->image().size(), pin->orig_size());
        // The right edge scales the whole pin, as with an image.
        const QSize before = pin->size();
        drag(pin, QPoint(before.width() - 1, before.height() / 2), QPoint(before.width() + 99, before.height() / 2));
        QCOMPARE(pin->width(), before.width() + 100);
        QVERIFY(std::abs(pin->height() - pin->orig_size().height() * (before.width() + 100.0) / pin->orig_size().width()) <= 1);
    }

    void aTextPinSelectsItsCharactersWhereTheyAreDrawn() {
        arial_family();
        auto *pin = make_text_pin("hello world");
        const auto &layout = pin->text_selection()->layout();
        QCOMPARE(layout.lines().size(), size_t(1));
        // Each character starts after the advance of those before it, inside the page and document margins.
        const auto &line = layout.lines()[0];
        const QFontMetricsF metrics(pin->text_content()->document()->defaultFont());
        const QString text = "hello world";
        for (int index = 0; index < text.size(); ++index) {
            const double expected = 6 + 4 + metrics.horizontalAdvance(text.left(index));
            QVERIFY2(std::abs(line.bounds.left() + line.starts[index] - expected) < 0.5, qPrintable(QString::number(index)));
        }
        // Dragging over "world" copies it.
        const double world_start = line.bounds.left() + line.starts[6], world_end = line.bounds.left() + line.ends[10];
        const double y = line.bounds.center().y();
        drag(pin, pin->view->mapFromScene(QPointF(world_start + 1, y)), pin->view->mapFromScene(QPointF(world_end + 2, y)));
        QCOMPARE(pin->text_selection()->selected_text(), QString("world"));
        pin->copy_to_clipboard();
        QCOMPARE(clipboard_text(), QString("world"));
    }

    void aTextPinDragsFromEverythingButItsText() {
        arial_family();
        auto *pin = make_text_pin("one   line");
        auto *viewport = pin->view->viewport();
        const auto &line = pin->text_selection()->layout().lines()[0];
        const auto at = [pin, &line](double along, double across) {
            return pin->view->mapFromScene(QPointF(line.bounds.left() + along, line.bounds.top() + across));
        };
        const double y = line.bounds.height() / 2;
        // The text sits 10 pixels in, as wide as its characters.
        QCOMPARE(line.bounds.topLeft(), QPointF(10, 10));
        // Right outside its box the pin drags, so a one-line pin keeps a frame to drag by.
        for (const QPoint blank : {at(-2, y), at(line.ends.back() + 2, y), at(line.ends[1], -2), at(line.ends[1], line.bounds.height() + 2)}) {
            send_mouse(pin, QEvent::MouseMove, blank, Qt::NoButton, Qt::NoButton);
            QVERIFY2(viewport->cursor().shape() != Qt::IBeamCursor, qPrintable(QString("%1,%2").arg(blank.x()).arg(blank.y())));
            const QPoint before = pin->pos();
            drag(pin, blank, blank + QPoint(30, 20));
            QCOMPARE(pin->pos(), before + QPoint(30, 20));
            QVERIFY(!pin->text_selection()->has_selection());
        }
        // So does the middle of a run of spaces; a space next to a character is still text.
        const double gap = (line.starts[4] + line.ends[4]) / 2;
        const QPoint before = pin->pos();
        drag(pin, at(gap, y), at(gap, y) + QPoint(30, 20));
        QCOMPARE(pin->pos(), before + QPoint(30, 20));
        send_mouse(pin, QEvent::MouseMove, at((line.starts[3] + line.ends[3]) / 2, y), Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::IBeamCursor);
        // Inside the box the edge characters still select.
        drag(pin, at(1, y), at(line.ends[2], y));
        QCOMPARE(pin->text_selection()->selected_text(), QString("one"));
        QCOMPARE(pin->pos(), before + QPoint(30, 20));
    }

    void aTextPinSelectsAWordOrAParagraph() {
        arial_family();
        const QString paragraph = QString("Hello world, ") + QString("word ").repeated(300).trimmed();
        auto *pin = make_text_pin(paragraph + "\nsecond paragraph here");
        auto *selection = pin->text_selection();
        const auto &lines = selection->layout().lines();
        QVERIFY(lines.size() > 3);
        double_click(pin, char_point(pin, lines[0], 8), Qt::LeftButton);
        QCOMPARE(selection->selected_text(), QString("world"));
        pin->copy_to_clipboard();
        QCOMPARE(clipboard_text(), QString("world"));
        // A third click takes the paragraph across the lines it wraps into, and no more.
        triple_click(pin, char_point(pin, lines[1], 2));
        QCOMPARE(selection->selected_text(), paragraph);
        pin->copy_to_clipboard();
        QCOMPARE(clipboard_text(), paragraph);
        const auto &last = lines.back();
        triple_click(pin, char_point(pin, last, 3));
        QCOMPARE(selection->selected_text(), QString("second paragraph here"));
        // Chinese, and Chinese mixed with Latin, by their words.
        if (yahei_family().isEmpty()) QSKIP("No Chinese font on this system");
        const QString chinese = QString::fromUtf8("今天天气很好，我们去公园散步。用Qt的QTextEdit测试abc中文");
        auto *cjk = make_text_pin(chinese + QString::fromUtf8("\n第二段"));
        auto *cjk_selection = cjk->text_selection();
        const auto &cjk_line = cjk_selection->layout().lines()[0];
        double_click(cjk, char_point(cjk, cjk_line, chinese.indexOf(QString::fromUtf8("公"))), Qt::LeftButton);
        QCOMPARE(cjk_selection->selected_text(), QString::fromUtf8("公园"));
        double_click(cjk, char_point(cjk, cjk_line, chinese.indexOf("QText")), Qt::LeftButton);
        QCOMPARE(cjk_selection->selected_text(), QString("QTextEdit"));
        cjk->copy_to_clipboard();
        QCOMPARE(clipboard_text(), QString("QTextEdit"));
        triple_click(cjk, char_point(cjk, cjk_line, 2));
        QCOMPARE(cjk_selection->selected_text(), chinese);
    }

    void aWrappedParagraphCopiesAsTheTextRuns() {
        arial_family();
        const QString paragraph = QString("word ").repeated(300).trimmed();
        auto *pin = make_text_pin(paragraph + "\n\nnext");
        auto *selection = pin->text_selection();
        const auto &lines = selection->layout().lines();
        QVERIFY(lines.size() > 3);
        // Lines broken by wrapping join with nothing between them; a blank line stays.
        QCOMPARE(selection->layout().all_text(), paragraph + "\n\nnext");
        const auto first = lines[0].bounds, second = lines[1].bounds;
        drag(pin, pin->view->mapFromScene(QPointF(first.left() + 1, first.center().y())), pin->view->mapFromScene(second.center()));
        const QString selected = selection->selected_text();
        QVERIFY(selected.size() > lines[0].text.size());
        QVERIFY(!selected.contains('\n'));
        QVERIFY(paragraph.startsWith(selected));
    }

    void aRichTextPinKeepsItsFormattingReadableOnTheWhitePage() {
        arial_family();
        const QString html =
            "<body style=\"background-color:#0d1117\">"
            "<p>plain <b>bold</b> <code>code()</code></p>"
            "<ul><li>item</li></ul>"
            "<p><span style=\"color:#e6edf3;background-color:#161b22\">dark</span> <span style=\"color:#c00000\">red</span> "
            "<span style=\"background-color:#fff59d\">marked</span> <span style=\"color:#fff59d;background-color:#fff59d\">hidden</span></p>"
            "<p>before<img src=\"https://example.com/a.png\" width=\"40\" height=\"40\">after</p>"
            "<table><tr><td style=\"background-color:#000000;color:#ffffff\">cell</td><td>next</td></tr></table></body>";
        const QString text = "plain bold code()\nitem\ndark red marked hidden\nbeforeafter\ncell\tnext";
        auto *pin = make_text_pin(text, html);
        auto *content = pin->text_content();
        QVERIFY(content->formatted());
        const auto *document = content->document();
        // Structure and styles stay.
        QCOMPARE(format_of(document, "bold").fontWeight(), int(QFont::Bold));
        QVERIFY(format_of(document, "code()").font().families().contains("Consolas"));
        QVERIFY(document->find("item").block().textList());
        QVERIFY(document->find("cell").currentTable());
        // Colors that read on white stay.
        QCOMPARE(format_of(document, "red").foreground().color(), QColor("#c00000"));
        QCOMPARE(format_of(document, "marked").background().color(), QColor("#fff59d"));
        // A dark page's colors go, leaving the page's own text and background.
        for (const char *part : {"dark", "cell"}) {
            QVERIFY2(!format_of(document, part).hasProperty(QTextFormat::ForegroundBrush), part);
            QVERIFY2(!format_of(document, part).hasProperty(QTextFormat::BackgroundBrush), part);
        }
        QVERIFY(!document->rootFrame()->frameFormat().hasProperty(QTextFormat::BackgroundBrush));
        const auto cell = document->find("cell");
        QVERIFY(!cell.currentTable()->cellAt(cell).format().hasProperty(QTextFormat::BackgroundBrush));
        // Text the color of its kept background turns dark on it.
        QVERIFY(!format_of(document, "hidden").hasProperty(QTextFormat::ForegroundBrush));
        QVERIFY(format_of(document, "hidden").hasProperty(QTextFormat::BackgroundBrush));
        // The picture, which the pin cannot load, is gone.
        QVERIFY(document->toPlainText().contains("beforeafter"));
        QVERIFY(!document->toPlainText().contains(QChar::ObjectReplacementCharacter));
        // Cells of a row join with a tab.
        QVERIFY(pin->text_selection()->layout().all_text().endsWith("cell\tnext"));
        // Copied, it carries the formatting along with the text it came with.
        pin->copy_to_clipboard();
        const auto *mime = QApplication::clipboard()->mimeData();
        QVERIFY(mime->hasHtml());
        QCOMPARE(mime->text(), text);
        QVERIFY(mime->html().contains("<b>bold</b>") || mime->html().contains("font-weight:700;\">bold"));
        pin->copy_plain_text();
        QVERIFY(!QApplication::clipboard()->mimeData()->hasHtml());
        QCOMPARE(clipboard_text(), text);

        // Formatting that leaves no text of its own shows the plain text, copied plain.
        auto *picture = make_text_pin("alt text", "<img src=\"a.png\">");
        QVERIFY(!picture->text_content()->formatted() && !picture->text_content()->has_formatting());
        QCOMPARE(picture->text_content()->document()->toPlainText(), QString("alt text"));
        picture->copy_to_clipboard();
        QVERIFY(!QApplication::clipboard()->mimeData()->hasHtml());
        QCOMPARE(clipboard_text(), QString("alt text"));
        QVERIFY(!pin_menu(picture).join('\n').contains("Keep text formatting"));
    }

    void aRichTextPinIsSizedByItsFormattedText() {
        arial_family();
        // Lines that refused to wrap where they came from wrap at the width limit.
        const QString code = QString("word ").repeated(400);
        const QSize pre = PinTextContent(code, "<pre>" + code + "</pre>", true).size();
        QCOMPARE(pre.width(), 812);
        QVERIFY(pre.height() > 3 * PinTextContent("word", {}, true).size().height());
        // A heading takes more room than the same text plain.
        const QSize plain = PinTextContent("Title", {}, true).size();
        const QSize heading = PinTextContent("Title", "<h1>Title</h1>", true).size();
        QVERIFY(heading.width() > plain.width());
        QVERIFY(heading.height() > plain.height());
    }

    void aTextPinSwitchesItsFormattingFromTheMenu() {
        arial_family();
        const QString html = "<h1>Title</h1><p>body <b>bold</b></p>";
        const QString text = "Title\nbody bold";
        auto *pin = make_text_pin(text, html);
        QVERIFY(pin_menu(pin).contains("[x] Keep text formatting"));
        // Zoomed to 150%, with an annotation at the top left.
        pin->reset_to_original_size();
        wheel(pin, 120, Qt::NoModifier, false, 5);
        const double scale = pin->scale_factor;
        QVERIFY(scale > 1.0);
        auto *mark = pin->canvas->scene->addRect(QRectF(2, 2, 10, 10));
        mark->setZValue(10);
        const QPoint corner = pin->pos();
        // Off: the same text plain, laid out as a new pin of it would be, at the same zoom and place,
        // once the worker has laid it out.
        pin_menu(pin, "Keep text formatting");
        QVERIFY(!pin->text_formatted());
        QTRY_VERIFY(!pin->text_content()->formatted());
        auto *content = pin->text_content();
        QVERIFY(content->has_formatting());
        QCOMPARE(format_of(content->document(), "bold").fontWeight(), int(QFont::Normal));
        QCOMPARE(pin->orig_size(), worker_page_size(text, {}, true));
        QCOMPARE(pin->canvas->scene->sceneRect().size(), QSizeF(pin->orig_size()));
        QCOMPARE(pin->scale_factor, scale);
        QVERIFY(std::abs(pin->width() - pin->orig_size().width() * scale) <= 1);
        QVERIFY(std::abs(pin->height() - pin->orig_size().height() * scale) <= 1);
        QCOMPARE(pin->pos(), corner);
        QCOMPARE(mark->rect(), QRectF(2, 2, 10, 10));
        QTRY_VERIFY(same_pixels(pin->canvas->scene->background->pixmap().toImage(), content->render(pin->size(), pin->devicePixelRatioF())));
        // Selecting follows the new layout.
        QCOMPARE(pin->text_selection()->layout().all_text(), text);
        pin->copy_to_clipboard();
        QVERIFY(!QApplication::clipboard()->mimeData()->hasHtml());
        QVERIFY(pin_menu(pin).contains("[ ] Keep text formatting"));
        // On again: the formatting and its size are back.
        pin_menu(pin, "Keep text formatting");
        QTRY_VERIFY(pin->text_content()->formatted());
        QCOMPARE(format_of(pin->text_content()->document(), "bold").fontWeight(), int(QFont::Bold));
        QCOMPARE(pin->orig_size(), worker_page_size(text, html, true));
    }

    void formattingSwitchedAgainWhileLayingOutEndsAsLastAsked() {
        arial_family();
        const QString html = "<h1>Title</h1><p>body</p>";
        auto *pin = make_text_pin("Title\nbody", html);
        const auto *first = pin->text_content();
        pin->toggle_text_formatting();
        pin->toggle_text_formatting();
        QVERIFY(pin->text_formatted());
        // The plain layout comes back unwanted, and the formatted one is laid out again.
        QTRY_VERIFY(pin->text_content() != first);
        QVERIFY(pin->text_content()->formatted());
        QCOMPARE(pin->orig_size(), worker_page_size("Title\nbody", html, true));
        // What is still on its way for a closed pin is dropped.
        pin->toggle_text_formatting();
        wheel(pin, 120);
        pin->refresh_background_for_scale();
        pin->close_window();
        QTest::qWait(300);
        flush_deletes();
    }

    void aTextPinPinnedPlainCanShowItsFormatting() {
        arial_family();
        const QString html = "<p><b>bold</b> plain</p>";
        auto *pin = make_text_pin("bold plain", html, false);
        auto *content = pin->text_content();
        QVERIFY(!content->formatted() && content->has_formatting());
        QCOMPARE(pin->orig_size(), PinTextContent("bold plain", {}, true).size());
        QVERIFY(pin_menu(pin).contains("[ ] Keep text formatting"));
        pin->toggle_text_formatting();
        QTRY_VERIFY(pin->text_content()->formatted());
        QCOMPARE(format_of(pin->text_content()->document(), "bold").fontWeight(), int(QFont::Bold));
    }

    void textModeAndTextSelectionKeys() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 80, 20), "hello", 0.9}});
        PinShortcutController::instance()->reload_bindings();
        QCursor::setPos(pin->mapToGlobal(QPoint(100, 60)));
        press_key(pin, Qt::Key_P);
        QVERIFY(!pin->text_selection_enabled());
        QCOMPARE(hint_text(pin), QString("Text selection off"));
        press_key(pin, Qt::Key_P);
        QVERIFY(pin->text_selection_enabled());
        // While reading T goes back to the image.
        press_key(pin, Qt::Key_T);
        QVERIFY(pin->text_mode());
        press_key(pin, Qt::Key_T);
        QVERIFY(!pin->text_mode());
        press_key(pin, Qt::Key_T);
        press_key(pin, Qt::Key_Return);
        auto *editor = pin->text_mode_controller()->editor();
        QTRY_COMPARE(QApplication::focusWidget(), editor);
        // While editing both keys are typed.
        QTest::keyClick(editor, Qt::Key_T);
        QTest::keyClick(editor, Qt::Key_P);
        QVERIFY(pin->text_mode());
        QCOMPARE(editor->toPlainText(), QString("hellotp"));
        QVERIFY(pin->text_selection_enabled());
        // Bound with Ctrl the key also leaves from inside the text.
        config_->set_inapp_shortcut("inapp_pin_text_mode", "ctrl+t");
        PinShortcutController::instance()->reload_bindings();
        QTest::keyClick(editor, Qt::Key_T, Qt::ControlModifier);
        QVERIFY(!pin->text_mode());
        // A thumbnail lets both keys go on.
        pin->toggle_thumbnail_mode();
        press_key(pin, Qt::Key_T, Qt::ControlModifier);
        press_key(pin, Qt::Key_P);
        QVERIFY(!pin->text_mode());
        QVERIFY(pin->text_selection_enabled());
        config_->set_inapp_shortcut("inapp_pin_text_mode", "t");
        PinShortcutController::instance()->reload_bindings();
    }

    void textModeRecognizesFirstWhenThePinHasNoText() {
        StandInEngine engine([](const QImage &) { return ocr_result({{box(10, 10, 100, 20), "hello pin", 0.9}}); });
        auto *pin = make_pin();
        pin->toggle_text_mode();
        QVERIFY(!pin->text_mode());
        QTRY_VERIFY_WITH_TIMEOUT(pin->text_mode(), 10000);
        QCOMPARE(pin->text_mode_controller()->text(), QString("hello pin"));
    }

    void theRecognizedImageMapsBackThroughRotationAndFlips() {
        // A white block at the top-left corner and a red one on the right, centred at (2, 2) and (70, 20).
        QImage image(80, 40, QImage::Format_RGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        painter.fillRect(0, 0, 4, 4, Qt::white);
        painter.fillRect(68, 18, 4, 4, Qt::red);
        painter.end();
        auto *pin = new PinWindow(image, QPoint(100, 100), config_);
        pins_.append(pin);
        // Pixel-centre average of the pixels matching a colour.
        const auto centre_of = [](const QImage &shown, const std::function<bool(QRgb)> &matches) {
            QPointF sum;
            int count = 0;
            for (int y = 0; y < shown.height(); ++y)
                for (int x = 0; x < shown.width(); ++x)
                    if (matches(shown.pixel(x, y))) {
                        sum += QPointF(x + 0.5, y + 0.5);
                        ++count;
                    }
            return count ? sum / count : QPointF(-1, -1);
        };
        const auto white = [](QRgb c) { return qRed(c) > 200 && qGreen(c) > 200 && qBlue(c) > 200; };
        const auto red = [](QRgb c) { return qRed(c) > 200 && qGreen(c) < 80 && qBlue(c) < 80; };
        for (int rotation = 0; rotation < 360; rotation += 90) {
            for (const bool flip : {false, true}) {
                pin->reset_image_transform();
                for (int turned = 0; turned < rotation; turned += 90) pin->rotate_image_cw();
                if (flip) pin->flip_image_horizontal();
                const auto shown = pin->get_current_image();
                const auto to_image = pin->from_current_image(shown.size());
                const auto corner = to_image.map(centre_of(shown, white)), right = to_image.map(centre_of(shown, red));
                const auto state = QString("rotation %1 flip %2: corner (%3, %4) right (%5, %6)").arg(rotation).arg(flip)
                    .arg(corner.x()).arg(corner.y()).arg(right.x()).arg(right.y());
                QVERIFY2(QLineF(corner, QPointF(2, 2)).length() < 1 && QLineF(right, QPointF(70, 20)).length() < 1, qPrintable(state));
            }
        }
    }

    void draggingOverTextSelectsAcrossLines() {
        auto *pin = make_pin(QSize(200, 120), QPoint(100, 100));
        auto *selection = give_text(pin, {{box(10, 10, 100, 20), "abcdefghij", 0.9}, {box(10, 50, 100, 20), "klmnopqrst", 0.9}});
        auto *viewport = pin->view->viewport();
        QCursor::setPos(0, 0);
        send_mouse(pin, QEvent::MouseMove, QPoint(30, 20), Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::IBeamCursor);
        send_mouse(pin, QEvent::MouseButtonPress, QPoint(30, 20), Qt::LeftButton, Qt::LeftButton);
        QVERIFY(!pin->view->window_dragging);
        send_mouse(pin, QEvent::MouseMove, QPoint(60, 60), Qt::NoButton, Qt::LeftButton);
        send_mouse(pin, QEvent::MouseButtonRelease, QPoint(60, 60), Qt::LeftButton, Qt::NoButton);
        QCOMPARE(selection->selected_text(), QString("cdefghij\nklmno"));
        QVERIFY(!pin->is_dragging());
        // Dragging backwards selects the same span.
        drag(pin, QPoint(60, 60), QPoint(30, 20));
        QCOMPARE(selection->selected_text(), QString("cdefghij\nklmno"));
        // Ctrl+A on the view selects everything; other keys pass.
        QTest::keyClick(pin->view, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(selection->selected_text(), QString("abcdefghij\nklmnopqrst"));
        QKeyEvent other(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b");
        QVERIFY(!selection->key_press(&other));
        // Off the text the arrow comes back, and a press there clears the selection and drags the window.
        send_mouse(pin, QEvent::MouseMove, QPoint(180, 100), Qt::NoButton, Qt::NoButton);
        QCOMPARE(viewport->cursor().shape(), Qt::ArrowCursor);
        send_mouse(pin, QEvent::MouseButtonPress, QPoint(180, 100), Qt::LeftButton, Qt::LeftButton);
        QVERIFY(!selection->has_selection());
        QVERIFY(pin->view->window_dragging);
        send_mouse(pin, QEvent::MouseButtonRelease, QPoint(180, 100), Qt::LeftButton, Qt::NoButton);
        QVERIFY(!pin->view->window_dragging);
        // Drawing turns selection off and clears it; leaving the tool turns it back on.
        QTest::keyClick(pin->view, Qt::Key_A, Qt::ControlModifier);
        pin->ocr_manager()->set_drawing_mode(true);
        QVERIFY(!selection->is_active() && !selection->has_selection());
        pin->ocr_manager()->set_drawing_mode(false);
        QVERIFY(selection->is_active());
    }

    void doubleClickSelectsAWordUnlessBoundToAnAction() {
        auto *pin = make_pin();
        auto *selection = give_text(pin, {{box(10, 10, 100, 20), "hello you", 0.9}, {box(10, 50, 100, 20), "other", 0.9}});
        QApplication::clipboard()->setText("before");
        double_click(pin, QPoint(40, 20), Qt::LeftButton);
        QCOMPARE(selection->selected_text(), QString("hello"));
        QCOMPARE(clipboard_text(), QString("before"));

        config_->set_app_setting("mouse_pin_close", "doubleleft");
        auto *bound = make_pin();
        give_text(bound, {{box(10, 10, 100, 20), "hello", 0.9}});
        double_click(bound, QPoint(40, 20), Qt::LeftButton);
        QVERIFY(bound->is_closed());
        QCOMPARE(clipboard_text(), QString("before"));
    }

    // Selection lives in image pixels: dragging over the text where it is shown selects the same
    // characters however the pin is zoomed, turned or flipped, and a selection survives those.
    void theSelectionFollowsZoomRotationAndFlips() {
        auto *pin = make_pin(QSize(200, 120), QPoint(100, 100));
        auto *selection = give_text(pin, {{box(10, 10, 100, 20), "abcdefghij", 0.9}});
        const auto drag_shown = [pin](const QPointF &from, const QPointF &to) { drag(pin, pin->view->mapFromScene(from), pin->view->mapFromScene(to)); };
        // Centres at 15, 25, ... 105: the drag passes those of c, d, e and f.
        const auto check = [&](const char *label) {
            drag_shown(QPointF(32, 20), QPointF(68, 20));
            QVERIFY2(selection->selected_text() == "cdef", qPrintable(QString("%1: %2").arg(label, selection->selected_text())));
        };
        check("plain");
        pin->rotate_image_cw();
        QCOMPARE(selection->selected_text(), QString("cdef"));
        check("turned");
        pin->flip_image_horizontal();
        check("turned and flipped");
        wheel(pin, 120, Qt::NoModifier, false, 3);
        QVERIFY(pin->scale_factor > 1.2);
        check("zoomed");
        pin->reset_image_transform();
        check("reset");
    }

    // The highlight is drawn where the text is shown, and never goes into the pin's image.
    void theHighlightIsShownButNotExported() {
        auto *pin = make_pin(QSize(200, 120), QPoint(100, 100));
        auto *selection = give_text(pin, {{box(10, 10, 100, 20), "abcdefghij", 0.9}});
        const auto shown = [pin](const QPointF &image_point) {
            const auto image = pin->view->viewport()->grab().toImage();
            return image.pixelColor(pin->view->mapFromScene(image_point) * image.devicePixelRatio());
        };
        const auto plain = shown(QPointF(35, 20));
        drag(pin, pin->view->mapFromScene(QPointF(22, 20)), pin->view->mapFromScene(QPointF(58, 20)));
        QVERIFY(selection->has_selection());
        QVERIFY(shown(QPointF(35, 20)) != plain);
        QCOMPARE(shown(QPointF(95, 20)), plain);
        const auto image = pin->get_current_image();
        QCOMPARE(image.pixelColor(QPointF(35, 20).toPoint() * image.devicePixelRatio()), solid(QSize(1, 1)).pixelColor(0, 0));
        // Turned, it is still over the same characters.
        pin->rotate_image_cw();
        QVERIFY(shown(QPointF(35, 20)) != plain);
        QCOMPARE(shown(QPointF(95, 20)), plain);
    }

    // ---- Recognition on demand ----

    void textSelectionStartsOffWithoutAutomaticRecognition() {
        auto *pin = make_pin();
        QVERIFY(!pin->text_selection_enabled());
        QVERIFY(!pin->text_selection());
        QTest::qWait(350);
        // The automatic run after pinning follows the setting.
        QVERIFY(!pin->ocr_manager()->is_running());
        QVERIFY(!pin->text_selection());
    }

    void copyingAllTextUsesAnExistingResultOrRecognizes() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        const QString arial = arial_family();
        if (arial.isEmpty()) QSKIP("Arial is not installed");
        QImage image(360, 90, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont(arial, 28));
        painter.drawText(image.rect(), Qt::AlignCenter, "Hello 2026");
        painter.end();
        auto *pin = new PinWindow(image, QPoint(100, 100), config_);
        pins_.append(pin);
        QApplication::clipboard()->clear();
        // Nothing recognized yet: copying recognizes first, then copies.
        pin->copy_all_text();
        QVERIFY(pin->ocr_manager()->is_running());
        QTRY_VERIFY_WITH_TIMEOUT(!pin->ocr_manager()->is_running(), 30000);
        QVERIFY2(clipboard_text().contains("Hello"), qPrintable(QString("clipboard=%1 hint=%2 result=%3 text=%4")
            .arg(clipboard_text(), hint_text(pin)).arg(pin->ocr_has_result()).arg(pin->text_selection() ? pin->text_selection()->layout().all_text() : "-")));
        QVERIFY(pin->ocr_has_result());
        QVERIFY(pin->text_selection_enabled());
        QCOMPARE(hint_text(pin), QString("Text copied"));
        // With a result it copies at once.
        QApplication::clipboard()->clear();
        pin->copy_all_text();
        QVERIFY(!pin->ocr_manager()->is_running());
        QVERIFY(clipboard_text().contains("2026"));
        // The engine's character positions put a drag over "2026" on exactly those characters.
        const QFontMetricsF metrics(QFont(arial, 28), &image);
        const double left = (image.width() - metrics.horizontalAdvance("Hello 2026")) / 2;
        const double start = left + metrics.horizontalAdvance("Hello "), end = left + metrics.horizontalAdvance("Hello 2026");
        auto *selection = pin->text_selection();
        QVERIFY(selection);
        drag(pin, pin->view->mapFromScene(QPointF(start + 2, 45)), pin->view->mapFromScene(QPointF(end - 2, 45)));
        QCOMPARE(selection->selected_text(), QString("2026"));
        // Turning text selection off stops it; on starts it again without recognizing.
        pin->toggle_text_selection();
        QVERIFY(!selection->is_active() && !selection->has_selection());
        pin->toggle_text_selection();
        QVERIFY(selection->is_active());
        QVERIFY(!pin->ocr_manager()->is_running());
    }

    void closingThePinDropsQueuedCallbacks() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        auto *pin = make_pin();
        bool called = false;
        QVERIFY(pin->ocr_manager()->recognize_then([&](bool, const QString &) { called = true; }));
        // A second request joins the running recognition.
        QVERIFY(pin->ocr_manager()->recognize_then([&](bool, const QString &) { called = true; }));
        QPointer<PinOcrThread> thread = pin->ocr_manager()->ocr_thread.data();
        pin->close_window();
        if (thread) QTRY_VERIFY_WITH_TIMEOUT(!thread || thread->isFinished(), 30000);
        QTest::qWait(50);
        QVERIFY(!called);
    }

    // Callers waiting on a recognition share one run and all hear its text; the automatic run after
    // that does not recognize again.
    void waitingCallbacksShareOneStandInRecognition() {
        std::atomic_int runs = 0;
        StandInEngine engine([&runs](const QImage &) {
            ++runs;
            return ocr_result({{box(10, 10, 100, 20), "hello pin", 0.9}});
        });
        auto *pin = make_pin();
        int answered = 0;
        bool all_succeeded = true;
        QStringList texts;
        const auto callback = [&](bool success, const QString &text) {
            ++answered;
            all_succeeded = all_succeeded && success;
            texts << text;
        };
        QVERIFY(pin->ocr_manager()->recognize_then(callback));
        QVERIFY(pin->ocr_manager()->recognize_then(callback));
        QTRY_COMPARE_WITH_TIMEOUT(answered, 2, 10000);
        QVERIFY(all_succeeded);
        QCOMPARE(texts, QStringList({"hello pin", "hello pin"}));
        QTRY_VERIFY(!pin->ocr_manager()->is_running());
        QCOMPARE(runs.load(), 1);
        QVERIFY(pin->ocr_has_result());
        QCOMPARE(pin->text_selection()->layout().all_text(), QString("hello pin"));
        QVERIFY(pin->ocr_manager()->init_now());
        QVERIFY(!pin->ocr_manager()->is_running());
        QCOMPARE(runs.load(), 1);
    }

    // A recognition that finds no text, or fails, tells the waiting callbacks so and leaves the pin without a result.
    void aStandInRecognitionWithoutTextOrWithAFailureAnswersNo() {
        for (const bool fail : {false, true}) {
            StandInEngine engine([fail](const QImage &) -> OcrResult {
                if (fail) throw std::runtime_error("engine lost");
                return ocr_result({});
            });
            auto *pin = make_pin();
            bool answered = false, succeeded = true;
            QString text;
            QVERIFY(pin->ocr_manager()->recognize_then([&](bool success, const QString &result) {
                answered = true;
                succeeded = success;
                text = result;
            }));
            QTRY_VERIFY_WITH_TIMEOUT(answered, 10000);
            QVERIFY2(!succeeded, fail ? "failed run" : "empty run");
            QCOMPARE(text, QString("No text was recognized"));
            QTRY_VERIFY(!pin->ocr_manager()->is_running());
            QVERIFY(!pin->ocr_has_result());
        }
    }

    // Copying all text recognizes first and then copies; with a result it copies at once. Translating
    // with a result sends that text to the translation window without recognizing again.
    void copyingAndTranslatingUseTheStandInResult() {
        config_->set_translation_provider("deepl");
        std::atomic_int runs = 0;
        StandInEngine engine([&runs](const QImage &) {
            ++runs;
            return ocr_result({{box(10, 10, 100, 20), "hello pin", 0.9}});
        });
        auto *pin = make_pin();
        pin->show();
        QApplication::clipboard()->clear();
        pin->copy_all_text();
        QTRY_COMPARE_WITH_TIMEOUT(clipboard_text(), QString("hello pin"), 10000);
        QCOMPARE(hint_text(pin), QString("Text copied"));
        QTRY_VERIFY(!pin->ocr_manager()->is_running());
        QApplication::clipboard()->clear();
        pin->copy_all_text();
        QCOMPARE(clipboard_text(), QString("hello pin"));
        QCOMPARE(runs.load(), 1);
        const auto dialog = [] {
            TranslationDialog *found = nullptr;
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *typed = qobject_cast<TranslationDialog *>(widget); typed && typed->isVisible()) found = typed;
            return found;
        };
        pin_menu(pin, "Translate");
        QVERIFY(dialog());
        QTRY_VERIFY_WITH_TIMEOUT(dialog()->source_edit->toPlainText().contains("hello pin"), 10000);
        QCOMPARE(runs.load(), 1);
        TranslationManager::cleanup();
        // Translating a pin before it has any result recognizes it first.
        auto *second = make_pin();
        second->show();
        pin_menu(second, "Translate");
        QVERIFY(dialog());
        QTRY_VERIFY_WITH_TIMEOUT(dialog()->source_edit->toPlainText().contains("hello pin"), 10000);
        QVERIFY(second->ocr_has_result());
        QCOMPARE(runs.load(), 2);
        TranslationManager::cleanup();
    }

    // A drag whose release went elsewhere ends when the next move shows the button is no longer held.
    void aSelectionDragEndsWhenTheButtonIsGone() {
        auto *pin = make_pin();
        auto *selection = give_text(pin, {{box(10, 10, 150, 20), "selectable", 1}});
        QVERIFY(selection);
        send_mouse(pin, QEvent::MouseButtonPress, pin->view->mapFromScene(QPointF(30, 20)), Qt::LeftButton, Qt::LeftButton);
        // The release never came: the pointer moves with no button held, then with the left one pressed again.
        send_mouse(pin, QEvent::MouseMove, pin->view->mapFromScene(QPointF(60, 20)), Qt::NoButton, Qt::NoButton);
        send_mouse(pin, QEvent::MouseMove, pin->view->mapFromScene(QPointF(120, 20)), Qt::NoButton, Qt::LeftButton);
        QVERIFY(!selection->has_selection());
    }

    // A slanted line's selection is filled as a polygon; an upright one as a rectangle.
    void aSlantedSelectionIsPainted() {
        auto *pin = make_pin(QSize(260, 200), QPoint(100, 100), {});
        pin->show();
        const QPointF along(std::cos(0.3), std::sin(0.3)), across(-std::sin(0.3), std::cos(0.3));
        const QPointF origin(30, 40);
        const QVector<QPointF> slanted{origin, origin + 150 * along, origin + 150 * along + 24 * across, origin + 24 * across};
        auto *selection = give_text(pin, {{slanted, "slanted text", 1}});
        QVERIFY(selection);
        const auto painted = [&] {
            QImage image(pin->view->viewport()->size(), QImage::Format_ARGB32);
            image.fill(Qt::white);
            pin->view->viewport()->render(&image);
            return image;
        };
        const auto before = painted();
        QKeyEvent select_all(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
        QVERIFY(selection->key_press(&select_all));
        QVERIFY(selection->has_selection());
        QVERIFY(painted() != before);
    }

    // The hotkey of smart translation hands a pin's selected text over directly; without a selection it
    // is left to the normal copy probe.
    void theTranslationHotkeyTakesAPinsSelectedText() {
        auto *pin = make_pin();
        pin->show();
        pin->move(100, 100);
        auto *selection = give_text(pin, {{box(10, 10, 150, 20), "selectable", 1}});
        QVERIFY(selection);
        PinNormalShortcutHandler handler(PinShortcutController::instance());
        RecordingTranslator translator;
        const std::function<void()> hotkey = SelectionTranslationHotkey{&translator, [] {}};
        QCursor::setPos(pin->mapToGlobal(QPoint(50, 40)));
        QVERIFY(!handler.handle_hotkey(0, hotkey));
        QVERIFY(translator.texts.isEmpty());
        QKeyEvent select_all(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
        QVERIFY(selection->key_press(&select_all));
        QVERIFY(handler.handle_hotkey(0, hotkey));
        QCOMPARE(translator.texts, QStringList({"selectable"}));
        // Another callback is none of its business.
        QVERIFY(!handler.handle_hotkey(0, [] {}));
    }

    // A double click the text selection leaves alone reaches the canvas.
    void aDoubleClickOutsideTheTextReachesTheCanvas() {
        auto *pin = make_pin();
        give_text(pin, {{box(10, 10, 40, 20), "text", 1}});
        send_mouse(pin, QEvent::MouseButtonDblClick, pin->view->mapFromScene(QPointF(150, 100)), Qt::LeftButton, Qt::LeftButton);
        send_mouse(pin, QEvent::MouseButtonRelease, pin->view->mapFromScene(QPointF(150, 100)), Qt::LeftButton, Qt::NoButton);
    }

    // Translating or copying text right after pinning recognizes at once; when that is done before
    // the automatic run, the automatic run does not recognize again.
    void automaticRecognitionSkipsAPinAlreadyRecognizedOnDemand() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        for (const bool automatic : {true, false}) {
            config_->set_ocr_enabled(automatic);
            auto *pin = make_pin();
            QVERIFY(pin->ocr_manager()->init_now(true));
            QTRY_VERIFY_WITH_TIMEOUT(!pin->ocr_manager()->is_running(), 30000);
            QVERIFY(pin->ocr_manager()->init_now());
            QVERIFY2(!pin->ocr_manager()->is_running(), automatic ? "automatic on" : "automatic off");
        }
    }

    // ---- Pin toolbar: its own layout and "..." ----

    // The toolbar and its "..." popup are two windows; the pointer on either keeps the toolbar. A click in the
    // popup hides it without a Leave, which ends that.
    void hoveringTheMorePopupCountsAsHoveringTheToolbar() {
        tl::save_layout(*config_, folded_layout({"copy"}), tl::PIN);
        auto *pin = make_pin({{"pin_auto_toolbar", true}});
        auto &hover = *pin->hover_controls;
        auto *session = pin_timer(pin, PinHoverControls::TOOLBAR_GRACE_MS);
        hover.set_pin_hovered(true);
        hover.set_pin_hovered(false);
        auto *toolbar = pin->toolbar;
        QVERIFY(toolbar_shown(pin));
        toolbar->more_btn->click();
        auto *popup = toolbar->more_popup();
        QVERIFY(popup && popup->isVisible());
        QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
        QApplication::sendEvent(toolbar, &enter);
        QApplication::sendEvent(popup, &enter);
        // The toolbar's Leave arrives late.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(toolbar, &leave);
        QVERIFY(!session->isActive());
        toolbar->copy_btn->click();
        QVERIFY(!popup->isVisible());
        QVERIFY(session->isActive());
        QVERIFY(wait_for_clipboard_writes(10.0));
    }

    // Adjusting from a pin edits only the pin layout, and the toolbar stays up while the dialog is open.
    void adjustingThePinToolbarEditsThePinLayout() {
        // Closing the dialog rechecks the real pointer, which must not be over the pin.
        QCursor::setPos(0, 0);
        auto *pin = make_pin({{"pin_auto_toolbar", true}});
        auto &hover = *pin->hover_controls;
        auto *session = pin_timer(pin, PinHoverControls::TOOLBAR_GRACE_MS);
        hover.set_pin_hovered(true);
        auto *toolbar = pin->toolbar;
        toolbar->more_btn->click();
        QStringList listed;
        bool kept_up = false;
        QTimer poll;
        poll.setInterval(10);
        connect(&poll, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<ToolbarLayoutDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            poll.stop();
            // The pointer has left the pin for the dialog.
            hover.set_pin_hovered(false);
            kept_up = !session->isActive();
            for (auto *row : dialog->findChild<ReorderableRowList *>()->rows()) {
                auto *layout_row = static_cast<ToolbarLayoutRow *>(row);
                listed << layout_row->key;
                if (layout_row->key == "pen") layout_row->set_mode(tl::HIDE);
            }
            dialog->accept();
        });
        poll.start();
        toolbar->more_popup()->adjust_btn->click();
        QVERIFY(kept_up);
        // Closing the dialog ends the open-menu state, so the hover session can end again.
        QVERIFY(session->isActive());
        auto order = tl::PIN.order;
        listed.sort();
        order.sort();
        QCOMPARE(listed, order);
        QCOMPARE(mode_in(tl::load_layout(*config_, tl::PIN), "pen"), tl::HIDE);
        QCOMPARE(mode_in(tl::load_layout(*config_), "pen"), tl::SHOW);
        QVERIFY(toolbar->pen_btn->isHidden());
    }

    void foldedSpotlightAndWatermarkWorkOnAPin() {
        config_->set_screenshot_watermark_text("PIN");
        auto *pin = make_pin(QSize(200, 150));
        pin->toggle_toolbar();
        auto *toolbar = pin->toolbar;
        auto *scene = pin->canvas->scene;
        toolbar->more_btn->click();
        toolbar->spotlight_btn->click();
        auto *controller = scene->tool_controller.get();
        QCOMPARE(controller->current_tool_id(), QString("spotlight"));
        controller->on_press(QPointF(60, 40), Qt::LeftButton);
        controller->on_move(QPointF(140, 110));
        controller->on_release(QPointF(140, 110));
        const auto items = scene->items();
        QVERIFY(std::any_of(items.begin(), items.end(), [](QGraphicsItem *item) { return dynamic_cast<SpotlightItem *>(item); }));
        const auto image = pin->get_current_image();
        QVERIFY(image.pixelColor(10, 10).lightness() < image.pixelColor(100, 75).lightness());

        toolbar->more_btn->click();
        toolbar->watermark_btn->click();
        auto *item = WatermarkItem::find(scene);
        QVERIFY(item && item->text() == "PIN");
        QCOMPARE(toolbar->watermark_panel->text_field->text(), QString("PIN"));
        scene->undo_stack->undo();
        QVERIFY(!WatermarkItem::find(scene));
    }

    // The panels and the "..." popup are top-level windows without a parent and do not go with the toolbar;
    // closing the pin deletes each of them.
    void closingThePinDeletesTheToolbarSideWindows() {
        config_->set_screenshot_watermark_text("PIN");
        auto *pin = make_pin();
        pin->toggle_toolbar();
        auto *toolbar = pin->toolbar;
        toolbar->more_btn->click();
        // Panels other than the common ones are built on first use.
        for (const char *tool : {"arrow", "text", "mosaic", "watermark"}) QVERIFY(toolbar->ensure_panel_for_tool(tool));
        QApplication::processEvents();
        QList<QPointer<QWidget>> windows;
        for (QWidget *window : std::initializer_list<QWidget *>{
                 toolbar->paint_panel, toolbar->shape_panel, toolbar->arrow_panel, toolbar->number_panel,
                 toolbar->text_panel, toolbar->mosaic_panel, toolbar->watermark_panel, toolbar->more_popup()})
            windows.append(window);
        for (const auto &window : windows) QVERIFY(window && !window->parent());
        pin->close_window();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        for (const auto &window : windows) QVERIFY(!window);
    }

    // ---- PinCanvas: inherited annotations and editing through the toolbar ----

    // Every annotation kind of the capture comes along, moved by the selection offset, as undoable steps.
    void pinsInheritEveryKindOfAnnotation() {
        QImage background(400, 300, QImage::Format_RGB32);
        background.fill(Qt::white);
        CanvasScene capture(background, QRectF(0, 0, 400, 300), true);
        QList<QGraphicsItem *> items;
        const auto add = [&capture, &items](QGraphicsItem *item) {
            capture.addItem(item);
            items << item;
        };
        add(new EllipseItem(QRectF(60, 60, 80, 40), QPen(Qt::red, 3)));
        auto *arrow = new ArrowItem(QPointF(60, 150), QPointF(200, 180), QPen(Qt::blue, 4), "double");
        arrow->set_control_point(QPointF(130, 120));
        arrow->set_bend_point(1, QPointF(170, 190));
        add(arrow);
        auto *text = new TextItem("note", QPointF(70, 200), QFont("Arial", 14), Qt::darkGreen);
        text->setTextWidth(120);
        text->set_outline(true, Qt::white, QVariant(2.0));
        text->set_shadow(true, QColor(0, 0, 0, 90));
        text->has_background = true;
        text->background_color = Qt::yellow;
        add(text);
        add(new NumberItem(3, QPointF(250, 80), 14, Qt::red));
        auto *marker = new RectItem(QRectF(220, 150, 90, 30), QPen(Qt::yellow, 2));
        marker->is_highlighter = true;
        marker->is_highlighter_rect = true;
        add(marker);
        add(new SpotlightItem(QRectF(260, 200, 60, 50), 8));
        add(new WatermarkItem("jietuba", WatermarkStyle{}));
        QPainterPath smear(QPointF(100, 100));
        smear.lineTo(160, 120);
        add(new MosaicItem(smear, 20, 8, background.scaled(50, 38), QRectF(0, 0, 400, 300)));
        add(new QGraphicsLineItem(0, 0, 10, 10));

        auto *pin = make_pin(QSize(300, 200));
        auto *canvas = pin->canvas;
        const int before = canvas->undo_stack->count();
        canvas->initialize_from_items(items, QPoint(50, 50), 4);
        const auto ellipses = items_of<EllipseItem>(canvas->scene);
        QCOMPARE(ellipses.size(), 1);
        QCOMPARE(ellipses.first()->mapRectToScene(ellipses.first()->rect()), QRectF(10, 10, 80, 40));
        const auto arrows = items_of<ArrowItem>(canvas->scene);
        QCOMPARE(arrows.size(), 1);
        QCOMPARE(arrows.first()->arrow_style(), QString("double"));
        QVERIFY(arrows.first()->is_curved());
        QVERIFY(arrows.first()->has_bend_points());
        QCOMPARE(arrows.first()->bend_points(), arrow->bend_points());
        const auto texts = items_of<TextItem>(canvas->scene);
        QCOMPARE(texts.size(), 1);
        QCOMPARE(texts.first()->toPlainText(), QString("note"));
        QCOMPARE(texts.first()->pos(), QPointF(20, 150));
        QCOMPARE(texts.first()->textWidth(), 120.0);
        QVERIFY(texts.first()->has_background);
        QVERIFY(std::get<0>(texts.first()->outline_state()));
        QCOMPARE(items_of<NumberItem>(canvas->scene).first()->number, 3);
        bool highlighter = false;
        for (auto *rect : items_of<RectItem>(canvas->scene)) highlighter |= rect->is_highlighter_rect;
        QVERIFY(highlighter);
        QCOMPARE(items_of<SpotlightItem>(canvas->scene).size(), 1);
        QCOMPARE(items_of<WatermarkItem>(canvas->scene).size(), 1);
        QCOMPARE(items_of<MosaicItem>(canvas->scene).size(), 1);
        // The unsupported line is skipped; the counter continues after the capture's numbers.
        QCOMPARE(items_of<QGraphicsLineItem>(canvas->scene).size(), 0);
        QCOMPARE(NumberTool::get_next_number(canvas->scene), 4);
        QVERIFY(canvas->undo_stack->count() >= before + 8);
        // The pinned image shows them.
        QVERIFY(canvas->get_current_image().convertToFormat(QImage::Format_RGB32) != solid(QSize(300, 200)).convertToFormat(QImage::Format_RGB32));
        while (canvas->undo_stack->canUndo()) canvas->undo_stack->undo();
        QCOMPARE(items_of<EllipseItem>(canvas->scene).size(), 0);
        canvas->initialize_from_items({}, QPoint());
    }

    // The pin undoes in the order the capture added its annotations, not by stacking order.
    void pinsUndoInTheOrderTheCaptureAdded() {
        QImage background(200, 200, QImage::Format_RGB32);
        background.fill(Qt::white);
        CanvasScene capture(background, QRectF(0, 0, 200, 200), true);
        auto *first = new RectItem(QRectF(10, 120, 40, 30), QPen(Qt::red, 3));
        auto *text = new TextItem("note", QPointF(20, 20), QFont("Arial", 16), Qt::black);
        auto *rect = new RectItem(QRectF(60, 60, 40, 30), QPen(Qt::red, 3));
        QVERIFY(text->zValue() > rect->zValue());
        capture.undo_stack->push(new AddItemCommand(&capture, ItemReference(first)));
        capture.undo_stack->push(new AddItemCommand(&capture, ItemReference(text)));
        capture.undo_stack->push(new AddItemCommand(&capture, ItemReference(rect)));
        // Restoring a removed item puts it back on top, but not later in the order.
        capture.undo_stack->push(new RemoveItemCommand(&capture, ItemReference(first)));
        capture.undo_stack->undo();
        const auto items = capture.get_drawing_items_in_rect(QRectF(0, 0, 200, 200));
        QCOMPARE(items, (QList<QGraphicsItem *>{first, text, rect}));

        auto *canvas = make_pin(QSize(200, 200))->canvas;
        canvas->initialize_from_items(items, QPoint(0, 0));
        auto *cloned_text = items_of<TextItem>(canvas->scene).first();
        QGraphicsItem *cloned_rect = nullptr;
        for (auto *item : items_of<RectItem>(canvas->scene))
            if (item->rect() == rect->rect()) cloned_rect = item;
        QVERIFY(cloned_rect);
        QVERIFY(cloned_text->zValue() > cloned_rect->zValue());
        canvas->undo_stack->undo();
        QVERIFY(!cloned_rect->scene());
        QVERIFY(cloned_text->scene() == canvas->scene);
    }

    // The pin toolbar drives the canvas: tools, styles, undo and leaving editing.
    void theToolbarEditsThePin() {
        auto *pin = make_pin(QSize(300, 200));
        pin->show();
        pin->show_toolbar();
        auto *toolbar = pin->toolbar;
        auto *canvas = pin->canvas;
        QVERIFY(toolbar && canvas && pin->view);
        auto *viewport = pin->view->viewport();
        const auto drag = [viewport](const QPoint &from, const QPoint &to) {
            const auto send = [viewport](QEvent::Type type, const QPoint &pos, Qt::MouseButtons buttons) {
                QMouseEvent event(type, QPointF(pos), QPointF(viewport->mapToGlobal(pos)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons,
                                  Qt::NoModifier);
                QApplication::sendEvent(viewport, &event);
            };
            send(QEvent::MouseButtonPress, from, Qt::LeftButton);
            send(QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
            send(QEvent::MouseMove, to, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, to, Qt::NoButton);
        };
        emit toolbar->tool_changed("rect");
        QVERIFY(canvas->is_editing());
        QVERIFY(pin->is_editing);
        QCOMPARE(canvas->tool_controller->current_tool_id(), QString("rect"));
        drag(QPoint(40, 40), QPoint(140, 110));
        QCOMPARE(items_of<RectItem>(canvas->scene).size(), 1);
        emit toolbar->color_changed(QColor(0, 160, 0));
        QCOMPARE(canvas->tool_controller->ctx.color, QColor(0, 160, 0));
        emit toolbar->stroke_width_changed(9);
        QCOMPARE(canvas->tool_controller->ctx.stroke_width, 9.0);
        emit toolbar->opacity_changed(128);
        QVERIFY(std::abs(canvas->tool_controller->ctx.opacity - 128 / 255.0) < 0.01);
        emit toolbar->line_style_changed("dashed");
        emit toolbar->undo_clicked();
        emit toolbar->undo_clicked();
        emit toolbar->undo_clicked();
        emit toolbar->undo_clicked();
        QCOMPARE(items_of<RectItem>(canvas->scene).size(), 0);
        emit toolbar->redo_clicked();
        QCOMPARE(items_of<RectItem>(canvas->scene).size(), 1);

        // A selected arrow takes the new style as one undoable step.
        emit toolbar->tool_changed("arrow");
        drag(QPoint(50, 150), QPoint(200, 170));
        ArrowItem *arrow = nullptr;
        for (auto *item : canvas->scene->items())
            if (auto *found = dynamic_cast<ArrowItem *>(item)) arrow = found;
        QVERIFY(arrow);
        pin->view->smart_edit_controller->select_item(arrow);
        const auto old_style = arrow->arrow_style();
        emit toolbar->arrow_style_changed("bar");
        QCOMPARE(arrow->arrow_style(), QString("bar"));
        canvas->undo_stack->undo();
        QCOMPARE(arrow->arrow_style(), old_style);
        pin->view->smart_edit_controller->clear_selection();
        emit toolbar->arrow_style_changed("double");
        QCOMPARE(arrow->arrow_style(), old_style);

        // Numbers show the next value; unknown tools are refused; the cursor leaves editing.
        emit toolbar->tool_changed("number");
        QCOMPARE(canvas->tool_controller->current_tool_id(), QString("number"));
        emit toolbar->number_next_changed(7);
        QCOMPARE(NumberTool::get_next_number(canvas->scene), 7);
        QVERIFY(!canvas->activate_tool("no-such-tool"));
        emit toolbar->tool_changed("mosaic");
        emit toolbar->mosaic_block_size_changed(12);
        emit toolbar->tool_changed("cursor");
        QVERIFY(!canvas->is_editing());
        QVERIFY(!pin->is_editing);
        canvas->deactivate_tool();
        QVERIFY(!canvas->get_current_image().isNull());
    }

    // Translating a pin recognizes its text first, then fills the translation window beside it;
    // DeepL without a key answers locally, so nothing goes to the network.
    void translatingAPinRecognizesThenTranslates() {
        if (!is_ocr_available()) QSKIP("no OCR engine on this machine");
        const QString arial = arial_family();
        if (arial.isEmpty()) QSKIP("Arial is not installed");
        config_->set_translation_provider("deepl");
        QImage image(220, 80, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont(arial, 24));
        painter.drawText(image.rect(), Qt::AlignCenter, "Hello pin");
        painter.end();
        auto *pin = new PinWindow(image, QPoint(20, 40), config_);
        pins_.append(pin);
        pin->show();
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        const int width = TranslationDialog::initial_size_for_available_geometry(available).first;
        // Right of the pin when it fits, else left, else kept inside the screen.
        const auto placed_beside = [&](PinWindow *target, const QRect &window) {
            if (target->geometry().right() + 11 + width <= available.right() + 1) return window.x() > target->geometry().right();
            if (target->x() - 10 - width >= available.left()) return window.right() < target->x();
            // A window wider than the screen starts at its left edge.
            return available.contains(window.topLeft()) && (window.right() <= available.right() || window.x() == available.left());
        };
        const auto dialog = [] {
            TranslationDialog *found = nullptr;
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *typed = qobject_cast<TranslationDialog *>(widget); typed && typed->isVisible()) found = typed;
            return found;
        };
        pin_menu(pin, "Translate");
        // The window opens at once and waits for recognition.
        QVERIFY(dialog());
        QTRY_VERIFY_WITH_TIMEOUT(!pin->ocr_manager()->is_running() && pin->ocr_has_result(), 30000);
        QTRY_VERIFY(dialog()->source_edit->toPlainText().contains("Hello"));
        QVERIFY(placed_beside(pin, dialog()->geometry()));
        // With a result in hand, translating again uses it directly.
        dialog()->source_edit->clear();
        pin_menu(pin, "Translate");
        QVERIFY(!pin->ocr_manager()->is_running());
        QVERIFY(dialog()->source_edit->toPlainText().contains("pin"));
        TranslationManager::cleanup();
        // A pin at the right edge.
        auto *right = new PinWindow(image, QPoint(available.right() - 240, 40), config_);
        pins_.append(right);
        right->show();
        pin_menu(right, "Translate");
        QVERIFY(dialog());
        QVERIFY(placed_beside(right, dialog()->geometry()));
        QTRY_VERIFY_WITH_TIMEOUT(!right->ocr_manager()->is_running(), 30000);
        TranslationManager::cleanup();
    }

    // A click whose button also has a double-click binding waits to see whether a second click follows.
    void aClickWaitsWhenItsButtonAlsoHasADoubleClick() {
        config_->set_app_setting("mouse_pin_close", "middle");
        config_->set_app_setting("mouse_pin_reset", "doublemiddle");
        auto *pin = make_pin();
        click(pin, QPoint(100, 60), Qt::MiddleButton);
        QVERIFY(!pin->is_closed());
        QTRY_VERIFY_WITH_TIMEOUT(pin->is_closed(), QApplication::doubleClickInterval() + 1000);
    }

    // With the classic dialog, saving asks for a file, starting in the save folder; without an extension the
    // chosen filter decides the format. The save window writes the pin image too. Copying goes to the clipboard.
    void savingAndCopyingThePinImage() {
        QTemporaryDir out;
        QString opened_in;
        const auto save_folder = dir_.filePath("pin-saves");
        config_->set_screenshot_save_path(save_folder);
        auto *pin = make_pin(QSize(120, 80));
        {
            SaveDialogAnswerer answer(out.filePath("window.jpg"));
            pin->save_image();
            QVERIFY(answer.answered);
        }
        QCOMPARE(QImageReader(out.filePath("window.jpg")).size(), QSize(120, 80));
        QVERIFY(QFile::remove(out.filePath("window.jpg")));
        config_->set_app_setting("save_dialog_style", "classic");
        const auto answer = [&out, &opened_in](const QString &name, const QString &filter) {
            auto *timer = new QTimer;
            auto started = std::make_shared<QElapsedTimer>();
            started->start();
            QObject::connect(timer, &QTimer::timeout, [timer, started, &out, &opened_in, name, filter] {
                auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                auto *edit = dialog ? dialog->findChild<QLineEdit *>("fileNameEdit") : nullptr;
                const bool give_up = started->elapsed() > 10000;
                if ((!edit || !dialog->isVisible()) && !give_up) return;
                timer->stop();
                timer->deleteLater();
                if (!dialog) return;
                opened_in = dialog->directory().absolutePath();
                if (name.isEmpty() || give_up) {
                    dialog->reject();
                    return;
                }
                dialog->selectNameFilter(filter);
                dialog->setDirectory(out.path());
                edit->setText(name);
                static_cast<QDialog *>(dialog)->accept();
            });
            timer->start(50);
        };
        answer({}, {});
        pin->save_image();
        QVERIFY(QDir(out.path()).entryList(QDir::Files).isEmpty());
        QCOMPARE(QDir(opened_in), QDir(save_folder));
        QVERIFY(QFileInfo(save_folder).isDir());
        answer("plain", "BMP (*.bmp)");
        pin->save_image();
        QCOMPARE(QImageReader(out.filePath("plain.bmp")).size(), QSize(120, 80));
        answer("typed.png", "BMP (*.bmp)");
        pin->save_image();
        QCOMPARE(QImageReader(out.filePath("typed.png")).format(), QByteArray("png"));
        pin->copy_to_clipboard();
        QVERIFY(wait_for_clipboard_writes(10.0));
    }
};

QTEST_MAIN(PinTests)
#include "test_pin.moc"
