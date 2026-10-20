#pragma once
#include "translation/provider.h"
#include "ui/welcome/base_page.h"
#include <functional>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <memory>
#include <vector>

class QComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;

// The wizard pages.
namespace jietuba {
class HotkeyEdit;
class MouseBindingEditor;
class ProviderRegistry;
class ToolSettingsManager;
class WelcomeWizard;

// Capture toolbar buttons (toolbar_layout keys) the screenshot page's preview cycles through.
QStringList screenshot_preview_tools();

// Page 1: welcome, interface language and theme.
class WelcomePage : public BasePage {
    Q_OBJECT
public:
    explicit WelcomePage(ToolSettingsManager *config, WelcomeWizard *wizard, QWidget *parent = nullptr);
    void retranslate() override;
    // The language is saved as soon as it is chosen.
    void save() override {}
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    void on_lang_changed();
    // Fills the options from the global theme values, relabelled in place on a language change.
    void populate_theme_combo();
    // Hands the choice to the global theme manager.
    void on_theme_changed();
    ToolSettingsManager *config_;
    WelcomeWizard *wizard_;
    QString init_lang_;
    QLabel *row_lang_lbl_ = nullptr;
    QLabel *theme_lbl_ = nullptr;
    QComboBox *lang_combo_ = nullptr;
    QComboBox *theme_combo_ = nullptr;
};

// The global hotkeys page. The global hotkeys form one conflict domain: one real key bound
// to two actions fails silently on the second registration, so they sit together and share
// the settings window's duplicate rules (validate_hotkey_group).
class HotkeyPage : public BasePage {
    Q_OBJECT
public:
    explicit HotkeyPage(ToolSettingsManager *config, QWidget *parent = nullptr);
    ~HotkeyPage() override;
    void retranslate() override;
    // Writes the hotkeys back, skipping values still in conflict. Skip and the close button
    // also end here, so conflicts cannot be stopped only at "Next": a conflicting value on
    // disk always fails to register. The conflicting slot keeps its configuration.
    void save() override;
    // Global hotkey editors on other pages join this page's duplicate check.
    void join_conflict_group(const std::vector<HotkeyEdit *> &edits);
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    using Getter = std::function<QString(ToolSettingsManager *)>;
    using Setter = std::function<void(ToolSettingsManager *, const QString &)>;
    // (main read, main write, alternate read, alternate write, title), top to bottom.
    struct Row {
        Getter get;
        Setter set;
        Getter get_2;
        Setter set_2;
        const char *title;
    };
    static const std::vector<Row> &rows();
    void on_focus_changed(QWidget *now);
    void sync_key_highlight();
    bool revalidate();
    void write(const Setter &setter, HotkeyEdit *edit, bool conflicted, bool keep_when_blank);
    ToolSettingsManager *config_;
    std::vector<std::pair<QLabel *, const char *>> section_labels_, slot_labels_;
    std::vector<HotkeyEdit *> edits_;
    std::vector<HotkeyEdit *> peer_edits_;
    std::vector<std::pair<HotkeyEdit *, HotkeyEdit *>> row_edits_;
    QLabel *hint_ = nullptr;
    QPointer<HotkeyEdit> active_edit_;
    QMetaObject::Connection focus_connection_;
};

// Global mouse shortcuts: hold modifiers and drag to act on the region at once.
class MouseShortcutPage : public BasePage {
    Q_OBJECT
public:
    explicit MouseShortcutPage(ToolSettingsManager *config, QWidget *parent = nullptr);
    void retranslate() override;
    // A gesture given to two actions is not written for either; both keep their stored binding.
    void save() override;
    // Sets the demo's time, for previews and tests that check each phase.
    void set_animation_time(int elapsed_ms);
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    // Action keys sharing a gesture with another action.
    QSet<QString> conflicted_actions() const;
    void refresh_conflict_hint();
    ToolSettingsManager *config_;
    std::vector<std::pair<QString, MouseBindingEditor *>> editors_;
    std::vector<std::pair<QLabel *, const char *>> action_labels_;
    QLabel *hint_ = nullptr;
    QLabel *conflict_lbl_ = nullptr;
};

// Screen annotation: drawing over any window, and its hotkey.
class ScreenAnnotationPage : public BasePage {
    Q_OBJECT
public:
    explicit ScreenAnnotationPage(ToolSettingsManager *config, QWidget *parent = nullptr);
    void retranslate() override;
    // Hotkeys still in conflict keep their stored value; an empty main hotkey is written too,
    // since the tray menu still starts the feature.
    void save() override;
    // The two hotkey editors, for the hotkey page's duplicate check.
    std::vector<HotkeyEdit *> hotkey_edits() const { return {hotkey_edit_, hotkey_edit_2_}; }
    // Sets the demo's time, for previews and tests that check each phase.
    void set_animation_time(int elapsed_ms);
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    ToolSettingsManager *config_;
    HotkeyEdit *hotkey_edit_ = nullptr;
    HotkeyEdit *hotkey_edit_2_ = nullptr;
    QLabel *hotkey_lbl_ = nullptr, *hotkey_desc_ = nullptr, *main_lbl_ = nullptr, *alt_lbl_ = nullptr;
};

// The screenshot page: auto-save, folder, format and smart selection.
class ScreenshotHotkeyPage : public BasePage {
    Q_OBJECT
public:
    explicit ScreenshotHotkeyPage(ToolSettingsManager *config, QWidget *parent = nullptr);
    void retranslate() override;
    void save() override;
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    // Folder and format mean nothing with auto-save off, so they grey out rather than mislead.
    void sync_save_section_enabled(bool enabled);
    void browse_path();
    void populate_precision_combo();
    ToolSettingsManager *config_;
    WelcomeToggleSwitch *autosave_toggle_ = nullptr;
    WelcomeToggleSwitch *smart_toggle_ = nullptr;
    QLabel *autosave_lbl_ = nullptr, *autosave_desc_ = nullptr, *save_lbl_ = nullptr, *save_desc_ = nullptr;
    QLabel *format_lbl_ = nullptr, *smart_lbl_ = nullptr, *smart_desc_ = nullptr, *precision_lbl_ = nullptr;
    QComboBox *precision_combo_ = nullptr;
    QLineEdit *path_edit_ = nullptr;
    QPushButton *browse_btn_ = nullptr;
    QComboBox *format_combo_ = nullptr;
};

// The clipboard page: history limit and the clipboard window's appearance beside a live demo.
class ClipboardHotkeyPage : public BasePage {
    Q_OBJECT
public:
    explicit ClipboardHotkeyPage(ToolSettingsManager *config, QWidget *parent = nullptr);
    void retranslate() override;
    void save() override;
    // Sets the demo's time, for previews and tests that check each phase.
    void set_animation_time(int elapsed_ms);
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    void build_appearance_controls(QVBoxLayout *layout);
    void populate_opacity_combo();
    void on_font_size_picked(int index);
    void on_opacity_picked(int index);
    ToolSettingsManager *config_;
    QLabel *history_limit_lbl_ = nullptr, *history_limit_desc_ = nullptr, *appearance_lbl_ = nullptr;
    QLabel *theme_lbl_ = nullptr, *font_lbl_ = nullptr, *opacity_lbl_ = nullptr;
    QSpinBox *history_limit_spin_ = nullptr;
    QWidget *theme_row_ = nullptr;
    QComboBox *font_combo_ = nullptr;
    QComboBox *opacity_combo_ = nullptr;
    QVariantList opacity_options_;
};

// Translation provider and credentials.
class TranslationPage : public BasePage {
    Q_OBJECT
public:
    explicit TranslationPage(ToolSettingsManager *config, QWidget *parent = nullptr);
    void retranslate() override;
    void save() override;
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    void populate_lang_combo();
    // One form per provider, rendered from its declared credentials.
    void build_credential_page(const ProviderMetadata &metadata);
    void on_provider_changed();
    std::pair<QWidget *, QLabel *> config_row(const char *text, QWidget *control);
    ToolSettingsManager *config_;
    std::shared_ptr<ProviderRegistry> registry_;
    std::vector<ProviderMetadata> provider_metadata_;
    QComboBox *provider_combo_ = nullptr;
    QComboBox *lang_combo_ = nullptr;
    QStackedWidget *credential_stack_ = nullptr;
    QMap<QString, QWidget *> provider_pages_;
    QMap<QString, QPointer<QWidget>> credential_edits_;
    std::vector<std::pair<QLabel *, QString>> credential_labels_;
    QLabel *row_provider_lbl_ = nullptr;
    QLabel *row_lang_lbl_ = nullptr;
};

// The last page: startup options and the desktop shortcut.
class FinishPage : public BasePage {
    Q_OBJECT
public:
    explicit FinishPage(ToolSettingsManager *config, QWidget *parent = nullptr);
    void retranslate() override;
    // Marks the wizard done and stores preferences; autostart and the desktop shortcut run in
    // the background so file and network path checks never block the UI thread.
    void save() override;
    // The stored preferences alone, without touching autostart or the desktop.
    void save_preferences();
    // Creates the desktop shortcut to this program.
    static void create_desktop_shortcut();
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    ToolSettingsManager *config_;
    WelcomeToggleSwitch *autostart_switch_ = nullptr;
    WelcomeToggleSwitch *show_main_switch_ = nullptr;
    WelcomeToggleSwitch *desktop_switch_ = nullptr;
    WelcomeToggleSwitch *game_pause_switch_ = nullptr;
    QLabel *autostart_lbl_ = nullptr, *autostart_desc_ = nullptr, *show_main_lbl_ = nullptr, *show_main_desc_ = nullptr;
    QLabel *desktop_lbl_ = nullptr, *desktop_desc_ = nullptr, *game_pause_lbl_ = nullptr, *game_pause_desc_ = nullptr;
};
}
