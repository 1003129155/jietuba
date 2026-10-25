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
#include <QApplication>
#include <QCursor>
#include <QDialog>
#include <QDir>
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
#include <QTemporaryDir>
#include <QTest>
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
    explicit PageWindow(const QImage &page) : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool), page_(page) {
        setAttribute(Qt::WA_ShowWithoutActivating);
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
    ScrollCaptureWindow *open() {
        page_window_ = std::make_unique<PageWindow>(page_);
        page_window_->setGeometry(area_);
        page_window_->show();
        QTest::qWait(150);
        window_ = new ScrollCaptureWindow(area_, nullptr, config_);
        window_->show();
        if (!QTest::qWaitFor([&] { return window_ && window_->screenshots >= 1; }, 3000)) return nullptr;
        // The change watch starts after the first frame and takes its own first look as the
        // baseline; a person starts scrolling only once the window is up.
        QTest::qWait(CHANGE_WATCH_IDLE_MS + 150);
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
        page_window_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QTest::qWait(50);
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
        QCOMPARE(panel->panel_size().width(), side);
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
