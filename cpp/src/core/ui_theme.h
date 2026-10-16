#pragma once
#include <QObject>
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

struct UIThemeTokens {
    UIThemeMode mode;
    QString accent, accent_hover, accent_pressed, accent_soft;
    QString window, window_top, window_bottom;
    QString surface, surface_strong, surface_subtle, surface_hover;
    QString text, text_muted, text_disabled;
    QString border, border_hover, window_border;
    QString input_background, popup_background, popup_hover, separator, switch_off;
    // White text on accent is only 3.4:1; fills carrying white text use the darker
    // accent_strong (4.9:1), switches and indicators keep accent.
    QString accent_strong, accent_strong_hover, accent_strong_pressed;
    // Text and icons on accent_soft, e.g. the selected navigation or tab item.
    QString accent_text;
    QString success, danger, danger_soft, status_idle;
    QString selected_text = "#FFFFFF";
    bool is_dark() const { return mode == UIThemeMode::DARK; }
};

const UIThemeTokens &LIGHT_TOKENS();
const UIThemeTokens &DARK_TOKENS();

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
}
