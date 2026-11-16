#include "core/clipboard_utils.h"
#include "core/i18n.h"
#include "core/input_hub.h"
#include "canvas/scene.h"
#include "capture/capture_service.h"
#include "gif/record_window.h"
#include "ocr/ocr_manager.h"
#include "ui/screenshot_window.h"
#include "ui/toolbar.h"
#include "pin/pin_manager.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "stitch/auto_scroll.h"
#include "stitch/change_watch.h"
#include "stitch/incremental.h"
#include "stitch/scroll_toolbar.h"
#include "stitch/scroll_window.h"
#include "text_recognition/result_window.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/save_image_dialog.h"
#include "ui/toast.h"
#include "save_dialog_answerer.h"
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QWheelEvent>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTransform>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <future>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

using namespace jietuba;

namespace {
constexpr int PAGE_W = 400;
constexpr int VIEW_H = 300;
constexpr int PAGE_H = 1600;
constexpr ULONG_PTR MOUSE_TEST_MARKER = 0x4A425432;

bool native_mouse(const QPoint &point, DWORD flags, DWORD wheel = 0) {
    DWORD owner = 0;
    GetWindowThreadProcessId(WindowFromPoint(POINT{point.x(), point.y()}), &owner);
    // Never send a test click to an unrelated window covering the test surface.
    if (!(flags & (MOUSEEVENTF_LEFTUP | MOUSEEVENTF_MIDDLEUP | MOUSEEVENTF_RIGHTUP)) && owner != GetCurrentProcessId()) {
        qWarning() << "Test point covered" << point << "owner" << owner << "test" << GetCurrentProcessId();
        return false;
    }
    if (!SetCursorPos(point.x(), point.y())) return false;
    POINT cursor{};
    if (!GetCursorPos(&cursor) || cursor.x != point.x() || cursor.y != point.y()) return false;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = wheel;
    input.mi.dwExtraInfo = MOUSE_TEST_MARKER;
    return SendInput(1, &input, sizeof input) == 1;
}

void native_ctrl(bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_LCONTROL;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = MOUSE_TEST_MARKER;
    SendInput(1, &input, sizeof input);
}

bool native_click(const QPoint &point, Qt::MouseButton button, bool ctrl = false) {
    if (ctrl) native_ctrl(true);
    const bool sent = native_mouse(point, button == Qt::LeftButton ? MOUSEEVENTF_LEFTDOWN : button == Qt::MiddleButton ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_RIGHTDOWN);
    if (!sent) { if (ctrl) native_ctrl(false); return false; }
    QTest::qWait(20);
    native_mouse(point, button == Qt::LeftButton ? MOUSEEVENTF_LEFTUP : button == Qt::MiddleButton ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_RIGHTUP);
    if (ctrl) native_ctrl(false);
    QTest::qWait(40);
    return sent;
}

class MouseActionWindow : public ScrollCaptureWindow {
public:
    MouseActionWindow(const QRect &rect, ToolSettingsManager *config) : ScrollCaptureWindow(rect, nullptr, config) {}
    std::shared_ptr<QStringList> actions = std::make_shared<QStringList>();
protected:
    void trigger_mouse_action(const QString &action) override {
        actions->append(action);
        // Exercise double-click dispatch without writing the developer's real clipboard.
        if (action != "copy") ScrollCaptureWindow::trigger_mouse_action(action);
    }
};

// White with lines of dashes of varying length and colour, roughly a page of text.
// Every pixel row is drawn on its own, so no two rows repeat and matching is unambiguous.
QImage synthetic_page() {
    QImage page(PAGE_W, PAGE_H, QImage::Format_RGB32);
    page.fill(Qt::white);
    QRandomGenerator random(4);
    QPainter painter(&page);
    int y = 6;
    while (y < PAGE_H - 20) {
        const int line = 8 + 2 * random.bounded(3);
        const QColor color(random.bounded(121), random.bounded(121), random.bounded(121));
        for (int row = y; row < y + line; ++row) {
            int x = 10;
            while (x < PAGE_W - 40) {
                const int length = 2 + random.bounded(29);
                if (random.bounded(10) < 6) painter.fillRect(x, row, length, 1, color);
                x += length + 1 + random.bounded(6);
            }
        }
        y += line + 4 + random.bounded(11);
    }
    painter.end();
    return page;
}

// A topmost window showing the page from top downwards: what the capture sees scroll.
class PageWindow : public QWidget {
public:
    QMap<Qt::MouseButton, int> presses;
    explicit PageWindow(const QImage &page) : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool), page_(page) {
        setAttribute(Qt::WA_ShowWithoutActivating);
        square_corners();
    }
    void square_corners() {
        // Windows 11 rounds and borders frameless windows from Qt 6.11 on; that static rim
        // inside the captured area would keep frames from joining.
        const auto hwnd = reinterpret_cast<HWND>(winId());
        const DWORD square = 1, no_border = 0xFFFFFFFE;  // DWMWCP_DONOTROUND, DWMWA_COLOR_NONE
        DwmSetWindowAttribute(hwnd, 33, &square, sizeof square);         // DWMWA_WINDOW_CORNER_PREFERENCE
        DwmSetWindowAttribute(hwnd, 34, &no_border, sizeof no_border);  // DWMWA_BORDER_COLOR
    }
    void scroll_to(int top) {
        top_ = top;
        repaint();
    }
    void scroll_to_x(int left) {
        left_ = left;
        repaint();
    }
    // Another page entirely, which no earlier frame overlaps.
    void set_page(const QImage &page) {
        page_ = page;
        repaint();
    }
    int top() const { return top_; }
    int left() const { return left_; }
protected:
    void mousePressEvent(QMouseEvent *event) override { ++presses[event->button()]; QWidget::mousePressEvent(event); }
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        // Qt reapplies its Windows 11 corner preference while showing the HWND.
        QTimer::singleShot(0, this, [this] { square_corners(); });
    }
    // A notch scrolls 60 pixels, as a page would.
    void wheelEvent(QWheelEvent *event) override {
        const int notches = event->angleDelta().y() / 120;
        if (notches) scroll_to(std::clamp(top_ - notches * 60, 0, page_.height() - height()));
        // Horizontal notches arrive with the opposite sign: scrolling right is negative.
        const int sideways = event->angleDelta().x() / 120;
        if (sideways) scroll_to_x(std::clamp(left_ - sideways * 60, 0, page_.width() - width()));
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.drawImage(0, 0, page_, left_, top_, width(), height());
    }
private:
    QImage page_;
    int top_ = 0;
    int left_ = 0;
};

QSize clipboard_bitmap_size() {
    QSize size;
    for (int attempt = 0; attempt < 40 && size.isEmpty(); ++attempt) {
        if (OpenClipboard(nullptr)) {
            if (HANDLE handle = GetClipboardData(CF_DIBV5)) {
                if (const auto *header = static_cast<const BITMAPV5HEADER *>(GlobalLock(handle))) {
                    size = QSize(header->bV5Width, std::abs(header->bV5Height));
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
        }
        if (size.isEmpty()) QTest::qWait(50);
    }
    return size;
}

template <typename T> T *visible_top_level() {
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *typed = dynamic_cast<T *>(widget); typed && typed->isVisible()) return typed;
    return nullptr;
}

void clear_clipboard() {
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
}
}

class ScrollCaptureTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
    QImage page_;
    std::unique_ptr<PageWindow> page_window_;
    QPointer<ScrollCaptureWindow> window_;
    QRect area_;

    // Opens the capture over the page and waits for its first frame.
    ScrollCaptureWindow *open(bool mouse_actions = false) {
        page_window_ = std::make_unique<PageWindow>(page_);
        page_window_->setGeometry(area_);
        page_window_->show();
        QTest::qWait(150);
        window_ = mouse_actions ? new MouseActionWindow(area_, config_) : new ScrollCaptureWindow(area_, nullptr, config_);
        window_->show();
        window_->activateWindow();
        if (!QTest::qWaitFor([&] { return window_ && window_->screenshots >= 1; }, 3000)) return nullptr;
        // The change watch starts after the first frame and takes its own first look as the
        // baseline; a person starts scrolling only once the window is up.
        QTest::qWait(CHANGE_WATCH_INTERVAL_MS + 150);
        return window_;
    }

    // Scrolls the page in steps; each step must become one more captured frame.
    bool scroll_in_steps(const QList<int> &tops) {
        for (int top : tops) {
            const int before = window_->screenshots;
            page_window_->scroll_to(top);
            if (!QTest::qWaitFor([&] { return window_->screenshots > before; }, 5000)) return false;
        }
        return true;
    }

    // Mouse coordinates are physical; the window may move while dragging its top/left.
    void drag_edge(Qt::Edge edge, int pixels, bool cancel = false) {
        auto *window = window_.data();
        const auto rect = window->geometry();
        QPoint press, delta;
        switch (edge) {
        case Qt::TopEdge: press = QPoint(rect.center().x(), rect.top() + 1); delta = QPoint(0, -pixels); break;
        case Qt::BottomEdge: press = QPoint(rect.center().x(), rect.bottom() - 1); delta = QPoint(0, pixels); break;
        case Qt::LeftEdge: press = QPoint(rect.left() + 1, rect.center().y()); delta = QPoint(-pixels, 0); break;
        case Qt::RightEdge: press = QPoint(rect.right() - 1, rect.center().y()); delta = QPoint(pixels, 0); break;
        }
        QTest::mousePress(window, Qt::LeftButton, {}, window->mapFromGlobal(press));
        QTest::mouseMove(window, window->mapFromGlobal(press + delta));
        if (cancel) {
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(window, &escape);
        }
        QTest::mouseRelease(window, Qt::LeftButton, {}, window->mapFromGlobal(press + delta));
    }

private slots:
    void initTestCase() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows: the capture reads the real screen");
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
        config_->set_screenshot_save_path(dir_.filePath("captures"));
        config_->set_screenshot_save_enabled(false);
        config_->set_app_setting("ocr_enabled", false);
        page_ = synthetic_page();
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        area_ = QRect(available.center() - QPoint(PAGE_W / 2, VIEW_H / 2), QSize(PAGE_W, VIEW_H));
    }

    void cleanup() {
        if (window_) window_->close();
        PinManager::instance()->close_all();
        for (auto *widget : QApplication::topLevelWidgets()) if (dynamic_cast<Toast *>(widget)) widget->close();
        for (const auto &action : LONG_SCREENSHOT_MOUSE_ACTIONS()) config_->set_app_setting(QString("mouse_long_") + action.key, "");
        config_->set_screenshot_format("PNG");
        config_->set_screenshot_save_path(dir_.filePath("captures"));
        input_hub()->native->set_test_marker(0);
        page_window_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTest::qWait(50);
    }

    void unassignedLongScreenshotButtonsAndWheelPassThrough() {
        auto *window = open();
        QVERIFY(window);
        const auto original_pointer = QCursor::pos();
        const auto restore = qScopeGuard([&] { QCursor::setPos(original_pointer); });
        input_hub()->native->set_test_marker(MOUSE_TEST_MARKER);
        for (auto button : {Qt::LeftButton, Qt::MiddleButton, Qt::RightButton}) {
            QVERIFY2(native_click(area_.center(), button), "test point is covered by another application");
            QTRY_COMPARE(page_window_->presses.value(button), 1);
            QVERIFY(window->isVisible());
        }
        QVERIFY(native_mouse(area_.center(), MOUSEEVENTF_WHEEL, DWORD(-WHEEL_DELTA)));
        QTRY_VERIFY(page_window_->top() > 0);
        QTRY_VERIFY(window->screenshots > 1);
    }

    void longScreenshotMouseActions_data() {
        QTest::addColumn<QString>("action");
        for (const auto &action : {"copy", "pin", "save", "quick_save", "close"}) QTest::newRow(action) << QString(action);
    }

    void longScreenshotMouseActions() {
        QFETCH(QString, action);
        const auto original_pointer = QCursor::pos();
        const auto restore = qScopeGuard([&] { native_ctrl(false); QCursor::setPos(original_pointer); });
        const QString folder = dir_.filePath("mouse-" + action);
        config_->set_screenshot_save_path(folder);
        config_->set_app_setting("mouse_long_" + action, "ctrl+middle");
        auto *window = static_cast<MouseActionWindow *>(open(true));
        QVERIFY(window);
        const auto actions = window->actions;
        QVERIFY(scroll_in_steps({100, 200}));
        auto *stitcher = window->findChild<IncrementalStitcher *>();
        const auto expected = stitcher->export_image().convertToFormat(QImage::Format_RGB32);
        input_hub()->native->set_test_marker(MOUSE_TEST_MARKER);
        // A middle click without Ctrl remains a normal page click.
        QVERIFY2(native_click(area_.center(), Qt::MiddleButton), "test point is covered by another application");
        QTRY_COMPARE(page_window_->presses.value(Qt::MiddleButton), 1);
        QVERIFY(actions->isEmpty());
        const QString path = QDir(folder).filePath("manual.png");
        SaveDialogAnswerer answer(path);
        QSignalSpy finished(window, &ScrollCaptureWindow::finished);
        QSignalSpy cancelled(window, &ScrollCaptureWindow::cancelled);
        QVERIFY2(native_click(area_.center(), Qt::MiddleButton, true), "test point is covered by another application");
        QTRY_COMPARE(actions->size(), 1);
        QCOMPARE(actions->first(), action);
        QCOMPARE(page_window_->presses.value(Qt::MiddleButton), 1);
        if (action == "copy") {
            QCOMPARE(stitcher->export_image().convertToFormat(QImage::Format_RGB32), expected);
        } else if (action == "pin") {
            QTRY_COMPARE(finished.count(), 1);
            QCOMPARE(PinManager::instance()->count(), 1);
            QCOMPARE(PinManager::instance()->get_all_pins().first()->base_pixmap().toImage().convertToFormat(QImage::Format_RGB32), expected);
        } else if (action == "save") {
            QTRY_VERIFY(answer.answered);
            QTRY_VERIFY(QFileInfo::exists(path));
            QCOMPARE(QImage(path).convertToFormat(QImage::Format_RGB32), expected);
        } else if (action == "quick_save") {
            QTRY_COMPARE(finished.count(), 1);
            const auto files = QDir(folder).entryList({"*.png"}, QDir::Files);
            QCOMPARE(files.size(), 1);
            QCOMPARE(QImage(QDir(folder).filePath(files.first())).convertToFormat(QImage::Format_RGB32), expected);
            QVERIFY(!answer.answered);
        } else QTRY_COMPARE(cancelled.count(), 1);
    }

    void doubleClickBindingCapturesThatButtonWithoutBlockingTheWheel() {
        config_->set_mouse_long_copy("doubleleft");
        auto *window = static_cast<MouseActionWindow *>(open(true));
        QVERIFY(window);
        const auto actions = window->actions;
        const auto pointer = QCursor::pos();
        const auto restore = qScopeGuard([&] { QCursor::setPos(pointer); });
        input_hub()->native->set_test_marker(MOUSE_TEST_MARKER);
        QVERIFY2(native_click(area_.center(), Qt::LeftButton), "test point is covered by another application");
        QVERIFY(actions->isEmpty());
        QCOMPARE(page_window_->presses.value(Qt::LeftButton), 0);
        QVERIFY2(native_click(area_.center(), Qt::LeftButton), "test point is covered by another application");
        QTRY_COMPARE(actions->size(), 1);
        QCOMPARE(actions->first(), QString("copy"));
        QVERIFY(native_mouse(area_.center(), MOUSEEVENTF_WHEEL, DWORD(-WHEEL_DELTA)));
        QTRY_VERIFY(page_window_->top() > 0);
        QTRY_VERIFY(window->screenshots > 1);
        // Left-button shortcuts do not take the border's left-button resize.
        drag_edge(Qt::BottomEdge, 10);
        QTRY_COMPARE(window->capture_rect, area_.adjusted(0, 0, 0, 10));
        QCOMPARE(actions->size(), 1);
    }

    void failedQuickSaveKeepsTheLongImageAndCanRetry_data() {
        QTest::addColumn<bool>("bad_directory");
        QTest::newRow("directory") << true;
        QTest::newRow("encoder") << false;
    }

    void failedQuickSaveKeepsTheLongImageAndCanRetry() {
        QFETCH(bool, bad_directory);
        const auto pointer = QCursor::pos();
        const auto restore = qScopeGuard([&] { native_ctrl(false); QCursor::setPos(pointer); });
        config_->set_mouse_long_quick_save("ctrl+middle");
        auto *window = open();
        QVERIFY(window);
        auto *stitcher = window->findChild<IncrementalStitcher *>();
        const auto before = stitcher->export_image();
        if (bad_directory) {
            const QString blocked = dir_.filePath("blocked-save");
            QFile file(blocked);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
            config_->set_screenshot_save_path(blocked);
        } else config_->set_screenshot_format("unsupported-test-format");
        input_hub()->native->set_test_marker(MOUSE_TEST_MARKER);
        QVERIFY2(native_click(area_.center(), Qt::MiddleButton, true), "test point is covered by another application");
        QTRY_VERIFY(window->preview_warning_active);
        QVERIFY(window->toolbar->isEnabled());
        QCOMPARE(stitcher->export_image(), before);
        QSignalSpy finished(window, &ScrollCaptureWindow::finished);
        config_->set_screenshot_format("PNG");
        config_->set_screenshot_save_path(dir_.filePath("retry-quick-save"));
        QVERIFY2(native_click(area_.center(), Qt::MiddleButton, true), "test point is covered by another application");
        QTRY_COMPARE(finished.count(), 1);
    }

    void otherControlsAreExcludedAndClearingABindingRestoresPassThrough() {
        const auto pointer = QCursor::pos();
        const auto restore = qScopeGuard([&] { QCursor::setPos(pointer); });
        config_->set_mouse_long_copy("right");
        auto *window = static_cast<MouseActionWindow *>(open(true));
        QVERIFY(window);
        const auto actions = window->actions;
        input_hub()->native->set_test_marker(MOUSE_TEST_MARKER);
        QDialog dialog;
        dialog.setWindowFlag(Qt::WindowStaysOnTopHint);
        dialog.setGeometry(QRect(area_.center() - QPoint(80, 60), QSize(160, 120)));
        dialog.show();
        QTest::qWait(100);
        QVERIFY2(native_click(area_.center(), Qt::RightButton), "test point is covered by another application");
        QVERIFY(actions->isEmpty());
        dialog.setWindowFlag(Qt::WindowStaysOnTopHint, false);
        dialog.show();
        page_window_->raise();
        page_window_->activateWindow();
        QTest::qWait(100);
        QVERIFY2(native_click(area_.center(), Qt::RightButton), "test point is covered by another application");
        QTRY_COMPARE(actions->size(), 1);
        config_->set_mouse_long_copy("");
        dialog.close(); // Like applying settings and closing them, refresh the native region.
        QTest::qWait(100);
        QVERIFY2(native_click(area_.center(), Qt::RightButton), "test point is covered by another application");
        QTRY_COMPARE(page_window_->presses.value(Qt::RightButton), 1);
        QCOMPARE(actions->size(), 1);
        QVERIFY(window->isVisible());
    }

    void lateMouseEventsCannotAffectAnotherLongScreenshot() {
        config_->set_mouse_long_copy("middle");
        auto *first = static_cast<MouseActionWindow *>(open(true));
        QVERIFY(first);
        QSignalSpy clicks(input_hub(), &InputHub::mouse_captured);
        const auto pointer = QCursor::pos();
        const auto restore = qScopeGuard([&] { QCursor::setPos(pointer); });
        input_hub()->native->set_test_marker(MOUSE_TEST_MARKER);
        QVERIFY(native_click(area_.center(), Qt::MiddleButton));
        QTRY_COMPARE(first->actions->size(), 1);
        QVERIFY(!clicks.isEmpty());
        const auto old_watcher = clicks.first()[0].toString();
        first->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto *next = static_cast<MouseActionWindow *>(open(true));
        QVERIFY(next);
        input_hub()->mouse_captured(old_watcher, {}, "middle", true, area_.center().x(), area_.center().y(), 100);
        input_hub()->mouse_captured(old_watcher, {}, "middle", false, area_.center().x(), area_.center().y(), 120);
        QTest::qWait(100);
        QVERIFY(next->actions->isEmpty());
        QVERIFY(next->isVisible());
    }

    void draggingVisibleEndTwiceExportsExactPixels_data() {
        QTest::addColumn<bool>("horizontal");
        QTest::addColumn<bool>("at_start");
        QTest::addColumn<int>("screen_index");
        QTest::addColumn<QString>("output");
        for (int i = 0; i < QGuiApplication::screens().size(); ++i) {
            QTest::newRow(qPrintable(QString("top-screen-%1").arg(i))) << false << true << i << QString("save");
            QTest::newRow(qPrintable(QString("bottom-screen-%1").arg(i))) << false << false << i << QString("save");
            QTest::newRow(qPrintable(QString("left-screen-%1").arg(i))) << true << true << i << QString("save");
            QTest::newRow(qPrintable(QString("right-screen-%1").arg(i))) << true << false << i << QString("save");
        }
        QTest::newRow("bottom-pin") << false << false << 0 << QString("pin");
        QTest::newRow("bottom-ocr") << false << false << 0 << QString("ocr");
    }

    void draggingVisibleEndTwiceExportsExactPixels() {
        QFETCH(bool, horizontal);
        QFETCH(bool, at_start);
        QFETCH(int, screen_index);
        QFETCH(QString, output);
        const auto capture_engine = config_->get_capture_engine();
        const auto restore = qScopeGuard([&] { config_->set_capture_engine(capture_engine); });
        config_->set_capture_engine("mss");
        const auto page = horizontal ? page_.transformed(QTransform().rotate(-90)) : page_;
        auto *screen = QGuiApplication::screens().at(screen_index);
        qInfo() << screen->name() << screen->geometry() << "device pixel ratio" << screen->devicePixelRatio();
        const auto center = screen->availableGeometry().center();
        const QSize size = horizontal ? QSize(VIEW_H, PAGE_W) : QSize(PAGE_W, VIEW_H);
        const QRect core(center - QPoint(size.width() / 2, size.height() / 2), size);
        const int lead = at_start ? 40 : 80, tail = at_start ? 80 : 40;
        page_window_ = std::make_unique<PageWindow>(page);
        page_window_->setGeometry(horizontal ? core.adjusted(-lead, 0, tail, 0) : core.adjusted(0, -lead, 0, tail));
        const int offset = at_start ? 0 : PAGE_H - VIEW_H - lead - tail;
        if (horizontal) page_window_->scroll_to_x(offset);
        else page_window_->scroll_to(offset);
        page_window_->show();
        QTest::qWait(150);
        window_ = new ScrollCaptureWindow(core, nullptr, config_);
        window_->show();
        QTRY_VERIFY(window_->screenshots >= 1);
        auto *stitcher = window_->findChild<IncrementalStitcher *>();
        QVERIFY(stitcher);
        stitcher->export_image();
        QCoreApplication::processEvents();
        if (horizontal) {
            window_->toolbar->direction_btn->click();
            stitcher->export_image();
        }
        QTest::qWait(200);
        const int initial = offset + lead;
        const auto expected_image = [&](int extra) {
            const int first = initial - (at_start ? extra : 0);
            return horizontal ? page.copy(first, 0, VIEW_H + extra, PAGE_W) : page.copy(0, first, PAGE_W, VIEW_H + extra);
        };
        QCOMPARE(stitcher->export_image().convertToFormat(QImage::Format_RGB32), expected_image(0));
        const Qt::Edge edge = horizontal ? (at_start ? Qt::LeftEdge : Qt::RightEdge) : (at_start ? Qt::TopEdge : Qt::BottomEdge);
        for (int extra : {20, 40}) {
            drag_edge(edge, 20);
            const auto expected_rect = horizontal ? (at_start ? core.adjusted(-extra, 0, 0, 0) : core.adjusted(0, 0, extra, 0))
                                                  : (at_start ? core.adjusted(0, -extra, 0, 0) : core.adjusted(0, 0, 0, extra));
            QTRY_COMPARE(window_->capture_rect, expected_rect);
            const auto actual = stitcher->export_image().convertToFormat(QImage::Format_RGB32);
            const auto expected = expected_image(extra);
            QCOMPARE(actual, expected);
        }
        const auto expected = expected_image(40);
        const auto label = window_->toolbar->size_label->text();
        QVERIFY(label.contains(QString::number(expected.width())) && label.contains(QString::number(expected.height())));
        if (output == "pin") {
            window_->toolbar->pin_btn->click();
            QCOMPARE(PinManager::instance()->count(), 1);
            QCOMPARE(PinManager::instance()->get_all_pins().first()->base_pixmap().toImage().convertToFormat(QImage::Format_RGB32), expected);
            return;
        }
        if (output == "ocr") {
            auto captured = std::make_shared<std::promise<QImage>>();
            auto result = captured->get_future();
            set_recognizer_for_test([captured](const QImage &image) {
                captured->set_value(image);
                return OcrResult{100, "ok", {OcrLine{{QPointF(0, 0), QPointF(80, 0), QPointF(80, 16), QPointF(0, 16)}, "long text"}}, 0.0};
            });
            const auto restore_recognizer = qScopeGuard([] { set_recognizer_for_test({}); });
            config_->set_ocr_copy_directly_enabled(false);
            window_->toolbar->ocr_btn->click();
            QTRY_VERIFY(result.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
            QCOMPARE(result.get().convertToFormat(QImage::Format_RGB32), expected);
            TextRecognitionWindow *text_window = nullptr;
            QTRY_VERIFY(text_window = visible_top_level<TextRecognitionWindow>());
            text_window->close();
            return;
        }
        const QString path = dir_.filePath(QString("edge-%1-%2.png").arg(horizontal ? "horizontal" : "vertical", at_start ? "start" : "end"));
        SaveDialogAnswerer answer(path);
        window_->toolbar->save_btn->click();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path), 5000);
        QVERIFY(answer.answered);
        QCOMPARE(QImage(path).convertToFormat(QImage::Format_RGB32), expected);
    }

    void cancellingOrMisaligningAnEdgePreservesTheLastImage() {
        const auto capture_engine = config_->get_capture_engine();
        const auto restore = qScopeGuard([&] { config_->set_capture_engine(capture_engine); });
        config_->set_capture_engine("mss");
        page_window_ = std::make_unique<PageWindow>(page_);
        page_window_->setGeometry(area_.adjusted(0, -40, 0, 80));
        page_window_->show();
        QTest::qWait(150);
        window_ = new ScrollCaptureWindow(area_, nullptr, config_);
        window_->show();
        QTRY_VERIFY(window_->screenshots >= 1);
        auto *stitcher = window_->findChild<IncrementalStitcher *>();
        const auto before = stitcher->export_image();
        QTest::qWait(150);
        drag_edge(Qt::BottomEdge, 20, true);
        QCOMPARE(window_->capture_rect, area_);
        QCOMPARE(window_->geometry(), area_.adjusted(-3, -3, 3, 3));
        QCOMPARE(stitcher->export_image(), before);

        const QPoint press(window_->geometry().center().x(), window_->geometry().bottom() - 1);
        QTest::mousePress(window_, Qt::LeftButton, {}, window_->mapFromGlobal(press));
        QTest::mouseMove(window_, window_->mapFromGlobal(press + QPoint(0, 20)));
        page_window_->scroll_to(17);
        page_window_->repaint();
        DwmFlush();
        QTest::mouseRelease(window_, Qt::LeftButton, {}, window_->mapFromGlobal(press + QPoint(0, 20)));
        QTRY_VERIFY(window_->preview_warning_active);
        QCOMPARE(window_->capture_rect, area_);
        QCOMPARE(stitcher->export_image(), before);
    }

    void cancellingAQueuedExtensionReplyCannotCommitItsPixels() {
        auto *window = open();
        QVERIFY(window);
        auto *stitcher = window->findChild<IncrementalStitcher *>();
        const auto before = stitcher->export_image();
        QSignalSpy ready(stitcher, &IncrementalStitcher::extension_ready);
        const QPoint press(window->geometry().center().x(), window->geometry().bottom() - 1);
        const QPoint end = press + QPoint(0, 10);
        QTest::mousePress(window, Qt::LeftButton, {}, window->mapFromGlobal(press));
        QTest::mouseMove(window, window->mapFromGlobal(end));
        QMouseEvent up(QEvent::MouseButtonRelease, window->mapFromGlobal(end), end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(window, &up);
        QCOMPARE(stitcher->export_image(), before);
        QCOMPARE(ready.count(), 1);
        QVERIFY(ready.first()[1].toBool());
        // Send directly, without dispatching the queued candidate callback first.
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(window, &escape);
        QCoreApplication::processEvents();
        QCOMPARE(window->capture_rect, area_);
        QCOMPARE(stitcher->export_image(), before);
    }

    void switchingDirectionAfterSupplementStartsFromTheExpandedView() {
        page_window_ = std::make_unique<PageWindow>(page_);
        page_window_->setGeometry(area_.adjusted(0, -40, 0, 80));
        page_window_->show();
        QTest::qWait(150);
        window_ = new ScrollCaptureWindow(area_, nullptr, config_);
        window_->show();
        auto *stitcher = window_->findChild<IncrementalStitcher *>();
        QTRY_VERIFY(window_->screenshots >= 1);
        stitcher->export_image();
        QTest::qWait(150);
        drag_edge(Qt::BottomEdge, 20);
        QTRY_COMPARE(window_->capture_rect, area_.adjusted(0, 0, 0, 20));
        for (const auto &direction : {QString("horizontal"), QString("vertical")}) {
            window_->toolbar->direction_btn->click();
            QCOMPARE(window_->scroll_direction, direction);
            QCOMPARE(stitcher->export_image().convertToFormat(QImage::Format_RGB32), page_.copy(0, 40, PAGE_W, VIEW_H + 20));
            QCoreApplication::processEvents();
        }
        // After switching back the whole expanded view is one frame, so both ends grow.
        drag_edge(Qt::TopEdge, 20);
        QTRY_COMPARE(window_->capture_rect, area_.adjusted(0, -20, 0, 20));
        QCOMPARE(stitcher->export_image().convertToFormat(QImage::Format_RGB32), page_.copy(0, 20, PAGE_W, VIEW_H + 40));
    }

    void scrollingStitchesALongImageAndPinsIt() {
        auto *window = open();
        QVERIFY(window);
        QVERIFY(window->toolbar->isVisible());
        QVERIFY(window->preview_panel && window->preview_panel->isVisible());
        // The frame sits just outside the captured area.
        QCOMPARE(window->geometry(), area_.adjusted(-3, -3, 3, 3));
        QVERIFY(scroll_in_steps({100, 200, 300, 400, 500}));
        QSignalSpy finished(window, &ScrollCaptureWindow::finished);
        QPointer<ScrollCaptureWindow> alive = window;
        window->toolbar->pin_btn->click();
        QCOMPARE(finished.count(), 1);
        const auto pins = PinManager::instance()->get_all_pins();
        QCOMPARE(pins.size(), 1);
        const auto image = pins.first()->base_pixmap().toImage().convertToFormat(QImage::Format_RGB32);
        // The page from the top to the last view: 300 + 500 rows.
        QCOMPARE(image.width(), PAGE_W);
        QVERIFY2(std::abs(image.height() - (VIEW_H + 500)) <= 2, qPrintable(QString::number(image.height())));
        // Rows match the page they came from.
        int matching = 0;
        for (int y = 0; y < std::min(image.height(), VIEW_H + 500); y += 37)
            matching += image.copy(0, y, PAGE_W, 1) == page_.copy(0, y, PAGE_W, 1).convertToFormat(QImage::Format_RGB32);
        QVERIFY2(matching >= 18, qPrintable(QString::number(matching)));
        // The pin sits centred on the capture area.
        QVERIFY(std::abs(pins.first()->geometry().center().x() - area_.center().x()) <= 1);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!alive);
    }

    void finishingCopiesTheLongImage() {
        auto *window = open();
        QVERIFY(window);
        QVERIFY(scroll_in_steps({150, 300}));
        clear_clipboard();
        window->toolbar->finish_btn->click();
        QVERIFY(wait_for_clipboard_writes(10.0));
        const auto size = clipboard_bitmap_size();
        QCOMPARE(size.width(), PAGE_W);
        QVERIFY2(std::abs(size.height() - (VIEW_H + 300)) <= 2, qPrintable(QString::number(size.height())));
        QCOMPARE(PinManager::instance()->count(), 0);
    }

    // The button ends the capture and hands the whole long image to the result window, like the
    // capture toolbar's button: nothing is pinned, saved or copied as an image.
    void recognizingTextReadsTheLongImage() {
        auto *window = open();
        QVERIFY(window);
        QVERIFY(scroll_in_steps({150, 300}));
        std::atomic_int width = 0, height = 0;
        set_recognizer_for_test([&](const QImage &image) {
            width = image.width();
            height = image.height();
            return OcrResult{100, "ok", {OcrLine{{QPointF(0, 0), QPointF(80, 0), QPointF(80, 16), QPointF(0, 16)}, "long text"}}, 0.0};
        });
        const auto restore = qScopeGuard([] { set_recognizer_for_test({}); });
        config_->set_ocr_copy_directly_enabled(false);
        clear_clipboard();
        QSignalSpy finished(window, &ScrollCaptureWindow::finished);
        window->toolbar->ocr_btn->click();
        QCOMPARE(finished.count(), 1);
        TextRecognitionWindow *result = nullptr;
        QTRY_VERIFY(result = visible_top_level<TextRecognitionWindow>());
        QTRY_VERIFY(height > 0);
        // The page from the top to the last view: 300 + 300 rows.
        QCOMPARE(width.load(), PAGE_W);
        QVERIFY2(std::abs(height - (VIEW_H + 300)) <= 2, qPrintable(QString::number(height)));
        result->close();
        QCOMPARE(PinManager::instance()->count(), 0);
        QCOMPARE(clipboard_bitmap_size(), QSize());
    }

    // Save ends the capture and asks where to save the whole long image, named like an auto-saved
    // one, as the capture toolbar's Save does: nothing is pinned or copied.
    void savingAsksWhereToPutTheLongImage() {
        auto *window = open();
        QVERIFY(window);
        QVERIFY(scroll_in_steps({150, 300}));
        config_->set_save_dialog_style("modern");
        clear_clipboard();
        const QString folder = dir_.filePath("saved");
        QVERIFY(QDir().mkpath(folder));
        // The save window is modal, so it is answered from a timer inside its own loop.
        QString suggested;
        QTimer answer;
        answer.setInterval(20);
        connect(&answer, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<SaveImageDialog *>(QApplication::activeModalWidget());
            if (!dialog || !dialog->isVisible()) return;
            answer.stop();
            suggested = dialog->name_edit->text();
            dialog->directory_edit->setText(QDir::toNativeSeparators(folder));
            dialog->name_edit->setText("long");
            dialog->format_combo->setCurrentIndex(dialog->format_combo->findData("PNG"));
            dialog->save_button->click();
        });
        answer.start();
        QSignalSpy finished(window, &ScrollCaptureWindow::finished);
        window->toolbar->save_btn->click();
        QCOMPARE(finished.count(), 1);
        const QString path = QDir(folder).filePath("long.png");
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path), 5000);
        QVERIFY2(suggested.startsWith(i18n_tr("Long Screenshot", "ScrollCaptureWindow") + "_"), qPrintable(suggested));
        QVERIFY2(QRegularExpression("_\\d{8}_\\d{6}$").match(suggested).hasMatch(), qPrintable(suggested));
        const QImage saved(path);
        // The page from the top to the last view: 300 + 300 rows.
        QCOMPARE(saved.width(), PAGE_W);
        QVERIFY2(std::abs(saved.height() - (VIEW_H + 300)) <= 2, qPrintable(QString::number(saved.height())));
        QCOMPARE(PinManager::instance()->count(), 0);
        QCOMPARE(clipboard_bitmap_size(), QSize());
    }

    // The page runs to its end at 1500 pixels a second without pausing: frames are taken while
    // it moves, each still overlapping the one before.
    void aFastScrollWithoutPausesJoins() {
        auto *window = open();
        QVERIFY(window);
        const int end = PAGE_H - VIEW_H;
        QElapsedTimer clock;
        QTimer mover;
        mover.setInterval(8);
        connect(&mover, &QTimer::timeout, this, [&] {
            page_window_->scroll_to(std::min(end, static_cast<int>(clock.elapsed() * 3 / 2)));
            if (page_window_->top() == end) mover.stop();
        });
        clock.start();
        mover.start();
        QTRY_VERIFY_WITH_TIMEOUT(!mover.isActive(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(window->toolbar->size_label->text().contains(QString::number(PAGE_H)), 3000);
        window->toolbar->pin_btn->click();
        const auto pins = PinManager::instance()->get_all_pins();
        QCOMPARE(pins.size(), 1);
        const auto image = pins.first()->base_pixmap().toImage().convertToFormat(QImage::Format_RGB32);
        QCOMPARE(image.size(), page_.size());
        int matching = 0;
        for (int y = 0; y < PAGE_H; y += 37)
            matching += image.copy(0, y, PAGE_W, 1) == page_.copy(0, y, PAGE_W, 1).convertToFormat(QImage::Format_RGB32);
        QVERIFY2(matching >= 40, qPrintable(QString::number(matching)));
    }

    void scrollingBackDoesNotDuplicateTheTop() {
        auto *window = open();
        QVERIFY(window);
        QVERIFY(scroll_in_steps({200, 400, 200}));
        QVERIFY(window->toolbar->size_label->text().contains(QString::number(VIEW_H + 400)));
        window->toolbar->cancel_btn->click();
    }

    void cancellingLeavesNothingBehind() {
        auto *window = open();
        QVERIFY(window);
        QVERIFY(scroll_in_steps({100}));
        QSignalSpy cancelled(window, &ScrollCaptureWindow::cancelled);
        QPointer<FloatingToolbar> toolbar = window->toolbar;
        clear_clipboard();
        window->toolbar->cancel_btn->click();
        QCOMPARE(cancelled.count(), 1);
        QVERIFY(!toolbar || !toolbar->isVisible());
        QCOMPARE(PinManager::instance()->count(), 0);
        QTest::qWait(200);
        QCOMPARE(clipboard_bitmap_size(), QSize());
    }

    void cropKeepsOnlyUpToTheCurrentView() {
        auto *window = open();
        QVERIFY(window);
        QVERIFY(scroll_in_steps({150, 300, 450}));
        QVERIFY(scroll_in_steps({300}));
        QTimer::singleShot(0, qApp, [] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            // The first entry crops off what lies below the current view.
            menu->setActiveAction(menu->actions().value(1));
            QTest::keyClick(menu, Qt::Key_Return);
        });
        window->toolbar->crop_btn->click();
        QTRY_VERIFY_WITH_TIMEOUT(window->toolbar->size_label->text().contains(QString::number(VIEW_H + 300)), 5000);
        window->toolbar->cancel_btn->click();
    }

    // Switching direction starts a new long image from the current view; horizontally its
    // preview is scaled to the panel's height, in a strip a screen wide.
    void theDirectionButtonSwitchesToHorizontal() {
        auto *window = open();
        QVERIFY(window);
        auto *panel = window->preview_panel.data();
        const QRect screen = window->screen()->geometry();
        const int side = panel->panel_size().width();
        QVERIFY(scroll_in_steps({100, 200}));
        QTRY_VERIFY(window->toolbar->size_label->text().contains(QString::number(VIEW_H + 200)));
        QCOMPARE(window->scroll_direction, QString("vertical"));
        window->toolbar->direction_btn->click();
        QCOMPARE(window->scroll_direction, QString("horizontal"));
        QTRY_VERIFY(std::abs(panel->panel_size().width() - PAGE_W * side / VIEW_H) <= 1);
        QCOMPARE(panel->panel_size().height(), side);
        QCOMPARE(panel->geometry().left(), screen.left());
        QCOMPARE(panel->geometry().width(), screen.width());
        QCOMPARE(window->toolbar->size_label->text(), QString::fromUtf8("%1 × %2").arg(PAGE_W).arg(VIEW_H));
        window->toolbar->direction_btn->click();
        QCOMPARE(window->scroll_direction, QString("vertical"));
        QTRY_COMPARE(panel->geometry().height(), screen.height());
        // The strip changes orientation before the worker returns its first thumbnail.
        QTRY_COMPARE(panel->panel_size().width(), side);
        QCOMPARE(window->toolbar->size_label->text(), QString::fromUtf8("%1 × %2").arg(PAGE_W).arg(VIEW_H));
        window->toolbar->cancel_btn->click();
    }

    void thePreviewPanelShowsWarningsUntilCleared() {
        PreviewPanel panel;
        panel.show();
        panel.update_preview(page_.scaled(80, 320), "vertical", std::make_pair(200, 320));
        QVERIFY(!panel.grab().isNull());
        panel.update_preview(page_.scaled(80, 2000), "vertical", std::make_pair(1800, 2000));
        QVERIFY(!panel.grab().isNull());
        panel.update_preview(page_.scaled(2000, 80).transformed(QTransform().rotate(90)), "horizontal", std::make_pair(0, 120));
        QVERIFY(!panel.grab().isNull());
        panel.update_preview(QImage(), "vertical");
        QVERIFY(!panel.grab().isNull());
        panel.show_warning("Couldn't join");
        QVERIFY(panel.warning_label->isVisible());
        QCOMPARE(panel.warning_label->text(), QString("Couldn't join"));
        panel.clear_warning();
        QVERIFY(!panel.warning_label->isVisible());
    }

    // The preview grows inside a strip a screen long that stays put, its bottom on the region's.
    // A preview taller than the region reaches the toolbar above it when the toolbar is wider than
    // the region, and then moves aside to keep the buttons clear; this region is tall enough.
    void thePreviewGrowsWithoutMovingItsWindow() {
        const QRect saved = area_;
        const auto restore = qScopeGuard([this, saved] { area_ = saved; });
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        area_ = QRect(available.center() - QPoint(PAGE_W / 2, 300), QSize(PAGE_W, 600));
        auto *window = open();
        QVERIFY(window);
        auto *panel = window->preview_panel.data();
        const QRect screen = window->screen()->geometry();
        const QRect strip = panel->geometry();
        QCOMPARE(strip.top(), screen.top());
        QCOMPARE(strip.height(), screen.height());
        const int before = panel->panel_size().height();
        QVERIFY(scroll_in_steps({100, 200, 300, 400, 500}));
        QTRY_VERIFY_WITH_TIMEOUT(panel->panel_size().height() > before, 5000);
        QVERIFY2(panel->panel_geometry().top() > window->geometry().top(), "the preview outgrew the region");
        QCOMPARE(panel->geometry(), strip);
        QCOMPARE(panel->panel_geometry().bottom(), window->geometry().bottom());
        window->toolbar->cancel_btn->click();
    }

    // The strip covers whatever lies beside the panel, such as the toolbar; real clicks go through it.
    void clicksGoThroughThePreviewStrip() {
        QPushButton button("Under the strip");
        button.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        button.setGeometry(area_.left(), area_.top(), 160, 40);
        button.show();
        PreviewPanel panel;
        panel.place(area_.topLeft() + QPoint(0, 200), QGuiApplication::primaryScreen()->geometry());
        panel.show();
        panel.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const QPoint target = button.geometry().center();
        QVERIFY(panel.geometry().contains(target) && !panel.panel_geometry().contains(target));
        QSignalSpy clicked(&button, &QPushButton::clicked);
        const QPoint restore = QCursor::pos();
        QCursor::setPos(target);
        INPUT clicks[2] = {};
        clicks[0].type = clicks[1].type = INPUT_MOUSE;
        clicks[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        clicks[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, clicks, sizeof(INPUT));
        QTRY_COMPARE_WITH_TIMEOUT(clicked.count(), 1, 3000);
        QCursor::setPos(restore);
    }

    // The panel fades once the pointer has rested on it for a moment, and what lies beneath
    // shows through. Read from the screen, so the compositor's result counts.
    void thePreviewFadesWhileThePointerRestsOnIt() {
        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        QWidget backdrop(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        QPalette palette;
        palette.setColor(QPalette::Window, Qt::red);
        backdrop.setPalette(palette);
        backdrop.setAutoFillBackground(true);
        backdrop.setGeometry(area_);
        backdrop.show();
        PreviewPanel panel;
        QImage blue(600, panel.panel_size().height(), QImage::Format_RGB32);
        blue.fill(Qt::blue);
        panel.update_preview(blue, "horizontal");
        panel.place(area_.topLeft(), screen);
        panel.show();
        panel.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        const QPoint probe = area_.topLeft() + QPoint(95, 100);
        const auto red_on_screen = [probe] { return QGuiApplication::primaryScreen()->grabWindow(0, probe.x(), probe.y(), 1, 1).toImage().pixelColor(0, 0).red(); };
        QTRY_VERIFY(red_on_screen() < 60);
        const QPoint restore = QCursor::pos();
        const auto put_back = qScopeGuard([restore] { QCursor::setPos(restore); });
        QCursor::setPos(probe + QPoint(20, 20));
        // A pointer passing over leaves it alone.
        QTest::qWait(100);
        QVERIFY(red_on_screen() < 60);
        QTRY_VERIFY(red_on_screen() > 150);
        QCursor::setPos(area_.bottomRight() + QPoint(40, 40));
        QTRY_VERIFY(red_on_screen() < 60);
    }

    // ---- Automatic scrolling ----

    // The scroller's rhythm with stand-ins for capture, wheel and cursor: scroll, capture,
    // continue while the page moves, stop after three steps without movement.
    void theScrollerStopsWhenThePageStopsMoving() {
        QList<std::pair<int, bool>> wheel;
        QPoint cursor(5, 5);
        AutoScroller::Done pending;
        int next = 1;
        AutoScroller scroller([&](AutoScroller::Done done) { pending = std::move(done); }, [&](int delta, bool horizontal) { wheel.append({delta, horizontal}); },
                              {[&] { return cursor; }, [&](const QPoint &point) { cursor = point; }});
        QSignalSpy stopped(&scroller, &AutoScroller::stopped);
        const auto frame = [](int index, int top, bool ok = true) {
            FrameResult result;
            result.ok = ok;
            result.index = index;
            result.top = top;
            return result;
        };
        // The first frame is captured before scrolling starts.
        scroller.on_frame(frame(0, 0));
        scroller.start(QPoint(200, 150), false, true);
        QVERIFY(scroller.running());
        QCOMPARE(cursor, QPoint(200, 150));
        QCOMPARE(wheel.size(), 1);
        QCOMPARE(wheel.first(), std::make_pair(-WHEEL_DELTA, false));
        // Each step: wait to settle, capture, report the stitch result.
        const auto step = [&](int top) {
            if (!QTest::qWaitFor([&] { return bool(pending); }, 2000)) return false;
            auto done = std::move(pending);
            pending = nullptr;
            const int index = next++;
            done(index);
            scroller.on_frame(frame(index, top));
            return true;
        };
        QVERIFY(step(100));
        QVERIFY(step(200));
        for (int idle = 0; idle < IDLE_STEPS_AT_END; ++idle) QVERIFY(step(200));
        QCOMPARE(stopped.size(), 1);
        QCOMPARE(stopped.first().first().toString(), QString("end"));
        QVERIFY(!scroller.running());
        QCOMPARE(wheel.size(), 2 + IDLE_STEPS_AT_END);
        // Results not waited for (manual frames) never stop it; stopping twice reports once.
        scroller.stop();
        QCOMPARE(stopped.size(), 1);
    }

    void theScrollerStopsOnMouseMovesAndFailures() {
        QList<std::pair<int, bool>> wheel;
        QPoint cursor;
        AutoScroller::Done pending;
        AutoScroller scroller([&](AutoScroller::Done done) { pending = std::move(done); }, [&](int delta, bool horizontal) { wheel.append({delta, horizontal}); },
                              {[&] { return cursor; }, [&](const QPoint &point) { cursor = point; }});
        QSignalSpy stopped(&scroller, &AutoScroller::stopped);
        // Moving the mouse away stops it.
        scroller.start(QPoint(50, 50), true, true);
        QCOMPARE(wheel.last(), std::make_pair(WHEEL_DELTA, true));
        cursor += QPoint(10, 0);
        QTRY_COMPARE(stopped.size(), 1);
        QCOMPARE(stopped.last().first().toString(), QString("mouse"));
        // A failed capture.
        pending = nullptr;
        scroller.start(QPoint(50, 50), false, false);
        QCOMPARE(wheel.last(), std::make_pair(WHEEL_DELTA, false));
        QTRY_VERIFY(bool(pending));
        pending(std::nullopt);
        QCOMPARE(stopped.last().first().toString(), QString("failed"));
        // A failed stitch of the awaited frame.
        pending = nullptr;
        scroller.start(QPoint(50, 50), false, true);
        QTRY_VERIFY(bool(pending));
        pending(7);
        FrameResult failed;
        failed.index = 7;
        scroller.on_frame(failed);
        QCOMPARE(stopped.last().first().toString(), QString("failed"));
        // Without a direction it follows the latest movement: the page last went up.
        FrameResult up;
        up.ok = true;
        up.index = 8;
        up.top = 300;
        scroller.on_frame(up);
        up.index = 9;
        up.top = 200;
        scroller.on_frame(up);
        pending = nullptr;
        scroller.start(QPoint(50, 50), false);
        QCOMPARE(wheel.last(), std::make_pair(WHEEL_DELTA, false));
        // Flushing captures the pending step at once; cancelling stops without one.
        QVERIFY(!pending);
        scroller.flush();
        QVERIFY(bool(pending));
        scroller.cancel();
        QCOMPARE(stopped.last().first().toString(), QString("user"));
        pending = nullptr;
        scroller.start(QPoint(50, 50), false, true);
        scroller.cancel();
        QTest::qWait(SETTLE_MS + 50);
        QVERIFY(!pending);
        wheel_follows_focus();
    }

    // The toolbar button scrolls the page under the capture through real wheel input until its end.
    void autoScrollScrollsThePageToItsEnd() {
        auto *window = open();
        QVERIFY(window);
        QSignalSpy toggled(window->toolbar->auto_scroll_btn, &QPushButton::toggled);
        window->toolbar->auto_scroll_btn->click();
        QVERIFY(window->toolbar->auto_scroll_btn->isChecked());
        // Stops at the page end, or early if someone moves the mouse.
        QTRY_VERIFY_WITH_TIMEOUT(!window->toolbar->auto_scroll_btn->isChecked(), 30000);
        if (page_window_->top() < PAGE_H - VIEW_H) QSKIP("the mouse moved during auto scrolling");
        // The long image covers the whole page.
        const auto stitched_height = [window] {
            int best = 0;
            const QRegularExpression number(R"(\d+)");
            for (auto it = number.globalMatch(window->toolbar->size_label->text()); it.hasNext();) best = std::max(best, it.next().captured().toInt());
            return best;
        };
        QTRY_VERIFY2_WITH_TIMEOUT(std::abs(stitched_height() - PAGE_H) <= 3, qPrintable(window->toolbar->size_label->text()), 10000);
        // Clicking again while running stops it.
        page_window_->scroll_to(0);
        window->toolbar->auto_scroll_btn->click();
        QVERIFY(window->toolbar->auto_scroll_btn->isChecked());
        window->toolbar->auto_scroll_btn->click();
        QVERIFY(!window->toolbar->auto_scroll_btn->isChecked());
        window->toolbar->cancel_btn->click();
    }

    // Horizontal mode: each Shift press scrolls the page under the pointer one notch right,
    // and the long image grows sideways.
    void shiftScrollsSidewaysInHorizontalMode() {
        page_ = synthetic_page().transformed(QTransform().rotate(-90)).copy(0, 0, PAGE_H, VIEW_H);
        auto *window = open();
        QVERIFY(window);
        window->toolbar->direction_btn->click();
        QCOMPARE(window->scroll_direction, QString("horizontal"));
        const QPoint restore = QCursor::pos();
        QCursor::setPos(area_.center());
        for (int press = 0; press < 4; ++press) {
            const int before = window->screenshots;
            emit input_hub()->key("stitch", 0xA0, true);
            // Held, it does not repeat.
            emit input_hub()->key("stitch", 0xA0, true);
            emit input_hub()->key("stitch", 0xA0, false);
            QTRY_VERIFY_WITH_TIMEOUT(window->screenshots > before, 5000);
        }
        QCursor::setPos(restore);
        QCOMPARE(page_window_->left(), 4 * 60);
        // Keys of other watchers or other keys are ignored.
        emit input_hub()->key("other", 0xA0, true);
        emit input_hub()->key("stitch", 0x41, true);
        QTest::qWait(300);
        QCOMPARE(page_window_->left(), 4 * 60);
        QTRY_VERIFY_WITH_TIMEOUT(window->toolbar->size_label->text().contains(QString::number(PAGE_W + 4 * 60)), 5000);
        window->toolbar->cancel_btn->click();
        page_ = synthetic_page();
    }

    // Horizontally the preview goes above the toolbar, leaving the page's scrollbar below the
    // region uncovered, and stays put as it grows past the toolbar's width.
    void theHorizontalPreviewGoesAboveTheToolbar() {
        page_ = synthetic_page().transformed(QTransform().rotate(-90)).copy(0, 0, PAGE_H, VIEW_H);
        const auto restore = qScopeGuard([this] { page_ = synthetic_page(); });
        auto *window = open();
        QVERIFY(window);
        window->toolbar->direction_btn->click();
        auto *panel = window->preview_panel.data();
        const QRect bar = window->toolbar->geometry();
        QVERIFY(bar.bottom() < window->geometry().top());
        QTRY_VERIFY(panel->panel_geometry().bottom() < bar.top());
        const QRect before = panel->panel_geometry();
        for (int left = 150; left <= PAGE_H - PAGE_W; left += 150) {
            const int shots = window->screenshots;
            page_window_->scroll_to_x(left);
            QTRY_VERIFY_WITH_TIMEOUT(window->screenshots > shots, 5000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(panel->panel_geometry().left() < bar.left(), 5000);
        QCOMPARE(panel->panel_geometry().top(), before.top());
        QCOMPARE(panel->panel_geometry().right(), before.right());
        window->toolbar->cancel_btn->click();
    }

    // A frame that overlaps nothing is dropped; the warning waits a moment, then shows until a frame joins again.
    void anUnjoinableFrameWarnsUntilScrolledBack() {
        auto *window = open();
        QVERIFY(window);
        QVERIFY(scroll_in_steps({120}));
        QImage stranger(PAGE_W, PAGE_H, QImage::Format_RGB32);
        stranger.fill(Qt::white);
        QPainter painter(&stranger);
        QRandomGenerator random(99);
        for (int i = 0; i < 400; ++i) painter.fillRect(random.bounded(PAGE_W), random.bounded(PAGE_H), 12, 3, QColor(random.bounded(200), 0, random.bounded(200)));
        painter.end();
        const int before = window->screenshots;
        page_window_->set_page(stranger);
        QTRY_VERIFY_WITH_TIMEOUT(window->preview_panel->warning_label->isVisible(), 8000);
        QCOMPARE(window->preview_panel->warning_label->text(), i18n_tr("Couldn't join. Scroll back", "ScrollCaptureWindow"));
        QCOMPARE(window->screenshots, before);
        page_window_->set_page(page_);
        page_window_->scroll_to(180);
        QTRY_VERIFY_WITH_TIMEOUT(!window->preview_panel->warning_label->isVisible(), 8000);
        window->toolbar->cancel_btn->click();
    }

    // The toolbar goes above the region, below it at the top of the screen, beside it when the region is full height.
    void theToolbarAvoidsTheRegion() {
        const auto available = QGuiApplication::primaryScreen()->availableGeometry();
        const QRect saved = area_;
        auto *window = open();
        QVERIFY(window);
        QVERIFY(window->toolbar->geometry().bottom() < window->geometry().top() || window->toolbar->geometry().top() > window->geometry().bottom());
        window->toolbar->cancel_btn->click();
        cleanup();
        area_ = QRect(available.center().x() - PAGE_W / 2, available.top() + 4, PAGE_W, VIEW_H);
        window = open();
        QVERIFY(window);
        QVERIFY(window->toolbar->geometry().top() > window->geometry().bottom());
        window->toolbar->cancel_btn->click();
        cleanup();
        area_ = QRect(available.center().x() - PAGE_W / 2, available.top() + 2, PAGE_W, available.height() - 4);
        window = open();
        QVERIFY(window);
        const auto bar = window->toolbar->geometry();
        QVERIFY(bar.right() < window->geometry().left() || bar.left() > window->geometry().right());
        QVERIFY(window->preview_panel->isVisible());
        window->toolbar->cancel_btn->click();
        area_ = saved;
    }

    // The capture window's long screenshot and GIF buttons hand the selection to their windows.
    void theCaptureWindowStartsLongScreenshotsAndRecordings() {
        const QRect screen = QGuiApplication::primaryScreen()->geometry();
        QImage image(screen.size(), QImage::Format_RGB32);
        image.fill(Qt::gray);
        QPointer<ScreenshotWindow> capture = new ScreenshotWindow(config_, image, QRectF(screen));
        // Without a selection a warning explains why nothing starts.
        capture->start_long_screenshot_mode();
        QVERIFY(capture->session_active());
        for (auto *widget : QApplication::topLevelWidgets())
            if (qobject_cast<QDialog *>(widget) && widget->isVisible()) widget->close();
        capture->scene->preset_selection(QRectF(area_));
        capture->toolbar->long_screenshot_btn->click();
        QVERIFY(!capture->session_active());
        auto *scroll = qobject_cast<ScrollCaptureWindow *>(qApp->property("_scroll_window").value<QObject *>());
        QVERIFY(scroll);
        QCOMPARE(scroll->capture_rect, area_);
        window_ = scroll;
        QTRY_VERIFY_WITH_TIMEOUT(scroll->screenshots >= 1, 3000);
        // A long screenshot started from a new capture replaces the running one.
        const QPointer<ScrollCaptureWindow> first_scroll = scroll;
        const QRect other = area_.translated(20, 20);
        QPointer<ScreenshotWindow> again = new ScreenshotWindow(config_, image, QRectF(screen));
        again->scene->preset_selection(QRectF(other));
        again->toolbar->long_screenshot_btn->click();
        scroll = active_scroll_capture();
        QVERIFY(scroll && scroll != first_scroll);
        QCOMPARE(scroll->capture_rect, other);
        window_ = scroll;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!first_scroll);
        QTRY_VERIFY(!again);
        QTRY_VERIFY_WITH_TIMEOUT(scroll->screenshots >= 1, 3000);
        scroll->toolbar->cancel_btn->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTRY_VERIFY(!qApp->property("_scroll_window").value<QObject *>());

        // Each capture has its own window; the ended one is gone.
        QVERIFY(!capture);
        capture = new ScreenshotWindow(config_, image, QRectF(screen));
        capture->start_gif_record_mode();
        QVERIFY(capture->session_active());
        for (auto *widget : QApplication::topLevelWidgets())
            if (qobject_cast<QDialog *>(widget) && widget->isVisible()) widget->close();
        capture->scene->preset_selection(QRectF(area_));
        capture->toolbar->gif_btn->click();
        QVERIFY(!capture->session_active());
        QVERIFY(active_gif_window());
        // A second recording replaces the first while it is idle.
        QPointer<GifRecordWindow> first = active_gif_window();
        capture = new ScreenshotWindow(config_, image, QRectF(screen));
        capture->scene->preset_selection(QRectF(area_));
        capture->toolbar->gif_btn->click();
        QVERIFY(active_gif_window() && active_gif_window() != first.data());
        active_gif_window()->close_all();
        QTRY_VERIFY(!active_gif_window());
        delete capture.data();
        shutdown_hdr_session();
    }
};

QTEST_MAIN(ScrollCaptureTests)
#include "test_scroll_capture.moc"
