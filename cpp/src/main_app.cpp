#include "main_app.h"
#include "clipboard/core/manager.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "clipboard/ui/windows/pin_window.h"
#include "annotation/screen_annotation.h"
#include "gif/composer.h"
#include "gif/record_window.h"
#include "pin/pin_manager.h"
#include "core/elevation.h"
#include "core/game_mode_monitor.h"
#include "ui/settings_ui/dialog.h"
#include "ui/welcome/wizard.h"
#include "capture/capture_service.h"
#include "capture/display_watcher.h"
#include "capture/quick_capture_controller.h"
#include "capture/system_cursor.h"
#include "core/background_tasks.h"
#include "core/bootstrap.h"
#include "core/clipboard_utils.h"
#include "core/i18n.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/qt_utils.h"
#include "core/resource_manager.h"
#include "core/shortcut_manager.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "pin/pin_manager.h"
#include "settings/tool_settings.h"
#include "text_recognition/recognizer.h"
#include "translation/smart_translation_controller.h"
#include "translation/translation_manager.h"
#include "ui/dialogs.h"
#include "ui/screenshot_countdown.h"
#include "ui/screenshot_window.h"
#include "ui/tray_menu.h"
#include <QCursor>
#include <QFile>
#include <QGuiApplication>
#include <QMenu>
#include <QPainter>
#include <QScreen>
#include <QTimer>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "MainApp"); }
}

QIcon create_fallback_app_icon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(QColor("#40E0D0")));
    painter.drawRoundedRect(4, 4, 56, 56, 12, 12);
    QFont font;
    font.setPixelSize(38);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor("#10322E"));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, "J");
    painter.end();
    return QIcon(pixmap);
}

QIcon create_app_icon() {
    const auto icon_path = ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg"));
    if (QFile::exists(icon_path)) {
        // Rendered before checking: a damaged SVG gives an empty pixmap, a fully
        // transparent tray icon as unusable as a missing one.
        const auto icon_pixmap = QIcon(icon_path).pixmap(64, 64);
        if (!icon_pixmap.isNull()) {
            QPixmap pixmap(64, 64);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawPixmap(0, 0, icon_pixmap);
            painter.end();
            return QIcon(pixmap);
        }
    }
    log_warning(T("托盘图标资源不可用，改用占位图标: {icon_path}", {{"icon_path", icon_path}}), "Tray");
    return create_fallback_app_icon();
}

MainApp::MainApp(int &argc, char **argv) : app(std::make_unique<QApplication>(argc, argv)) {
    app->setQuitOnLastWindowClosed(false);
    // The reference also installs a main-thread Python garbage collector here; C++ has none.

    config_manager = get_tool_settings_manager();

    // The UI theme must exist before any window; the capture accent colour is managed
    // separately by core/theme.
    ui_theme_manager = get_ui_theme()->init(config_manager, app.get());
    connect(ui_theme_manager, &UIThemeManager::theme_changed, this, &MainApp::on_ui_theme_changed);

    setup_logger(config_manager);
    connect(app.get(), &QCoreApplication::aboutToQuit, this, &MainApp::on_about_to_quit);

    // Windows without an icon of their own use it in the title bar and on the taskbar. Without it
    // Qt looks for an icon named IDI_ICON1 in the exe, which only has icon 1, and shows the
    // generic Windows one.
    app->setWindowIcon(create_app_icon());

    // Qt's automatic high-DPI scaling is off. Before the welcome wizard first runs, both
    // UI scales get a start value from Windows' display scaling; existing values stay.
    if (const auto recommended_scale = apply_first_run_scale_defaults(*config_manager))
        log_info(T("首次启动，系统推荐界面比例: {percent}%", {{"percent", *recommended_scale}}), "DPI");

    // Translations: the saved language, or the system language on first start.
    auto saved_lang = config_manager->get_app_setting("language", "__NOT_SET__").toString();
    if (saved_lang == "__NOT_SET__") {
        saved_lang = I18nManager::get_system_language();
        config_manager->set_app_setting("language", saved_lang);
        log_info(T("首次启动，检测到系统语言: {saved_lang}", {{"saved_lang", saved_lang}}), "I18n");
    }
    I18nManager::load_language(saved_lang);
    log_info(T("语言设置: {lang_name}", {{"lang_name", I18nManager::get_current_language_name()}}), "I18n");
    // Updates the tray menu and other UI on language switches.
    connect(I18nManager::instance().language_signals(), &I18nSignals::language_changed, this, &MainApp::on_language_changed);

    get_theme().init(config_manager);
    // The capture UI scale must be loaded before toolbars and panels are built.
    get_ui_scale().init(config_manager);
    // The scale of standalone windows (settings, clipboard manager, translation).
    get_dialog_scale().init(config_manager);

    if (auto *primary_screen = QGuiApplication::primaryScreen()) {
        log_debug(QString("Device Pixel Ratio: %1").arg(python_str(primary_screen->devicePixelRatio())), "DPI");
        log_debug(QString("Logical DPI: %1").arg(python_str(primary_screen->logicalDotsPerInch())), "DPI");
        log_debug(QString("Physical DPI: %1").arg(python_str(primary_screen->physicalDotsPerInch())), "DPI");
    }

    // The hotkey system exists now; an early start-up step registers the hotkeys.
    hotkey_system = std::make_unique<HotkeySystem>();
    hotkey_system->set_suppressed(config_manager->get_app_setting("global_hotkeys_disabled", false).toBool());

    quick_capture = new QuickCaptureController(this);
    connect(ShortcutManager::instance(), &ShortcutManager::global_hotkeys_paused_changed, quick_capture, &QuickCaptureController::refresh);
    // Started before the hotkeys register, so a game already running keeps them from registering at all.
    game_mode_monitor = new GameModeMonitor(this);
    connect(game_mode_monitor, &GameModeMonitor::changed, this, &MainApp::on_game_mode_changed);
    sync_game_mode_monitor();
    update_controller = new UpdateController(this, this);
    app->setProperty("_update_controller", QVariant::fromValue<QObject *>(update_controller));
    connect(app.get(), &QCoreApplication::aboutToQuit, update_controller, &UpdateController::cancel);

    set_trim_busy_check([this] { return capture_busy(); });

    // After plugging monitors, changing resolution or toggling HDR the capture session is
    // rebuilt in the background instead of on the next capture.
    display_watcher_ = new DisplayChangeWatcher([this] { on_display_changed(); }, this);

    smart_translation_controller = new SmartTranslationController(this);
    smart_translation_controller->clipboard_monitor_available = [this] { return clipboard_manager && clipboard_manager->is_available() && clipboard_manager->is_monitoring(); };
    connect(this, &MainApp::clipboard_item_received, this, &MainApp::on_clipboard_item_received);
    connect(this, &MainApp::clipboard_item_received, smart_translation_controller,
            [this](const clipboard::ClipboardItem &item) { smart_translation_controller->on_clipboard_item(item.content_type, item.content); });

    // Start-up: OCR engine choice -> clipboard -> hotkeys and tray, then preloading of
    // capture -> toolbar -> OCR -> settings, then the main UI.
    preloader_ = std::make_unique<PreloadManager>(this);
    preloader_->build_and_start();
}

bool MainApp::starting_up() const { return preloader_ && !preloader_->finished(); }

MainApp::~MainApp() {
    delete screenshot_countdown_.data();
    // The watcher calls back into this object; stopping joins its thread.
    if (clipboard_manager && clipboard_manager->is_available()) {
        clipboard_manager->clear_callback();
        clipboard_manager->stop_monitoring();
    }
    // Children (tray icon, display watcher, controllers) own native windows and must go
    // while the application object still exists; the app member is destroyed before
    // QObject's destructor would delete them.
    while (!children().isEmpty()) delete children().first();
}

void MainApp::on_display_changed() {
    log_debug(T("显示器配置变化，后台重建截图会话"), "MainApp");
    refresh_hdr_session(config_manager->get_capture_engine());
}

bool MainApp::capture_busy() const {
    return capture_pending_ || (screenshot_window && screenshot_window->session_active()) || quick_capture->busy();
}

void MainApp::on_about_to_quit() {
    delete screenshot_countdown_.data();
    capture_pending_ = false;
    set_trim_busy_check({});
    try {
        wait_for_clipboard_writes(3.0);
    } catch (const std::exception &e) {
        log_exception(e, T("等待剪贴板写入"));
    }
    // Screenshots still being saved finish before static teardown; a worker left running then
    // may use objects already destroyed, which the reference's daemon threads never see.
    if (!background_tasks::wait_idle(3.0)) log_warning(LogMsg("Background tasks still running at exit"), "MainApp");
    quick_capture->close();
    close_input_hub();
    display_watcher_->close();
    try {
        TranslationManager::cleanup();
    } catch (const std::exception &e) {
        log_exception(e, T("清理翻译线程"));
    }
    try {
        shutdown_recognition();
    } catch (const std::exception &e) {
        log_exception(e, T("等待文字识别线程"));
    }
    try {
        shutdown_hdr_session();
    } catch (const std::exception &e) {
        log_exception(e, T("关闭 HDR 捕获会话"));
    }
    get_logger().close();
}

void MainApp::on_language_changed(const QString &lang_code) {
    log_debug(T("语言已切换到: {lang_code}，更新 UI", {{"lang_code", lang_code}}), "I18n");
    update_tray_menu();
    // The settings window is preloaded, so it is rebuilt to pick up the translations.
    // A switch made by the settings window itself rebuilds it once that save is done.
    if (settings_window && !settings_window->saving()) recreate_settings_window();
    // Translation windows are created again in the new language.
    TranslationManager::instance()->close_dialog();
}

void MainApp::on_ui_theme_changed(const UIThemeTokens &) {
    update_tray_menu();
    if (settings_window) settings_window->update();
}

void MainApp::setup_pin_tray_updates() {
    auto *pin_manager = PinManager::instance();
    connect(pin_manager, &PinManager::pin_created, this, [this] { update_tray_menu(); });
    connect(pin_manager, &PinManager::pin_closed, this, [this] { update_tray_menu(); });
    connect(pin_manager, &PinManager::all_pins_closed, this, &MainApp::update_tray_menu);
}

void MainApp::update_tray_menu() {
    if (!tray_icon) return;
    tray_icon->setToolTip(tr_text("jietuba - Click to screenshot"));
    auto *old_menu = tray_icon->contextMenu();
    tray_icon->setContextMenu(create_tray_menu(this));
    // QSystemTrayIcon does not own its menu.
    if (old_menu) old_menu->deleteLater();
}

void MainApp::update_hotkey(bool show_error) {
    hotkey_system->unregister_all();
    hotkey_system->set_suppressed(config_manager->get_app_setting("global_hotkeys_disabled", false).toBool());
    std::vector<std::tuple<QString, QString, std::function<void()>>> functions = {
        {"screenshot", tr_text("Screenshot"), [this] { start_screenshot(); }},
        // A selection opens the compact popup, otherwise the input window.
        {"translation", tr_text("Translation"), smart_translation_controller->hotkey()},
    };
    // Also with monitoring off: the window still shows the history already recorded.
    functions.emplace_back("clipboard", tr_text("Clipboard"), [this] { open_clipboard_window(); });
    // Pinning prefers the system clipboard's image, so it is useful with history turned off.
    functions.emplace_back("pin_clipboard", tr_text("Pin Clipboard Image"), [this] { pin_clipboard_image(); });
    functions.emplace_back("screen_annotation", tr_text("Screen Annotation"), [this] { toggle_screen_annotation(); });

    std::vector<std::pair<QString, QString>> failed_hotkeys;
    for (const auto &[function, label, callback] : functions) {
        const auto hotkeys = config_manager->get_global_hotkeys(function);
        for (int slot = 0; slot < static_cast<int>(hotkeys.size()); ++slot) {
            const QString &hotkey = hotkeys[slot];
            if (hotkey.isEmpty()) continue;
            if (hotkey_system->register_hotkey(hotkey, callback)) {
                log_info(T("全局热键已注册: {function} 第 {slot} 个 = {hotkey}", {{"function", function}, {"slot", slot + 1}, {"hotkey", hotkey}}),
                         "Hotkey");
            } else {
                log_warning(T("全局热键注册失败: {function} 第 {slot} 个 = {hotkey}", {{"function", function}, {"slot", slot + 1}, {"hotkey", hotkey}}),
                            "Hotkey");
                failed_hotkeys.emplace_back(slot ? QString("%1 (%2)").arg(label).arg(slot + 1) : label, hotkey);
            }
        }
    }

    quick_capture->refresh();
    // The tray shows each function's hotkey.
    update_tray_menu();
    if (show_error && !failed_hotkeys.empty()) show_hotkey_error(failed_hotkeys);
}

void MainApp::set_global_hotkeys_disabled(bool disabled) {
    config_manager->set_app_setting("global_hotkeys_disabled", disabled);
    hotkey_system->set_suppressed(disabled);
    if (disabled) {
        log_info(T("全局热键已临时禁用（已释放注册）"), "Hotkey");
    } else {
        log_info(T("全局热键已启用"), "Hotkey");
        if (!hotkey_system->has_registered_hotkeys()) update_hotkey(true);
    }
}

void MainApp::set_pause_hotkeys_in_games(bool enabled) {
    config_manager->set_app_setting("pause_hotkeys_in_games", enabled);
    log_info(enabled ? T("已开启玩游戏时暂停热键") : T("已关闭玩游戏时暂停热键"), "Hotkey");
    sync_game_mode_monitor();
}

void MainApp::sync_game_mode_monitor() {
    if (config_manager->get_app_setting("pause_hotkeys_in_games", false).toBool()) game_mode_monitor->start();
    else game_mode_monitor->stop();
}

void MainApp::set_clipboard_enabled(bool enabled) {
    config_manager->set_clipboard_enabled(enabled);
    set_clipboard_monitoring_enabled(enabled);
}

void MainApp::on_game_mode_changed(bool active) {
    log_info(active ? T("游戏在前台，暂停全局热键") : T("游戏已不在前台，恢复全局热键"), "Hotkey");
    hotkey_system->set_game_mode_suppressed(active);
}

void MainApp::show_hotkey_error(const std::vector<std::pair<QString, QString>> &failed_hotkeys) {
    QStringList lines;
    QStringList listed;
    for (const auto &[name, key] : failed_hotkeys) {
        lines << QString::fromUtf8("• %1: %2").arg(name, key);
        listed << QString("('%1', '%2')").arg(name, key);
    }
    QString message = tr_text("The following hotkeys failed to register:") + "\n\n";
    message += lines.join("\n");
    message += "\n\n" + tr_text("The hotkey may be occupied by other programs. Please try a different combination.");
    log_debug(T("显示热键错误提示: {failed_hotkeys}", {{"failed_hotkeys", "[" + listed.join(", ") + "]"}}), "Hotkey");
    show_warning_dialog(nullptr, tr_text("Hotkey Registration Failed"), message);
}

void MainApp::setup_tray() {
    if (tray_icon) return;
    if (!QSystemTrayIcon::isSystemTrayAvailable()) show_error_dialog(nullptr, tr_text("Error"), tr_text("System tray not available"));
    tray_icon = new QSystemTrayIcon(this);
    tray_icon->setIcon(create_app_icon());
    tray_icon->setToolTip(tr_text("jietuba - Click to screenshot"));
    tray_icon->setContextMenu(create_tray_menu(this));
    connect(tray_icon, &QSystemTrayIcon::activated, this, &MainApp::on_tray_activated);
    tray_icon->show();
}

void MainApp::on_tray_activated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger) start_screenshot();
}

bool MainApp::activate_blocking_modal() {
    auto *modal = blocking_modal();
    if (!modal) return false;
    log_debug(T("检测到模态窗口 {modal_type}，忽略截图触发", {{"modal_type", QString::fromLatin1(modal->metaObject()->className()).section("::", -1)}}), "MainApp");
    modal->raise();
    modal->activateWindow();
    return true;
}

void MainApp::start_screenshot() {
    request_screenshot(false);
}

void MainApp::start_delayed_screenshot() {
    request_screenshot(true);
}

void MainApp::request_screenshot(bool delayed) {
    if (update_preparing || quick_capture->busy()) return;

    // An active session: ignore the repeat and give focus back to the capture window.
    if (screenshot_window && screenshot_window->session_active()) {
        log_debug(T("截图窗口已存在，忽略重复触发"), "MainApp");
        screenshot_window->activateWindow();
        screenshot_window->raise();
        // Dialogs opened during a capture (colour picker, save, adjust) lock the capture
        // window; bring them above the full-screen layer again.
        if (auto *modal = blocking_modal(screenshot_window)) {
            modal->raise();
            modal->activateWindow();
            return;
        }
        screenshot_window->setFocus();
        return;
    }
    // A nested QDialog::exec() loop still handles tray signals, but application modality
    // would block the new capture window's input; the modal window comes forward instead.
    if (activate_blocking_modal()) return;
    // Queued but not yet on screen: ignore the repeat.
    if (capture_pending_) {
        log_debug(T("截图进行中，忽略重复触发"), "MainApp");
        return;
    }
    // A dialog left on the capture window from the last capture would cover the new
    // layer; dialogs of other windows stay.
    if (screenshot_window) {
        if (auto *leftover = blocking_modal(screenshot_window)) leftover->close();
    }
    // The annotation's drawings are captured; its hit layer and its focus are not.
    if (screen_annotation) screen_annotation->prepare_for_capture();
    // A clipboard window kept open after pasting would be captured too. close() rather than
    // hide(), which leaves its topmost context menu on screen.
    try {
        if (clipboard_window && clipboard_window->isVisible()) clipboard_window->close();
    } catch (const std::exception &e) {
        log_exception(e, T("关闭剪贴板窗口"));
    }
    // One event loop later, so windows just closed have left the screen.
    capture_pending_ = true;
    quick_capture->set_capture_pending(true);
    if (delayed) {
        auto *screen = QGuiApplication::screenAt(QCursor::pos());
        auto *countdown = new ScreenshotCountdown(screen ? screen : QGuiApplication::primaryScreen());
        screenshot_countdown_ = countdown;
        connect(countdown, &ScreenshotCountdown::finished, this, [this, countdown] {
            screenshot_countdown_ = nullptr;
            countdown->deleteLater();
            // Wait for the hidden countdown to leave the composed desktop, also for GDI capture.
            QTimer::singleShot(0, this, [this] {
                flush_desktop();
                capture_and_prepare_window();
            });
        });
        return;
    }
    QTimer::singleShot(0, this, &MainApp::capture_and_prepare_window);
}

void MainApp::capture_and_prepare_window() {
    if (!capture_pending_) return;
    const bool include_cursor = config_manager->get_app_setting("capture_include_cursor", false).toBool();
    std::shared_ptr<SystemCursor> cursor;
    QImage image;
    QRectF rect;
    try {
        cursor = include_cursor ? SystemCursor::grab() : nullptr;
        std::tie(image, rect) = CaptureService().capture_all_screens(cursor);
    } catch (const std::exception &e) {
        log_exception(e, T("截图失败"));
        capture_pending_ = false;
        quick_capture->set_capture_pending(false);
        return;
    }
    capture_pending_ = false;
    quick_capture->set_capture_pending(false);
    on_capture_ready(image, rect, cursor);
}

void MainApp::on_capture_ready(const QImage &image, const QRectF &rect, std::shared_ptr<SystemCursor> cursor) {
    log_debug(T("截图完成，准备截图窗口"), "MainApp");
    // A modal window may have appeared meanwhile; a locked capture window is not created.
    if (activate_blocking_modal()) return;
    // Open popups (tray menu, pin context menu) would starve the capture window of mouse
    // moves and smart selection would lag; the screen is already captured.
    close_active_popups();

    quick_capture->set_capture_pending(true);
    // A new window per capture; the last one deletes itself when its capture ends.
    screenshot_window = new ScreenshotWindow(config_manager, image, rect, cursor);
    // After the capture global dragging resumes.
    connect(screenshot_window, &ScreenshotWindow::session_ended, quick_capture, &QuickCaptureController::sync_input_availability);
    quick_capture->set_capture_pending(false);
}

void MainApp::preload_settings() {
    if (settings_window) return;
    log_debug(T("预加载设置窗口..."), "Preload");
    auto *window = new SettingsDialog(config_manager, config_manager->get_hotkey());
    settings_window = window;
    connect(window, &SettingsDialog::settings_applied, this, [this, window] { on_settings_accepted(window); });
    connect(window, &SettingsDialog::wizard_requested, this, &MainApp::on_wizard_requested);
    connect(window, &SettingsDialog::admin_mode_enabled, this, &MainApp::hand_off_to_admin);
    log_debug(T("设置窗口预加载完成"), "Preload");
}

void MainApp::on_settings_accepted(SettingsDialog *accepted_window) {
    // Saves follow every edit, so only what the save touched is redone: re-registering hotkeys or
    // warming the capture engine on each toggle would repeat their work and their warnings.
    const auto &saved = accepted_window->saved_keys();
    if (saved.contains("clipboard_enabled_toggle")) set_clipboard_monitoring_enabled(config_manager->get_clipboard_enabled());
    if (accepted_window->hotkeys_saved()) update_hotkey(true);
    if (saved.contains("capture_engine_combo")) apply_capture_engine(config_manager->get_capture_engine());
    if (clipboard_window) {
        clipboard_window->load_settings();
        clipboard_window->controller->load_settings();
        if (clipboard_manager) clipboard_manager->apply_history_limit();
    }
    PinManager::instance()->refresh_all_appearance();
    const bool dialog_scale_changed = accepted_window->dialog_scale_changed();
    if (dialog_scale_changed) recreate_clipboard_manage_dialog();
    // After the save returns, on the page it was made on.
    if (dialog_scale_changed || accepted_window->language_changed()) QTimer::singleShot(0, this, [this] { recreate_settings_window(); });
}

void MainApp::recreate_clipboard_manage_dialog() {
    auto *old_dialog = clipboard::get_existing_manage_dialog();
    if (!old_dialog) return;
    auto *manager = old_dialog->manager;
    if (!clipboard::destroy_manage_dialog()) return;
    auto *dialog = clipboard::get_manage_dialog(manager);
    if (clipboard_window) clipboard_window->connect_manage_dialog(dialog);
    dialog->show_and_activate();
}

void MainApp::on_wizard_requested() {
    log_info(T("向导请求：隐藏设置窗口并注销热键"), "MainApp");
    if (settings_window) settings_window->hide();
    // No shortcuts are intercepted while the wizard runs.
    hotkey_system->unregister_all();
    quick_capture->suspend();
    try {
        WelcomeWizard wizard(config_manager);
        exec_dialog(&wizard);
    } catch (const std::exception &e) {
        log_exception(e, T("向导启动失败"));
    }
    sync_game_mode_monitor();
    update_hotkey();
    log_info(T("向导完成，热键已恢复"), "MainApp");
}

void MainApp::recreate_settings_window(std::optional<bool> reopen) {
    SettingsDialog *window = settings_window;
    if (update_preparing || !window) return;
    const bool show_again = reopen.value_or(window->isVisible());
    const auto view = window->view_state();
    window->hide();
    window->deleteLater();
    settings_window = nullptr;
    preload_settings();
    if (!show_again) return;
    settings_window->restore_view_state(view);
    open_settings();
}

void MainApp::open_settings() {
    if (update_preparing) return;
    // Not preloaded yet: built now.
    if (!settings_window) preload_settings();
    settings_window->fit_to_screen();
    settings_window->setWindowState((settings_window->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    settings_window->show();
    settings_window->raise();
    settings_window->activateWindow();
}

void MainApp::open_translator() {
    if (update_preparing) return;
    TranslationManager::instance()->translate({}, std::nullopt, config_manager->get_translation_request_params());
}

void MainApp::on_clipboard_item_received(const clipboard::ClipboardItem &item) {
    if (clipboard_window) clipboard_window->notify_new_content(item);
}

bool MainApp::set_clipboard_monitoring_enabled(bool enabled) {
    auto *manager = clipboard_manager;
    if (!enabled) {
        if (!manager) {
            log_debug(T("剪贴板监听已禁用，未创建管理器"), "Clipboard");
            return true;
        }
        if (!manager->is_available()) {
            log_warning(T("剪贴板管理器不可用，无法停止监听"), "Clipboard");
            return false;
        }
        if (!manager->is_monitoring()) return true;
        manager->stop_monitoring();
        const bool stopped = !manager->is_monitoring();
        if (stopped) log_info(T("剪贴板监听已按设置关闭"), "Clipboard");
        return stopped;
    }
    try {
        if (!manager) {
            manager = &clipboard::ClipboardManager::instance();
            clipboard_manager = manager;
        }
        if (!manager->is_available()) {
            log_warning(T("剪贴板管理器不可用（pyclipboard 未安装）"), "Clipboard");
            return false;
        }
        if (manager->is_monitoring()) return true;
        // Runs on the watcher thread; the signal is emitted on the GUI thread.
        manager->start_monitoring([this](const clipboard::ClipboardItem &item) {
            QMetaObject::invokeMethod(this, [this, item] { emit clipboard_item_received(item); }, Qt::QueuedConnection);
        });
        const bool started = manager->is_monitoring();
        if (started)
            log_info(T("剪贴板监听已按设置启动"), "Clipboard");
        else
            log_warning(T("剪贴板监听启动失败"), "Clipboard");
        return started;
    } catch (const std::exception &e) {
        log_exception(e, T("启动剪贴板监听"));
    }
    return false;
}

void MainApp::open_clipboard_window() {
    if (update_preparing) return;
    try {
        // Visible: the hotkey closes it.
        if (clipboard_window && clipboard_window->isVisible()) {
            clipboard_window->close();
            return;
        }
        if (!clipboard_window) clipboard_window = new clipboard::ClipboardWindow;
        // Not WindowActive: activating before show would hide the previous foreground window (the paste target) from showEvent.
        clipboard_window->setWindowState(clipboard_window->windowState() & ~Qt::WindowMinimized);
        clipboard_window->show();
        clipboard_window->raise();
        clipboard_window->activateWindow();
        log_debug(T("剪切板窗口已打开"), "Clipboard");
    } catch (const std::exception &e) {
        log_exception(e, T("打开剪切板窗口失败"));
    }
}

void MainApp::pin_clipboard_image() {
    if (update_preparing) return;
    if (screenshot_window && screenshot_window->session_active()) {
        screenshot_window->pin_from_global_hotkey();
        return;
    }
    try {
        clipboard::pin_latest_clipboard_image(QCursor::pos());
    } catch (const std::exception &e) {
        log_exception(e, T("钉住剪贴板图片失败"));
    }
}

void MainApp::toggle_screen_annotation() {
    if (update_preparing) return;
    // The capture window is above any layer; a session started under it would sit on top of it.
    if (capture_busy()) {
        log_debug(T("截图进行中，忽略屏幕标注"), "MainApp");
        return;
    }
    if (!screen_annotation) {
        screen_annotation = new ScreenAnnotation(this);
        // The tray entry shows whether a session runs.
        connect(screen_annotation, &ScreenAnnotation::mode_changed, this, [this](ScreenAnnotation::Mode mode) {
            const bool running = mode != ScreenAnnotation::Mode::Off;
            if (running == screen_annotation_running_) return;
            screen_annotation_running_ = running;
            update_tray_menu();
        });
    }
    screen_annotation->toggle();
}

QString MainApp::update_busy_reason() {
    if (background_tasks::busy() || capture_busy() || (clipboard_manager && clipboard_manager->pending_writes()))
        return tr_text("Finish capturing or saving before updating. The download has been kept.");
    // Preserve unexported recordings and scrolling captures until the user closes their windows.
    if (active_gif_window() || app->property("_scroll_window").value<QObject *>())
        return tr_text("Finish recording or scrolling and close that window before updating. The download has been kept.");
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (auto *composer = qobject_cast<ComposerProgressDialog *>(widget); composer && composer->exporting())
            return tr_text("Finish exporting before updating. The download has been kept.");
    }
    if (preloader_ && preloader_->threads_running()) return tr_text("Wait for application initialization to finish before updating.");
    return {};
}

std::pair<bool, QString> MainApp::prepare_for_update() {
    const QString reason = update_busy_reason();
    if (!reason.isEmpty() || !background_tasks::pause_if_idle()) return {false, reason.isEmpty() ? tr_text("Finish saving before updating.") : reason};
    update_preparing = true;
    quick_capture->suspend();
    try {
        if (clipboard_manager && !clipboard_manager->pause_for_update()) {
            cancel_update_preparation();
            return {false, tr_text("Finish saving before updating.")};
        }
        if (settings_window && settings_window->clipboard_move_thread && settings_window->clipboard_move_thread->isRunning()) {
            cancel_update_preparation();
            return {false, tr_text("Finish saving before updating.")};
        }
        if (settings_window && !settings_window->close()) {
            cancel_update_preparation();
            return {false, tr_text("Update cancelled because the settings window was not closed.")};
        }
        QSettings &settings = config_manager->qsettings.settings();
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            cancel_update_preparation();
            return {false, tr_text("Settings could not be saved. Update cancelled.")};
        }
        settings_window = nullptr;
        return {true, {}};
    } catch (const std::exception &e) {
        cancel_update_preparation();
        return {false, QString::fromStdString(e.what())};
    }
}

void MainApp::cancel_update_preparation() {
    background_tasks::resume();
    if (clipboard_manager) clipboard_manager->resume_after_update();
    if (update_preparing) {
        update_preparing = false;
        quick_capture->refresh();
    }
}

void MainApp::hand_off_to_admin() {
    if (!hand_off_to_admin_task()) return;
    quit_app();
}

bool MainApp::quit_app() {
    if (settings_window) {
        if (!settings_window->close()) return false;
        settings_window = nullptr;
    }
    // MP4 muxing and the helper exit finish before the application leaves.
    if (auto recording = active_gif_window(); recording && recording->video_busy()) {
        if (recording_quit_pending_ != recording) {
            recording_quit_pending_ = recording;
            QObject::connect(recording, &GifRecordWindow::closed, app.get(), [this] { quit_app(); });
        }
        recording->close_all();
        return false;
    }
    recording_quit_pending_ = nullptr;
    delete screenshot_countdown_.data();
    capture_pending_ = false;
    quick_capture->close();
    if (screen_annotation) screen_annotation->stop();
    // Ends an open capture; its window deletes itself.
    if (screenshot_window) {
        try {
            screenshot_window->cleanup_and_close();
        } catch (const std::exception &e) {
            log_exception(e, T("销毁截图窗口"));
        }
        screenshot_window = nullptr;
    }
    if (clipboard_window) {
        try {
            clipboard_window->close();
        } catch (const std::exception &e) {
            log_exception(e, T("关闭剪贴板窗口"));
        }
        clipboard_window = nullptr;
    }
    if (clipboard_manager && clipboard_manager->is_available()) {
        try {
            clipboard_manager->stop_monitoring();
        } catch (const std::exception &e) {
            log_exception(e, T("停止剪贴板监听"));
        }
    }
    // Waits for preload threads (at most 2 s, so exit cannot hang).
    preloader_->wait_for_threads(2000);
    hotkey_system->unregister_all();
    app->quit();
    return true;
}

int MainApp::run() { return app->exec(); }
}
