#include "annotation/annotation_toolbar.h"
#include "annotation/annotation_view.h"
#include "annotation/cursor_highlight.h"
#include "annotation/screen_annotation.h"
#include "canvas/items/drawing_items.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include "canvas/smart_edit_controller.h"
#include "capture/capture_service.h"
#include "core/input_hub.h"
#include "gif/frame_recorder.h"
#include "gif/gifrecorder.h"
#include "gif/video_recorder.h"
#include "clipboard/controllers/paste_keystroke.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "ui/number_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/toolbar_layout.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileSystemWatcher>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRandomGenerator>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolTip>
#include <qpa/qplatformnativeinterface.h>
#include <memory>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <psapi.h>

using namespace jietuba;
using Mode = ScreenAnnotation::Mode;

namespace {
bool offscreen() { return QGuiApplication::platformName() == "offscreen"; }

const QList<QRect> TWO_SCREENS = {QRect(0, 0, 1280, 720), QRect(1280, 0, 1024, 768)};

QJsonObject offscreen_config(const QList<QRect> &screens) {
    QJsonArray list;
    for (int i = 0; i < screens.size(); ++i) {
        const auto &rect = screens[i];
        list.append(QJsonObject{{"name", QString("Screen%1").arg(i + 1)}, {"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()},
                                {"logicalDpi", 96}, {"logicalBaseDpi", 96}, {"dpr", 1.0}});
    }
    return QJsonObject{{"synchronousWindowSystemEvents", true}, {"windowFrameMargins", false}, {"screens", list}};
}

// The offscreen platform takes new screens at run time, as plugging or unplugging monitors does.
void set_offscreen_screens(const QList<QRect> &screens) {
    auto *native = QGuiApplication::platformNativeInterface();
    using SetConfiguration = void (*)(const QJsonObject &, QPlatformNativeInterface *);
    auto set = reinterpret_cast<SetConfiguration>(native->nativeResourceForIntegration("setConfiguration"));
    QVERIFY(set);
    set(offscreen_config(screens), native);
}

void send_mouse(QWidget *target, QEvent::Type type, const QPoint &local, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(local), QPointF(target->mapToGlobal(local)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

void draw_stroke(AnnotationView *view, const QPoint &from = QPoint(100, 100), const QPoint &to = QPoint(260, 180)) {
    auto *viewport = view->viewport();
    send_mouse(viewport, QEvent::MouseButtonPress, from, Qt::LeftButton);
    for (int i = 1; i <= 4; ++i) send_mouse(viewport, QEvent::MouseMove, from + (to - from) * i / 4, Qt::LeftButton);
    send_mouse(viewport, QEvent::MouseButtonRelease, to, Qt::NoButton);
}

// Counts how often a window is hidden and shown.
class VisibilityCounter : public QObject {
public:
    int hides = 0;
    int shows = 0;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Hide) ++hides;
        else if (event->type() == QEvent::Show) ++shows;
        return false;
    }
};

int drawing_count(AnnotationView *view) {
    int count = 0;
    auto *scene = view->scene_obj();
    for (auto *item : scene->items())
        if (!item->parentItem() && scene->is_drawing(item) && dynamic_cast<DrawingItemMixin *>(item)) ++count;
    return count;
}

HWND hwnd_of(QWidget *widget) { return reinterpret_cast<HWND>(widget->winId()); }

LONG ex_style(QWidget *widget) { return GetWindowLongW(hwnd_of(widget), GWL_EXSTYLE); }

// A real click: what Windows decides about activation is the point.
void real_click(const QPoint &global) {
    SetCursorPos(global.x(), global.y());
    INPUT inputs[2] = {};
    inputs[0].type = inputs[1].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// The test process may not own the foreground: attaching to the foreground thread lifts the lock.
bool bring_to_front(QWidget *widget) {
    const HWND hwnd = hwnd_of(widget);
    const DWORD this_thread = GetCurrentThreadId();
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const bool attached = foreground_thread && foreground_thread != this_thread && AttachThreadInput(this_thread, foreground_thread, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(this_thread, foreground_thread, FALSE);
    return QTest::qWaitFor([hwnd] { return GetForegroundWindow() == hwnd; }, 3000);
}

// Class, title and owner of a window's root, and the Qt class when it is one of ours.
QString window_class(HWND hwnd) {
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    wchar_t name[256] = {}, title[256] = {};
    GetClassNameW(root, name, 256);
    GetWindowTextW(root, title, 256);
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    auto *widget = QWidget::find(reinterpret_cast<WId>(root));
    return QString("%1 \"%2\" pid %3 %4").arg(QString::fromWCharArray(name), QString::fromWCharArray(title)).arg(pid).arg(widget ? widget->metaObject()->className() : "");
}

AnnotationView *view_on(ScreenAnnotation &annotation, QScreen *screen) {
    for (auto *view : annotation.views())
        if (view->target_screen() == screen) return view;
    return nullptr;
}

struct ProcessCounters {
    DWORD gdi = 0, user = 0;
    SIZE_T private_bytes = 0;
};

ProcessCounters process_counters() {
    ProcessCounters counters;
    counters.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    counters.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) counters.private_bytes = memory.PrivateUsage;
    return counters;
}

// A hub whose native side does nothing but remember what was watched, so the tests feed the events.
class StubHubNative : public InputHubNative {
public:
    QStringList mouse_watchers;
    std::optional<QPoint> pointer;
    std::optional<InputHubEvent> next_event() override { return std::nullopt; }
    void close() override {}
    bool configure_gestures(const std::vector<std::pair<QStringList, QString>> &, bool) override { return true; }
    void set_blocked(bool) override {}
    void cancel_gesture() override {}
    std::optional<QPoint> take_position(quint64) override { return std::nullopt; }
    bool dragging() override { return false; }
    bool accepts(quint64) override { return false; }
    std::optional<std::pair<QStringList, QString>> gesture_binding(quint64) override { return std::nullopt; }
    void configure_side_buttons(bool, const QStringList &, bool) override {}
    bool bind_hotkey(const QString &, const QStringList &, quint32) override { return true; }
    void unbind_hotkey(const QString &) override {}
    void configure_key_capture(bool) override {}
    void watch_wheel(const QString &, const std::optional<InputHubRect> &) override {}
    void unwatch_wheel(const QString &) override {}
    void watch_keys(const QString &, const std::vector<quint32> &) override {}
    void unwatch_keys(const QString &) override {}
    void watch_mouse(const QString &watcher) override {
        if (!mouse_watchers.contains(watcher)) mouse_watchers << watcher;
    }
    void unwatch_mouse(const QString &watcher) override { mouse_watchers.removeAll(watcher); }
    std::optional<QPoint> take_pointer(const QString &) override { return std::exchange(pointer, std::nullopt); }
    void watch_foreground(const QString &) override {}
    void unwatch_foreground(const QString &) override {}
    bool hooks_installed() override { return false; }
    void set_test_marker(quintptr) override {}
};

struct StubHub {
    StubHubNative *native = nullptr;
    std::unique_ptr<InputHub> hub;
    StubHub() {
        auto stub = std::make_unique<StubHubNative>();
        native = stub.get();
        hub = std::make_unique<InputHub>(std::move(stub));
    }
    // What the hook thread would send: the pointer moved to at, or a button pressed or released.
    void move_to(const QPoint &at) {
        native->pointer = at;
        InputHubEvent event;
        event.kind = InputHubEvent::Pointer;
        event.text = CursorHighlight::WATCHER;
        hub->dispatch(event);
    }
    void button(int button, bool pressed, const QPoint &at, const QString &watcher = CursorHighlight::WATCHER) {
        InputHubEvent event;
        event.kind = InputHubEvent::MouseButton;
        event.text = watcher;
        event.value = button;  // input::Button: 0 left, 1 middle, 2 right
        event.flag = pressed;
        event.x = at.x();
        event.y = at.y();
        hub->dispatch(event);
    }
};

QColor pixel_at(const QImage &image, const QRectF &image_rect, const QPoint &global) {
    return image.pixelColor(global - image_rect.topLeft().toPoint());
}

bool near_grey(const QColor &color, int level, int tolerance) {
    return std::abs(color.red() - level) <= tolerance && std::abs(color.green() - level) <= tolerance && std::abs(color.blue() - level) <= tolerance;
}

bool is_red(const QColor &color) { return color.red() > 180 && color.green() < 80 && color.blue() < 80; }

// A stand-in desktop: a white page with lines of black "text", a coloured bar and a dark window with
// light text, so a render shows what drawings do to each.
struct Backdrop {
    QImage image;
    QRect page, dark;
    // Rows of text on the page and in the dark window; each is a run of glyph-sized blocks.
    QList<QRect> page_lines, dark_lines;
};

Backdrop document_backdrop(const QSize &size) {
    Backdrop backdrop;
    backdrop.image = QImage(size, QImage::Format_ARGB32_Premultiplied);
    backdrop.image.fill(QColor(225, 228, 232));
    QPainter painter(&backdrop.image);
    backdrop.page = QRect(60, 40, 560, size.height() - 80);
    painter.fillRect(backdrop.page, Qt::white);
    const auto glyphs = [&painter](const QRect &line, const QColor &color) {
        for (int x = line.left(); x + 10 <= line.right(); x += 14) painter.fillRect(QRect(x, line.top(), 10, line.height()), color);
    };
    for (int i = 0; i < 12; ++i) {
        const QRect line(backdrop.page.left() + 30, backdrop.page.top() + 40 + i * 36, i % 4 == 3 ? 300 : 480, 14);
        backdrop.page_lines << line;
        glyphs(line, Qt::black);
    }
    painter.fillRect(QRect(backdrop.page.left() + 30, backdrop.page.bottom() - 120, 360, 40), QColor(40, 110, 220));
    backdrop.dark = QRect(660, 40, 560, 300);
    painter.fillRect(backdrop.dark, QColor(32, 33, 36));
    for (int i = 0; i < 5; ++i) {
        const QRect line(backdrop.dark.left() + 30, backdrop.dark.top() + 40 + i * 44, 460, 14);
        backdrop.dark_lines << line;
        glyphs(line, QColor(232, 234, 237));
    }
    return backdrop;
}

// The layer over the backdrop as the desktop compositor shows it: premultiplied source-over.
QImage composite(const Backdrop &backdrop, AnnotationView *view) {
    QImage out = backdrop.image;
    QPainter painter(&out);
    painter.drawImage(0, 0, view->viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied));
    painter.end();
    return out.convertToFormat(QImage::Format_RGB32);
}

void save_render(const QImage &image, const QString &name) {
    const QString dir = qEnvironmentVariable("JIETUBA_ANNOTATION_RENDER_DIR");
    if (!dir.isEmpty()) QVERIFY(image.save(QDir(dir).filePath(name + ".png")));
}

void drag(AnnotationView *view, const QList<QPoint> &points, int wait_ms = 0) {
    auto *viewport = view->viewport();
    send_mouse(viewport, QEvent::MouseButtonPress, points.front(), Qt::LeftButton);
    for (const auto &point : points.mid(1)) {
        send_mouse(viewport, QEvent::MouseMove, point, Qt::LeftButton);
        if (wait_ms) QTest::qWait(wait_ms);
    }
    send_mouse(viewport, QEvent::MouseButtonRelease, points.back(), Qt::NoButton);
}

QList<QPoint> line_points(const QPoint &from, const QPoint &to, int steps) {
    QList<QPoint> points;
    for (int i = 0; i <= steps; ++i) points << from + (to - from) * i / steps;
    return points;
}

// The last frame of a video file, decoded by Media Foundation's source reader.
QImage last_video_frame(const QString &path) {
    QImage frame;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return frame;
    IMFAttributes *attributes = nullptr;
    IMFSourceReader *reader = nullptr;
    IMFMediaType *type = nullptr;
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    if (SUCCEEDED(MFCreateAttributes(&attributes, 1)) && SUCCEEDED(attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) &&
        SUCCEEDED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(path.utf16()), attributes, &reader)) && SUCCEEDED(MFCreateMediaType(&type)) &&
        SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) && SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) &&
        SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, type))) {
        IMFMediaType *current = nullptr;
        UINT32 width = 0, height = 0, default_stride = 0;
        if (SUCCEEDED(reader->GetCurrentMediaType(stream, &current))) {
            MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &width, &height);
            current->GetUINT32(MF_MT_DEFAULT_STRIDE, &default_stride);
            current->Release();
        }
        for (;;) {
            DWORD flags = 0;
            IMFSample *sample = nullptr;
            const bool read = SUCCEEDED(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample));
            if (sample) {
                IMFMediaBuffer *buffer = nullptr;
                BYTE *data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    // A negative default stride means the rows are stored bottom-up.
                    const qsizetype pitch = height ? static_cast<qsizetype>(length / height) : 0;
                    if (pitch >= static_cast<qsizetype>(width) * 4) {
                        const bool bottom_up = static_cast<INT32>(default_stride) < 0;
                        frame = QImage(static_cast<int>(width), static_cast<int>(height), QImage::Format_RGB32);
                        for (int y = 0; y < frame.height(); ++y)
                            memcpy(frame.scanLine(bottom_up ? frame.height() - 1 - y : y), data + y * pitch, width * 4);
                    }
                    buffer->Unlock();
                }
                if (buffer) buffer->Release();
                sample->Release();
            }
            if (!read || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        }
    }
    if (type) type->Release();
    if (reader) reader->Release();
    if (attributes) attributes->Release();
    MFShutdown();
    return frame;
}

}

class ScreenAnnotationTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir settings_dir_;
    QSettings *settings_ = nullptr;
    std::unique_ptr<ScreenAnnotation> annotation_;

    std::unique_ptr<StubHub> stub_;

    ScreenAnnotation &start() {
        annotation_ = std::make_unique<ScreenAnnotation>();
        stub_ = std::make_unique<StubHub>();
        annotation_->cursor_highlight()->set_hub(stub_->hub.get());
        annotation_->start();
        return *annotation_;
    }

    // Mode, layers, buttons and undo history agree after every operation.
    void check_consistent(ScreenAnnotation &annotation) {
        auto *toolbar = annotation.toolbar();
        QVERIFY(toolbar);
        const auto mode = annotation.mode();
        QCOMPARE(toolbar->pointer_btn->isChecked(), mode == Mode::PassThrough);
        QCOMPARE(toolbar->hide_btn->isChecked(), mode == Mode::Hidden);
        QCOMPARE(toolbar->current_tool.has_value(), mode == Mode::Drawing);
        bool any_undo = false, any_redo = false;
        for (auto *view : annotation.views()) {
            QCOMPARE(view->isVisible(), mode != Mode::Hidden);
            QCOMPARE(view->passthrough(), mode != Mode::Drawing);
            QCOMPARE(view->scene_obj()->hit_rect->isVisible(), mode == Mode::Drawing);
            QCOMPARE(view->tool_controller()->current_tool_id(), mode == Mode::Drawing ? *toolbar->current_tool : QString("cursor"));
            any_undo = any_undo || view->undo_stack()->canUndo();
            any_redo = any_redo || view->undo_stack()->canRedo();
        }
        // Every step on a layer is in the session's history.
        QCOMPARE(annotation.can_undo(), any_undo);
        if (annotation.can_redo()) QVERIFY(any_redo);
    }

private slots:
    void initTestCase() {
        QVERIFY(settings_dir_.isValid());
        // The settings singleton reads a throwaway file, never the registry.
        settings_ = new QSettings(settings_dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        QCOMPARE(&get_tool_settings_manager(settings_)->settings(), settings_);
        // Most cases end a session and start afresh; keeping the drawings (on by default) has its own case.
        get_tool_settings_manager()->set_app_setting("screen_annotation_keep_drawings", false);
        get_tool_settings_manager()->set_app_setting("screen_annotation_toolbar_position", "bottom");
        if (offscreen()) QCOMPARE(QGuiApplication::screens().size(), 2);
    }

    void cleanup() {
        annotation_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (offscreen() && QGuiApplication::screens().size() != 2) set_offscreen_screens(TWO_SCREENS);
    }

    void startShowsLayerPerScreenAndToolbar() {
        auto &annotation = start();
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        QCOMPARE(static_cast<qsizetype>(annotation.views().size()), QGuiApplication::screens().size());
        for (auto *view : annotation.views()) {
            QVERIFY(view->isVisible());
            QVERIFY(view->passthrough());
            QCOMPARE(view->geometry(), view->target_screen()->geometry());
            QVERIFY(!view->scene_obj()->hit_rect->isVisible());
        }
        auto *toolbar = annotation.toolbar();
        QVERIFY(toolbar->isVisible());
        QVERIFY(toolbar->pointer_btn->isChecked());
        auto *screen = QGuiApplication::screenAt(toolbar->geometry().center());
        QVERIFY(screen);
        QVERIFY(screen->availableGeometry().contains(toolbar->geometry()));
    }

    void stopRemovesEveryWindow() {
        auto &annotation = start();
        annotation.select_tool("pen");
        draw_stroke(annotation.views().front());
        QList<QPointer<QWidget>> windows;
        for (auto *view : annotation.views()) windows.append(view);
        windows.append(annotation.toolbar());
        for (auto *window : annotation.toolbar()->floating_windows()) windows.append(window);
        annotation.stop();
        QCOMPARE(annotation.mode(), Mode::Off);
        QVERIFY(!annotation.toolbar());
        QVERIFY(annotation.views().empty());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        for (const auto &window : windows) QVERIFY(window.isNull());
        // A new session starts empty.
        annotation.start();
        for (auto *view : annotation.views()) QCOMPARE(drawing_count(view), 0);
        QVERIFY(!annotation.can_undo());
    }

    void keptDrawingsComeBackNextSession() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("screen_annotation_keep_drawings", true);
        auto &annotation = start();
        annotation.select_tool("pen");
        auto *view = annotation.views().front();
        draw_stroke(view);
        const auto layers = annotation.views();
        annotation.stop();
        QCOMPARE(annotation.mode(), Mode::Off);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(annotation.views(), layers);
        for (auto *layer : layers) QVERIFY(!layer->isVisible());
        annotation.start();
        QCOMPARE(annotation.views(), layers);
        for (auto *layer : layers) QVERIFY(layer->isVisible());
        QCOMPARE(drawing_count(view), 1);
        // The undo step came along.
        annotation.undo();
        QCOMPARE(drawing_count(view), 0);
        annotation.redo();
        annotation.stop();
        // Turned off, the next session starts empty.
        config->set_app_setting("screen_annotation_keep_drawings", false);
        QPointer<AnnotationView> old = view;
        annotation.start();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(old.isNull());
        for (auto *layer : annotation.views()) QCOMPARE(drawing_count(layer), 0);
        QVERIFY(!annotation.can_undo());
    }

    void toolbarShowsAnnotationButtonsOnly() {
        auto *toolbar = start().toolbar();
        for (auto *button : {toolbar->pen_btn, toolbar->highlighter_btn, toolbar->arrow_btn, toolbar->number_btn, toolbar->rect_btn, toolbar->ellipse_btn,
                             toolbar->text_btn, toolbar->eraser_btn, toolbar->undo_btn, toolbar->redo_btn, toolbar->pointer_btn, toolbar->clear_btn,
                             toolbar->hide_btn, toolbar->exit_btn})
            QVERIFY(button->isVisible());
        for (auto *button : {toolbar->mosaic_btn, toolbar->spotlight_btn, toolbar->watermark_btn, toolbar->save_btn, toolbar->copy_btn, toolbar->confirm_btn,
                             toolbar->cancel_btn, toolbar->pin_btn, toolbar->gif_btn, toolbar->long_screenshot_btn, toolbar->text_recognize_btn,
                             toolbar->scan_code_btn, toolbar->screenshot_translate_btn})
            QVERIFY(!button->isVisible());
        // Its own layout setting; leaving cannot be hidden.
        toolbar_layout::save_layout(*get_tool_settings_manager(), QJsonArray{QJsonArray{"annotation_exit", "hide"}, QJsonArray{"pen", "hide"}}, toolbar_layout::ANNOTATION);
        toolbar->reload_layout();
        QVERIFY(toolbar->exit_btn->isVisible());
        QVERIFY(!toolbar->pen_btn->isVisible());
        QCOMPARE(toolbar_layout::load_layout(*get_tool_settings_manager(), toolbar_layout::PIN), toolbar_layout::default_layout(toolbar_layout::PIN));
        get_tool_settings_manager()->set_app_setting("annotation_toolbar_layout", "");
    }

    void toolSelectionDrawsOnEveryScreen() {
        auto &annotation = start();
        auto *toolbar = annotation.toolbar();
        QSignalSpy modes(&annotation, &ScreenAnnotation::mode_changed);
        annotation.select_tool("pen");
        QCOMPARE(annotation.mode(), Mode::Drawing);
        QCOMPARE(modes.size(), 1);
        for (auto *view : annotation.views()) {
            QVERIFY(!view->passthrough());
            QCOMPARE(view->tool_controller()->current_tool_id(), QString("pen"));
            QVERIFY(view->scene_obj()->hit_rect->isVisible());
        }
        QVERIFY(toolbar->pen_btn->isChecked());
        QVERIFY(!toolbar->pointer_btn->isChecked());
        // The same tool again returns to pass-through, as on the capture toolbar.
        toolbar->pen_btn->click();
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        for (auto *view : annotation.views()) QVERIFY(view->passthrough());
        QVERIFY(toolbar->pointer_btn->isChecked());
        // Tools that need a captured image are not offered.
        annotation.select_tool("mosaic");
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        toolbar->rect_btn->click();
        QCOMPARE(annotation.mode(), Mode::Drawing);
        toolbar->pointer_btn->click();
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        QVERIFY(toolbar->pointer_btn->isChecked());
        for (auto *view : annotation.views()) QCOMPARE(view->tool_controller()->current_tool_id(), QString("cursor"));
    }

    // While drawing a layer is see-through except for what is drawn; only the alpha 1 hit layer covers it.
    void layersStayTransparentWhileDrawing() {
        get_tool_settings_manager()->update_settings("pen", {{"color", "#ff0000"}, {"stroke_width", 8}, {"opacity", 1.0}});
        auto &annotation = start();
        annotation.select_tool("pen");
        auto *view = annotation.views().front();
        draw_stroke(view, QPoint(100, 100), QPoint(300, 100));
        const QImage image = view->viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        const QColor stroke = image.pixelColor(200, 100);
        QVERIFY2(stroke.alpha() == 255 && stroke.red() > 200 && stroke.green() < 60, qPrintable(stroke.name(QColor::HexArgb)));
        const QColor elsewhere = image.pixelColor(500, 400);
        QVERIFY2(elsewhere.alpha() <= 1, qPrintable(elsewhere.name(QColor::HexArgb)));
        get_tool_settings_manager()->reset_tool("pen");
    }

    // The highlighter over a page and a dark window, as the screen shows it: with nothing in the layer
    // to multiply onto, it tints at a fixed translucency, one stroke one tint, and the text stays readable.
    void highlighterOverDocument() {
        get_tool_settings_manager()->reset_tool("highlighter");
        auto &annotation = start();
        auto *view = annotation.views().front();
        const Backdrop backdrop = document_backdrop(view->size());
        annotation.select_tool("highlighter");
        const QRect line = backdrop.page_lines[1];
        drag(view, line_points(QPoint(line.left() - 4, line.center().y()), QPoint(line.right(), line.center().y()), 24));
        // Back over part of the same stroke: one stroke stays one tint.
        const QRect next = backdrop.page_lines[2];
        drag(view, {QPoint(next.left(), next.center().y()), QPoint(next.center().x(), next.center().y()), QPoint(next.left() + 40, next.center().y() + 2)});
        const QRect dark = backdrop.dark_lines[1];
        drag(view, line_points(QPoint(dark.left(), dark.center().y()), QPoint(dark.right(), dark.center().y()), 24));
        get_tool_settings_manager()->update_settings("highlighter", {{"draw_mode", "rect"}});
        annotation.select_tool("pen");
        annotation.select_tool("highlighter");
        const QRect boxed = backdrop.page_lines[5].adjusted(-6, -8, 6, 8);
        drag(view, line_points(boxed.topLeft(), boxed.bottomRight(), 8));
        get_tool_settings_manager()->reset_tool("highlighter");
        const QImage shown = composite(backdrop, view);
        save_render(shown, "highlighter");
        // Glyph blocks are 10 px wide every 14 px, so left + 14k + 11 is paper between two of them.
        const auto gap = [&shown](const QRect &row, int k) { return shown.pixelColor(row.left() + 14 * k + 11, row.center().y()); };
        const auto glyph = [&shown](const QRect &row, int k) { return shown.pixelColor(row.left() + 14 * k + 5, row.center().y()); };
        const auto same = [](const QColor &a, const QColor &b) {
            return std::abs(a.red() - b.red()) <= 2 && std::abs(a.green() - b.green()) <= 2 && std::abs(a.blue() - b.blue()) <= 2;
        };
        // White under 35% yellow: 255 * 0.65 blue left.
        const QColor paper = gap(line, 3), text = glyph(line, 3);
        QVERIFY2(paper.red() >= 250 && paper.green() >= 250 && std::abs(paper.blue() - 166) <= 4, qPrintable(paper.name()));
        QVERIFY2(text.red() <= 100 && text.green() <= 100 && text.blue() <= 10, qPrintable(text.name()));
        // Where the stroke turned back over itself it is no darker than where it went once.
        QVERIFY2(same(gap(next, 0), gap(next, 6)), qPrintable(gap(next, 0).name() + " " + gap(next, 6).name()));
        QVERIFY(same(gap(next, 0), paper));
        // Light text on the dark window stays apart from its background.
        const QColor dark_text = glyph(dark, 3), dark_paper = gap(dark, 3);
        QVERIFY2(dark_text.red() - dark_paper.red() > 100, qPrintable(dark_text.name() + " " + dark_paper.name()));
        // The rectangle is one even tint up to its rounded border, with no darker frame.
        const QRect row = backdrop.page_lines[5];
        const QColor inside = shown.pixelColor(boxed.left() + 2, boxed.top() + 2), border = shown.pixelColor(boxed.left() - 4, boxed.center().y());
        QVERIFY2(same(inside, paper) && same(border, paper) && same(gap(row, 4), paper), qPrintable(inside.name() + " " + border.name() + " " + gap(row, 4).name()));
        // Beyond it the page, but for the hit layer's one level.
        QVERIFY(same(shown.pixelColor(boxed.left() - 12, boxed.center().y()), Qt::white));
    }

    // A shape just drawn is not selected: its handles would show on the screen and in recordings. A new
    // text is, to type into.
    void newShapesAreNotSelected() {
        auto &annotation = start();
        auto *view = annotation.views().front();
        // Apart, so no press lands on the shape before, which would select that one to edit.
        int y = 100;
        for (const char *tool : {"rect", "ellipse", "arrow"}) {
            annotation.select_tool(tool);
            draw_stroke(view, QPoint(100, y), QPoint(260, y + 60));
            QVERIFY2(!view->smart_edit_controller->selected_item(), tool);
            y += 150;
        }
        QCOMPARE(drawing_count(view), 3);
        annotation.select_tool("text");
        send_mouse(view->viewport(), QEvent::MouseButtonPress, QPoint(400, 400), Qt::LeftButton);
        send_mouse(view->viewport(), QEvent::MouseButtonRelease, QPoint(400, 400), Qt::NoButton);
        QVERIFY(dynamic_cast<QGraphicsTextItem *>(view->smart_edit_controller->selected_item()));
    }

    void panelStaysShownWhileNumbersAreSelectedAndDeleted() {
        auto &annotation = start();
        auto *view = annotation.views().front();
        annotation.select_tool("number");
        auto *panel = annotation.toolbar()->number_panel;
        QVERIFY(panel && panel->isVisible());
        int x = 200;
        for (int i = 0; i < 3; ++i, x += 200) {
            send_mouse(view->viewport(), QEvent::MouseButtonPress, QPoint(x, 200), Qt::LeftButton);
            send_mouse(view->viewport(), QEvent::MouseButtonRelease, QPoint(x, 200), Qt::NoButton);
        }
        QList<NumberItem *> numbers;
        for (auto *item : view->scene_obj()->items())
            if (auto *number = dynamic_cast<NumberItem *>(item)) numbers.append(number);
        QCOMPARE(numbers.size(), 3);
        // A separate top-level window: hiding and showing it again for each change flickers.
        VisibilityCounter counter;
        panel->installEventFilter(&counter);
        auto *controller = view->smart_edit_controller.get();
        controller->select_item(numbers[0]);
        QVERIFY(panel->isVisible());
        controller->delete_selected(true, true);
        QVERIFY(panel->isVisible());
        controller->select_item(numbers[1]);
        controller->delete_selected(true, true);
        QVERIFY(panel->isVisible());
        QCOMPARE(drawing_count(view), 1);
        QCOMPARE(counter.hides, 0);
        QCOMPARE(counter.shows, 0);
    }

    void cursorHighlightComesBackNextSession() {
        auto &annotation = start();
        annotation.set_cursor_highlight(true);
        QVERIFY(annotation.toolbar()->highlight_btn->isChecked());
        annotation.stop();
        annotation.start();
        QVERIFY(annotation.cursor_highlight_enabled());
        QVERIFY(annotation.toolbar()->highlight_btn->isChecked());
        annotation.set_cursor_highlight(false);
        annotation.stop();
        annotation.start();
        QVERIFY(!annotation.cursor_highlight_enabled());
    }

    // A session starts in pass-through, with the pen, or with the tool the last one used.
    void sessionStartsWithTheChosenTool() {
        auto *config = get_tool_settings_manager();
        auto &annotation = start();
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        annotation.select_tool("arrow");
        QCOMPARE(config->get_app_setting("screen_annotation_last_tool").toString(), QString("arrow"));
        annotation.stop();
        config->set_app_setting("screen_annotation_start_tool", "pen");
        annotation.start();
        QCOMPARE(annotation.mode(), Mode::Drawing);
        QCOMPARE(annotation.toolbar()->current_tool.value_or(QString()), QString("pen"));
        annotation.select_tool("rect");
        annotation.stop();
        config->set_app_setting("screen_annotation_start_tool", "last");
        annotation.start();
        QCOMPARE(annotation.toolbar()->current_tool.value_or(QString()), QString("rect"));
        annotation.stop();
        // A stored tool the layers do not have leaves the session in pass-through.
        config->set_app_setting("screen_annotation_last_tool", "mosaic");
        annotation.start();
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        annotation.stop();
        config->set_app_setting("screen_annotation_start_tool", "cursor");
        config->set_app_setting("screen_annotation_last_tool", "pen");
    }

    // The toolbar starts at the bottom centre, or where the last session left it.
    void toolbarStartsWhereItWasLeft() {
        auto *config = get_tool_settings_manager();
        auto &annotation = start();
        const QPoint home = annotation.toolbar()->pos();
        const QPoint moved = home - QPoint(200, 300);
        annotation.toolbar()->move(moved);
        annotation.stop();
        QCOMPARE(config->get_app_setting("screen_annotation_toolbar_pos").toString(), QString("%1,%2").arg(moved.x()).arg(moved.y()));
        config->set_app_setting("screen_annotation_toolbar_position", "last");
        annotation.start();
        QCOMPARE(annotation.toolbar()->pos(), moved);
        annotation.stop();
        config->set_app_setting("screen_annotation_toolbar_position", "bottom");
        annotation.start();
        QCOMPARE(annotation.toolbar()->pos(), home);
        annotation.stop();
        config->set_app_setting("screen_annotation_toolbar_position", "last");
        // A place no screen has any more is brought back onto one.
        config->set_app_setting("screen_annotation_toolbar_pos", "-5000,-5000");
        annotation.start();
        QVERIFY(QGuiApplication::screenAt(annotation.toolbar()->geometry().center()));
        annotation.stop();
        config->set_app_setting("screen_annotation_toolbar_position", "bottom");
    }

    // The light's size and whether clicks leave rings come from the settings when it is turned on.
    void cursorHighlightSizeAndRingsFromSettings() {
        auto *config = get_tool_settings_manager();
        config->set_app_setting("screen_annotation_highlight_size", "large");
        config->set_app_setting("screen_annotation_click_ripples", false);
        auto &annotation = start();
        annotation.set_cursor_highlight(true, false);
        auto *view = annotation.views().front();
        const QPoint at = view->geometry().topLeft() + QPoint(300, 300);
        annotation.cursor_highlight()->on_pointer(at);
        auto *light = view->pointer_light(false);
        QVERIFY(light);
        QCOMPARE(light->disc_radius(), PointerLight::DISC_RADIUS * 1.4);
        annotation.cursor_highlight()->on_button("left", true, at);
        QCOMPARE(light->ripple_count(), 0);
        QVERIFY(light->disc_pressed());
        annotation.cursor_highlight()->on_button("left", false, at);
        annotation.set_cursor_highlight(false, false);
        config->set_app_setting("screen_annotation_highlight_size", "small");
        config->set_app_setting("screen_annotation_click_ripples", true);
        annotation.set_cursor_highlight(true, false);
        annotation.cursor_highlight()->on_pointer(at);
        light = view->pointer_light(false);
        QCOMPARE(light->disc_radius(), PointerLight::DISC_RADIUS * 0.7);
        annotation.cursor_highlight()->on_button("left", true, at);
        QCOMPARE(light->ripple_count(), 1);
        annotation.set_cursor_highlight(false, false);
        config->set_app_setting("screen_annotation_highlight_size", "medium");
    }

    // The whiteboard is white under the drawings and goes on and off with one click each. Pass-through
    // shows the desktop and the next tool brings the board back; clearing the drawings leaves it.
    void boardTogglesWithOneClick() {
        get_tool_settings_manager()->update_settings("pen", {{"color", "#ff0000"}, {"stroke_width", 8}, {"opacity", 1.0}});
        auto &annotation = start();
        auto *view = annotation.views().front();
        auto *button = annotation.toolbar()->board_btn;
        const auto pixel = [view](const QPoint &at) {
            return view->viewport()->grab().toImage().convertToFormat(QImage::Format_ARGB32).pixelColor(at);
        };
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        button->click();
        // A board needs a tool: the layers take the mouse.
        QCOMPARE(annotation.mode(), Mode::Drawing);
        QVERIFY(annotation.board_on());
        QVERIFY(button->isChecked());
        QCOMPARE(pixel(QPoint(500, 400)), QColor(Qt::white));
        draw_stroke(view, QPoint(100, 100), QPoint(300, 100));
        const QColor stroke = pixel(QPoint(200, 100));
        QVERIFY2(stroke.red() > 200 && stroke.green() < 60, qPrintable(stroke.name()));
        annotation.clear_drawings();
        QCOMPARE(pixel(QPoint(500, 400)), QColor(Qt::white));
        // Esc: the desktop again; the board waits for the next tool.
        annotation.select_tool("cursor");
        QVERIFY(!view->board().has_value());
        QVERIFY(button->isChecked());
        QVERIFY2(pixel(QPoint(500, 400)).alpha() <= 1, "the layer must be see-through in pass-through");
        annotation.select_tool("pen");
        QCOMPARE(pixel(QPoint(500, 400)), QColor(Qt::white));
        // One click takes it away, and the layer is see-through again while drawing goes on.
        button->click();
        QVERIFY(!annotation.board_on());
        QVERIFY(!button->isChecked());
        QVERIFY(!view->board().has_value());
        QCOMPARE(annotation.mode(), Mode::Drawing);
        QVERIFY(pixel(QPoint(500, 400)).alpha() <= 1);
        // Ending the session starts the next one without a board.
        button->click();
        annotation.stop();
        annotation.start();
        QVERIFY(!annotation.board_on());
        annotation.stop();
        get_tool_settings_manager()->reset_tool("pen");
    }

    // The highlight lives on the hub's mouse events, so it works in pass-through: a disc around the
    // pointer, smaller while a button is held, and a ring for every press that is gone after a moment.
    void cursorHighlightShowsDiscAndRipples() {
        auto &annotation = start();
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        annotation.set_cursor_highlight(true, false);
        auto *highlight = annotation.cursor_highlight();
        QVERIFY(highlight && highlight->enabled());
        QCOMPARE(stub_->native->mouse_watchers, QStringList{CursorHighlight::WATCHER});
        auto *view = annotation.views().front();
        const QPoint at = view->geometry().topLeft() + QPoint(400, 300);
        stub_->move_to(at);
        // Events cross threads queued; here they are delivered with the event loop.
        // It is already placed where the pointer was when it started.
        auto *light = view->pointer_light(false);
        QVERIFY(light && light->disc_visible());
        QTRY_COMPARE(light->disc_position(), QPointF(400, 300));
        QCOMPARE(light->ripple_count(), 0);
        QVERIFY(!light->disc_pressed());
        stub_->button(0, true, at);
        QTRY_COMPARE(light->ripple_count(), 1);
        QVERIFY(light->disc_pressed());
        // The right button adds its own ring; the disc stays small while any button is held.
        stub_->button(2, true, at);
        QTRY_COMPARE(light->ripple_count(), 2);
        stub_->button(0, false, at);
        QTRY_VERIFY(light->disc_pressed());
        stub_->button(2, false, at);
        QTRY_VERIFY(!light->disc_pressed());
        // Other buttons and other watchers' events make no ring.
        stub_->button(1, true, at);
        stub_->button(0, true, at, "someone else");
        QTest::qWait(50);
        QCOMPARE(light->ripple_count(), 2);
        stub_->button(1, false, at);
        // The rings run out by themselves, on a timer that stops with the last one.
        QTRY_COMPARE_WITH_TIMEOUT(light->ripple_count(), 0, 3000);
        // Not an annotation.
        QCOMPARE(drawing_count(view), 0);
        QVERIFY(!view->has_drawings());
        QVERIFY(!annotation.can_undo());
        // Off the screen the disc goes.
        stub_->move_to(view->geometry().bottomRight() + QPoint(50, 50));
        QTRY_VERIFY(!light->disc_visible());
        // Turning it off stops listening and takes the light off every layer.
        annotation.set_cursor_highlight(false, false);
        QVERIFY(stub_->native->mouse_watchers.isEmpty());
        QVERIFY(!view->pointer_light(false));
        QVERIFY(!annotation.toolbar()->highlight_btn->isChecked());
        stub_->button(0, true, at);
        QTest::qWait(50);
        QVERIFY2(!view->pointer_light(false), "a disabled highlight hears nothing");
    }

    // While a layer takes the mouse it moves the disc itself, in the repaint of the stroke; the hook's
    // position, a little ahead of the layer's own events, must not pull the disc away.
    void highlightDiscMovesWithTheMouseWhileDrawing() {
        auto &annotation = start();
        annotation.select_tool("pen");
        annotation.set_cursor_highlight(true, false);
        auto *view = annotation.views().front();
        send_mouse(view->viewport(), QEvent::MouseMove, QPoint(321, 234), Qt::NoButton);
        auto *light = view->pointer_light(false);
        QVERIFY(light && light->disc_visible());
        QCOMPARE(light->disc_position(), QPointF(321, 234));
        stub_->move_to(view->geometry().topLeft() + QPoint(500, 400));
        QTest::qWait(50);
        QCOMPARE(light->disc_position(), QPointF(321, 234));
        draw_stroke(view, QPoint(100, 100), QPoint(260, 180));
        QCOMPARE(light->disc_position(), QPointF(260, 180));
        // A press and release the hook reports change only the size.
        stub_->button(0, true, view->geometry().topLeft() + QPoint(260, 180));
        QTRY_VERIFY(light->disc_pressed());
        QCOMPARE(light->disc_position(), QPointF(260, 180));
        stub_->button(0, false, view->geometry().topLeft() + QPoint(260, 180));
        QTRY_VERIFY(!light->disc_pressed());
        // Leaving the screen takes the disc off it.
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(view, &leave);
        QVERIFY(!light->disc_visible());
        annotation.set_cursor_highlight(false, false);
    }

    // The real hook: the pointer moved and clicked by input reaches the highlight in pass-through. The
    // click lands on a window of the test, so nothing else on the desktop gets it.
    void theHooksMouseEventsDriveTheHighlight() {
        if (offscreen()) QSKIP("needs the real hooks");
        QWidget target;
        target.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        target.setGeometry(500, 500, 400, 300);
        target.show();
        QVERIFY(QTest::qWaitForWindowExposed(&target));
        auto &annotation = start();
        annotation.cursor_highlight()->set_hub(nullptr);
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        annotation.set_cursor_highlight(true, false);
        auto *view = view_on(annotation, QGuiApplication::primaryScreen());
        QVERIFY(view);
        const auto move_to = [](int x, int y) {
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dx = static_cast<LONG>(x * 65535.0 / (GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1) + 0.5);
            input.mi.dy = static_cast<LONG>(y * 65535.0 / (GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1) + 0.5);
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            SendInput(1, &input, sizeof input);
        };
        const auto click = [](DWORD down, DWORD up) {
            INPUT inputs[2] = {};
            inputs[0].type = inputs[1].type = INPUT_MOUSE;
            inputs[0].mi.dwFlags = down;
            inputs[1].mi.dwFlags = up;
            SendInput(2, inputs, sizeof(INPUT));
        };
        move_to(650, 650);
        QTRY_VERIFY_WITH_TIMEOUT(view->pointer_light(false) && view->pointer_light(false)->disc_visible() && view->pointer_light(false)->disc_position() == QPointF(650, 650), 3000);
        auto *light = view->pointer_light(false);
        move_to(700, 620);
        QTRY_COMPARE_WITH_TIMEOUT(light->disc_position(), QPointF(700, 620), 3000);
        // A click shorter than a frame is not missed.
        click(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
        QTRY_VERIFY_WITH_TIMEOUT(light->ripple_count() >= 1, 3000);
        click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
        QTRY_VERIFY_WITH_TIMEOUT(light->ripple_count() >= 2, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(light->ripple_count(), 0, 3000);
        annotation.set_cursor_highlight(false, false);
        QVERIFY(!view->pointer_light(false));
    }

    // Another hook can keep the mouse events from ours: a live wallpaper does while the desktop has the
    // focus. The stub hub sends none here; the system's cursor location events still move the light.
    void cursorHighlightFollowsWithoutHookEvents() {
        if (offscreen()) QSKIP("needs the real cursor");
        auto &annotation = start();
        annotation.set_cursor_highlight(true, false);
        auto *view = view_on(annotation, QGuiApplication::primaryScreen());
        QVERIFY(view);
        SetCursorPos(640, 610);
        QTRY_VERIFY_WITH_TIMEOUT(view->pointer_light(false) && view->pointer_light(false)->disc_position() == QPointF(640, 610), 3000);
        SetCursorPos(900, 700);
        QTRY_COMPARE_WITH_TIMEOUT(view->pointer_light(false)->disc_position(), QPointF(900, 700), 3000);
        annotation.set_cursor_highlight(false, false);
    }

    // A screenshot leaves the light out; the choice stays for the next session.
    void cursorHighlightStepsAsideForACapture() {
        auto *config = get_tool_settings_manager();
        auto &annotation = start();
        annotation.set_cursor_highlight(true);
        QVERIFY(annotation.cursor_highlight_enabled());
        annotation.prepare_for_capture();
        QVERIFY(!annotation.cursor_highlight_enabled());
        QVERIFY(!annotation.views().front()->pointer_light(false));
        QVERIFY(config->get_app_setting("screen_annotation_cursor_highlight", false).toBool());
        annotation.set_cursor_highlight(false);
    }

    void undoFollowsDrawingOrderAcrossScreens() {
        auto &annotation = start();
        if (annotation.views().size() < 2) QSKIP("needs two screens");
        annotation.select_tool("pen");
        auto *a = annotation.views()[0], *b = annotation.views()[1];
        draw_stroke(a);
        draw_stroke(b);
        draw_stroke(a, QPoint(300, 300), QPoint(400, 320));
        QCOMPARE(drawing_count(a), 2);
        QCOMPARE(drawing_count(b), 1);
        annotation.undo();
        QCOMPARE(drawing_count(a), 1);
        QCOMPARE(drawing_count(b), 1);
        annotation.toolbar()->undo_btn->click();
        QCOMPARE(drawing_count(a), 1);
        QCOMPARE(drawing_count(b), 0);
        annotation.redo();
        QCOMPARE(drawing_count(b), 1);
        annotation.undo();
        annotation.undo();
        QCOMPARE(drawing_count(a), 0);
        QCOMPARE(drawing_count(b), 0);
        QVERIFY(!annotation.can_undo());
        annotation.toolbar()->redo_btn->click();
        QCOMPARE(drawing_count(a), 1);
        QCOMPARE(drawing_count(b), 0);
        // A new drawing drops what could be redone on every screen.
        draw_stroke(b);
        QVERIFY(!annotation.can_redo());
        annotation.undo();
        QCOMPARE(drawing_count(b), 0);
        annotation.undo();
        QCOMPARE(drawing_count(a), 0);
    }

    void clearIsOneUndoStep() {
        auto &annotation = start();
        annotation.select_tool("pen");
        for (auto *view : annotation.views()) draw_stroke(view);
        annotation.clear_drawings();
        for (auto *view : annotation.views()) QCOMPARE(drawing_count(view), 0);
        annotation.undo();
        for (auto *view : annotation.views()) QCOMPARE(drawing_count(view), 1);
        annotation.redo();
        for (auto *view : annotation.views()) QCOMPARE(drawing_count(view), 0);
        annotation.undo();
        annotation.undo();
        // The last screen's stroke was the last step before clearing.
        QCOMPARE(drawing_count(annotation.views().back()), 0);
        if (annotation.views().size() > 1) QCOMPARE(drawing_count(annotation.views().front()), 1);
        // Nothing to clear adds no step.
        annotation.undo();
        QVERIFY(!annotation.can_undo());
        annotation.clear_drawings();
        QVERIFY(!annotation.can_undo());
    }

    void hidingKeepsDrawings() {
        auto &annotation = start();
        auto *toolbar = annotation.toolbar();
        annotation.select_tool("pen");
        auto *view = annotation.views().front();
        draw_stroke(view);
        annotation.set_drawings_visible(false);
        QCOMPARE(annotation.mode(), Mode::Hidden);
        for (auto *layer : annotation.views()) QVERIFY(!layer->isVisible());
        QCOMPARE(drawing_count(view), 1);
        QVERIFY(toolbar->hide_btn->isChecked());
        QVERIFY(!toolbar->current_tool);
        toolbar->hide_btn->click();
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        for (auto *layer : annotation.views()) QVERIFY(layer->isVisible());
        // Picking a tool while hidden shows the drawings again.
        annotation.set_drawings_visible(false);
        annotation.select_tool("rect");
        QCOMPARE(annotation.mode(), Mode::Drawing);
        for (auto *layer : annotation.views()) QVERIFY(layer->isVisible());
        QCOMPARE(drawing_count(view), 1);
    }

    void prepareForCaptureLeavesDrawing() {
        auto &annotation = start();
        annotation.select_tool("pen");
        draw_stroke(annotation.views().front());
        annotation.prepare_for_capture();
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        for (auto *view : annotation.views()) {
            QVERIFY(view->passthrough());
            QVERIFY(!view->scene_obj()->hit_rect->isVisible());
        }
        QCOMPARE(drawing_count(annotation.views().front()), 1);
        // Hidden drawings stay hidden.
        annotation.set_drawings_visible(false);
        annotation.prepare_for_capture();
        QCOMPARE(annotation.mode(), Mode::Hidden);
    }

    void screensPluggedAndUnpluggedDuringSession() {
        if (!offscreen()) QSKIP("the offscreen platform changes its screens on request");
        auto &annotation = start();
        annotation.select_tool("pen");
        QCOMPARE(static_cast<int>(annotation.views().size()), 2);
        draw_stroke(annotation.views()[0]);
        draw_stroke(annotation.views()[1]);
        draw_stroke(annotation.views()[0], QPoint(300, 300), QPoint(400, 320));
        set_offscreen_screens({TWO_SCREENS[0]});
        QTRY_COMPARE(static_cast<int>(annotation.views().size()), 1);
        QCOMPARE(annotation.mode(), Mode::Drawing);
        // The unplugged screen's step goes with it.
        auto *left = annotation.views().front();
        annotation.undo();
        QCOMPARE(drawing_count(left), 1);
        annotation.undo();
        QCOMPARE(drawing_count(left), 0);
        QVERIFY(!annotation.can_undo());
        QVERIFY(QGuiApplication::screenAt(annotation.toolbar()->geometry().center()));
        // A screen plugged in joins with the current tool.
        set_offscreen_screens(TWO_SCREENS);
        QTRY_COMPARE(static_cast<int>(annotation.views().size()), 2);
        auto *added = annotation.views()[1];
        QVERIFY(added->isVisible());
        QVERIFY(!added->passthrough());
        QCOMPARE(added->tool_controller()->current_tool_id(), QString("pen"));
        QCOMPARE(added->geometry(), TWO_SCREENS[1]);
        // A resolution change resizes the layer and keeps its drawings.
        draw_stroke(left);
        set_offscreen_screens({QRect(0, 0, 1600, 900), TWO_SCREENS[1].translated(320, 0)});
        QTRY_COMPARE(left->geometry(), QRect(0, 0, 1600, 900));
        QCOMPARE(left->scene_obj()->sceneRect(), QRectF(0, 0, 1600, 900));
        QCOMPARE(drawing_count(left), 1);
        check_consistent(annotation);
    }

    void randomOperationsKeepStateConsistent() {
        auto &annotation = start();
        QRandomGenerator rng(93);
        const QStringList tools = {"pen", "highlighter", "arrow", "number", "rect", "ellipse", "eraser", "text", "cursor", "mosaic"};
        const QStringList drawable = {"pen", "highlighter", "arrow", "number", "rect", "ellipse", "eraser"};
        for (int step = 0; step < 100; ++step) {
            switch (rng.bounded(10)) {
            case 0:
            case 1:
            case 2: annotation.select_tool(tools[rng.bounded(static_cast<int>(tools.size()))]); break;
            case 3: annotation.set_drawings_visible(annotation.mode() == Mode::Hidden); break;
            case 4:
            case 5:
                if (annotation.mode() == Mode::Drawing && drawable.contains(*annotation.toolbar()->current_tool)) {
                    auto *view = annotation.views()[rng.bounded(static_cast<int>(annotation.views().size()))];
                    const QPoint from(40 + rng.bounded(400), 40 + rng.bounded(300));
                    draw_stroke(view, from, from + QPoint(rng.bounded(-30, 120), rng.bounded(-30, 120)));
                }
                break;
            case 6: rng.bounded(2) ? annotation.undo() : annotation.redo(); break;
            case 7: annotation.clear_drawings(); break;
            case 8: annotation.prepare_for_capture(); break;
            default:
                if (rng.bounded(3) == 0) {
                    annotation.stop();
                    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                    annotation.start();
                }
                break;
            }
            check_consistent(annotation);
            if (QTest::currentTestFailed()) qWarning("inconsistent after step %d", step);
            if (QTest::currentTestFailed()) return;
        }
    }

    // Extremes: the widest pen, a long text, the whole screen filled; clearing and undoing stay quick.
    void extremeInputStaysResponsive() {
        auto &annotation = start();
        auto *view = annotation.views().front();
        annotation.select_tool("pen");
        emit annotation.toolbar()->stroke_width_changed(10000);
        const double widest = view->tool_controller()->ctx.stroke_width;
        QVERIFY(widest >= 1 && widest < 10000);
        const QRect area = view->rect();
        QElapsedTimer clock;
        clock.start();
        for (int y = 20; y < area.height(); y += 40) draw_stroke(view, QPoint(10, y), QPoint(area.width() - 10, y + 5));
        annotation.select_tool("text");
        send_mouse(view->viewport(), QEvent::MouseButtonPress, QPoint(200, 200), Qt::LeftButton);
        send_mouse(view->viewport(), QEvent::MouseButtonRelease, QPoint(200, 200), Qt::NoButton);
        QTest::keyClicks(view->viewport(), QString(2000, QChar('x')));
        annotation.select_tool("cursor");
        const qint64 drawn = clock.elapsed();
        clock.start();
        annotation.clear_drawings();
        QCoreApplication::processEvents();
        annotation.undo();
        QCoreApplication::processEvents();
        annotation.redo();
        QCoreApplication::processEvents();
        const qint64 cleared = clock.elapsed();
        qInfo("filled in %lld ms, clear/undo/redo in %lld ms, width clamped to %.0f", static_cast<long long>(drawn), static_cast<long long>(cleared), widest);
        QCOMPARE(drawing_count(view), 0);
        QVERIFY(cleared < 2000);
        get_tool_settings_manager()->reset_tool("pen");
    }

    // A layer undone by itself (not through the session) drops those steps from the history, wherever
    // they sit between the other layer's steps, and they can be redone from the session.
    void aLayersOwnUndoLeavesTheSessionHistory() {
        auto &annotation = start();
        if (annotation.views().size() < 2) QSKIP("needs two screens");
        annotation.select_tool("pen");
        auto *a = annotation.views()[0], *b = annotation.views()[1];
        draw_stroke(a);
        draw_stroke(b);
        draw_stroke(a, QPoint(300, 300), QPoint(400, 320));
        a->undo_stack()->setIndex(0);
        QCOMPARE(drawing_count(a), 0);
        QCOMPARE(drawing_count(b), 1);
        // Only the other layer's step is left to undo; the layer's two are waiting to be redone.
        QVERIFY(annotation.can_redo());
        annotation.undo();
        QCOMPARE(drawing_count(b), 0);
        QVERIFY(!annotation.can_undo());
        for (int i = 0; i < 3; ++i) annotation.redo();
        QCOMPARE(drawing_count(a), 2);
        QCOMPARE(drawing_count(b), 1);
        QVERIFY(!annotation.can_redo());
    }

    // The session's keys: Esc ends drawing, undo, redo and delete act on the layers, the tool keys pick
    // tools, and none of them takes a key while a text is being edited.
    void sessionKeysDriveTheLayers() {
        auto &annotation = start();
        ScreenAnnotationShortcutHandler handler(&annotation);
        const auto press = [&](int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool repeat = false) {
            QKeyEvent event(QEvent::KeyPress, key, modifiers, QString(), repeat);
            return handler.handle_key(&event);
        };
        annotation.select_tool("pen");
        auto *view = annotation.views().front();
        draw_stroke(view);
        QCOMPARE(drawing_count(view), 1);
        QVERIFY(press(Qt::Key_Z, Qt::ControlModifier));
        QCOMPARE(drawing_count(view), 0);
        QVERIFY(press(Qt::Key_Y, Qt::ControlModifier));
        QCOMPARE(drawing_count(view), 1);
        // Delete removes the selected drawing.
        QGraphicsItem *drawn = nullptr;
        for (auto *item : view->scene_obj()->items())
            if (!item->parentItem() && view->scene_obj()->is_drawing(item) && dynamic_cast<DrawingItemMixin *>(item)) drawn = item;
        QVERIFY(drawn);
        view->smart_edit_controller->select_item(drawn);
        QVERIFY(press(Qt::Key_Delete));
        QCOMPARE(drawing_count(view), 0);
        // A tool key picks the tool, unless it is only the key repeating.
        QVERIFY(press(Qt::Key_R));
        QCOMPARE(annotation.toolbar()->current_tool.value_or(QString()), QString("rect"));
        QVERIFY(press(Qt::Key_O, Qt::NoModifier, true));
        QCOMPARE(annotation.toolbar()->current_tool.value_or(QString()), QString("rect"));
        QVERIFY(press(Qt::Key_O));
        QCOMPARE(annotation.toolbar()->current_tool.value_or(QString()), QString("ellipse"));
        QVERIFY(!press(Qt::Key_F5));
        // The mouse buttons have no binding in a session.
        QMouseEvent click(QEvent::MouseButtonPress, QPointF(5, 5), QPointF(5, 5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QVERIFY(!handler.handle_mouse(&click));
        // While a text is being edited the keys are the text's.
        annotation.select_tool("text");
        auto *text = new TextItem("", QPointF(150, 150), QFont(), Qt::red, false, true);
        view->scene()->addItem(text);
        // The scene gives its focus item the focus once its window is active; offscreen it never is.
        QEvent activate(QEvent::WindowActivate);
        QCoreApplication::sendEvent(view->scene(), &activate);
        QFocusEvent scene_focus(QEvent::FocusIn, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(view->scene(), &scene_focus);
        text->setFocus();
        QVERIFY(view->is_text_editing());
        QVERIFY(!press(Qt::Key_Z, Qt::ControlModifier));
        QVERIFY(!press(Qt::Key_R));
        QVERIFY(view->is_text_editing());
        QVERIFY(press(Qt::Key_Escape));
        QCOMPARE(annotation.mode(), ScreenAnnotation::Mode::PassThrough);
        QVERIFY(!view->is_text_editing());
    }

    // Before any layer is drawn on, the keys and the toolbar follow the layer on the screen under the
    // pointer, or the first one when the pointer is on none.
    void theActiveLayerFollowsThePointerBeforeAnyDrawing() {
        auto &annotation = start();
        QVERIFY(annotation.active_view());
        QVERIFY(std::find(annotation.views().begin(), annotation.views().end(), annotation.active_view()) != annotation.views().end());
        if (!offscreen()) return;
        QCursor::setPos(-9000, -9000);
        if (QGuiApplication::screenAt(QCursor::pos())) QSKIP("the platform keeps the pointer on a screen");
        QCOMPARE(annotation.active_view(), annotation.views().front());
        auto *second = annotation.views().back();
        const QPoint inside = second->target_screen()->geometry().center();
        QCursor::setPos(inside);
        QCOMPARE(annotation.active_view(), second);
    }

    // Switching to another program hands the mouse back; coming back changes nothing, and outside
    // drawing there is nothing to hand back.
    void leavingTheApplicationEndsDrawing() {
        auto &annotation = start();
        emit qGuiApp->applicationStateChanged(Qt::ApplicationInactive);
        QCOMPARE(annotation.mode(), ScreenAnnotation::Mode::PassThrough);
        annotation.select_tool("pen");
        emit qGuiApp->applicationStateChanged(Qt::ApplicationActive);
        QCOMPARE(annotation.mode(), ScreenAnnotation::Mode::Drawing);
        emit qGuiApp->applicationStateChanged(Qt::ApplicationInactive);
        QCOMPARE(annotation.mode(), ScreenAnnotation::Mode::PassThrough);
        check_consistent(annotation);
    }

    // The toolbar goes back onto a screen when the one it was on is gone, and double-clicking its
    // handle puts it at the bottom centre again.
    void toolbarComesBackOntoAScreen() {
        auto &annotation = start();
        auto *toolbar = annotation.toolbar();
        toolbar->move(-9000, -9000);
        toolbar->keep_on_screen();
        QVERIFY(QGuiApplication::screenAt(QRect(toolbar->pos(), toolbar->size()).center()));
        const QPoint home = toolbar->pos();
        toolbar->move(home + QPoint(40, -60));
        emit toolbar->drag_handle->reset_requested();
        QCOMPARE(toolbar->pos(), home);
        // The layer drawn on is the toolbar's host, and a layer is its own canvas.
        annotation.select_tool("pen");
        auto *view = annotation.views().front();
        QCOMPARE(static_cast<CanvasHost *>(view)->canvas_view(), static_cast<CanvasView *>(view));
        emit toolbar->stroke_width_changed(11);
        emit toolbar->color_changed(QColor("#00aa00"));
        QCOMPARE(view->tool_controller()->ctx.stroke_width, 11.0);
        get_tool_settings_manager()->reset_tool("pen");
    }

    void hotkeyFunctionHasFourSlots() {
        auto *config = get_tool_settings_manager();
        const auto values = config->get_global_hotkeys("screen_annotation");
        QCOMPARE(values.size(), GLOBAL_HOTKEY_SLOTS);
        for (const auto &value : values) QVERIFY(value.isEmpty());
        config->set_global_hotkeys("screen_annotation", {"ctrl+alt+f9", "", "", ""});
        QCOMPARE(settings_->value("app/screen_annotation_hotkey").toString(), QString("ctrl+alt+f9"));
        QCOMPARE(config->get_global_hotkeys("screen_annotation").front(), QString("ctrl+alt+f9"));
        config->set_global_hotkeys("screen_annotation", {"", "", "", ""});
    }

    // Window styles and hit testing need real windows from here on.

    void passThroughLetsTheMouseThrough() {
        if (offscreen()) QSKIP("needs real windows");
        QWidget below;
        below.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        below.setGeometry(QRect(QGuiApplication::primaryScreen()->geometry().center() - QPoint(100, 100), QSize(200, 200)));
        below.show();
        QVERIFY(QTest::qWaitForWindowExposed(&below));
        auto &annotation = start();
        const auto centre = below.geometry().center();
        const POINT point{centre.x(), centre.y()};
        for (auto *view : annotation.views()) {
            QVERIFY(ex_style(view) & WS_EX_LAYERED);
            QVERIFY(ex_style(view) & WS_EX_TRANSPARENT);
        }
        QTRY_VERIFY2(WindowFromPoint(point) == hwnd_of(&below), qPrintable(window_class(WindowFromPoint(point))));
        annotation.select_tool("pen");
        auto *view = view_on(annotation, QGuiApplication::primaryScreen());
        QVERIFY(!(ex_style(view) & WS_EX_TRANSPARENT));
        // Drawing: the alpha 1 hit layer takes the point.
        QTRY_COMPARE(WindowFromPoint(point), hwnd_of(view));
        annotation.select_tool("cursor");
        QVERIFY(ex_style(view) & WS_EX_TRANSPARENT);
        QTRY_COMPARE(WindowFromPoint(point), hwnd_of(&below));
        // The toolbar never activates.
        QVERIFY(ex_style(annotation.toolbar()) & WS_EX_NOACTIVATE);
    }

    void drawingGivesFocusBackToPreviousWindow() {
        if (offscreen()) QSKIP("needs real windows");
        QWidget target;
        target.setWindowTitle("annotation focus target");
        target.setGeometry(200, 200, 320, 200);
        target.show();
        QVERIFY(QTest::qWaitForWindowExposed(&target));
        QVERIFY(bring_to_front(&target));
        auto &annotation = start();
        QTest::qWait(100);
        QVERIFY2(GetForegroundWindow() == hwnd_of(&target), qPrintable(window_class(GetForegroundWindow())));
        annotation.select_tool("pen");
        QTRY_VERIFY(annotation.owns_window(QWidget::find(reinterpret_cast<WId>(GetForegroundWindow()))));
        QTRY_VERIFY(annotation.owns_window(QApplication::activeWindow()));
        // Esc through the shortcut chain.
        QTest::keyClick(QApplication::activeWindow(), Qt::Key_Escape);
        QCOMPARE(annotation.mode(), Mode::PassThrough);
        QTRY_VERIFY2(GetForegroundWindow() == hwnd_of(&target), qPrintable(window_class(GetForegroundWindow())));
        // The pointer button and switching tools on the way.
        annotation.select_tool("rect");
        QTRY_VERIFY(annotation.owns_window(QWidget::find(reinterpret_cast<WId>(GetForegroundWindow()))));
        annotation.select_tool("arrow");
        annotation.toolbar()->pointer_btn->click();
        QTRY_VERIFY2(GetForegroundWindow() == hwnd_of(&target), qPrintable(window_class(GetForegroundWindow())));
        // Leaving the session from drawing as well.
        annotation.select_tool("pen");
        QTRY_VERIFY(annotation.owns_window(QWidget::find(reinterpret_cast<WId>(GetForegroundWindow()))));
        annotation.stop();
        QTRY_VERIFY2(GetForegroundWindow() == hwnd_of(&target), qPrintable(window_class(GetForegroundWindow())));
    }

    // While drawing, the toolbar and its panel stay above the layers and take real clicks, also after
    // a stroke: a layer that activates itself or is pressed comes to the top of the topmost windows.
    void toolbarTakesClicksWhileDrawing() {
        if (offscreen()) QSKIP("needs real windows");
        auto &annotation = start();
        auto *toolbar = annotation.toolbar();
        const auto click = [](QWidget *button) {
            const QPoint at = button->mapToGlobal(button->rect().center());
            real_click(at);
            return at;
        };
        const auto window_at = [](const QPoint &at) { return GetAncestor(WindowFromPoint(POINT{at.x(), at.y()}), GA_ROOT); };
        QTest::qWait(200);
        click(toolbar->pen_btn);
        QTRY_COMPARE(annotation.mode(), Mode::Drawing);
        QTest::qWait(300);
        for (auto *button : {toolbar->rect_btn, toolbar->highlighter_btn}) {
            const QPoint at = button->mapToGlobal(button->rect().center());
            QVERIFY2(window_at(at) == hwnd_of(toolbar), qPrintable(window_class(window_at(at))));
            click(button);
            QTest::qWait(300);
        }
        QCOMPARE(toolbar->current_tool.value_or(QString()), QString("highlighter"));
        // A stroke on the layer, then the toolbar and the highlighter's panel again.
        auto *view = view_on(annotation, QGuiApplication::screenAt(toolbar->geometry().center()));
        QVERIFY(view);
        const QPoint from = view->geometry().center() - QPoint(200, 100);
        SetCursorPos(from.x(), from.y());
        INPUT down = {};
        down.type = INPUT_MOUSE;
        down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        SendInput(1, &down, sizeof(INPUT));
        for (int i = 1; i <= 10; ++i) {
            SetCursorPos(from.x() + i * 20, from.y());
            QTest::qWait(10);
        }
        INPUT up = {};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(1, &up, sizeof(INPUT));
        QTRY_COMPARE(drawing_count(view), 1);
        QTest::qWait(200);
        auto *panel = toolbar->paint_panel;
        QVERIFY(panel->isVisible());
        const QPoint mode_at = panel->rect_btn->mapToGlobal(panel->rect_btn->rect().center());
        QVERIFY2(window_at(mode_at) == hwnd_of(panel), qPrintable(window_class(window_at(mode_at))));
        click(panel->rect_btn);
        QTRY_COMPARE(get_tool_settings_manager()->get_tool_settings("highlighter")->get("draw_mode").toString(), QString("rect"));
        const QPoint pointer_at = toolbar->pointer_btn->mapToGlobal(toolbar->pointer_btn->rect().center());
        QVERIFY2(window_at(pointer_at) == hwnd_of(toolbar), qPrintable(window_class(window_at(pointer_at))));
        click(toolbar->pointer_btn);
        QTRY_COMPARE(annotation.mode(), Mode::PassThrough);
        annotation.stop();
        get_tool_settings_manager()->reset_tool("highlighter");
    }

    // The toolbar never becomes the active window, yet hovering a button shows its tooltip.
    void toolbarShowsTooltips() {
        if (offscreen()) QSKIP("needs real windows");
        auto &annotation = start();
        auto *toolbar = annotation.toolbar();
        QTest::qWait(200);
        auto *button = toolbar->pen_btn;
        const QPoint at = button->mapToGlobal(button->rect().center());
        SetCursorPos(at.x() - 4, at.y());
        QTest::qWait(50);
        SetCursorPos(at.x(), at.y());
        QTRY_VERIFY_WITH_TIMEOUT(QToolTip::isVisible(), 3000);
        QCOMPARE(QToolTip::text(), button->toolTip());
        SetCursorPos(at.x(), at.y() - 300);
        Toolbar capture_toolbar;
        save_render(capture_toolbar.grab().toImage(), "toolbar_capture");
        save_render(toolbar->grab().toImage(), "toolbar_annotation");
    }

    void toolbarClickKeepsForegroundWindow() {
        if (offscreen()) QSKIP("needs real windows");
        QWidget target;
        target.setWindowTitle("annotation focus target");
        target.setGeometry(200, 200, 320, 200);
        target.show();
        QVERIFY(QTest::qWaitForWindowExposed(&target));
        QVERIFY(bring_to_front(&target));
        auto &annotation = start();
        auto *button = annotation.toolbar()->hide_btn;
        QTest::qWait(200);
        real_click(button->mapToGlobal(button->rect().center()));
        QTRY_COMPARE(annotation.mode(), Mode::Hidden);
        QVERIFY2(GetForegroundWindow() == hwnd_of(&target), qPrintable(window_class(GetForegroundWindow())));
        real_click(button->mapToGlobal(button->rect().center()));
        QTRY_COMPARE(annotation.mode(), Mode::PassThrough);
        QVERIFY2(GetForegroundWindow() == hwnd_of(&target), qPrintable(window_class(GetForegroundWindow())));
    }

    void captureTakesDrawingsWithoutHitLayerOrToolbar() {
        if (offscreen()) QSKIP("needs real windows");
        auto *config = get_tool_settings_manager();
        config->update_settings("pen", {{"color", "#ff0000"}, {"stroke_width", 8}, {"opacity", 1.0}});
        auto *screen = QGuiApplication::primaryScreen();
        // Mid grey: the alpha 1 hit layer darkens it by at most one level, and anything opaque shows.
        QWidget backdrop;
        // Topmost so no other window shows through; the layers are shown later and go above it.
        backdrop.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        backdrop.setStyleSheet("background: #808080;");
        backdrop.setGeometry(screen->geometry());
        backdrop.show();
        QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
        auto &annotation = start();
        annotation.select_tool("pen");
        auto *view = view_on(annotation, screen);
        QVERIFY(view);
        const QRect area = screen->geometry();
        const QPoint from(area.width() / 4, area.height() / 3), to(area.width() / 2, area.height() / 3);
        draw_stroke(view, from, to);
        QTest::qWait(300);
        const QPoint on_stroke = view->mapToGlobal((from + to) / 2);
        const QPoint off_stroke = view->mapToGlobal(QPoint(area.width() / 3, area.height() / 2));
        const QPoint on_toolbar = annotation.toolbar()->mapToGlobal(annotation.toolbar()->rect().center());
        // While drawing the hit layer darkens the screen by up to one level, so the exact check below can fail.
        const auto [drawing_image, drawing_rect] = CaptureService().capture_all_screens();
        const QColor drawing_off = pixel_at(drawing_image, drawing_rect, off_stroke);
        QVERIFY2(drawing_off.red() >= 127 && drawing_off.red() <= 128 && drawing_off.green() == drawing_off.red(), qPrintable(drawing_off.name()));
        annotation.prepare_for_capture();
        const auto [image, rect] = CaptureService().capture_all_screens();
        const auto stroke = pixel_at(image, rect, on_stroke);
        QVERIFY2(stroke.red() > 200 && stroke.green() < 60 && stroke.blue() < 60, qPrintable(stroke.name()));
        QCOMPARE(pixel_at(image, rect, off_stroke), QColor(128, 128, 128));
        // The toolbar is left out of captures.
        QCOMPARE(pixel_at(image, rect, on_toolbar), QColor(128, 128, 128));
        config->reset_tool("pen");
    }

    // GIF and MP4 recordings capture the screen like screenshots do: the drawings are in the frames, the
    // toolbar is not, even when it sits inside the recorded area.
    void recordingsTakeDrawingsWithoutToolbar() {
        if (offscreen()) QSKIP("needs real windows");
        auto *config = get_tool_settings_manager();
        config->update_settings("pen", {{"color", "#ff0000"}, {"stroke_width", 8}, {"opacity", 1.0}});
        auto *screen = QGuiApplication::primaryScreen();
        QWidget backdrop;
        // Topmost so no other window shows through; the layers are shown later and go above it.
        backdrop.setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        backdrop.setStyleSheet("background: #808080;");
        backdrop.setGeometry(screen->geometry());
        backdrop.show();
        QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
        auto &annotation = start();
        annotation.select_tool("pen");
        auto *view = view_on(annotation, screen);
        QVERIFY(view);
        const QRect area = screen->geometry();
        const QPoint from(area.width() / 4, area.height() / 3), to(area.width() / 2, area.height() / 3);
        draw_stroke(view, from, to);
        // The MP4 frame is 1024 x 512: the decoder pads rows and columns to multiples of 16.
        const QRect region(view->mapToGlobal(from) - QPoint(96, 128), QSize(1026, 514));
        auto *toolbar = annotation.toolbar();
        toolbar->move(region.topLeft() + QPoint(40, 320));
        annotation.prepare_for_capture();
        SetCursorPos(area.right(), area.bottom());
        QTest::qWait(300);
        QVERIFY(region.contains(toolbar->geometry()));
        const QPoint on_stroke = view->mapToGlobal((from + to) / 2) - region.topLeft();
        const QPoint off_stroke = QPoint(region.width() - 60, 150);
        const QPoint on_toolbar = toolbar->geometry().center() - region.topLeft();
        // origin: where the frame's top-left pixel is in the region.
        const auto check = [&](const QImage &frame, const QPoint &origin, const char *what) {
            QVERIFY2(!frame.isNull(), what);
            const QColor stroke = frame.pixelColor(on_stroke - origin), background = frame.pixelColor(off_stroke - origin);
            const QColor under_toolbar = frame.pixelColor(on_toolbar - origin);
            QVERIFY2(is_red(stroke), qPrintable(QString("%1 stroke %2").arg(what, stroke.name())));
            QVERIFY2(near_grey(background, 128, 10), qPrintable(QString("%1 background %2").arg(what, background.name())));
            QVERIFY2(near_grey(under_toolbar, 128, 10), qPrintable(QString("%1 toolbar %2").arg(what, under_toolbar.name())));
        };

        FrameRecorder gif;
        gif.set_fps(15);
        gif.set_rect(region);
        gif.start();
        QTest::qWait(600);
        gif.stop();
        auto store = gif.store();
        QVERIFY(store && store->frame_count() > 0);
        const QByteArray rgb = store->get_frame_rgb(store->frame_count() - 1);
        check(QImage(reinterpret_cast<const uchar *>(rgb.constData()), store->width(), store->height(), store->width() * 3, QImage::Format_RGB888).copy(),
              QPoint(0, 0), "gif");
        if (QTest::currentTestFailed()) return;
        const auto released = gif.release();
        if (released.valid()) released.wait();

        QTemporaryDir dir;
        const QString output = dir.filePath("annotation.mp4");
        VideoRecorder mp4(nullptr, {QStringLiteral(JIETUBA_APP_PATH), "--video-recorder-worker"});
        QSignalSpy ready(&mp4, &VideoRecorder::ready);
        QSignalSpy failed(&mp4, &VideoRecorder::failed);
        QSignalSpy finished(&mp4, &VideoRecorder::finished);
        RecordingOptions options;
        options.system_audio = false;
        options.cursor = false;
        QVERIFY(mp4.start(region, output, options));
        QVERIFY(ready.wait(30000) || !failed.isEmpty());
        if (!failed.isEmpty() && failed.at(0).at(0).toString() == "capture") QSKIP("No capture device here.");
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.at(0).at(1).toString()));
        QTest::qWait(800);
        mp4.stop();
        QVERIFY(finished.wait(30000));
        QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.at(0).at(1).toString()));
        // The MP4 path leaves out a one pixel border and a trailing odd pixel.
        const QImage video = last_video_frame(output);
        QCOMPARE(video.size(), QSize((region.width() - 2) & ~1, (region.height() - 2) & ~1));
        check(video, QPoint(1, 1), "mp4");
        if (QTest::currentTestFailed()) return;
        config->reset_tool("pen");
    }

    void startStopCyclesReleaseHandlesAndMemory() {
        if (offscreen()) QSKIP("GDI and USER objects need real windows");
        StubHub stub;
        const auto cycle = [&stub](bool with_tools) {
            {
                ScreenAnnotation annotation;
                annotation.cursor_highlight()->set_hub(stub.hub.get());
                annotation.start();
                annotation.set_drawings_visible(false);
                annotation.set_drawings_visible(true);
                annotation.clear_drawings();
                if (with_tools) {
                    annotation.select_tool("pen");
                    draw_stroke(annotation.views().front());
                    annotation.toggle_board();
                    annotation.set_cursor_highlight(true, false);
                    annotation.cursor_highlight()->on_pointer(QPoint(60, 60));
                    annotation.select_tool("text");
                    annotation.select_tool("cursor");
                }
                annotation.stop();
            }
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents();
            return process_counters();
        };
        // Windows, toolbar, panels and scenes: flat from the tenth session to the twentieth.
        std::vector<ProcessCounters> plain;
        for (int i = 1; i <= 20; ++i) {
            const auto counters = cycle(false);
            if (i == 1 || i == 10 || i == 20) plain.push_back(counters);
        }
        qInfo("without tool cursors: GDI %lu/%lu/%lu USER %lu/%lu/%lu private KB %llu/%llu/%llu", plain[0].gdi, plain[1].gdi, plain[2].gdi, plain[0].user,
              plain[1].user, plain[2].user, static_cast<unsigned long long>(plain[0].private_bytes / 1024),
              static_cast<unsigned long long>(plain[1].private_bytes / 1024), static_cast<unsigned long long>(plain[2].private_bytes / 1024));
        QVERIFY(plain[2].gdi <= plain[1].gdi);
        QVERIFY(plain[2].user <= plain[1].user);
        // A layer left behind keeps two USER objects (checked above; a hidden window has given back its
        // backing store); the heap itself settles over the first fifty sessions or so.
        QVERIFY(plain[2].private_bytes <= plain[1].private_bytes + 2 * 1024 * 1024);
        // Every drawing tool cursor is a new pixmap cursor, which Qt keeps until it holds 50 and then drops
        // those not in use (QTBUG-43515): bounded, so the second fifty sessions peak no higher than the first.
        DWORD first_gdi = 0, first_user = 0, second_gdi = 0, second_user = 0;
        SIZE_T fiftieth = 0, last = 0;
        for (int i = 1; i <= 100; ++i) {
            const auto counters = cycle(true);
            auto &gdi = i <= 50 ? first_gdi : second_gdi;
            auto &user = i <= 50 ? first_user : second_user;
            gdi = std::max(gdi, counters.gdi);
            user = std::max(user, counters.user);
            if (i == 50) fiftieth = counters.private_bytes;
            if (i == 100) last = counters.private_bytes;
        }
        qInfo("with tool cursors, peak of sessions 1-50 / 51-100: GDI %lu/%lu USER %lu/%lu private KB at 50/100 %llu/%llu", first_gdi, second_gdi, first_user,
              second_user, static_cast<unsigned long long>(fiftieth / 1024), static_cast<unsigned long long>(last / 1024));
        QVERIFY(second_gdi <= first_gdi + 3);
        QVERIFY(second_user <= first_user + 1);
        QVERIFY(last <= fiftieth + 2 * 1024 * 1024);
    }

    // A host for the external latency and memory comparison: with JIETUBA_ANNOTATION_BENCH=<base path>
    // it starts drawing with a red 6 px pen, writes <base>.ready with its process id and then follows
    // commands written to <base>.cmd (start, stop, pen, pass, clear, highlight, highlight_off, board, quit).
    void benchHold() {
        const QString base = qEnvironmentVariable("JIETUBA_ANNOTATION_BENCH");
        if (base.isEmpty()) QSKIP("benchmark host only");
        get_tool_settings_manager()->update_settings("pen", {{"color", "#ff0000"}, {"stroke_width", 6}, {"opacity", 1.0}});
        // "idle" waits for a "start" command, so memory before the session can be read.
        annotation_ = std::make_unique<ScreenAnnotation>();
        auto &annotation = *annotation_;
        const auto write = [](const QString &path, const QByteArray &text) {
            QFile file(path);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(text);
        };
        const auto run = [&annotation](const QString &command) {
            if (command == "pen") annotation.select_tool("pen");
            else if (command == "pass") annotation.select_tool("cursor");
            else if (command == "clear") annotation.clear_drawings();
            else if (command == "highlight") annotation.set_cursor_highlight(true, false);
            else if (command == "highlight_off") annotation.set_cursor_highlight(false, false);
            else if (command == "board") annotation.toggle_board();
            else if (command == "start") {
                annotation.start();
                annotation.select_tool("pen");
            } else if (command == "stop") annotation.stop();
        };
        const QString mode = qEnvironmentVariable("JIETUBA_ANNOTATION_BENCH_MODE", "start");
        if (mode != "idle") run(mode);
        // A real event loop as in the application: QTest::qWaitFor sleeps 10 ms between passes, which
        // would add that to each stage a mouse move goes through before it reaches the screen.
        QEventLoop loop;
        int handled = 0;
        QFileSystemWatcher watcher;
        const QString command_path = base + ".cmd";
        write(command_path, "");
        watcher.addPath(command_path);
        QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, [&](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) return;
            const auto command = QString::fromUtf8(file.readAll()).trimmed();
            if (command.isEmpty()) return;
            QElapsedTimer clock;
            clock.start();
            if (command != "quit") run(command);
            // Until the layers are on the screen with the pen ready.
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            for (auto *view : annotation.views())
                if (view->isVisible()) view->viewport()->repaint();
            const qint64 elapsed = clock.elapsed();
            write(base + ".ack", QByteArray::number(++handled) + " " + command.toUtf8() + " " + QByteArray::number(elapsed));
            if (command == "quit") loop.quit();
            else if (!watcher.files().contains(path)) watcher.addPath(path);
        });
        write(base + ".ready", QByteArray::number(static_cast<qulonglong>(GetCurrentProcessId())));
        loop.exec();
    }
};

int main(int argc, char **argv) {
    // The offscreen run gets two screens side by side, so the per-screen layers are tested without monitors.
    // Beside the working directory: platform options are separated by colons, so the path given
    // to the platform is relative and carries no drive letter.
    QTemporaryDir config_dir(QDir::current().filePath("annotation-screens-XXXXXX"));
    bool platform_given = false;
    for (int i = 1; i < argc; ++i) platform_given = platform_given || QByteArray(argv[i]) == "-platform";
    if (!platform_given && qgetenv("QT_QPA_PLATFORM") == "offscreen") {
        const QString path = config_dir.filePath("screens.json");
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(offscreen_config(TWO_SCREENS)).toJson());
        file.close();
        qputenv("QT_QPA_PLATFORM", ("offscreen:configfile=" + QDir::current().relativeFilePath(path)).toLocal8Bit());
    }
    QApplication app(argc, argv);
    ScreenAnnotationTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "test_screen_annotation.moc"
