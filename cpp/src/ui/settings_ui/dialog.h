#pragma once
#include "settings/color_formats.h"
#include "settings/special_paste.h"
#include "ui/fluent_lite/frameless.h"
#include <QColor>
#include <QMap>
#include <QPointer>
#include <QThread>
#include <QVariantMap>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QLabel;
class QStackedWidget;
class QPushButton;

// The settings dialog: navigation, page stack, footer buttons and apply / refresh / reset.
// Pages live in page_*.cpp and register their controls here by the reference's attribute
// names, so "the page that owns this control has been built" is has_attr(name).
namespace jietuba {
class InAppKeyEdit;
class NavigationInterface;
class ProviderRegistry;
class SegmentedWidget;
class SettingsCardGroup;
class ToolSettingsManager;

QString settings_tr(const char *text);

// Saves in-app editors, keeping empty as unbound.
void save_inapp_shortcut_edits(ToolSettingsManager *config_manager, const std::vector<std::pair<QString, InAppKeyEdit *>> &edits);

// Default size for a screen's available area (logical pixels) at a window scale: at 100% linear through
// 1920×1032→1050×750 and 2560×1392→1150×900, capped at the latter, at least 1050 wide (narrower, the
// shortcut page scrolls sideways), and never larger than the area; pages and navigation scroll the rest.
QSize settings_window_size(int available_width, int available_height, double factor);

class SettingsDialog : public FrostedFramelessDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(ToolSettingsManager *config_manager, const QString &current_hotkey = "ctrl+shift+a", QWidget *parent = nullptr);
    QString tr_text(const char *text) const { return settings_tr(text); }

    // Sizes the dialog for the screen it is about to open on; a size the user dragged is kept and only
    // shrunk to fit. Position is Qt's: a never-moved dialog is centred within that screen on each show.
    void fit_to_screen();
    // fit_to_screen for a given available area.
    void fit_to_available(const QSize &available);

    // Saves and notifies the application without closing this window.
    bool apply_settings(bool close_after = false);
    // Explicit save-and-close.
    void accept() override;
    QString get_hotkey() const;
    void update_hotkey(const QString &new_hotkey);
    // Rereads every setting from the configuration into the controls.
    void refresh_settings();
    // Import and export go through every page's controls, so all pages are built first.
    void build_all_pages();
    // Fills the controls with imported values; applying writes them like manual edits.
    void load_imported_settings(const QVariantMap &values);
    // The configuration keys the settings pages read, for export.
    std::set<QString> settings_keys();
    bool has_unsaved_changes() const;
    bool dialog_scale_changed_on_accept() const { return dialog_scale_changed_on_accept_; }
    bool language_changed_on_apply() const { return language_changed_on_apply_; }
    bool close_after_apply() const { return close_after_apply_; }
    bool skip_unsaved_close_prompt() const { return skip_unsaved_close_prompt_; }
    void set_skip_unsaved_close_prompt(bool skip) { skip_unsaved_close_prompt_ = skip; }

    // Controls by their reference attribute names.
    void set_attr(const QString &name, QWidget *widget) { attrs_[name] = widget; }
    bool has_attr(const QString &name) const { return attrs_.contains(name) && attrs_.value(name); }
    template <class T> T *attr(const QString &name) const { return qobject_cast<T *>(attrs_.value(name).data()); }
    QWidget *attr_widget(const QString &name) const { return attrs_.value(name).data(); }

    ToolSettingsManager *config_manager;
    // Changes one entry of the snapshot unsaved changes are measured against, when it has one.
    void update_snapshot(const QString &name, const QVariant &value);
    QString current_hotkey;
    QStackedWidget *content_stack = nullptr;
    // Combos, switches and mouse binding editors saved under their configuration key, in creation order.
    std::vector<std::pair<QString, QPointer<QWidget>>> behavior_controls;
    QWidget *behavior_control(const QString &key) const;
    // In-app shortcut editors by configuration key, in creation order, and their conflict group.
    std::vector<std::pair<QString, InAppKeyEdit *>> inapp_edits;
    QMap<QString, QString> inapp_groups;
    InAppKeyEdit *inapp_edit(const QString &key) const;
    QString clipboard_pick_mode;
    QPointer<SegmentedWidget> inapp_shortcut_tabs;
    QPointer<SegmentedWidget> mouse_inapp_tabs;
    // Edited in their own window and kept here until Apply.
    std::optional<QVector<color_formats::ColorFormat>> magnifier_color_formats;
    std::optional<QColor> appearance_theme_color;
    std::optional<QColor> appearance_mask_color;
    std::optional<QList<special_paste::Entry>> clipboard_special_paste_entries;
    std::optional<QString> clip_theme_name;
    // The clipboard storage move in progress, if any.
    QPointer<QThread> clipboard_move_thread;
    // Translation page state.
    std::shared_ptr<ProviderRegistry> translation_registry;
    std::optional<QMap<QString, QPointer<QWidget>>> provider_field_widgets;
    QMap<QString, QPointer<QWidget>> provider_sections;
    QMap<QString, QPointer<QLabel>> provider_status_labels;
    QMap<QString, QPointer<QWidget>> request_option_cards;

    void change_save_dir();
    void open_save_dir();
    void change_log_dir();
    void open_log_dir();
    void open_welcome_wizard() { emit wizard_requested(); }
    // The pin page's shortcut entries: switch to the keyboard or mouse page and select the pin tab.
    void show_pin_shortcuts(bool mouse = false);
    void on_nav_changed(int stack_index, const QString &route_key = {});
    void refresh_latest_log_label();
    // Shows the clipboard storage size, now or after delay_ms; skipped until the clipboard page is built.
    void refresh_clipboard_size(int delay_ms = 0);
    Q_INVOKABLE void update_action_buttons();
signals:
    void wizard_requested();
    // Admin mode was just turned on in an unelevated instance; the main app hands off to the elevated one.
    void admin_mode_enabled();
    void settings_applied();
protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    // Asks before closing with unsaved changes.
    void closeEvent(QCloseEvent *event) override;
private:
    void setup_titlebar();
    void setup_ui();
    NavigationInterface *create_navigation(QWidget *parent);
    void set_current_nav(const QString &route_key);
    // Builds the page at index if needed and adds its current values to the unsaved-changes baseline.
    void ensure_page(int index);
    void show_logo_context_menu(const QPoint &pos);
    void open_developer_page();
    // Clears the translucent backing store before painting a new page.
    void refresh_after_page_change();
    QLayout *create_button_area();
    void apply_footer_styles();
    // Keeps Apply in sync with edits made anywhere in the settings pages.
    void connect_action_button_tracking(QWidget *page);
    // Autostart through the Run entry or, in admin mode, the scheduled task.
    void apply_startup();
    void save_magnifier_color_formats();
    void save_special_paste_entries();
    void apply_clipboard_appearance();
    void reset_current_page();
    void reset_pin_page();
    void reset_mouse_page();
    void reset_hotkey_page();
    void reset_long_screenshot_page();
    void reset_appearance_page();
    void reset_quick_actions_page();
    void reset_screenshot_settings_page();
    void reset_log_page();
    void reset_misc_page();
    void reset_translation_page();
    void reset_clipboard_page();
    void apply_dialog_stylesheet();
    void on_ui_theme_changed();
    // The current values of every editable control.
    QVariantMap snapshot_settings() const;
    QString confirm_close_with_unsaved_changes();
    void apply_taskbar_icon();
    void refresh_behavior_controls(bool defaults = false, const QString &prefix = {}, const std::optional<std::set<QString>> &keys = std::nullopt);
    // The appearance values in effect, held by their managers; shown fresh after changes elsewhere.
    static QVariant runtime_appearance(const QString &key);
    // How the managers read appearance values from config at startup.
    static std::function<QVariant(const QString &)> configured_appearance(ToolSettingsManager *config);
    void refresh_appearance_controls(const std::function<QVariant(const QString &)> &value_of);

    QMap<QString, QPointer<QWidget>> attrs_;
    std::set<int> built_pages_;
    // The size last set for a screen; a different current size means the user resized the dialog.
    std::optional<QSize> fitted_size_;
    std::optional<QVariantMap> settings_snapshot_;
    // Whether the dialog scale changed on apply; the main app rebuilds the cached window then.
    bool dialog_scale_changed_on_accept_ = false;
    bool language_changed_on_apply_ = false;
    bool close_after_apply_ = false;
    bool skip_unsaved_close_prompt_ = false;
    // Imported settings are in the controls but not applied; some are not in the snapshot.
    bool import_unsaved_ = false;
    // The imported toolbar layout (JSON text); no control shows it, Apply writes it.
    std::vector<std::pair<QString, QString>> imported_toolbar_layouts_;
    NavigationInterface *nav_list_ = nullptr;
    QWidget *right_area_ = nullptr;
    QLabel *content_title_ = nullptr;
    QPushButton *footer_cancel_btn_ = nullptr;
    QPushButton *footer_ok_btn_ = nullptr;
};
}
