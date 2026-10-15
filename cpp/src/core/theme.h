#pragma once
#include <QColor>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <array>

namespace jietuba {
class ToolSettingsManager;

// Global theme colour and selection appearance shared by every capture component.
class ThemeManager {
public:
    // Selection border widths in physical pixels; 1 is the thinnest still visible.
    static constexpr std::array<int, 8> BORDER_WIDTH_OPTIONS = {1, 2, 3, 4, 5, 6, 8, 10};
    static constexpr int DEFAULT_BORDER_WIDTH = 4;
    static inline const QString HANDLES_ALL = "all";
    static inline const QString HANDLES_CORNERS = "corners";
    static inline const QString HANDLES_NONE = "none";
    static inline const QStringList HANDLE_STYLE_OPTIONS = {HANDLES_ALL, HANDLES_CORNERS, HANDLES_NONE};
    // Dot diameter and ring width scale together, so only small/medium/large are offered.
    static inline const QString HANDLE_SIZE_SMALL = "small";
    static inline const QString HANDLE_SIZE_MEDIUM = "medium";
    static inline const QString HANDLE_SIZE_LARGE = "large";
    static inline const QStringList HANDLE_SIZE_OPTIONS = {HANDLE_SIZE_SMALL, HANDLE_SIZE_MEDIUM, HANDLE_SIZE_LARGE};
    static inline const QString DEFAULT_HANDLE_SIZE = HANDLE_SIZE_SMALL;

    static ThemeManager &instance();
    void init(ToolSettingsManager *config_manager);

    QColor theme_color() const { return theme_color_; }
    QString theme_color_hex() const { return theme_color_.name().toUpper(); }
    QColor mask_color() const { return mask_color_; }
    int selection_border_width() const { return selection_border_width_; }
    QString selection_handle_style() const { return selection_handle_style_; }
    QString selection_handle_size() const { return selection_handle_size_; }
    int selection_handle_diameter() const;
    int selection_handle_ring_width() const;

    void set_theme_color(const QColor &color);
    void set_mask_color(const QColor &color);
    void set_selection_border_width(const QVariant &width);
    void set_selection_handle_style(QString style);
    void set_selection_handle_size(QString size);
    void reset_to_defaults();

    static int clamp_border_width(const QVariant &value);
private:
    ThemeManager() = default;
    void load_from_settings();
    ToolSettingsManager *config_manager_ = nullptr;
    QColor theme_color_ = QColor("#40E0D0");
    QColor mask_color_ = QColor(0, 0, 0, 120);
    int selection_border_width_ = DEFAULT_BORDER_WIDTH;
    QString selection_handle_style_ = HANDLES_ALL;
    QString selection_handle_size_ = DEFAULT_HANDLE_SIZE;
};

inline ThemeManager &get_theme() { return ThemeManager::instance(); }
// Dark or white ink for content drawn on background, chosen by luminance.
QColor contrast_ink(const QColor &background);
}
