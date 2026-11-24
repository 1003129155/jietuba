#pragma once
#include "app_version.h"
#include "clipboard/core/models.h"
#include "core/update_controller.h"
#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QPointer>
#include <optional>
#include <QRectF>
#include <QSystemTrayIcon>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// The application: tray icon, global hotkeys, window instances and start-up flow,
// initialised once for the life of the process.
namespace jietuba {
namespace clipboard {
class ClipboardManager;
class ClipboardWindow;
}
class AutoUpdater;
class DisplayChangeWatcher;
class GameModeMonitor;
class GifRecordWindow;
class HotkeySystem;
class PinInbox;
class PinRequestQueue;
class PreloadManager;
class QuickCaptureController;
class ScreenAnnotation;
class ScreenshotCountdown;
class ScreenshotWindow;
class SettingsDialog;
class SmartTranslationController;
class SystemCursor;
class ToolSettingsManager;
class UIThemeManager;
struct UIThemeTokens;

inline const QString APP_VERSION = QStringLiteral(JIETUBA_APP_VERSION);

// A placeholder tray icon so the tray stays visible and clickable when the icon resource
// is missing. Uses no resource or theme manager: reaching it means resources already failed.
QIcon create_fallback_app_icon();
// The application icon from SVG, falling back to the placeholder; always a valid icon,
// since the tray is the only resident entry point.
QIcon create_app_icon();

class MainApp : public QObject, public UpdateHost {
    Q_OBJECT
public:
    MainApp(int &argc, char **argv);
    ~MainApp() override;
    int run();

    // Re-registers every global hotkey (screenshot, clipboard, smart translation).
    // show_error: report failures (on saving settings, not on start-up).
    void update_hotkey(bool show_error = false);
    void set_global_hotkeys_disabled(bool disabled);
    // The tray switch: global input pauses on its own while a game holds the foreground.
    void set_pause_hotkeys_in_games(bool enabled);
    // Starts or stops watching for games as pause_hotkeys_in_games says.
    void sync_game_mode_monitor();
    void setup_tray();
    void start_screenshot();
    void start_delayed_screenshot();
    // A capture whose selection opens the recording window.
    void start_screen_recording();
    void open_settings();
    // Files to pin: those on this process's command line and the ones other processes hand over
    // through inbox (Explorer's menu entry). Pinned in batches at the cursor.
    void accept_pin_requests(PinInbox &inbox, const QStringList &initial_files);
    // Makes Explorer's right-click entry match the setting, the interface language and this
    // executable's place.
    void sync_explorer_menu();
    // Opens the settings window on one page (SettingsDialog::on_nav_changed's index and route).
    void open_settings_page(int stack_index, const QString &route_key);
    void open_translator();
    void open_clipboard_window();
    // Creates the clipboard window once, wired to the app; start-up preloading and the hotkey both
    // go through here.
    clipboard::ClipboardWindow *ensure_clipboard_window();
    // The tray switch: saves the setting and starts or stops the watcher. The hotkey and the recorded
    // history stay available.
    void set_clipboard_enabled(bool enabled);
    // Starts or stops the clipboard watcher to match the setting; whether that state was reached.
    // The manager stays once created, so enabling again only restarts its watcher.
    bool set_clipboard_monitoring_enabled(bool enabled);
    // Pins the clipboard image at the cursor; during a capture pins the selection instead.
    void pin_clipboard_image();
    // Starts the screen annotation, or ends the running one; not while a capture is being prepared.
    void toggle_screen_annotation();
    // Admin mode was just turned on here, unelevated: runs the task so the elevated instance takes
    // over (its single-instance check ends this one). Stays when that instance does not come up.
    void hand_off_to_admin();
    bool quit_app() override;
    QString update_busy_reason() override;
    std::pair<bool, QString> prepare_for_update() override;
    void cancel_update_preparation() override;
    // A tray balloon; on_click runs when this balloon, not an earlier one, is clicked.
    void show_tray_message(const QString &title, const QString &text, QSystemTrayIcon::MessageIcon icon, std::function<void()> on_click = {});
    // A one-line balloon under the program's name and icon (the Start menu shortcut gives Windows both).
    void show_tray_line(const QString &text, std::function<void()> on_click = {});
    void setup_pin_tray_updates();
    // Creates the capture window once the screen is captured.
    void on_capture_ready(const QImage &image, const QRectF &rect, std::shared_ptr<SystemCursor> cursor = nullptr);
    // Capturing is being prepared or running, or the global mouse drags or grabs.
    bool capture_busy() const;
    // Start-up steps remain or a preload thread still runs.
    bool starting_up() const;
    // Builds the cached settings window (also when switching language or opening settings).
    void preload_settings();
    // After settings are applied: hotkeys and pin appearance follow; a new language or window
    // scale rebuilds the cached settings window.
    void on_settings_accepted(SettingsDialog *accepted_window);

    std::unique_ptr<QApplication> app;
    ToolSettingsManager *config_manager;
    UIThemeManager *ui_theme_manager = nullptr;
    std::unique_ptr<HotkeySystem> hotkey_system;
    // Runs while the game switch is on.
    GameModeMonitor *game_mode_monitor = nullptr;
    QSystemTrayIcon *tray_icon = nullptr;
    QPointer<SettingsDialog> settings_window;
    QPointer<ScreenshotWindow> screenshot_window;
    QPointer<clipboard::ClipboardWindow> clipboard_window;
    // Set once monitoring is first enabled.
    clipboard::ClipboardManager *clipboard_manager = nullptr;
    // The recording whose closing will retry quitting.
    QPointer<GifRecordWindow> recording_quit_pending_;
    QuickCaptureController *quick_capture = nullptr;
    SmartTranslationController *smart_translation_controller = nullptr;
    // Created on first use.
    ScreenAnnotation *screen_annotation = nullptr;
    // Whether a session runs, as the tray menu last showed it.
    bool screen_annotation_running_ = false;
    bool update_preparing = false;
    UpdateController *update_controller = nullptr;
    AutoUpdater *auto_updater = nullptr;

signals:
    // Each recorded clipboard item, on the GUI thread.
    void clipboard_item_received(const jietuba::clipboard::ClipboardItem &item);

private:
    void on_display_changed();
    void on_about_to_quit();
    void on_language_changed(const QString &lang_code);
    void on_ui_theme_changed(const UIThemeTokens &tokens);
    void update_tray_menu();
    void on_game_mode_changed(bool active);
    void show_hotkey_error(const std::vector<std::pair<QString, QString>> &failed_hotkeys);
    void on_tray_activated(QSystemTrayIcon::ActivationReason reason);
    // While an application-modal window exists no capture layer is created that could
    // not take input; the modal window comes to the front instead.
    bool activate_blocking_modal();
    // A clipboard window opened over a capture is owned by it: it stays above the capture when the
    // capture takes the focus back (a click, a paste). Released when the capture ends.
    void attach_clipboard_to_capture();
    void release_clipboard_from_capture();
    void request_screenshot(bool delayed, bool record = false);
    void capture_and_prepare_window();
    // The settings window asks for the wizard: hide it and unregister hotkeys while the wizard runs.
    void on_wizard_requested();
    // Rebuilds the cached settings UI after a process-wide UI setting changes. reopen unset
    // keeps the old window's visibility; applying asks for true, saving on close for false,
    // so rebuilding the cache does not cancel the user's close.
    void recreate_settings_window(std::optional<bool> reopen = std::nullopt);
    // Rebuilds the cached clipboard manager window after a window scale change.
    void recreate_clipboard_manage_dialog();
    void on_clipboard_item_received(const clipboard::ClipboardItem &item);
    // A downloaded release: the tray says it installs at the next start, and a click installs it now.
    // Waits while a capture is open, so the balloon does not cover it.
    void notify_update_ready(const ReleaseInfo &release);
    // The Start menu shortcut that names the program, in the interface language, on its notifications.
    void update_notification_identity();

    std::function<void()> tray_message_click_;
    bool capture_pending_ = false;
    // The pending capture is for recording.
    bool record_pending_ = false;
    QPointer<ScreenshotCountdown> screenshot_countdown_;
    DisplayChangeWatcher *display_watcher_ = nullptr;
    PinRequestQueue *pin_requests_ = nullptr;
    std::unique_ptr<PreloadManager> preloader_;
};
}
