#pragma once
#include <QColor>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPalette>
#include <QString>
#include <QVariant>

class QApplication;
class QWidget;

namespace jietuba {
class ToolSettingsManager;

// Light/dark appearance of application windows. Screenshot accent and mask
// colours live in core/theme instead.
enum class UIThemeMode { SYSTEM, LIGHT, DARK };
QString ui_theme_mode_value(UIThemeMode mode);
UIThemeMode coerce_ui_theme_mode(const QVariant &value);

// A new colour field must also be listed in ui_token_entries(), which the design gallery shows.
struct UIThemeTokens {
    UIThemeMode mode;
    QString accent, accent_hover, accent_pressed, accent_soft;
    // White text on accent is only 3.4:1; fills carrying white text use the darker
    // accent_strong (4.9:1), switches and indicators keep accent.
    QString accent_strong, accent_strong_hover, accent_strong_pressed;
    // Text and icons on accent_soft, e.g. the selected navigation or tab item.
    QString accent_text;
    QString success, danger, danger_soft, status_idle;
    QString selected_text = "#FFFFFF";
    // Window, the content layer over it, the page cards and their expanded bodies, and the window's outline.
    QString page_background, layer, layer_border, card, card_border, card_body, window_border;
    // Text, controls (buttons, inputs), hover fill of list items, the line between rows, a switch's off fill.
    QString fg, fg_muted, fg_disabled;
    QString control, control_hover, control_pressed, control_border, control_border_bottom;
    QString item_hover, divider, switch_off_fill;
    // Menus, drop-down lists and tooltips, opaque because they float over other windows.
    QString popup_background, popup_hover;
    bool is_dark() const { return mode == UIThemeMode::DARK; }
};

const UIThemeTokens &LIGHT_TOKENS();
const UIThemeTokens &DARK_TOKENS();
// Every colour field as (field name, value), in declaration order; the mode is not included.
QList<QPair<QString, QString>> ui_token_entries(const UIThemeTokens &tokens);

// Parses token colours for QPainter and QPalette; QColor::fromString rejects rgb()/rgba().
QColor css_color(const QString &value);
// `top` as it looks painted over the opaque `under`.
QColor painted_over(const QColor &top, const QColor &under);

class UIThemeManager : public QObject {
    Q_OBJECT
public:
    explicit UIThemeManager(QObject *parent = nullptr);
    UIThemeManager *init(ToolSettingsManager *config_manager = nullptr, QApplication *application = nullptr);
    UIThemeMode mode() const { return mode_; }
    UIThemeMode effective_mode() const { return effective_mode_; }
    bool is_dark() const { return effective_mode_ == UIThemeMode::DARK; }
    const UIThemeTokens &tokens() const { return is_dark() ? DARK_TOKENS() : LIGHT_TOKENS(); }
    void set_mode(const QVariant &mode, bool persist = true);
    void apply(bool force = false);
    static QPalette build_palette(const UIThemeTokens &t);
    static QString build_global_stylesheet(const UIThemeTokens &t);
signals:
    void theme_changed(const jietuba::UIThemeTokens &tokens);
    void mode_changed(const QString &mode);
private:
    UIThemeMode resolve_effective_mode() const;
    void connect_system_theme();
    ToolSettingsManager *config_ = nullptr;
    QApplication *application_ = nullptr;
    UIThemeMode mode_ = UIThemeMode::SYSTEM;
    UIThemeMode effective_mode_ = UIThemeMode::LIGHT;
    bool system_signal_connected_ = false;
};

// setStyleSheet does not compare contents and restyles every child; skip unchanged sheets.
void apply_style_sheet(QWidget *widget, const QString &sheet);
// Styles a widget with a rule matching only its own class. A selector-less sheet
// (or QWidget/QFrame/QLabel rules) would also reach QTipLabel, and tooltips
// inherit ancestor sheets with priority over the global QToolTip rule.
void set_own_style(QWidget *widget, const QString &declarations);

UIThemeManager *get_ui_theme();

// Translation style: the translation window and the translation popup keep a look of
// their own instead of the window palette above.
struct DashboardPalette {
    QString window, field, surface_strong, text, text_2, text_3, accent, accent_hover, accent_tint, fill, fill_hover;
    QColor fill_color, fill_hover_color;
    QString danger, green;
    // Text, selection and caption symbols drawn over accent or danger fills.
    QString on_accent;
    // The primary button while disabled.
    QString accent_disabled;
    // Window background gradient from the top edge; equal stops draw a flat window.
    QString window_top, window_bottom;
    // The pinned (checked) always-on-top button's background.
    QColor pin_checked_fill;
};
const DashboardPalette &dashboard_dark();
const DashboardPalette &dashboard_light();
}
