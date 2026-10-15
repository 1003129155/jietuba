#pragma once
#include <QMetaType>
#include <QObject>
#include <QString>
#include <utility>
#include <vector>

namespace jietuba::clipboard {
// Colours of one clipboard theme; defaults are the light theme.
struct ThemeColors {
    // Backgrounds
    QString bg_primary = "#FFFFFF";
    QString bg_secondary = "#E0EAF0";
    QString bg_tertiary = "#F5F6F8";
    QString bg_hover = "#F0F0F0";
    QString bg_selected = "#DCE6ED";
    QString bg_selected_highlight = "rgba(0, 0, 0, 0.15)";
    // Text
    QString text_primary = "#333333";
    QString text_secondary = "#666666";
    QString text_tertiary = "#999999";
    QString text_disabled = "#CCCCCC";
    QString text_accent = "#58738C";
    // Borders
    QString border_primary = "#E0E0E0";
    QString border_secondary = "#E8E8E8";
    QString border_accent = "#6F8FAB";
    QString border_selected = "#6F8FAB";
    // Functional colours
    QString accent_primary = "#6F8FAB";
    QString accent_hover = "#627F99";
    QString success = "#4CAF50";
    QString success_bg = "#E8F5E9";
    QString success_hover = "#388E3C";
    QString error = "#F44336";
    QString error_bg = "#FFEBEE";
    QString error_hover = "#FFCDD2";
    QString warning = "#FF9800";
    QString warning_bg = "#FFF3E0";
    // Special
    QString drag_drop_border = "#4DAF50";
    QString separator = "#E0E0E0";
    QString shortcut_key_color = "#58738C";
};

struct Theme {
    QString name;
    QString display_name;
    ThemeColors colors;
    QString description;
};

// Preset themes in menu order.
const std::vector<std::pair<QString, Theme>> &PRESET_THEMES();
const Theme *preset_theme(const QString &name);
const Theme &THEME_LIGHT();
// The two-tone chip each theme shows in pickers: (accent, background). Chosen to look
// like the theme rather than computed, so light and dark do not read as blue.
const std::vector<std::pair<QString, std::pair<QString, QString>>> &PRESET_THEME_SWATCHES();

class ThemeManager : public QObject {
    Q_OBJECT
public:
    ThemeManager();
    const Theme &get_current_theme() const { return current_theme_; }
    // Applies and saves a preset; false for an unknown name.
    bool set_theme(const QString &theme_name);
    // Announcements from the settings pages.
    void notify_font_size_changed(int size) { emit font_size_changed(size); }
    void notify_opacity_changed(int percent) { emit opacity_changed(percent); }

signals:
    void theme_changed(const jietuba::clipboard::Theme &theme);
    void font_size_changed(int size);
    void opacity_changed(int percent);

private:
    Theme current_theme_;
};

ThemeManager *get_theme_manager();
}

Q_DECLARE_METATYPE(jietuba::clipboard::Theme)
