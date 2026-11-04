#pragma once
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

// The wizard pages.
namespace jietuba {
class HotkeyEdit;
class MouseBindingEditor;
class ToolSettingsManager;
class WelcomeWizard;

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

// The global hotkeys page: the main hotkey of each function, the settings window's names and
// order; further slots are set there. One real key bound to two actions fails silently on the
// second registration, so the editors share the settings window's duplicate rules
// (validate_hotkey_group).
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
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    struct Row {
        const char *function;
        // A SettingsDialog name.
        const char *title;
        bool keep_when_blank;
    };
    static const std::vector<Row> &rows();
    void on_focus_changed(QWidget *now);
    void sync_key_highlight();
    bool revalidate();
    ToolSettingsManager *config_;
    std::vector<std::pair<QLabel *, const char *>> labels_;
    std::vector<HotkeyEdit *> edits_;
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

// Where things are kept: the screenshot folder, the clipboard data folder and the settings
// file (green mode keeps it beside the program). The clipboard data and the settings file move
// at once, as in the settings window; the screenshot folder is written with the other pages.
class StoragePage : public BasePage {
    Q_OBJECT
public:
    explicit StoragePage(ToolSettingsManager *config, QWidget *parent = nullptr);
    void retranslate() override;
    void save() override;
protected:
    IllustrationArea *create_illustration() override;
    void build_controls(QVBoxLayout *layout) override;
private:
    QString green_description() const;
    ToolSettingsManager *config_;
    WelcomeToggleSwitch *autosave_toggle_ = nullptr;
    WelcomeToggleSwitch *green_toggle_ = nullptr;
    bool green_writable_ = false;
    QLabel *autosave_lbl_ = nullptr, *autosave_desc_ = nullptr, *clipboard_lbl_ = nullptr, *clipboard_desc_ = nullptr;
    QLabel *green_lbl_ = nullptr, *green_desc_ = nullptr;
    QLineEdit *path_edit_ = nullptr;
    QLineEdit *clipboard_edit_ = nullptr;
    QLineEdit *settings_edit_ = nullptr;
    QPushButton *browse_btn_ = nullptr;
    QPushButton *change_btn_ = nullptr;
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
