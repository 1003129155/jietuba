#include "ocr/ocr_manager.h"
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
#include "pin/ocr_text_layer.h"
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
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include "core/clipboard_utils.h"
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
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jietuba;
namespace tl = jietuba::toolbar_layout;

namespace {
constexpr double ORIG_W = 200.0;
constexpr double ORIG_H = 100.0;

PinImageTransform transform_state(int rotation, bool flip_h, bool flip_v) {
    PinImageTransform transform;
    for (int turned = 0; turned < rotation; turned += 90) transform.rotate_cw();
    if (flip_h) transform.flip_horizontal();
    if (flip_v) transform.flip_vertical();
    return transform;
}

template <typename F>
void for_each_state(F check) {
    for (int rotation : {0, 90, 180, 270})
        for (bool flip_h : {false, true})
            for (bool flip_v : {false, true}) check(transform_state(rotation, flip_h, flip_v), QString("rotation=%1 flip_h=%2 flip_v=%3").arg(rotation).arg(flip_h).arg(flip_v));
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

// Mouse events go to the view's viewport, through the same event filter as real input.
void send_mouse(PinWindow *pin, QEvent::Type type, const QPoint &local, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    auto *viewport = pin->view->viewport();
    QMouseEvent event(type, QPointF(local), QPointF(viewport->mapToGlobal(local)), button, buttons, modifiers);
    QApplication::sendEvent(viewport, &event);
}

void send_mouse_global(PinWindow *pin, QEvent::Type type, const QPoint &global, Qt::MouseButton button, Qt::MouseButtons buttons) {
    auto *viewport = pin->view->viewport();
    QMouseEvent event(type, QPointF(viewport->mapFromGlobal(global)), QPointF(global), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(viewport, &event);
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

OCRTextItem text_item(const QString &text, double x = 0, double y = 0, double w = 100, double h = 20) { return OCRTextItem(text, box(x, y, w, h), 0.99); }

OcrResult ocr_result(const QList<OcrLine> &lines, int code = 100) {
    OcrResult result;
    result.code = code;
    result.data = QVector<OcrLine>(lines.begin(), lines.end());
    return result;
}

// Gives a pin a text layer with these blocks, as a finished recognition would.
OCRTextLayer *give_text(PinWindow *pin, const QList<OcrLine> &lines) {
    const auto size = pin->orig_size();
    auto *layer = new OCRTextLayer(pin, size.width(), size.height());
    layer->setGeometry(pin->content_rect().toRect());
    pin->ocr_manager()->ocr_text_layer = layer;
    auto [items, union_rect] = OCRTextLayer::prepare_ocr_items(ocr_result(lines));
    layer->load_prepared_ocr_items(std::move(items), union_rect, size.width(), size.height());
    return layer;
}

void send_layer_mouse(QWidget *layer, QEvent::Type type, const QPoint &local, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(local), QPointF(layer->mapToGlobal(local)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(layer, &event);
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
// separators as "-"), triggers the entry starting with trigger, then closes it.
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
            entries << action->text();
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

    bool desktop() const { return QGuiApplication::platformName() == "windows"; }

    // The offscreen platform has no font directory; the font file is loaded once per process,
    // so a repeated run (check.ps1 -Mode Leak) does not add it again.
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
        // The OCR layer scales by mapped_image_size, so both must agree.
        for_each_state([&](const PinImageTransform &state, const QString &label) {
            const auto shown = state.display_size(size);
            QVERIFY2(state.mapped_image_size(200, 100) == std::make_pair(double(shown.width()), double(shown.height())), qPrintable(label));
        });
    }

    // ---- OCR coordinate mapping ----

    void ocrPointMappingFollowsEachTransform() {
        QVERIFY(transform_state(0, false, false).map_ocr_point(37, 21, ORIG_W, ORIG_H) == std::make_pair(37.0, 21.0));
        // Clockwise 90: the top-left corner lands top-right.
        auto [x, y] = transform_state(90, false, false).map_ocr_point(0, 0, ORIG_W, ORIG_H);
        QVERIFY(close_to(x, ORIG_H) && close_to(y, 0));
        std::tie(x, y) = transform_state(180, false, false).map_ocr_point(30, 20, ORIG_W, ORIG_H);
        QVERIFY(close_to(x, ORIG_W - 30) && close_to(y, ORIG_H - 20));
        std::tie(x, y) = transform_state(270, false, false).map_ocr_point(0, 0, ORIG_W, ORIG_H);
        QVERIFY(close_to(x, 0) && close_to(y, ORIG_W));
        std::tie(x, y) = transform_state(0, true, false).map_ocr_point(30, 20, ORIG_W, ORIG_H);
        QVERIFY(close_to(x, ORIG_W - 30) && close_to(y, 20));
        std::tie(x, y) = transform_state(0, false, true).map_ocr_point(30, 20, ORIG_W, ORIG_H);
        QVERIFY(close_to(x, 30) && close_to(y, ORIG_H - 20));
    }

    void centreAndCornersMapOntoTheTransformedImage() {
        for_each_state([](const PinImageTransform &state, const QString &label) {
            const auto [mapped_w, mapped_h] = state.mapped_image_size(ORIG_W, ORIG_H);
            const auto [cx, cy] = state.map_ocr_point(ORIG_W / 2, ORIG_H / 2, ORIG_W, ORIG_H);
            QVERIFY2(close_to(cx, mapped_w / 2) && close_to(cy, mapped_h / 2), qPrintable(label));
            // Corners land exactly on corners: nothing out of bounds, nothing collapsed.
            QSet<QPair<qint64, qint64>> got, want;
            const auto key = [](double px, double py) { return qMakePair(std::llround(px * 1e6), std::llround(py * 1e6)); };
            for (const auto &corner : {QPointF(0, 0), QPointF(ORIG_W, 0), QPointF(ORIG_W, ORIG_H), QPointF(0, ORIG_H)}) {
                const auto [px, py] = state.map_ocr_point(corner.x(), corner.y(), ORIG_W, ORIG_H);
                got.insert(key(px, py));
            }
            for (const auto &corner : {QPointF(0, 0), QPointF(mapped_w, 0), QPointF(mapped_w, mapped_h), QPointF(0, mapped_h)}) want.insert(key(corner.x(), corner.y()));
            QVERIFY2(got == want, qPrintable(label));
        });
    }

    void ocrRectMappingStaysInsideTheImage() {
        const QRectF rect(10, 20, 50, 30);
        const auto identity = transform_state(0, false, false).map_ocr_rect(rect, ORIG_W, ORIG_H);
        QVERIFY(close_to(identity.left(), 10) && close_to(identity.top(), 20) && close_to(identity.width(), 50) && close_to(identity.height(), 30));
        const auto turned = transform_state(90, false, false).map_ocr_rect(rect, ORIG_W, ORIG_H);
        QVERIFY(close_to(turned.width(), 30) && close_to(turned.height(), 50));
        for_each_state([&](const PinImageTransform &state, const QString &label) {
            const auto [mapped_w, mapped_h] = state.mapped_image_size(ORIG_W, ORIG_H);
            const auto mapped = state.map_ocr_rect(rect, ORIG_W, ORIG_H);
            QVERIFY2(mapped.left() >= -1e-6 && mapped.top() >= -1e-6 && mapped.right() <= mapped_w + 1e-6 && mapped.bottom() <= mapped_h + 1e-6, qPrintable(label));
        });
        // 180 degrees twice puts the rectangle back.
        const auto half_turn = transform_state(180, false, false);
        const auto twice = half_turn.map_ocr_rect(half_turn.map_ocr_rect(rect, ORIG_W, ORIG_H), ORIG_W, ORIG_H);
        QVERIFY(close_to(twice.left(), 10) && close_to(twice.top(), 20) && close_to(twice.width(), 50) && close_to(twice.height(), 30));
    }

    // The OCR text layer and the picture must line up: both use the same order of rotate and flip.
    void viewTransformAgreesWithOcrMapping() {
        for_each_state([](const PinImageTransform &state, const QString &label) {
            const auto [mapped_w, mapped_h] = state.mapped_image_size(ORIG_W, ORIG_H);
            const auto view = state.build_view_transform(mapped_w, mapped_h, ORIG_W, ORIG_H);
            for (const auto &point : {QPointF(0, 0), QPointF(ORIG_W, 0), QPointF(ORIG_W, ORIG_H), QPointF(0, ORIG_H), QPointF(ORIG_W / 2, ORIG_H / 2), QPointF(37, 21)}) {
                const auto via_view = view.map(point);
                const auto [ox, oy] = state.map_ocr_point(point.x(), point.y(), ORIG_W, ORIG_H);
                QVERIFY2(close_to(via_view.x(), ox) && close_to(via_view.y(), oy), qPrintable(label + QString(" point=%1,%2").arg(point.x()).arg(point.y())));
            }
        });
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

    void topmostSuppressionIsAnIdempotentSwitch() {
        auto *manager = PinManager::instance();
        auto *pin = manager->create_pin(solid(QSize(80, 60)), QPoint(10, 20), config_);
        manager->restore_topmost();
        QVERIFY(!manager->topmost_suppressed());
        for (int round = 0; round < 3; ++round) {
            manager->suppress_topmost();
            manager->suppress_topmost();
            QVERIFY(manager->topmost_suppressed());
            manager->restore_topmost();
            QVERIFY(!manager->topmost_suppressed());
        }
        QVERIFY(pin->windowFlags() & Qt::WindowStaysOnTopHint);
    }

    void topmostSuppressionUsesTheRealWindowStyle() {
        if (!desktop()) QSKIP("needs -platform windows");
        auto *manager = PinManager::instance();
        auto *topmost = manager->create_pin(solid(QSize(80, 60)), QPoint(10, 20), config_);
        auto *plain = manager->create_pin(solid(QSize(80, 60)), QPoint(200, 20), config_);
        plain->toggle_stay_on_top();
        QVERIFY(ex_style(topmost) & WS_EX_TOPMOST);
        QVERIFY(!(ex_style(plain) & WS_EX_TOPMOST));
        manager->suppress_topmost();
        QVERIFY(!(ex_style(topmost) & WS_EX_TOPMOST));
        // A pin created during a capture sinks too and comes back with the rest.
        auto *late = manager->create_pin(solid(QSize(80, 60)), QPoint(400, 20), config_);
        QVERIFY(!(ex_style(late) & WS_EX_TOPMOST));
        manager->restore_topmost();
        QVERIFY(ex_style(topmost) & WS_EX_TOPMOST);
        QVERIFY(ex_style(late) & WS_EX_TOPMOST);
        QVERIFY(!(ex_style(plain) & WS_EX_TOPMOST));

        // A pin closed meanwhile is skipped.
        manager->suppress_topmost();
        topmost->close_window();
        manager->restore_topmost();
        QVERIFY(ex_style(late) & WS_EX_TOPMOST);
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
            QString::fromUtf8("Toolbar (SPACE)\t○"), QString::fromUtf8("Always on top\t●"), QString::fromUtf8("Mouse click-through\t○"),
            QString::fromUtf8("Lock (L)\t○"), QString::fromUtf8("Border\t●"), QString::fromUtf8("Text selection\t○"),
            QString::fromUtf8("Thumbnail mode (R)\t○"), "-", "Close (ESC)"};
        QCOMPARE(pin_menu(pin), want);
    }

    void aThumbnailMenuKeepsOnlySevenEntries() {
        auto *pin = make_pin();
        QCursor::setPos(0, 0);
        pin->toggle_thumbnail_mode();
        const QStringList want = {"Copy (CTRL+C)", "Save as", "-", QString::fromUtf8("Always on top\t●"), QString::fromUtf8("Mouse click-through\t○"),
                                  QString::fromUtf8("Lock (L)\t○"), QString::fromUtf8("Thumbnail mode (R)\t●"), "-", "Close (ESC)"};
        QCOMPARE(pin_menu(pin), want);
    }

    void theMenuReportsLockAndWhetherEscCloses() {
        auto *pin = make_pin();
        pin->toggle_lock();
        const auto entries = pin_menu(pin);
        QVERIFY(entries.contains(QString::fromUtf8("Lock (L)\t●")));
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

    // ---- Keyboard shortcuts over the pin under the pointer ----

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

    void keysSkipPinsThatAreNotReallyUnderThePointer() {
        auto *lower = make_pin(QSize(200, 120), QPoint(100, 100));
        auto *upper = make_pin(QSize(200, 120), QPoint(150, 130));
        auto *controller = PinShortcutController::instance();
        QCursor::setPos(500, 500);
        QCOMPARE(controller->find_pin_under_cursor(), nullptr);
        press_key(lower, Qt::Key_Escape);
        QVERIFY(!lower->is_closed() && !upper->is_closed());
        // The newest pin is on top where they overlap.
        QCursor::setPos(200, 180);
        QCOMPARE(controller->find_pin_under_cursor(), upper);
        // Click-through pins take no keys.
        upper->set_click_through(true);
        QCOMPARE(controller->find_pin_under_cursor(), lower);
        upper->set_click_through(false);
        // During a capture pins sit below it and take no keys.
        PinManager::instance()->suppress_topmost();
        QCOMPARE(controller->find_pin_under_cursor(), nullptr);
        press_key(upper, Qt::Key_Escape);
        QVERIFY(!upper->is_closed());
        PinManager::instance()->restore_topmost();
        // Closed pins are dropped.
        upper->close_window();
        QCOMPARE(controller->find_pin_under_cursor(), lower);
        controller->register_pin(lower);
        controller->unregister_pin(upper);
        QCOMPARE(controller->find_pin_under_cursor(), lower);
    }

    void editModeKeysLeaveTheToolBeforeClosing() {
        auto *pin = make_pin();
        QCursor::setPos(pin->mapToGlobal(QPoint(150, 100)));
        PinShortcutController::instance()->reload_bindings();
        auto *layer = give_text(pin, {{box(10, 10, 100, 20), "hello", 0.9}});
        QVERIFY(pin->canvas->activate_tool("pen"));
        layer->selection_start = std::make_pair(0, 0);
        layer->selection_end = std::make_pair(0, 5);
        // The text selection is cleared first, but only while the layer is active.
        layer->set_drawing_mode(false);
        press_key(pin, Qt::Key_Escape);
        QVERIFY(!layer->selection_start);
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
        auto *layer = give_text(pin, {{box(10, 10, 100, 20), "hello world", 0.9}});
        layer->selection_start = std::make_pair(0, 6);
        layer->selection_end = std::make_pair(0, 11);
        QApplication::clipboard()->clear();
        press_key(pin, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard_text(), QString("world"));
        // Editing does not change that.
        QVERIFY(pin->canvas->activate_tool("pen"));
        layer->selection_start = std::make_pair(0, 0);
        layer->selection_end = std::make_pair(0, 5);
        layer->set_drawing_mode(false);
        press_key(pin, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard_text(), QString("hello"));
    }

    // ---- OCR text layer: geometry and character positions ----

    void textItemsTakeTheBoundingBoxOfTheirCorners() {
        const auto item = text_item("abc", 10, 20, 100, 30);
        QCOMPARE(item.norm_rect, QRectF(10, 20, 100, 30));
        // Engines give corners in any order.
        const OCRTextItem shuffled("x", {QPointF(10, 5), QPointF(0, 0), QPointF(0, 5), QPointF(10, 0)}, 1.0);
        QCOMPARE(shuffled.norm_rect, QRectF(0, 0, 10, 5));
        QCOMPARE(item.get_scaled_rect(2.0, 3.0), QRect(20, 60, 200, 90));
        const auto plain = text_item("abc", 0, 0, 100, 20);
        QVERIFY(plain.contains(QPoint(50, 10), 1.0, 1.0));
        QVERIFY(!plain.contains(QPoint(500, 500), 1.0, 1.0));
        // A small margin makes edge characters easy to hit.
        QVERIFY(text_item("abc", 10, 10, 100, 20).contains(QPoint(7, 8), 1.0, 1.0));
    }

    void charPositionsWeighWideCharactersDouble() {
        auto ascii = text_item("abcd");
        ascii.calculate_char_positions(QRect(0, 0, 80, 20));
        QCOMPARE(ascii.char_positions, (std::vector<int>{0, 20, 40, 60, 80}));
        auto mixed = text_item(QString::fromUtf8("中a"));
        mixed.calculate_char_positions(QRect(0, 0, 90, 20));
        QCOMPARE(mixed.char_positions, (std::vector<int>{0, 60, 90}));
        auto punctuation = text_item(QString::fromUtf8("，a"));
        punctuation.calculate_char_positions(QRect(0, 0, 90, 20));
        QCOMPARE(punctuation.char_positions, (std::vector<int>{0, 60, 90}));
        auto offset = text_item("ab");
        offset.calculate_char_positions(QRect(100, 0, 40, 20));
        QCOMPARE(offset.char_positions.front(), 100);
        QCOMPARE(offset.char_positions.back(), 140);
        // Characters outside the BMP count once.
        auto emoji = text_item(QString::fromUtf8("a😀b"));
        emoji.calculate_char_positions(QRect(0, 0, 90, 20));
        QCOMPARE(emoji.length(), 3);
        QCOMPARE(emoji.slice(1, 2), QString::fromUtf8("😀"));
        // A tall narrow block (after rotation) runs vertically.
        auto vertical = text_item("abcd");
        vertical.calculate_char_positions(QRect(0, 0, 20, 80));
        QVERIFY(vertical.is_vertical);
        QCOMPARE(vertical.char_positions, (std::vector<int>{0, 20, 40, 60, 80}));
        auto empty = text_item("");
        empty.calculate_char_positions(QRect(0, 0, 80, 20));
        QVERIFY(empty.char_positions.empty());
    }

    void clicksSnapToTheNearestCharacterBoundary() {
        auto item = text_item("abcd");
        const QRect rect(0, 0, 80, 20);
        QCOMPARE(item.get_char_index_at_pos(50, rect), 0);
        item.calculate_char_positions(rect);
        QCOMPARE(item.get_char_index_at_pos(-50, rect), 0);
        QCOMPARE(item.get_char_index_at_pos(500, rect), 4);
        QCOMPARE(item.get_char_index_at_pos(0, rect), 0);
        QCOMPARE(item.get_char_index_at_pos(22, rect), 1);
        QCOMPARE(item.get_char_index_at_pos(38, rect), 2);
        auto vertical = text_item("abcd");
        const QRect tall(0, 0, 20, 80);
        vertical.calculate_char_positions(tall);
        QCOMPARE(vertical.get_char_index_at_pos(999, tall, 42), 2);
    }

    void prepareOcrItemsSkipsFailuresAndBadEntries() {
        auto [items, union_rect] = OCRTextLayer::prepare_ocr_items(ocr_result({{box(0, 0, 50, 10), "hello", 0.9}, {box(0, 20, 60, 10), "world", 0.8}}));
        QCOMPARE(items.size(), size_t(2));
        QCOMPARE(items[1].text, QString("world"));
        QCOMPARE(*union_rect, QRectF(0, 0, 60, 30));
        std::tie(items, union_rect) = OCRTextLayer::prepare_ocr_items(ocr_result({{box(10, 10, 20, 5), "a", 1}, {box(100, 50, 30, 5), "b", 1}}));
        QCOMPARE(*union_rect, QRectF(10, 10, 120, 45));
        for (const auto &result : {OcrResult{}, ocr_result({{box(0, 0, 1, 1), "x", 1}}, 101), ocr_result({}), ocr_result({{box(0, 0, 10, 10), "", 0.9}, {{}, "ok", 0.9}})}) {
            std::tie(items, union_rect) = OCRTextLayer::prepare_ocr_items(result);
            QVERIFY(items.empty());
            QVERIFY(!union_rect);
        }
        std::tie(items, union_rect) = OCRTextLayer::prepare_ocr_items(ocr_result({{box(0, 0, 10, 10), "", 0.9}, {{}, "ok", 0.9}, {box(0, 0, 10, 10), "good", 0.9}}));
        QCOMPARE(items.size(), size_t(1));
        QCOMPARE(items[0].text, QString("good"));
    }


    // A recognition result replaces the layer's text; an empty one clears it.
    void aRecognitionResultLoadsIntoTheLayer() {
        OCRTextLayer layer(nullptr, 200, 100);
        layer.load_ocr_result(ocr_result({{box(10, 10, 80, 20), "loaded", 0.9}, {box(10, 40, 80, 20), "second", 0.9}}), 200, 100);
        QCOMPARE(layer.text_items.size(), size_t(2));
        QVERIFY(layer.has_text());
        QCOMPARE(layer.get_all_text("\n"), QString("loaded\nsecond"));
        layer.load_ocr_result(ocr_result({}), 200, 100);
        QVERIFY(!layer.has_text());
        layer.close();
    }
    void selectedTextJoinsBlocksByLine() {
        OCRTextLayer layer(nullptr, 200, 100);
        const auto fill = [&](const QList<std::tuple<QString, int, int>> &specs) {
            layer.text_items.clear();
            for (const auto &[text, x, y] : specs) layer.text_items.push_back(text_item(text, x, y, text.size() * 10, 20));
        };
        const auto select = [&](int a, int b, int c, int d) {
            layer.selection_start = std::make_pair(a, b);
            layer.selection_end = std::make_pair(c, d);
            return layer.get_selected_text();
        };
        fill({{"hello world", 0, 0}});
        QCOMPARE(layer.get_selected_text(), QString());
        QCOMPARE(select(0, 0, 0, 5), QString("hello"));
        // Backwards selections read the same.
        QCOMPARE(select(0, 5, 0, 0), QString("hello"));
        QCOMPARE(select(0, 2, 0, 2), QString());
        fill({{"hello", 0, 0}, {"world", 60, 0}});
        QCOMPARE(select(0, 0, 1, 5), QString("hello world"));
        fill({{"hello", 0, 0}, {"world", 0, 60}});
        QCOMPARE(select(0, 0, 1, 5), QString("hello\nworld"));
        fill({{"abcdef", 0, 0}, {"ghijkl", 0, 60}});
        QCOMPARE(select(0, 3, 1, 2), QString("def\ngh"));
        fill({{"aa", 0, 0}, {"bb", 0, 60}, {"cc", 0, 120}});
        QCOMPARE(select(0, 1, 2, 1), QString("a\nbb\nc"));
        fill({{"abc", 0, 0}, {"def", 0, 60}});
        QCOMPARE(select(1, 2, 0, 1), QString("bc\nde"));
        // A selection left over from an older result stops cleanly.
        fill({{"hello", 0, 0}});
        QCOMPARE(select(0, 0, 5, 3), QString("hello"));
        layer.clear_selection();
        QVERIFY(!layer.selection_start && !layer.selection_end && !layer.is_selecting);
        QCOMPARE(layer.get_selected_text(), QString());
        // All text keeps reading order: same-line blocks with spaces.
        fill({{"one", 0, 0}, {"two", 40, 2}, {"three", 0, 60}});
        QCOMPARE(layer.get_all_text("\n"), QString("one two\nthree"));
        QCOMPARE(layer.get_all_text(" | "), QString("one two | three"));
    }

    void layerScalesAndFollowsImageTransforms() {
        OCRTextLayer layer(nullptr, 200, 100);
        QVERIFY(!layer.has_text());
        auto [items, union_rect] = OCRTextLayer::prepare_ocr_items(ocr_result({{box(0, 0, 20, 10), "hi", 1}}));
        layer.load_prepared_ocr_items(std::move(items), union_rect, 200, 100);
        QVERIFY(layer.has_text());
        layer.resize(400, 200);
        QVERIFY(layer.get_scale_factors() == std::make_pair(2.0, 2.0));
        // A rotated pin maps every block and swaps the reference size.
        layer.selection_start = std::make_pair(0, 0);
        layer.selection_end = std::make_pair(0, 2);
        const auto turned = transform_state(90, false, false);
        layer.set_image_transform(&turned);
        QCOMPARE(layer.original_width, 100);
        QCOMPARE(layer.original_height, 200);
        QCOMPARE(layer.text_items[0].norm_rect, turned.map_ocr_rect(QRectF(0, 0, 20, 10), 200, 100));
        QVERIFY(!layer.selection_start);
        const PinImageTransform none;
        layer.set_image_transform(&none);
        QCOMPARE(layer.text_items[0].norm_rect, QRectF(0, 0, 20, 10));
        QCOMPARE(layer.original_width, 200);
        layer.original_width = 0;
        layer.original_height = 0;
        QVERIFY(layer.get_scale_factors() == std::make_pair(1.0, 1.0));
    }

    void draggingOverTextSelectsAcrossBlocks() {
        auto *pin = make_pin(QSize(200, 120), QPoint(100, 100));
        auto *layer = give_text(pin, {{box(10, 10, 100, 20), "abcdefghij", 0.9}, {box(10, 50, 100, 20), "klmnopqrst", 0.9}});
        QVERIFY(layer->isVisible());
        QCursor::setPos(0, 0);
        send_layer_mouse(layer, QEvent::MouseMove, QPoint(30, 20), Qt::NoButton, Qt::NoButton);
        QCOMPARE(layer->cursor().shape(), Qt::IBeamCursor);
        send_layer_mouse(layer, QEvent::MouseButtonPress, QPoint(30, 20), Qt::LeftButton, Qt::LeftButton);
        QVERIFY(layer->is_selecting);
        QVERIFY(!pin->is_dragging());
        send_layer_mouse(layer, QEvent::MouseMove, QPoint(60, 60), Qt::NoButton, Qt::LeftButton);
        send_layer_mouse(layer, QEvent::MouseButtonRelease, QPoint(60, 60), Qt::LeftButton, Qt::NoButton);
        QVERIFY(!layer->is_selecting);
        QCOMPARE(layer->get_selected_text(), QString("cdefghij\nklmno"));
        // The selection is painted from the start block's caret to the end block's.
        QVERIFY(!layer->grab().isNull());
        // Dragging backwards selects the same span.
        send_layer_mouse(layer, QEvent::MouseButtonPress, QPoint(60, 60), Qt::LeftButton, Qt::LeftButton);
        send_layer_mouse(layer, QEvent::MouseMove, QPoint(30, 20), Qt::NoButton, Qt::LeftButton);
        send_layer_mouse(layer, QEvent::MouseButtonRelease, QPoint(30, 20), Qt::LeftButton, Qt::NoButton);
        QVERIFY(!layer->grab().isNull());
        // Ctrl+A selects everything, Ctrl+C copies it; Esc clears a selection first.
        QTest::keyClick(layer, Qt::Key_A, Qt::ControlModifier);
        QVERIFY(!layer->grab().isNull());
        QTest::keyClick(layer, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard_text(), QString("abcdefghij\nklmnopqrst"));
        QTest::keyClick(layer, Qt::Key_Escape);
        QVERIFY(!layer->selection_start);
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(layer, &escape);
        QVERIFY(!escape.isAccepted());
        QKeyEvent other(QEvent::KeyPress, Qt::Key_B, Qt::NoModifier, "b");
        QApplication::sendEvent(layer, &other);
        QVERIFY(!other.isAccepted());
        QTest::keyClick(layer, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(layer->get_selected_text(), QString("abcdefghij\nklmnopqrst"));
        // A click on empty space clears the selection and lets the pin drag.
        send_layer_mouse(layer, QEvent::MouseButtonPress, QPoint(180, 100), Qt::LeftButton, Qt::LeftButton);
        QVERIFY(!layer->selection_start);
        send_layer_mouse(layer, QEvent::MouseButtonRelease, QPoint(180, 100), Qt::LeftButton, Qt::NoButton);
        // Drawing turns the layer off.
        layer->set_drawing_mode(true);
        QVERIFY(!layer->isVisible() && !layer->is_active());
    }

    void doubleClickSelectsABlockUnlessBoundToAnAction() {
        auto *pin = make_pin();
        auto *layer = give_text(pin, {{box(10, 10, 100, 20), "hello", 0.9}});
        const auto double_click_layer = [&] {
            send_layer_mouse(layer, QEvent::MouseButtonPress, QPoint(40, 20), Qt::LeftButton, Qt::LeftButton);
            send_layer_mouse(layer, QEvent::MouseButtonRelease, QPoint(40, 20), Qt::LeftButton, Qt::NoButton);
            send_layer_mouse(layer, QEvent::MouseButtonDblClick, QPoint(40, 20), Qt::LeftButton, Qt::LeftButton);
            send_layer_mouse(layer, QEvent::MouseButtonRelease, QPoint(40, 20), Qt::LeftButton, Qt::NoButton);
        };
        QApplication::clipboard()->setText("before");
        double_click_layer();
        QCOMPARE(layer->get_selected_text(), QString("hello"));
        QCOMPARE(clipboard_text(), QString("before"));

        config_->set_app_setting("mouse_pin_close", "doubleleft");
        auto *bound = make_pin();
        layer = give_text(bound, {{box(10, 10, 100, 20), "hello", 0.9}});
        double_click_layer();
        QVERIFY(bound->is_closed());
        QCOMPARE(clipboard_text(), QString("before"));
    }

    // ---- Recognition on demand ----

    void textSelectionStartsOffWithoutAutomaticRecognition() {
        auto *pin = make_pin();
        QVERIFY(!pin->text_selection_enabled());
        QVERIFY(!pin->ocr_text_layer());
        QTest::qWait(350);
        // The automatic run after pinning follows the setting.
        QVERIFY(!pin->ocr_manager()->is_running());
        QVERIFY(!pin->ocr_text_layer());
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
            .arg(clipboard_text(), hint_text(pin)).arg(pin->ocr_has_result()).arg(pin->ocr_text_layer() ? pin->ocr_text_layer()->get_all_text() : "-")));
        QVERIFY(pin->ocr_has_result());
        QVERIFY(pin->text_selection_enabled());
        QCOMPARE(hint_text(pin), QString("Text copied"));
        // With a result it copies at once.
        QApplication::clipboard()->clear();
        pin->copy_all_text();
        QVERIFY(!pin->ocr_manager()->is_running());
        QVERIFY(clipboard_text().contains("2026"));
        // Turning text selection off hides the layer; on shows it again without recognizing.
        pin->toggle_text_selection();
        QVERIFY(!pin->ocr_text_layer()->isVisible());
        pin->toggle_text_selection();
        QVERIFY(pin->ocr_text_layer()->isVisible());
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

    // Saving asks for a file, starting in the save folder; without an extension the chosen filter decides the
    // format. Copying goes to the clipboard.
    void savingAndCopyingThePinImage() {
        QTemporaryDir out;
        QString opened_in;
        const auto save_folder = dir_.filePath("pin-saves");
        config_->set_screenshot_save_path(save_folder);
        auto *pin = make_pin(QSize(120, 80));
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
