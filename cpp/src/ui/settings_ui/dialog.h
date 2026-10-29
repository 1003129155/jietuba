#pragma once
#include "settings/color_formats.h"
#include "settings/special_paste.h"
#include "ui/fluent_lite/frameless.h"
#include "ui/settings_ui/external_refresh.h"
#include <QColor>
#include <QMap>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <type_traits>
#include <vector>

class QComboBox;
class QLabel;
class QStackedWidget;
class QPushButton;
class QTimer;

// The settings dialog: navigation, page stack, saving / refresh / reset.
// Pages live in page_*.cpp and register their controls here by the reference's attribute
// names, so "the page that owns this control has been built" is has_attr(name).
// There is no Apply: every edit is saved shortly after it is made, only the controls that
// changed since the last save are written, and the application is told what changed.
namespace jietuba {
class InAppKeyEdit;
class NavigationInterface;
class ProviderRegistry;
class SegmentedWidget;
class SettingsCardGroup;
class SettingsSearchBox;
struct SettingsSearchEntry;
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
    // shrunk to fit. It cannot be made narrower than its cards need, unless the screen is. Position is
    // Qt's: a never-moved dialog is centred within that screen on each show; one with no part on any
    // screen is centred on that screen here.
    void fit_to_screen();
    // fit_to_screen for a given available area.
    void fit_to_available(const QSize &available);

    // Saves the pending edits now instead of after the short delay, and notifies the application.
    // Unfinished or rejected shortcuts are left out; always true.
    bool apply_settings();
    // Saves the pending edits and closes.
    void accept() override;
    QString get_hotkey() const;
    void update_hotkey(const QString &new_hotkey);
    // Rereads every setting from the configuration into the controls.
    void refresh_settings();
    // Import goes through every page's controls, so all pages are built first.
    void build_all_pages();
    // Fills the controls with imported values and saves them like manual edits.
    void load_imported_settings(const QVariantMap &values);
    // Edits made but not saved yet: the save waits for edits to pause.
    bool has_unsaved_changes() const;
    // Restarts the delay after which the pending edits are saved.
    Q_INVOKABLE void schedule_save();
    // While controls are filled programmatically (refresh, reset, import), conflict questions are
    // not asked: the values are checked when saved, and rejected ones are left unsaved.
    bool batch_editing() const { return batch_editing_ > 0; }
    // What the last save wrote, by control name; the application refreshes only what it affects.
    const QSet<QString> &saved_keys() const { return saved_keys_; }
    bool hotkeys_saved() const;
    bool dialog_scale_changed() const { return dialog_scale_changed_; }
    bool language_changed() const { return language_changed_; }
    // True during a save, when a language switch it makes must not rebuild this window midway.
    bool saving() const { return saving_; }

    // The page, scroll position and window position, carried over when the window is rebuilt.
    struct ViewState {
        int stack_index = 0;
        QString route_key;
        int scroll = 0;
        std::optional<QPoint> position;
    };
    ViewState view_state() const;
    // On a hidden window the page is opened when it shows.
    void restore_view_state(const ViewState &state);

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
    // What a page shows from outside the settings (disk, registry, the system) is read again each
    // time the window is shown or activated: the window stays alive hidden between uses. A page
    // registers its first query when built, so pages never opened cost nothing.
    template <typename Query, typename Apply> std::function<void()> add_external_refresh(Query query, Apply apply) {
        using Result = std::invoke_result_t<Query>;
        auto *job = new ExternalRefresh<Result>(this, std::move(query), std::move(apply));
        const QPointer<ExternalRefresh<Result>> guard(job);
        std::function<void()> refresh = [guard] { if (guard) guard->request(); };
        external_refreshes_.push_back(refresh);
        refresh();
        return refresh;
    }
    // Queues the registered queries; repeated requests are coalesced by each job.
    Q_INVOKABLE void refresh_external_state();
    // Prepares controls added after their page was built: scaling, the wheel guard and autosave.
    void adopt_controls(QWidget *root);
    // In-app shortcut editors by configuration key, in creation order, and their conflict group.
    std::vector<std::pair<QString, InAppKeyEdit *>> inapp_edits;
    QMap<QString, QString> inapp_groups;
    InAppKeyEdit *inapp_edit(const QString &key) const;
    QString clipboard_pick_mode;
    QPointer<SegmentedWidget> inapp_shortcut_tabs;
    QPointer<SegmentedWidget> mouse_inapp_tabs;
    // Edited in their own window; a change is saved like any edit.
    std::optional<QVector<color_formats::ColorFormat>> magnifier_color_formats;
    std::optional<QColor> appearance_theme_color;
    std::optional<QColor> appearance_mask_color;
    std::optional<QList<special_paste::Entry>> clipboard_special_paste_entries;
    std::optional<QString> clip_theme_name;
    // The clipboard storage move in progress, if any.
    QPointer<QThread> clipboard_move_thread;
    std::function<void()> clipboard_size_refresh;
    std::function<void()> ocr_refresh;
    QVariantMap ocr_install_state;
    // Translation page state.
    std::shared_ptr<ProviderRegistry> translation_registry;
    std::optional<QMap<QString, QPointer<QWidget>>> provider_field_widgets;
    QMap<QString, QPointer<QWidget>> provider_sections;
    QMap<QString, QPointer<QLabel>> provider_status_labels;
    // Reading voice choice per language key ("zh"); an empty item data is the automatic choice.
    std::vector<std::pair<QString, QPointer<QComboBox>>> speech_voice_combos;
    // Preferences include temporarily missing voices, independently of which rows are visible.
    QStringList speech_voice_preferences;
    QMap<QString, qint64> translation_usage;
    QMap<QString, QPointer<QWidget>> request_option_cards;

    void change_save_dir();
    void open_save_dir();
    void change_log_dir();
    void open_log_dir();
    void open_welcome_wizard() { emit wizard_requested(); }
    // The pin page's shortcut entries: switch to the keyboard or mouse page and select the pin tab.
    void show_pin_shortcuts(bool mouse = false);
    // The shortcut page, scrolled to the global hotkey editor attr.
    void show_hotkey_row(const QString &attr);
    SettingsSearchBox *search_box() const { return search_box_; }
    // Every navigation page, with the groups and rows of the pages built so far.
    std::vector<SettingsSearchEntry> search_entries() const;
    // Builds the first navigation page not built yet; false when there is none.
    bool build_next_page();
    // Opens the result's page and tab, scrolls the row into view and outlines it.
    void show_search_result(const SettingsSearchEntry &entry);
    // The screen annotation page's hotkey summary: the shortcut page's editors when built, else the settings.
    void refresh_screen_annotation_hotkey();
    void on_nav_changed(int stack_index, const QString &route_key = {});
    void refresh_latest_log_label();
    // Shows the clipboard storage size, now or after delay_ms; skipped until the clipboard page is built.
    void refresh_clipboard_size(int delay_ms = 0);
signals:
    void wizard_requested();
    // Admin mode was just turned on in an unelevated instance; the main app hands off to the elevated one.
    void admin_mode_enabled();
    void settings_applied();
protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
    // Ctrl+F opens the search field, except in shortcut editors, which record it.
    bool event(QEvent *event) override;
    // Saves what is pending before the window goes away.
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    void open_view(const ViewState &state);
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
    // The page title with the page's reset button on its right.
    QLayout *create_title_row();
    // Schedules a save on edits made anywhere in the page.
    void connect_save_tracking(QWidget *page);
    // Writes the controls named in changed and applies their effects.
    void save_changes(QSet<QString> changed);
    // Mouse bindings among changed that now collide are put back and dropped from changed.
    void reject_mouse_conflicts(QSet<QString> &changed);
    // Autostart through the Run entry or, in admin mode, the scheduled task.
    void apply_startup();
    void save_magnifier_color_formats();
    void save_special_paste_entries();
    void apply_clipboard_appearance();
    void reset_current_page();
    void reset_pin_page();
    void reset_screen_annotation_page();
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
    void apply_taskbar_icon();
    void refresh_behavior_controls(bool defaults = false, const QString &prefix = {}, const std::optional<std::set<QString>> &keys = std::nullopt);
    // The appearance values in effect, held by their managers; shown fresh after changes elsewhere.
    static QVariant runtime_appearance(const QString &key);
    // How the managers read appearance values from config at startup.
    static std::function<QVariant(const QString &)> configured_appearance(ToolSettingsManager *config);
    void refresh_appearance_controls(const std::function<QVariant(const QString &)> &value_of);

    QMap<QString, QPointer<QWidget>> attrs_;
    std::vector<std::function<void()>> external_refreshes_;
    std::set<int> built_pages_;
    // The size last set for a screen; a different current size means the user resized the dialog.
    std::optional<QSize> fitted_size_;
    // The control values last saved (or loaded); a save writes what differs from it.
    std::optional<QVariantMap> settings_snapshot_;
    QTimer *save_timer_ = nullptr;
    QSet<QString> saved_keys_;
    // Whether the last save changed these; the main app rebuilds the cached window then.
    bool dialog_scale_changed_ = false;
    bool language_changed_ = false;
    bool saving_ = false;
    int batch_editing_ = 0;
    // The current navigation item, for rebuilding the window on the same page.
    QString current_route_ = "shortcuts";
    // The page and scroll position to open once the window shows.
    std::optional<ViewState> pending_view_;
    NavigationInterface *nav_list_ = nullptr;
    SettingsSearchBox *search_box_ = nullptr;
    QPointer<QWidget> search_flash_;
    QWidget *right_area_ = nullptr;
    QLabel *content_title_ = nullptr;
    QPushButton *page_reset_btn_ = nullptr;
};
}
