#include "canvas/scene.h"
#include "clipboard/core/manager.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "core/background_tasks.h"
#include "core/constants.h"
#include "capture/quick_capture_controller.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include "core/shortcut_manager.h"
#include "main_app.h"
#include "pin/pin_manager.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "translation/translation_manager.h"
#include "ui/screenshot_window.h"
#include "ui/settings_ui/dialog.h"
#include "ui/welcome/wizard.h"
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QDialog>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QSignalSpy>
#include <QSystemTrayIcon>
#include <QTemporaryDir>
#include <QScreen>
#include <QTest>
#include <QTimer>
#include <QThread>
#include <atomic>
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

    void theClipboardHotkeyTogglesTheWindowAndACaptureClosesIt() {
        QVERIFY(main_app_->clipboard_manager && main_app_->clipboard_manager->is_monitoring());
        main_app_->open_clipboard_window();
        QVERIFY(main_app_->clipboard_window);
        QTRY_VERIFY(main_app_->clipboard_window->isVisible());
        main_app_->open_clipboard_window();
        QVERIFY(!main_app_->clipboard_window->isVisible());
        // A capture closes a clipboard window kept open, so it is not captured.
        main_app_->open_clipboard_window();
        QTRY_VERIFY(main_app_->clipboard_window->isVisible());
        main_app_->start_screenshot();
        QVERIFY(!main_app_->clipboard_window->isVisible());
        QTRY_VERIFY(main_app_->screenshot_window && main_app_->screenshot_window->session_active());
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

    void hotkeysRegisterAndCanBeSuppressed() {
        auto *config = main_app_->config_manager;
        config->set_app_setting("global_hotkeys_disabled", false);
        main_app_->update_hotkey();
        QVERIFY(ShortcutManager::instance()->has_registered_hotkeys());
        main_app_->set_global_hotkeys_disabled(true);
        QVERIFY(config->get_app_setting("global_hotkeys_disabled").toBool());
        // Suppressed hotkeys stay registered; their callbacks are swallowed.
        QVERIFY(ShortcutManager::instance()->has_registered_hotkeys());
        QVERIFY(ShortcutManager::instance()->global_hotkeys_suppressed());
        main_app_->set_global_hotkeys_disabled(false);
        QVERIFY(!ShortcutManager::instance()->global_hotkeys_suppressed());
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


    // A hotkey another program holds is listed in a warning with its slot; the others still register.
    void occupiedHotkeysAreReported() {
        auto *config = main_app_->config_manager;
        constexpr int OTHER_PROGRAM_ID = 0x6F00;
        QVERIFY(RegisterHotKey(nullptr, OTHER_PROGRAM_ID, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F11));
        config->set_global_hotkeys("pin_clipboard", {"", "", "ctrl+alt+shift+f11", ""});
        QString message;
        QTimer::singleShot(0, main_app_, [&message] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                for (auto *label : dialog->findChildren<QLabel *>()) message += label->text();
                dialog->reject();
            }
        });
        main_app_->update_hotkey(true);
        UnregisterHotKey(nullptr, OTHER_PROGRAM_ID);
        config->set_global_hotkeys("pin_clipboard", {"", "", "", ""});
        QVERIFY2(message.contains("ctrl+alt+shift+f11") && message.contains("(3)"), qPrintable(message));
        QVERIFY(ShortcutManager::instance()->has_registered_hotkeys());
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
