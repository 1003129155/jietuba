#include "clipboard/ui/theme/themes.h"
#include "settings/tool_settings.h"

namespace jietuba::clipboard {
namespace {
Theme make_dark() {
    ThemeColors c;
    c.bg_primary = "#1E1E1E";
    c.bg_secondary = "#3B3B3D";
    c.bg_tertiary = "#2D2D30";
    c.bg_hover = "#3E3E42";
    c.bg_selected = "#0B4B75";
    c.bg_selected_highlight = "rgba(0, 122, 204, 0.5)";
    c.text_primary = "#E0E0E0";
    c.text_secondary = "#AAAAAA";
    c.text_tertiary = "#888888";
    c.text_disabled = "#555555";
    c.text_accent = "#4FC3F7";
    c.border_primary = "#3E3E42";
    c.border_secondary = "#2D2D30";
    c.border_accent = "#007ACC";
    c.border_selected = "#007ACC";
    c.accent_primary = "#007ACC";
    c.accent_hover = "#005A9E";
    c.success = "#4CAF50";
    c.success_bg = "#1B5E20";
    c.success_hover = "#388E3C";
    c.error = "#F44336";
    c.error_bg = "#5D1A1A";
    c.error_hover = "#E53935";
    c.warning = "#FFA726";
    c.warning_bg = "#663C00";
    c.drag_drop_border = "#4CAF50";
    c.separator = "#3E3E42";
    c.shortcut_key_color = "#9CA2C5";
    return {"dark", QString::fromUtf8("深色"), c, QString::fromUtf8("护眼深色主题，适合夜间使用")};
}

// The pastel presets share their status colours; only these fields differ.
Theme make_tinted(const char *name, const char *display_name, const char *description, std::initializer_list<QString> values) {
    const auto v = std::data(values);
    ThemeColors c;
    c.bg_primary = v[0];
    c.bg_secondary = v[1];
    c.bg_tertiary = v[2];
    c.bg_hover = v[3];
    c.bg_selected = v[4];
    c.bg_selected_highlight = v[5];
    c.text_primary = v[6];
    c.text_secondary = v[7];
    c.text_tertiary = v[8];
    c.text_accent = v[9];
    c.border_primary = v[10];
    c.border_secondary = v[11];
    c.border_accent = v[12];
    c.border_selected = v[13];
    c.accent_primary = v[14];
    c.accent_hover = v[15];
    c.success = v[16];
    c.success_bg = v[17];
    c.error = v[18];
    c.error_bg = v[19];
    c.shortcut_key_color = v[20];
    return {name, QString::fromUtf8(display_name), c, QString::fromUtf8(description)};
}
}

const Theme &THEME_LIGHT() {
    static const Theme theme{"light", QString::fromUtf8("浅色（默认）"), ThemeColors(), QString::fromUtf8("Material Design 风格的浅色主题")};
    return theme;
}

const std::vector<std::pair<QString, Theme>> &PRESET_THEMES() {
    static const std::vector<std::pair<QString, Theme>> themes{
        {"light", THEME_LIGHT()},
        {"dark", make_dark()},
        {"blue", make_tinted("blue", "海洋蓝", "清新的蓝色系主题",
                             {"#EFF3F6", "#DDE6EC", "#D5E0E8", "#C8D6E0", "#AFC2D0", "rgba(0, 0, 0, 0.20)", "#283B4B", "#435D72", "#607A90", "#58738C",
                              "#BCCBD6", "#D1DCE4", "#6F8FAB", "#58738C", "#6F8FAB", "#627F99", "#4CAF50", "#C8E6C9", "#F44336", "#FFCDD2", "#435D72"})},
        {"green", make_tinted("green", "清新绿", "自然清新的绿色主题",
                              {"#F1F8F4", "#DBF7DD", "#C8E6C9", "#A5D6A7", "#78CF7D", "rgba(0, 0, 0, 0.20)", "#1B5E20", "#388E3C", "#66BB6A", "#4CAF50",
                               "#A5D6A7", "#C8E6C9", "#4CAF50", "#388E3C", "#4CAF50", "#388E3C", "#66BB6A", "#C8E6C9", "#F44336", "#FFCDD2", "#1B5E20"})},
        {"pink", make_tinted("pink", "樱花粉", "温柔的粉色系主题",
                             {"#FFF0F5", "#FAD6E2", "#F8BBD0", "#F48FB1", "#EE5A8C", "rgba(203, 30, 99, 0.30)", "#880E4F", "#C2185B", "#E91E63", "#E91E63",
                              "#F8BBD0", "#FCE4EC", "#E91E63", "#C2185B", "#E91E63", "#C2185B", "#4CAF50", "#C8E6C9", "#F44336", "#FFCDD2", "#880E4F"})},
        {"purple", make_tinted("purple", "优雅紫", "优雅的紫色系主题",
                               {"#F3E5F5", "#E4BCEB", "#CE93D8", "#BA68C8", "#AD42C0", "rgba(156, 39, 176, 0.50)", "#4A148C", "#6A1B9A", "#8E24AA", "#9C27B0",
                                "#CE93D8", "#E1BEE7", "#9C27B0", "#7B1FA2", "#9C27B0", "#7B1FA2", "#4CAF50", "#C8E6C9", "#F44336", "#FFCDD2", "#4A148C"})},
        {"orange", make_tinted("orange", "活力橙", "充满活力的橙色主题",
                               {"#FFF3E0", "#FCD9A5", "#FFCC80", "#FFB74D", "#FCA31F", "rgba(255, 152, 0, 0.50)", "#E65100", "#F57C00", "#FF9800", "#FF9800",
                                "#FFCC80", "#FFE0B2", "#FF9800", "#F57C00", "#FF9800", "#F57C00", "#4CAF50", "#C8E6C9", "#F44336", "#FFCDD2", "#BF360C"})},
    };
    return themes;
}

const Theme *preset_theme(const QString &name) {
    for (const auto &[key, theme] : PRESET_THEMES())
        if (key == name) return &theme;
    return nullptr;
}

const std::vector<std::pair<QString, std::pair<QString, QString>>> &PRESET_THEME_SWATCHES() {
    static const std::vector<std::pair<QString, std::pair<QString, QString>>> swatches{
        {"light", {"#DDE3E9", "#FFFFFF"}},  {"dark", {"#1D1F20", "#1E1E1E"}},   {"blue", {"#6F8FAB", "#B9CAD7"}},     {"green", {"#4CAF50", "#B8F3BD"}},
        {"pink", {"#E91E63", "#FA9BBB"}},   {"purple", {"#9C27B0", "#D471E4"}}, {"orange", {"#FF9800", "#F5C880"}},
    };
    return swatches;
}

ThemeManager::ThemeManager() {
    const auto *saved = preset_theme(get_tool_settings_manager()->get_clipboard_theme());
    current_theme_ = saved ? *saved : THEME_LIGHT();
}

bool ThemeManager::set_theme(const QString &theme_name) {
    const auto *theme = preset_theme(theme_name);
    if (!theme) return false;
    current_theme_ = *theme;
    get_tool_settings_manager()->set_clipboard_theme(theme_name);
    emit theme_changed(current_theme_);
    return true;
}

ThemeManager *get_theme_manager() {
    static auto *manager = new ThemeManager;
    return manager;
}
}
