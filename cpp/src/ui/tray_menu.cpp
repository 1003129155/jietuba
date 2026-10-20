#include "ui/tray_menu.h"
#include "core/constants.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/theme.h"
#include "core/ui_theme.h"
#include "main_app.h"
#include "pin/pin_manager.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include <QAction>
#include <QDateTime>
#include <QDir>
#include <QMouseEvent>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "SystemTray"); }

QString menu_style() {
    const auto tc = get_theme().theme_color_hex();
    const auto &t = get_ui_theme()->tokens();
    return QString(R"(
        QMenu {
            background-color: %1;
            border: 1px solid %2;
            border-radius: 4px;
            padding: 4px;
            font-family: %3;
            font-size: 9pt;
            color: %4;
        }
        QMenu::item {
            padding: 6px 12px;
            border-radius: 3px;
            color: %4;
            background-color: transparent;
        }
        QMenu::item:selected {
            background-color: %5;
            color: #ffffff;
        }
        QMenu::item:disabled {
            color: %6;
        }
        QMenu::separator {
            height: 1px;
            background: %7;
            margin: 4px 6px;
        }
    )")
        .arg(t.popup_background, t.border, CSS_FONT_FAMILY_UI, t.text, tc, t.text_disabled, t.separator);
}

void save_all_pins_as(PinManager *pin_manager) {
    const auto directory = get_existing_directory(nullptr, tr_text("Choose folder to save pinned images"), QDir::homePath());
    if (directory.isEmpty()) return;
    const auto prefix = QDateTime::currentDateTime().toString("'pins_'yyyyMMdd_HHmmss");
    std::pair<int, int> result;
    try {
        result = pin_manager->save_all_to_directory(directory, prefix);
    } catch (const std::exception &e) {
        log_exception(e, "Save all pins");
        show_warning_dialog(nullptr, tr_text("Save Failed"), QString::fromUtf8(e.what()));
        return;
    }
    const auto [saved, failed] = result;
    if (failed)
        show_warning_dialog(nullptr, tr_text("Save Failed"),
                            tr_text("Saved {saved} pinned images, failed {failed}.").replace("{saved}", QString::number(saved)).replace("{failed}", QString::number(failed)));
}

void add_pin_actions(QMenu *menu, MainApp *app) {
    auto *pin_manager = PinManager::instance();
    const int pin_count = pin_manager->count();
    const bool has_pins = pin_count > 0;

    auto *close_all_action = new QAction(tr_text("Close all pinned windows"), app);
    close_all_action->setEnabled(has_pins);
    QObject::connect(close_all_action, &QAction::triggered, pin_manager, &PinManager::close_all);
    menu->addAction(close_all_action);

    const auto pins_title = tr_text("Current pinned windows: {count}").replace("{count}", QString::number(pin_count));
    if (!has_pins) {
        auto *pins_action = new QAction(pins_title, app);
        pins_action->setEnabled(false);
        menu->addAction(pins_action);
        return;
    }
    auto *pins_menu = new QMenu(pins_title, menu);
    pins_menu->setStyleSheet(menu_style());

    auto *move_center_action = new QAction(tr_text("Move all pins to center"), app);
    QObject::connect(move_center_action, &QAction::triggered, pin_manager, &PinManager::move_all_to_screen_center);
    pins_menu->addAction(move_center_action);

    auto *thumbnail_all_action = new QAction(tr_text("Thumbnail all pins"), app);
    QObject::connect(thumbnail_all_action, &QAction::triggered, pin_manager, [pin_manager] { pin_manager->set_all_thumbnail_mode(true); });
    pins_menu->addAction(thumbnail_all_action);

    auto *restore_all_action = new QAction(tr_text("Restore all pin thumbnails"), app);
    QObject::connect(restore_all_action, &QAction::triggered, pin_manager, [pin_manager] { pin_manager->set_all_thumbnail_mode(false); });
    pins_menu->addAction(restore_all_action);

    pins_menu->addSeparator();

    auto *submenu_close_all_action = new QAction(tr_text("Close all pins"), app);
    QObject::connect(submenu_close_all_action, &QAction::triggered, pin_manager, &PinManager::close_all);
    pins_menu->addAction(submenu_close_all_action);

    auto *save_all_action = new QAction(tr_text("Save all pins as..."), app);
    QObject::connect(save_all_action, &QAction::triggered, pin_manager, [pin_manager] { save_all_pins_as(pin_manager); });
    pins_menu->addAction(save_all_action);

    menu->addMenu(pins_menu);
}
}

void CustomTrayMenu::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) return;
    QMenu::mouseReleaseEvent(event);
}

QMenu *create_tray_menu(MainApp *app) {
    auto *menu = new CustomTrayMenu();
    menu->setStyleSheet(menu_style());

    auto *action_screenshot = new QAction(tr_text("Screenshot"), app);
    QObject::connect(action_screenshot, &QAction::triggered, app, &MainApp::start_screenshot);
    menu->addAction(action_screenshot);

    auto *action_clipboard = new QAction(tr_text("Clipboard"), app);
    QObject::connect(action_clipboard, &QAction::triggered, app, &MainApp::open_clipboard_window);
    menu->addAction(action_clipboard);

    auto *action_translate = new QAction(tr_text("Translation"), app);
    QObject::connect(action_translate, &QAction::triggered, app, &MainApp::open_translator);
    menu->addAction(action_translate);

    // Checked while a session runs; choosing it again ends the session.
    auto *action_annotation = new QAction(tr_text("Screen Annotation"), app);
    action_annotation->setCheckable(true);
    action_annotation->setChecked(app->screen_annotation_running_);
    QObject::connect(action_annotation, &QAction::triggered, app, &MainApp::toggle_screen_annotation);
    menu->addAction(action_annotation);

    menu->addSeparator();

    auto *action_global_hotkeys = new QAction(tr_text("Disable Global Hotkeys"), app);
    action_global_hotkeys->setCheckable(true);
    action_global_hotkeys->setChecked(app->config_manager->get_app_setting("global_hotkeys_disabled", false).toBool());
    QObject::connect(action_global_hotkeys, &QAction::triggered, app, &MainApp::set_global_hotkeys_disabled);
    menu->addAction(action_global_hotkeys);

    auto *action_game_pause = new QAction(tr_text("Pause Hotkeys While Gaming"), app);
    action_game_pause->setCheckable(true);
    action_game_pause->setChecked(app->config_manager->get_app_setting("pause_hotkeys_in_games", false).toBool());
    QObject::connect(action_game_pause, &QAction::triggered, app, &MainApp::set_pause_hotkeys_in_games);
    menu->addAction(action_game_pause);

    menu->addSeparator();
    add_pin_actions(menu, app);
    menu->addSeparator();

    auto *action_settings = new QAction(tr_text("Settings"), app);
    QObject::connect(action_settings, &QAction::triggered, app, &MainApp::open_settings);
    menu->addAction(action_settings);

    menu->addSeparator();

    auto *action_quit = new QAction(tr_text("Exit"), app);
    QObject::connect(action_quit, &QAction::triggered, app, [app] { app->quit_app(); });
    menu->addAction(action_quit);
    return menu;
}
}
