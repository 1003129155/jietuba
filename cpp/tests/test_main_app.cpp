#include "annotation/annotation_view.h"
#include "annotation/screen_annotation.h"
#include "barcode/result_window.h"
#include "canvas/scene.h"
#include "capture/capture_service.h"
#include "clipboard/core/manager.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "core/background_tasks.h"
#include "core/constants.h"
#include "core/game_mode_monitor.h"
#include "capture/quick_capture_controller.h"
#include "core/i18n.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include "core/shortcut_manager.h"
#include "core/ui_scale.h"
#include "main_app.h"
#include "pin/pin_manager.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "translation/translation_manager.h"
#include "ui/screenshot_countdown.h"
#include "ui/screenshot_window.h"
#include "ui/settings_ui/dialog.h"
#include "ui/welcome/wizard.h"
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QPixmap>
#include <QSettings>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QScreen>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>
#include <QThread>
#include <atomic>
#include <tuple>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jietuba;

namespace {
QStringList action_texts(QMenu *menu) {
    QStringList texts;
    for (auto *action : menu->actions())
        if (!action->isSeparator()) texts << action->text();
    return texts;
}

// Injected input carrying this value counts as real input for the hooks.
constexpr ULONG_PTR TEST_MARKER = 0x4A425431;

void send_key(WORD vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = TEST_MARKER;
    SendInput(1, &input, sizeof input);
}

void send_mouse(const QPoint &physical, DWORD flags) {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>((physical.x() - left) * 65535.0 / (width - 1));
    input.mi.dy = static_cast<LONG>((physical.y() - top) * 65535.0 / (height - 1));
    input.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input.mi.dwExtraInfo = TEST_MARKER;
    SendInput(1, &input, sizeof input);
}

QImage solid(const QSize &size) {
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::darkCyan);
    return image;
}

// Whether another program could register the combination now.
bool system_can_register(UINT mods, UINT vk) {
    constexpr int PROBE_ID = 0x6F20;
    if (!RegisterHotKey(nullptr, PROBE_ID, mods, vk)) return false;
    UnregisterHotKey(nullptr, PROBE_ID);
    return true;
}
}

// The application object as main() builds it, driven through its public entry points.
// main() below isolates settings and logs before the application exists.
class MainAppTests : public QObject {
    Q_OBJECT
public:
    explicit MainAppTests(MainApp *main_app) : main_app_(main_app) {}
private:
    MainApp *main_app_;

private slots:
    // Hotkeys and the tray come before preloading; tests start once start-up has finished,
    // so no preload step runs alongside them.
    void initTestCase() {
        QTRY_VERIFY_WITH_TIMEOUT(!main_app_->starting_up(), 30000);
        QVERIFY(main_app_->tray_icon);
    }

    void cleanup() {
        if (main_app_->screenshot_window && main_app_->screenshot_window->session_active()) main_app_->screenshot_window->cleanup_and_close();
        PinManager::instance()->close_all();
        if (TranslationManager::has_instance()) TranslationManager::instance()->close_dialog();
        if (main_app_->settings_window) main_app_->settings_window->hide();
        QApplication::processEvents();
    }

    // Windows that set no icon of their own carry the application icon, which the title bar and
    // the taskbar show.
    void windowsWithoutTheirOwnIconUseTheAppIcon() {
        QVERIFY(!QApplication::windowIcon().isNull());
        QWidget window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        const auto hwnd = reinterpret_cast<HWND>(window.winId());
        QVERIFY(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
        QVERIFY(SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0));
    }

    void theTrayOffersTheMainEntriesAndFollowsPins() {
        main_app_->setup_tray();
        main_app_->setup_pin_tray_updates();
        QVERIFY(main_app_->tray_icon);
        QVERIFY(!main_app_->tray_icon->icon().isNull());
        QVERIFY(main_app_->tray_icon->toolTip().contains("jietuba"));
        auto *menu = main_app_->tray_icon->contextMenu();
        QVERIFY(menu);
        QVERIFY(action_texts(menu).size() >= 4);
        const auto actions = menu->actions();
        QVERIFY(actions.size() >= 3);
        QCOMPARE(actions.at(actions.size() - 3)->text(), i18n_tr("Settings", "SystemTray"));
        QVERIFY(actions.at(actions.size() - 2)->isSeparator());
        QCOMPARE(actions.last()->text(), i18n_tr("Exit", "SystemTray"));
        // A new pin rebuilds the menu.
        PinManager::instance()->create_pin(solid(QSize(60, 40)), QPoint(100, 100), main_app_->config_manager);
        QVERIFY(main_app_->tray_icon->contextMenu() != menu);
        // Setting up twice keeps one icon.
        auto *icon = main_app_->tray_icon;
        main_app_->setup_tray();
        QCOMPARE(main_app_->tray_icon, icon);
        QVERIFY(!create_fallback_app_icon().isNull());
        QVERIFY(!create_app_icon().isNull());
    }

    void startingAScreenshotCapturesIntoANewWindow() {
        QVERIFY(!main_app_->capture_busy());
        main_app_->start_screenshot();
        // The capture is queued for one event loop, and busy meanwhile.
        QVERIFY(main_app_->capture_busy());
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
        auto *window = main_app_->screenshot_window.data();
        QVERIFY(window->isVisible());
        QVERIFY(!window->original_image.isNull());
        // A repeat while the session is open only brings it forward.
        main_app_->start_screenshot();
        QApplication::processEvents();
        QCOMPARE(main_app_->screenshot_window.data(), window);
        QPointer<ScreenshotWindow> ended = window;
        window->cleanup_and_close();
        QVERIFY(!main_app_->capture_busy());
        // The ended window deletes itself; the next capture opens a new one.
        QTRY_VERIFY(!ended);
        main_app_->start_screenshot();
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
    }

    void theDelayedTrayScreenshotCountsDownWithoutBlockingTheDesktop_data() {
        QTest::addColumn<QString>("engine");
        QTest::newRow("auto") << QString("auto");
        QTest::newRow("gdi") << QString("mss");
    }

    void theDelayedTrayScreenshotCountsDownWithoutBlockingTheDesktop() {
        QFETCH(QString, engine);
        const auto saved_engine = main_app_->config_manager->get_capture_engine();
        main_app_->config_manager->set_capture_engine(engine);
        const auto restore_engine = qScopeGuard([&] { main_app_->config_manager->set_capture_engine(saved_engine); });
        auto *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        QPushButton target(nullptr);
        target.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        target.setFixedSize(scaled(200), scaled(200));
        target.move(screen->geometry().center() - target.rect().center());
        target.setStyleSheet("QPushButton { background: #dd5533; border: none; }");
        target.show();
        QVERIFY(QTest::qWaitForWindowExposed(&target));
        QSignalSpy clicked(&target, &QPushButton::clicked);
        auto countdowns = [] {
            QList<ScreenshotCountdown *> found;
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *countdown = qobject_cast<ScreenshotCountdown *>(widget)) found << countdown;
            return found;
        };
        QVERIFY(countdowns().isEmpty());
        QAction *delayed_action = nullptr;
        for (auto *action : main_app_->tray_icon->contextMenu()->actions())
            if (action->text() == i18n_tr("Delayed Screenshot", "SystemTray")) delayed_action = action;
        QVERIFY(delayed_action);
        QElapsedTimer elapsed;
        elapsed.start();
        delayed_action->trigger();
        QVERIFY(main_app_->capture_busy());
        QCOMPARE(countdowns().size(), 1);
        QPointer<ScreenshotCountdown> countdown = countdowns().first();
        const auto finish_pending = qScopeGuard([&] {
            if (countdown && countdown->isVisible()) QTest::qWait(3100);
        });
        QVERIFY(countdown->isVisible());
        QCOMPARE(countdown->text(), QString("3"));
        QCOMPARE(countdown->geometry().center(), screen->geometry().center());
        QCOMPARE(countdown->width(), scaled(96));
        QVERIFY(GetForegroundWindow() != reinterpret_cast<HWND>(countdown->winId()));
        delayed_action->trigger();
        main_app_->start_screenshot();
        QCOMPARE(countdowns().size(), 1);
        QCOMPARE(countdowns().first(), countdown.data());
        QVERIFY(!main_app_->screenshot_window || !main_app_->screenshot_window->session_active());
        const auto centre = countdown->geometry().center();
        QTRY_COMPARE(GetAncestor(WindowFromPoint(POINT{centre.x(), centre.y()}), GA_ROOT), reinterpret_cast<HWND>(target.winId()));
        QTest::mouseClick(&target, Qt::LeftButton, Qt::NoModifier, target.rect().center());
        QCOMPARE(clicked.count(), 1);
        // Changes made during the delay, rather than the initial screen, must be captured.
        target.setStyleSheet("QPushButton { background: #28b487; border: none; }");
        QTRY_COMPARE(countdown->text(), QString("2"));
        QVERIFY(!main_app_->screenshot_window || !main_app_->screenshot_window->session_active());
        QTRY_COMPARE(countdown->text(), QString("1"));
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
        QVERIFY(elapsed.elapsed() >= 3000);
        QVERIFY(!countdown || !countdown->isVisible());
        auto *window = main_app_->screenshot_window.data();
        const QPoint sample = target.geometry().center() - QPoint(qRound(window->virtual_x), qRound(window->virtual_y));
        QVERIFY(window->original_image.rect().contains(sample));
        const auto pixel = window->original_image.pixelColor(sample);
        QVERIFY(qAbs(pixel.red() - 40) <= 10 && qAbs(pixel.green() - 180) <= 10 && qAbs(pixel.blue() - 135) <= 10);
        main_app_->start_delayed_screenshot();
        QCOMPARE(main_app_->screenshot_window.data(), window);
        QTRY_VERIFY(countdowns().isEmpty());
    }

    // The window is a square around a disc: whatever lies under the corners and just outside it must
    // show through, with no system border, shadow or square background.
    void theCountdownIsADiscWithoutASystemFrame() {
        auto *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QColor under("#dd5533");
        QPushButton backdrop(nullptr);
        backdrop.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        backdrop.setFixedSize(scaled(300), scaled(300));
        backdrop.move(screen->geometry().center() - backdrop.rect().center());
        backdrop.setStyleSheet("QPushButton { background: #dd5533; border: none; }");
        backdrop.show();
        QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
        ScreenshotCountdown countdown(screen);
        QVERIFY(QTest::qWaitForWindowExposed(&countdown));
        const QRect area = countdown.geometry();
        const int w = area.width(), h = area.height();
        QImage image;
        QRectF rect;
        auto pixel = [&](const QPoint &global) { return image.pixelColor(global - rect.topLeft().toPoint()); };
        // Inside the disc and clear of the digit: dark, so the capture does contain the window.
        const QPoint inside = area.topLeft() + QPoint(w / 2, h / 8);
        QElapsedTimer waited;
        waited.start();
        bool drawn = false;
        while (!drawn && waited.elapsed() < 2000) {
            std::tie(image, rect) = CaptureService("mss").capture_all_screens();
            drawn = pixel(inside).red() < 120;
            if (!drawn) QTest::qWait(50);
        }
        QVERIFY2(drawn, qPrintable(pixel(inside).name()));
        const QPoint outside[] = {
            // The corners of the square.
            {area.left() + 2, area.top() + 2}, {area.right() - 2, area.top() + 2},
            {area.left() + 2, area.bottom() - 2}, {area.right() - 2, area.bottom() - 2},
            // On the window's edges, outside the disc.
            {area.left(), area.top() + h / 4}, {area.right(), area.top() + h / 4},
            {area.left(), area.bottom() - h / 4}, {area.right(), area.bottom() - h / 4},
            {area.left() + w / 4, area.top()}, {area.right() - w / 4, area.top()},
            {area.left() + w / 4, area.bottom()}, {area.right() - w / 4, area.bottom()},
            // Just outside the window, where a shadow would fall.
            {area.left() - 3, area.center().y()}, {area.right() + 3, area.center().y()},
            {area.center().x(), area.top() - 3}, {area.center().x(), area.bottom() + 3},
        };
        for (const auto &point : outside) {
            const auto color = pixel(point);
            QVERIFY2(qAbs(color.red() - under.red()) <= 6 && qAbs(color.green() - under.green()) <= 6 && qAbs(color.blue() - under.blue()) <= 6,
                     qPrintable(QString("(%1,%2) is %3").arg(point.x()).arg(point.y()).arg(color.name())));
        }
    }
    void delayedScreenshotsRespectModalAndUpdateGuards() {
        main_app_->update_preparing = true;
        main_app_->start_delayed_screenshot();
        const bool captured_during_update = main_app_->capture_busy();
        main_app_->update_preparing = false;
        QVERIFY(!captured_during_update);
        bool captured_while_modal = true;
        QTimer::singleShot(0, main_app_, [&] {
            auto *dialog = QApplication::activeModalWidget();
            main_app_->start_delayed_screenshot();
            captured_while_modal = main_app_->capture_busy();
            if (dialog) dialog->close();
        });
        QDialog modal;
        modal.setModal(true);
        modal.exec();
        QVERIFY(!captured_while_modal);
    }

    void thePinHotkeyPinsTheSelectionDuringACapture() {
        main_app_->start_screenshot();
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
        auto *window = main_app_->screenshot_window.data();
        // No selection yet: nothing to pin.
        main_app_->pin_clipboard_image();
        QCOMPARE(PinManager::instance()->count(), 0);
        QRectF selection(100, 100, 120, 80);
        window->scene->preset_selection(selection);
        main_app_->pin_clipboard_image();
        QCOMPARE(PinManager::instance()->count(), 1);
        QVERIFY(!window->session_active());
        QCOMPARE(PinManager::instance()->get_all_pins().first()->orig_size(), QSize(120, 80));
        // Outside a capture the system clipboard's image is pinned at the cursor.
        QApplication::clipboard()->setImage(solid(QSize(50, 30)));
        main_app_->pin_clipboard_image();
        QCOMPARE(PinManager::instance()->count(), 2);
        QCOMPARE(PinManager::instance()->get_all_pins().last()->orig_size(), QSize(50, 30));
    }

    void theClipboardHotkeyTogglesTheWindowAndACaptureKeepsIt() {
        QVERIFY(main_app_->clipboard_manager && main_app_->clipboard_manager->is_monitoring());
        main_app_->open_clipboard_window();
        QVERIFY(main_app_->clipboard_window);
        QTRY_VERIFY(main_app_->clipboard_window->isVisible());
        main_app_->open_clipboard_window();
        QVERIFY(!main_app_->clipboard_window->isVisible());
        // A capture leaves an open clipboard window on screen, so it is captured too.
        main_app_->open_clipboard_window();
        QTRY_VERIFY(main_app_->clipboard_window->isVisible());
        main_app_->start_screenshot();
        QVERIFY(main_app_->clipboard_window->isVisible());
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
        // Closing after a paste, it hides once the capture has the focus. Opened again it is owned by the
        // capture, so it stays above it; the capture releases it when it ends, before it is destroyed.
        QTRY_VERIFY(!main_app_->clipboard_window->isVisible());
        main_app_->open_clipboard_window();
        QTRY_VERIFY(main_app_->clipboard_window->isVisible());
        const auto clipboard = reinterpret_cast<HWND>(main_app_->clipboard_window->winId());
        QCOMPARE(GetWindow(clipboard, GW_OWNER), reinterpret_cast<HWND>(main_app_->screenshot_window->winId()));
        main_app_->screenshot_window->cleanup_and_close();
        QCOMPARE(GetWindow(clipboard, GW_OWNER), HWND(nullptr));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(IsWindow(clipboard));
        main_app_->clipboard_window->close();
    }

    void turningClipboardMonitoringOffAndOnFollowsTheSetting() {
        auto *manager = main_app_->clipboard_manager;
        QVERIFY(manager);
        QVERIFY(main_app_->set_clipboard_monitoring_enabled(false));
        QVERIFY(!manager->is_monitoring());
        QVERIFY(main_app_->set_clipboard_monitoring_enabled(true));
        QVERIFY(manager->is_monitoring());
        // The watcher thread registers its listener shortly after starting; earlier changes are not seen.
        QTest::qWait(500);
        // A recorded item reaches the GUI thread through the signal.
        QSignalSpy received(main_app_, &MainApp::clipboard_item_received);
        QApplication::clipboard()->setText(QString("clipboard monitor %1").arg(QDateTime::currentMSecsSinceEpoch()));
        QTRY_VERIFY_WITH_TIMEOUT(!received.isEmpty(), 5000);
        QCOMPARE(received.first().first().value<clipboard::ClipboardItem>().content_type, QString("text"));
    }

    // The tray switch releases the registrations, so other programs get the keys, and brings them back.
    void hotkeysRegisterAndCanBeSuppressed() {
        auto *config = main_app_->config_manager;
        config->set_app_setting("global_hotkeys_disabled", false);
        config->set_global_hotkeys("pin_clipboard", {"ctrl+alt+shift+f15", "", "", ""});
        main_app_->update_hotkey();
        QVERIFY(ShortcutManager::instance()->has_registered_hotkeys());
        QVERIFY(!system_can_register(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F15));
        main_app_->set_global_hotkeys_disabled(true);
        QVERIFY(config->get_app_setting("global_hotkeys_disabled").toBool());
        QVERIFY(ShortcutManager::instance()->global_hotkeys_suppressed());
        QVERIFY(system_can_register(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F15));
        // Kept to be applied again, so turning the switch off needs no re-registration.
        QVERIFY(ShortcutManager::instance()->has_registered_hotkeys());
        main_app_->set_global_hotkeys_disabled(false);
        QVERIFY(!ShortcutManager::instance()->global_hotkeys_suppressed());
        QVERIFY(!system_can_register(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F15));
        config->set_global_hotkeys("pin_clipboard", {"", "", "", ""});
        main_app_->update_hotkey();
    }

    void settingsArePreloadedOnceAndApplyingKeepsTheWindow() {
        main_app_->preload_settings();
        QPointer<SettingsDialog> window = main_app_->settings_window;
        QVERIFY(window);
        main_app_->preload_settings();
        QCOMPARE(main_app_->settings_window.data(), window.data());
        main_app_->open_settings();
        QVERIFY(window->isVisible());
        // A window left off every screen comes back to the primary one.
        window->move(-30000, -30000);
        window->hide();
        main_app_->open_settings();
        QVERIFY(QApplication::primaryScreen()->availableGeometry().contains(window->pos()));
        main_app_->on_settings_accepted(window);
        QCOMPARE(main_app_->settings_window.data(), window.data());
    }

    void theTranslatorOpensAnEmptyWindow() {
        main_app_->open_translator();
        QVERIFY(TranslationManager::instance()->is_dialog_open());
    }

    // The global mouse shortcut through the real hooks: hold the modifiers, drag, release.
    // Someone using the mouse at the same time breaks the injected drag, so it gets one retry.
    void aGlobalMouseDragPinsTheRegion() {
        auto *config = main_app_->config_manager;
        config->set_app_setting("quick_capture_pin", "ctrl+alt+dragleft");
        config->set_app_setting("global_hotkeys_disabled", false);
        main_app_->update_hotkey();
        input_hub()->native->set_test_marker(TEST_MARKER);
        const auto centre = QGuiApplication::primaryScreen()->geometry().center();
        const QPoint start = centre - QPoint(80, 50);
        const QPoint end = start + QPoint(160, 100);
        const auto drag = [&] {
            send_key(VK_CONTROL, true);
            send_key(VK_MENU, true);
            send_mouse(start, 0);
            QTest::qWait(50);
            send_mouse(start, MOUSEEVENTF_LEFTDOWN);
            QTest::qWait(50);
            for (int step = 1; step <= 5; ++step) {
                send_mouse(start + (end - start) * step / 5, 0);
                QTest::qWait(30);
            }
            // The selection is shown while dragging.
            const bool started = QTest::qWaitFor([&] { return main_app_->quick_capture->busy(); }, 2000);
            send_mouse(end, MOUSEEVENTF_LEFTUP);
            send_key(VK_MENU, false);
            send_key(VK_CONTROL, false);
            return started && QTest::qWaitFor([] { return PinManager::instance()->count() == 1; }, 5000);
        };
        bool pinned = drag();
        if (!pinned) {
            QTRY_VERIFY(!main_app_->quick_capture->busy());
            PinManager::instance()->close_all();
            pinned = drag();
        }
        input_hub()->native->set_test_marker(0);
        if (!pinned) {
            QFile log(get_logger().log_file_path());
            if (log.open(QIODevice::ReadOnly)) qWarning().noquote() << QString::fromUtf8(log.readAll()).right(5000);
            QFAIL("no pin from the global drag");
        }
        const auto size = PinManager::instance()->get_all_pins().first()->orig_size();
        QVERIFY2(std::abs(size.width() - 160) <= 1 && std::abs(size.height() - 100) <= 1, qPrintable(QString("%1x%2").arg(size.width()).arg(size.height())));
        QTRY_VERIFY(!main_app_->quick_capture->busy());
    }


    void aGlobalMouseDragScansCodes_data() {
        QTest::addColumn<int>("selection");
        QTest::newRow("multiple") << 0;
        QTest::newRow("single") << 1;
        QTest::newRow("empty") << 2;
        QTest::newRow("cancelled") << 3;
    }

    // 只框选测试窗口里的合成码，真实钩子到识别结果全链路；不写系统剪贴板。
    void aGlobalMouseDragScansCodes() {
        QFETCH(int, selection);
        auto *config = main_app_->config_manager;
        const auto saved = config->get_quick_capture_scan_code();
        const bool copy_single = config->get_barcode_copy_single_enabled();
        const bool include_cursor = config->get_capture_include_cursor();
        const auto original_cursor = QCursor::pos();
        const auto restore = qScopeGuard([&] {
            send_mouse(QCursor::pos(), MOUSEEVENTF_RIGHTUP);
            send_key(VK_MENU, false);
            send_key(VK_CONTROL, false);
            input_hub()->native->set_test_marker(0);
            main_app_->quick_capture->cancel();
            config->set_quick_capture_scan_code(saved);
            config->set_barcode_copy_single_enabled(copy_single);
            config->set_capture_include_cursor(include_cursor);
            main_app_->update_hotkey();
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *result = qobject_cast<BarcodeResultWindow *>(widget)) result->close();
            QCursor::setPos(original_cursor);
        });
        config->set_quick_capture_scan_code("ctrl+alt+dragright");
        config->set_barcode_copy_single_enabled(false);
        config->set_capture_include_cursor(true);
        main_app_->update_hotkey();
        input_hub()->native->set_test_marker(TEST_MARKER);
        QTRY_VERIFY(input_hub()->native->hooks_installed());

        QImage image(JIETUBA_BARCODE_FIXTURE_DIR "/barcodes.png");
        QVERIFY(!image.isNull());
        if (selection == 1) image = image.copy(QRect(30, 50, 110, 110));
        if (selection == 2) image.fill(Qt::white);
        const auto expected = read_codes(image);
        QLabel surface;
        surface.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        surface.setPixmap(QPixmap::fromImage(image));
        surface.setFixedSize(image.size());
        surface.move(QGuiApplication::primaryScreen()->availableGeometry().center() - surface.rect().center());
        surface.show();
        surface.raise();
        QVERIFY(QTest::qWaitForWindowExposed(&surface));
        QTest::qWait(100);
        const QPoint start = surface.mapToGlobal(QPoint(0, 0));
        const QPoint end = start + QPoint(image.width(), image.height());
        send_key(VK_CONTROL, true);
        send_key(VK_MENU, true);
        send_mouse(start, 0);
        QTest::qWait(50);
        send_mouse(start, MOUSEEVENTF_RIGHTDOWN);
        QTest::qWait(50);
        send_mouse(end, 0);
        QTRY_VERIFY(main_app_->quick_capture->busy());
        QTest::qWait(80);
        if (selection == 3) main_app_->quick_capture->cancel();
        send_mouse(end, MOUSEEVENTF_RIGHTUP);
        send_key(VK_MENU, false);
        send_key(VK_CONTROL, false);
        QTRY_VERIFY(!main_app_->quick_capture->busy());
        BarcodeResultWindow *result = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *window = qobject_cast<BarcodeResultWindow *>(widget); window && window->isVisible()) result = window;
        if (selection == 3) {
            QVERIFY(!result);
            return;
        }
        QVERIFY(result);
        QCOMPARE(result->cards.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
            QCOMPARE(result->cards[i]->findChild<CodeText *>()->toPlainText(), expected[i].text);
    }

    // A combination another program holds is taken over by the hook without a warning; a key bound
    // alone cannot be, and is listed in a warning with its slot while the others still register.
    void occupiedHotkeysAreReported() {
        auto *config = main_app_->config_manager;
        constexpr int OTHER_PROGRAM_ID = 0x6F00, OTHER_PROGRAM_BARE_ID = 0x6F02;
        QVERIFY(RegisterHotKey(nullptr, OTHER_PROGRAM_ID, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F11));
        QVERIFY(RegisterHotKey(nullptr, OTHER_PROGRAM_BARE_ID, 0, VK_F14));
        config->set_global_hotkeys("pin_clipboard", {"", "ctrl+alt+shift+f11", "f14", ""});
        QString message;
        QTimer::singleShot(0, main_app_, [&message] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                for (auto *label : dialog->findChildren<QLabel *>()) message += label->text();
                dialog->reject();
            }
        });
        main_app_->update_hotkey(true);
        UnregisterHotKey(nullptr, OTHER_PROGRAM_ID);
        UnregisterHotKey(nullptr, OTHER_PROGRAM_BARE_ID);
        config->set_global_hotkeys("pin_clipboard", {"", "", "", ""});
        QVERIFY2(message.contains("f14") && message.contains("(3)"), qPrintable(message));
        QVERIFY2(!message.contains("f11"), qPrintable(message));
        QVERIFY(ShortcutManager::instance()->has_registered_hotkeys());
        main_app_->update_hotkey();
    }

    // The tray's game switch: a game in the foreground pauses every global input, the mouse shortcut's hook
    // included, and its end brings them back; with the switch off nothing is checked.
    void aGameInTheForegroundPausesHotkeysWhileTheSwitchIsOn() {
        auto *config = main_app_->config_manager;
        config->set_app_setting("quick_capture_pin", "ctrl+alt+dragleft");
        config->set_global_hotkeys("pin_clipboard", {"ctrl+alt+shift+f13", "", "", ""});
        main_app_->update_hotkey();
        main_app_->setup_tray();
        const auto tray_action = [this]() -> QAction * {
            for (auto *action : main_app_->tray_icon->contextMenu()->actions())
                if (action->text() == "Pause Hotkeys While Gaming") return action;
            return nullptr;
        };
        QVERIFY(tray_action());
        QVERIFY(!tray_action()->isChecked());
        bool game = false;
        auto *monitor = main_app_->game_mode_monitor;
        monitor->set_probe([&game] { return game; });
        tray_action()->trigger();
        QVERIFY(config->get_app_setting("pause_hotkeys_in_games").toBool());
        QVERIFY(monitor->running());
        QVERIFY(!ShortcutManager::instance()->global_hotkeys_suppressed());
        QTRY_VERIFY(input_hub()->native->hooks_installed());
        game = true;
        monitor->check();
        QVERIFY(ShortcutManager::instance()->global_hotkeys_suppressed());
        QVERIFY(system_can_register(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F13));
        QTRY_VERIFY(!input_hub()->native->hooks_installed());
        // The tray switch for all hotkeys shows only the user's own choice.
        QVERIFY(!config->get_app_setting("global_hotkeys_disabled").toBool());
        game = false;
        monitor->check();
        QVERIFY(!ShortcutManager::instance()->global_hotkeys_suppressed());
        QVERIFY(!system_can_register(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F13));
        QTRY_VERIFY(input_hub()->native->hooks_installed());
        // Turned off during a game: everything comes back at once.
        game = true;
        monitor->check();
        QVERIFY(tray_action()->isChecked());
        tray_action()->trigger();
        QVERIFY(!config->get_app_setting("pause_hotkeys_in_games").toBool());
        QVERIFY(!monitor->running());
        QVERIFY(!ShortcutManager::instance()->global_hotkeys_suppressed());
        monitor->set_probe({});
        config->set_global_hotkeys("pin_clipboard", {"", "", "", ""});
        main_app_->update_hotkey();
    }

    // The tray lists a function's first hotkey beside its name and follows what the settings save.
    void theTrayShowsEachFunctionsHotkeyBesideItsName() {
        auto *config = main_app_->config_manager;
        const auto screenshot = config->get_global_hotkeys("screenshot");
        const auto translation = config->get_global_hotkeys("translation");
        const auto entry = [this](const QString &name) {
            for (const auto &text : action_texts(main_app_->tray_icon->contextMenu()))
                if (text.section('\t', 0, 0) == name) return text;
            return QString();
        };
        config->set_global_hotkeys("screenshot", {"", "ctrl+alt+shift+f12", "ctrl+alt+shift+f11", ""});
        config->set_global_hotkeys("translation", {"", "", "", ""});
        main_app_->update_hotkey();
        QCOMPARE(entry("Screenshot"), QString("Screenshot\tCtrl+Alt+Shift+F12"));
        QCOMPARE(entry("Translation"), QString("Translation"));
        config->set_global_hotkeys("translation", {"ctrl+alt+shift+f10", "", "", ""});
        main_app_->update_hotkey();
        QCOMPARE(entry("Translation"), QString("Translation\tCtrl+Alt+Shift+F10"));
        config->set_global_hotkeys("screenshot", screenshot);
        config->set_global_hotkeys("translation", translation);
        main_app_->update_hotkey();
    }

    // Rebuilding the menu takes its entries along: none stay behind.
    void rebuildingTheTrayMenuLeavesNoActionsBehind() {
        main_app_->setup_tray();
        QPointer<QMenu> menu = main_app_->tray_icon->contextMenu();
        QPointer<QAction> first = menu->actions().first();
        main_app_->update_hotkey();
        QVERIFY(main_app_->tray_icon->contextMenu() != menu);
        QTRY_VERIFY(!menu);
        QVERIFY(!first);
    }

    // The tray's clipboard switch stops the recording only: the hotkey and the history stay usable.
    void turningClipboardMonitoringOffFromTheTrayKeepsTheHotkey() {
        auto *config = main_app_->config_manager;
        const auto saved = config->get_global_hotkeys("clipboard");
        config->set_global_hotkeys("clipboard", {"ctrl+alt+shift+f14", "", "", ""});
        main_app_->update_hotkey();
        main_app_->setup_tray();
        const auto monitor_action = [this]() -> QAction * {
            for (auto *action : main_app_->tray_icon->contextMenu()->actions())
                if (action->text() == "Record Clipboard") return action;
            return nullptr;
        };
        QVERIFY(monitor_action());
        QVERIFY(monitor_action()->isChecked());
        QVERIFY(!system_can_register(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F14));
        monitor_action()->trigger();
        QVERIFY(!config->get_clipboard_enabled());
        QVERIFY(!main_app_->clipboard_manager->is_monitoring());
        // Registered again with monitoring off, as at the next start.
        main_app_->update_hotkey();
        QVERIFY(!system_can_register(MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F14));
        QVERIFY(!monitor_action()->isChecked());
        main_app_->open_clipboard_window();
        QTRY_VERIFY(main_app_->clipboard_window && main_app_->clipboard_window->isVisible());
        main_app_->open_clipboard_window();
        QVERIFY(!main_app_->clipboard_window->isVisible());
        monitor_action()->trigger();
        QVERIFY(config->get_clipboard_enabled());
        QVERIFY(main_app_->clipboard_manager->is_monitoring());
        config->set_global_hotkeys("clipboard", saved);
        main_app_->update_hotkey();
    }

    // A tray click captures; repeats while it is queued or while a dialog is open start nothing more.
    void theTrayStartsOneCaptureAtATime() {
        main_app_->setup_tray();
        emit main_app_->tray_icon->activated(QSystemTrayIcon::Context);
        QVERIFY(!main_app_->capture_busy());
        emit main_app_->tray_icon->activated(QSystemTrayIcon::Trigger);
        QVERIFY(main_app_->capture_busy());
        main_app_->start_screenshot();
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
        main_app_->screenshot_window->cleanup_and_close();
        // An open modal dialog comes forward instead.
        bool captured_while_modal = true;
        QTimer::singleShot(0, main_app_, [&] {
            auto *dialog = QApplication::activeModalWidget();
            main_app_->start_screenshot();
            QApplication::processEvents();
            captured_while_modal = main_app_->capture_busy() || (main_app_->screenshot_window && main_app_->screenshot_window->session_active());
            if (dialog) dialog->close();
        });
        QDialog modal;
        modal.setModal(true);
        modal.exec();
        QVERIFY(!captured_while_modal);
    }

    // The welcome wizard runs with every hotkey released, and they come back afterwards.
    void theWizardRunsWithoutHotkeys() {
        main_app_->update_hotkey();
        main_app_->preload_settings();
        QVERIFY(main_app_->settings_window);
        bool released = false;
        QTimer::singleShot(0, main_app_, [&released] {
            auto *wizard = qobject_cast<WelcomeWizard *>(QApplication::activeModalWidget());
            if (!wizard) return;
            released = !ShortcutManager::instance()->has_registered_hotkeys();
            // Rejected rather than closed: closing saves every page, including autostart.
            wizard->reject();
        });
        emit main_app_->settings_window->wizard_requested();
        QVERIFY(released);
        QVERIFY(ShortcutManager::instance()->has_registered_hotkeys());
    }

    // An update hands off only when idle: running work refuses it, and cancelling restores new tasks.
    void updatePreparationWaitsForWorkAndRestores() {
        std::atomic<bool> finish{false};
        background_tasks::start_thread([&finish] {
            while (!finish) QThread::msleep(10);
        });
        QVERIFY(!main_app_->update_busy_reason().isEmpty());
        QVERIFY(!main_app_->prepare_for_update().first);
        QVERIFY(!main_app_->update_preparing);
        finish = true;
        QTRY_VERIFY(!background_tasks::busy());
        const auto [ok, reason] = main_app_->prepare_for_update();
        QVERIFY2(ok, qPrintable(reason));
        QVERIFY(main_app_->update_preparing);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, background_tasks::start_thread([] {}));
        main_app_->cancel_update_preparation();
        QVERIFY(!main_app_->update_preparing);
        background_tasks::start_thread([] {});
        QTRY_VERIFY(!background_tasks::busy());
    }

    // Exit waits for running tasks within its budget; a task that throws ends only its own
    // thread, with the exception in the crash log.
    void backgroundTasksAreWaitedForAndGuarded() {
        std::atomic<bool> finish{false};
        background_tasks::start_thread([&finish] {
            while (!finish) QThread::msleep(10);
        });
        QVERIFY(!background_tasks::wait_idle(0.1));
        finish = true;
        QVERIFY(background_tasks::wait_idle(5.0));
        background_tasks::start_thread([] { throw std::runtime_error("thrown by a background task"); }, "ThrowingTask");
        QVERIFY(background_tasks::wait_idle(5.0));
        QFile crash_log(QDir(get_log_dir()).filePath(CRASH_LOG_NAME));
        QVERIFY(crash_log.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(crash_log.readAll());
        QVERIFY(text.contains("ThrowingTask"));
        QVERIFY(text.contains("thrown by a background task"));
    }

    // The screen annotation: its hotkey registers, the tray entry starts and ends it and shows whether it
    // runs, and a capture sends it back to pass-through under the capture window.
    void screenAnnotationTogglesAndStepsAsideForACapture() {
        auto *config = main_app_->config_manager;
        config->set_global_hotkeys("screen_annotation", {"ctrl+alt+shift+f7", "", "", ""});
        main_app_->update_hotkey();
        constexpr int PROBE_ID = 0x6F01;
        QVERIFY(!RegisterHotKey(nullptr, PROBE_ID, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F7));
        main_app_->setup_tray();
        const auto tray_action = [this]() -> QAction * {
            for (auto *action : main_app_->tray_icon->contextMenu()->actions())
                if (action->text().section('\t', 0, 0) == "Screen Annotation") return action;
            return nullptr;
        };
        QVERIFY(tray_action());
        QVERIFY(!tray_action()->isChecked());
        QElapsedTimer clock;
        clock.start();
        tray_action()->trigger();
        for (auto *view : main_app_->screen_annotation->views()) view->viewport()->repaint();
        qInfo("first session on screen after %lld ms", static_cast<long long>(clock.elapsed()));
        auto *annotation = main_app_->screen_annotation;
        QVERIFY(annotation && annotation->active());
        QVERIFY(tray_action()->isChecked());
        annotation->select_tool("pen");
        QCOMPARE(annotation->mode(), ScreenAnnotation::Mode::Drawing);
        main_app_->start_screenshot();
        QCOMPARE(annotation->mode(), ScreenAnnotation::Mode::PassThrough);
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
        const auto centre = QGuiApplication::primaryScreen()->geometry().center();
        const auto capture_hwnd = reinterpret_cast<HWND>(main_app_->screenshot_window->winId());
        QTRY_COMPARE(GetAncestor(WindowFromPoint(POINT{centre.x(), centre.y()}), GA_ROOT), capture_hwnd);
        // No session toggles under an open capture.
        main_app_->toggle_screen_annotation();
        QVERIFY(annotation->active());
        main_app_->screenshot_window->cleanup_and_close();
        main_app_->toggle_screen_annotation();
        QVERIFY(!annotation->active());
        QVERIFY(!tray_action()->isChecked());
        main_app_->toggle_screen_annotation();
        QVERIFY(annotation->active());
        config->set_global_hotkeys("screen_annotation", {"", "", "", ""});
        main_app_->update_hotkey();
    }

    void quittingClosesEverythingOnce() {
        main_app_->start_screenshot();
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
        QSignalSpy quitting(main_app_->app.get(), &QCoreApplication::aboutToQuit);
        // Quitting, like the tray's Exit, happens inside the running event loop.
        bool quit = false;
        QTimer::singleShot(0, main_app_, [&] { quit = main_app_->quit_app(); });
        QCOMPARE(main_app_->run(), 0);
        QVERIFY(quit);
        QCOMPARE(quitting.count(), 1);
        QVERIFY(!main_app_->screenshot_window);
        QVERIFY(!main_app_->settings_window);
        QVERIFY(!ShortcutManager::instance()->has_registered_hotkeys());
        QVERIFY(!main_app_->screen_annotation || !main_app_->screen_annotation->active());
    }
};

int main(int argc, char **argv) {
    QTemporaryDir dir;
    if (!dir.isValid()) return 1;
    // Settings and logs live in a throwaway folder, never the registry or the real log folder.
    QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    auto *config = get_tool_settings_manager(&settings);
    config->set_log_dir(dir.filePath("logs"));
    config->set_log_level("DEBUG");
    // Not a first run: start-up would otherwise open the welcome wizard modally.
    config->mark_as_run();
    config->set_show_main_window(false);
    config->set_app_setting("language", "en");
    config->set_app_setting("ocr_enabled", false);
    config->set_screenshot_save_path(dir.filePath("captures"));
    config->set_screenshot_save_enabled(false);
    config->set_smart_selection(false);
    config->set_smart_selection_mode("off");
    // The clipboard history lives in the throwaway folder too.
    config->set_clipboard_db_path(QDir::toNativeSeparators(dir.filePath("clipboard/clipboard.db")));
    // Another program may hold the default hotkeys (Ctrl+1, Ctrl+2); rarely used ones keep
    // registration independent of the machine. Win+V stays with Windows.
    config->set_hotkey("ctrl+alt+shift+f9");
    config->set_global_hotkeys("clipboard", {"ctrl+alt+shift+f8", "", "", ""});
    MainApp main_app(argc, argv);
    if (QGuiApplication::platformName() != "windows") {
        qWarning("needs -platform windows: the tray and the capture use the real desktop");
        return 0;
    }
    MainAppTests tests(&main_app);
    const int result = QTest::qExec(&tests, argc, argv);
    // The temporary folder holds the log; it can only go once the log is closed.
    get_logger().close();
    return result;
}

#include "test_main_app.moc"
