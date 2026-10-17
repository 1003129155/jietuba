#include "capture/system_cursor.h"
#include "capture/window_finder.h"
#include <QApplication>
#include <QCursor>
#include <QScreen>
#include <QTest>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

using namespace jietuba;

namespace {
WindowRect to_window_rect(const QRect &rect) { return {rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1}; }

// A topmost titled window at a known place on the primary screen.
std::unique_ptr<QWidget> probe_window(const QString &title, const QRect &geometry, Qt::WindowFlags extra = {}) {
    auto window = std::make_unique<QWidget>(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | extra);
    window->setWindowTitle(title);
    window->setGeometry(geometry);
    window->show();
    return window;
}

QRect area(int dx, int dy, int width, int height) {
    const auto available = QGuiApplication::primaryScreen()->availableGeometry();
    return QRect(available.center() + QPoint(dx, dy), QSize(width, height));
}

const WindowInfo *find(const WindowFinder &finder, const QWidget &window) {
    for (const auto &info : finder.windows)
        if (info.hwnd == static_cast<quintptr>(window.winId())) return &info;
    return nullptr;
}
}

// Smart selection's window and control lookup on the real desktop; the screen is 100% scaled
// in this setup, so Qt and physical pixels agree.
class CaptureFinderTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows: the finders read real windows");
        if (!qFuzzyCompare(QGuiApplication::primaryScreen()->devicePixelRatio(), 1.0)) QSKIP("expects a 100% scaled primary screen");
    }

    void windowsAreListedTopFirstWithTheirRects() {
        auto window = probe_window("jietuba finder probe", area(-200, -150, 300, 200));
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        WindowFinder finder;
        finder.debug = true;
        finder.find_windows();
        const auto *info = find(finder, *window);
        QVERIFY(info);
        QCOMPARE(info->title, QString("jietuba finder probe"));
        QCOMPARE(info->rect, to_window_rect(window->geometry()));
        QCOMPARE(get_window_rect_no_shadow(info->hwnd), info->rect);
        const auto centre = window->geometry().center();
        QCOMPARE(finder.find_window_at_point(centre.x(), centre.y()), info->rect);
        QCOMPARE(finder.find_window_at_point_info(centre.x(), centre.y())->hwnd, info->hwnd);
        // The monitor level is the screen under the point.
        QCOMPARE(finder.find_monitor_rect_at_point(centre.x(), centre.y()), to_window_rect(QGuiApplication::primaryScreen()->geometry()));
        // Offsets shift every rect into the overlay's coordinates.
        finder.set_screen_offset(100, 50);
        finder.find_windows();
        info = find(finder, *window);
        QVERIFY(info);
        QCOMPARE(info->rect, to_window_rect(window->geometry().translated(-100, -50)));
        // Outside every window the fallback (or the whole desktop) answers.
        const WindowRect fallback{1, 2, 3, 4};
        QCOMPARE(finder.find_window_at_point(-50000, -50000, fallback), fallback);
        const auto desktop = finder.find_window_at_point(-50000, -50000);
        QCOMPARE(desktop[0], GetSystemMetrics(SM_XVIRTUALSCREEN));
        QVERIFY(!finder.find_window_at_point_info(-50000, -50000));
        finder.clear();
        QVERIFY(finder.windows.empty());
        QVERIFY(is_smart_selection_available());
    }

    void toolUntitledTinyAndClickThroughWindowsAreLeftOut() {
        auto tool = probe_window("jietuba tool probe", area(-300, -200, 200, 120), Qt::Tool);
        // An empty title would show the application name instead; blank counts as untitled.
        auto untitled = probe_window("   ", area(-60, -200, 200, 120));
        auto tiny = probe_window("jietuba tiny probe", area(200, -200, 20, 20));
        auto through = probe_window("jietuba through probe", area(-300, 50, 200, 120), Qt::WindowTransparentForInput);
        for (auto *window : {tool.get(), untitled.get(), tiny.get(), through.get()}) QVERIFY(QTest::qWaitForWindowExposed(window));
        WindowFinder finder;
        finder.find_windows();
        for (auto *window : {tool.get(), untitled.get(), tiny.get(), through.get()}) QVERIFY2(!find(finder, *window), qPrintable(window->windowTitle()));
    }

    // A window hidden behind another can never be picked, so it is not worth scanning.
    void exposedAreaRanksAndDropsCoveredWindows() {
        auto back = probe_window("jietuba back probe", area(-150, -100, 200, 150));
        QVERIFY(QTest::qWaitForWindowExposed(back.get()));
        auto front = probe_window("jietuba front probe", area(-200, -150, 400, 300));
        QVERIFY(QTest::qWaitForWindowExposed(front.get()));
        front->raise();
        QApplication::processEvents();
        WindowFinder finder;
        finder.find_windows();
        QVERIFY(find(finder, *back));
        QVERIFY(find(finder, *front));
        const auto ranked = finder.windows_by_exposed_area();
        QVERIFY(std::find(ranked.begin(), ranked.end(), static_cast<quintptr>(front->winId())) != ranked.end());
        QVERIFY(std::find(ranked.begin(), ranked.end(), static_cast<quintptr>(back->winId())) == ranked.end());
    }

    // Windows on other virtual desktops and UWP apps the system suspended after closing report
    // visible, yet DWM keeps them off screen.
    void cloakedWindowsAreLeftOut() {
        auto window = probe_window("jietuba cloak probe", area(-150, -100, 300, 200));
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        const auto hwnd = reinterpret_cast<HWND>(window->winId());
        const auto cloak = [&](BOOL on) { return SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &on, sizeof on)); };
        WindowFinder finder;
        QVERIFY(!is_window_cloaked(window->winId()));
        QVERIFY(cloak(TRUE));
        QVERIFY(is_window_cloaked(window->winId()));
        finder.find_windows();
        QVERIFY(!find(finder, *window));
        QVERIFY(cloak(FALSE));
        QVERIFY(!is_window_cloaked(window->winId()));
        finder.find_windows();
        QVERIFY(find(finder, *window));
        QVERIFY(!is_window_cloaked(0));
    }

    void theWindowUnderTheCursorIsFound() {
        auto window = probe_window("jietuba cursor probe", area(-150, -100, 300, 200));
        QVERIFY(QTest::qWaitForWindowExposed(window.get()));
        window->raise();
        const QPoint restore = QCursor::pos();
        QCursor::setPos(window->geometry().center());
        const auto rect = find_window_at_cursor();
        QCursor::setPos(restore);
        QVERIFY(rect);
        QCOMPARE(*rect, to_window_rect(window->geometry()));
    }

    // The pointer is recorded and drawn into a capture where it was on screen.
    void theSystemPointerIsDrawnIntoTheImage() {
        const QPoint restore = QCursor::pos();
        const auto centre = QGuiApplication::primaryScreen()->geometry().center();
        // The program under the pointer decides whether it shows; a window of this test does.
        auto host = probe_window("jietuba pointer probe", QRect(centre - QPoint(100, 100), QSize(200, 200)));
        host->setCursor(Qt::ArrowCursor);
        QVERIFY(QTest::qWaitForWindowExposed(host.get()));
        host->raise();
        QCursor::setPos(centre);
        QApplication::setOverrideCursor(Qt::ArrowCursor);
        QTest::qWait(100);
        const auto cursor = SystemCursor::grab();
        QApplication::restoreOverrideCursor();
        QCursor::setPos(restore);
        if (!cursor) {
            CURSORINFO info{};
            info.cbSize = sizeof info;
            GetCursorInfo(&info);
            QSKIP(qPrintable(QString("No visible pointer in this session (flags %1, cursor %2).").arg(info.flags).arg(quintptr(info.hCursor))));
        }
        QVERIFY(cursor->width > 0 && cursor->height > 0);
        QVERIFY(std::abs(cursor->x - centre.x()) < 64 && std::abs(cursor->y - centre.y()) < 64);
        QImage image(200, 200, QImage::Format_RGB32);
        image.fill(QColor(128, 128, 128));
        const QImage blank = image;
        cursor->draw_onto(image, cursor->x - 50, cursor->y - 50);
        QVERIFY(image != blank);
        // Only the pointer's own box changes.
        QCOMPARE(image.pixel(10, 10), blank.pixel(10, 10));
        QCOMPARE(image.pixel(199, 199), blank.pixel(199, 199));
        // Far away it leaves the image alone.
        QImage untouched = blank;
        cursor->draw_onto(untouched, cursor->x + 5000, cursor->y + 5000);
        QCOMPARE(untouched, blank);
    }
};

QTEST_MAIN(CaptureFinderTests)
#include "test_capture_finders.moc"
