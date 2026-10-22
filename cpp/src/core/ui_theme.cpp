#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include <QApplication>
#include <QGuiApplication>
#include <QStyleHints>
#include <QWidget>

namespace jietuba {
QString ui_theme_mode_value(UIThemeMode mode) {
    switch (mode) {
    case UIThemeMode::LIGHT: return "light";
    case UIThemeMode::DARK: return "dark";
    default: return "system";
    }
}

UIThemeMode coerce_ui_theme_mode(const QVariant &value) {
    const auto text = value.toString().toLower();
    if (text == "light") return UIThemeMode::LIGHT;
    if (text == "dark") return UIThemeMode::DARK;
    return UIThemeMode::SYSTEM;
}

namespace {
UIThemeTokens common_tokens(UIThemeMode mode) {
    UIThemeTokens t;
    t.mode = mode;
    t.accent = "#6F8FAB";
    t.accent_hover = "#627F99";
    t.accent_pressed = "#526D85";
    t.accent_strong = "#58748D";
    t.accent_strong_hover = "#4F6A82";
    t.accent_strong_pressed = "#465E73";
    return t;
}
}

const UIThemeTokens &LIGHT_TOKENS() {
    static const UIThemeTokens tokens = [] {
        auto t = common_tokens(UIThemeMode::LIGHT);
        t.accent_soft = "#DFE8EF";
        t.accent_text = "#465E73";
        t.window = "#EEF0F3";
        t.window_top = "#F5F6F8";
        t.window_bottom = "#ECEEF1";
        t.surface = "rgba(255, 255, 255, 0.92)";
        t.surface_strong = "rgba(255, 255, 255, 0.98)";
        t.surface_subtle = "rgba(248, 251, 253, 0.85)";
        t.surface_hover = "#FFFFFF";
        t.text = "#17201B";
        t.text_muted = "#68736D";
        t.text_disabled = "#9AA39D";
        t.border = "rgba(112, 130, 119, 0.20)";
        t.border_hover = "rgba(76, 101, 86, 0.34)";
        t.window_border = "#D3D7DC";
        t.input_background = "#FFFFFF";
        t.popup_background = "#FFFFFF";
        t.popup_hover = "#EAF2FA";
        t.separator = "rgba(98, 116, 105, 0.13)";
        t.switch_off = "#C4CCC6";
        t.success = "#3E9B63";
        t.danger = "#CF4F4B";
        t.danger_soft = "#FBECEB";
        t.status_idle = "#BCC5BF";
        return t;
    }();
    return tokens;
}

const UIThemeTokens &DARK_TOKENS() {
    static const UIThemeTokens tokens = [] {
        auto t = common_tokens(UIThemeMode::DARK);
        t.accent_soft = "#31404C";
        t.accent_text = "#B9CCDD";
        t.window = "#1B1E22";
        t.window_top = "#24282D";
        t.window_bottom = "#191C20";
        t.surface = "rgba(43, 47, 52, 0.96)";
        t.surface_strong = "rgba(49, 54, 60, 0.99)";
        t.surface_subtle = "rgba(36, 40, 45, 0.92)";
        t.surface_hover = "#373C43";
        t.text = "#F2F4F6";
        t.text_muted = "#AEB5BD";
        t.text_disabled = "#737B84";
        t.border = "rgba(255, 255, 255, 0.12)";
        t.border_hover = "rgba(255, 255, 255, 0.28)";
        t.window_border = "#3B424A";
        t.input_background = "#2B2F34";
        t.popup_background = "#292D32";
        t.popup_hover = "#363B42";
        t.separator = "rgba(255, 255, 255, 0.10)";
        t.switch_off = "#626A73";
        t.success = "#4FAE74";
        t.danger = "#E0605B";
        t.danger_soft = "rgba(224, 96, 91, 0.14)";
        t.status_idle = "#5A6168";
        return t;
    }();
    return tokens;
}

UIThemeManager::UIThemeManager(QObject *parent) : QObject(parent) {}

UIThemeManager *UIThemeManager::init(ToolSettingsManager *config_manager, QApplication *application) {
    config_ = config_manager;
    application_ = application ? application : qobject_cast<QApplication *>(QCoreApplication::instance());
    const auto saved = config_manager ? QVariant(config_manager->get_ui_theme_mode()) : QVariant("system");
    mode_ = coerce_ui_theme_mode(saved);
    connect_system_theme();
    apply(true);
    return this;
}

void UIThemeManager::set_mode(const QVariant &mode, bool persist) {
    const auto normalized = coerce_ui_theme_mode(mode);
    const bool changed = normalized != mode_;
    mode_ = normalized;
    if (persist && config_) config_->set_ui_theme_mode(ui_theme_mode_value(normalized));
    apply(changed);
    if (changed) emit mode_changed(ui_theme_mode_value(normalized));
}

void UIThemeManager::apply(bool force) {
    const auto effective = resolve_effective_mode();
    const bool changed = effective != effective_mode_;
    effective_mode_ = effective;
    if (!(force || changed)) return;
    if (application_) {
        application_->setProperty("uiTheme", ui_theme_mode_value(effective));
        application_->setPalette(build_palette(tokens()));
        application_->setStyleSheet(build_global_stylesheet(tokens()));
    }
    emit theme_changed(tokens());
}

UIThemeMode UIThemeManager::resolve_effective_mode() const {
    if (mode_ != UIThemeMode::SYSTEM) return mode_;
    if (auto *hints = QGuiApplication::styleHints()) {
        const auto scheme = hints->colorScheme();
        if (scheme == Qt::ColorScheme::Dark) return UIThemeMode::DARK;
        if (scheme == Qt::ColorScheme::Light) return UIThemeMode::LIGHT;
    }
    // Some remote-desktop and older platform plugins report Unknown.
    auto *app = application_ ? application_ : qobject_cast<QApplication *>(QCoreApplication::instance());
    if (app) return app->palette().color(QPalette::Window).lightness() < 128 ? UIThemeMode::DARK : UIThemeMode::LIGHT;
    return UIThemeMode::LIGHT;
}

void UIThemeManager::connect_system_theme() {
    if (system_signal_connected_) return;
    if (auto *hints = QGuiApplication::styleHints()) {
        connect(hints, &QStyleHints::colorSchemeChanged, this, [this](Qt::ColorScheme) {
            if (mode_ == UIThemeMode::SYSTEM) apply();
        });
        system_signal_connected_ = true;
    }
}

QPalette UIThemeManager::build_palette(const UIThemeTokens &t) {
    QPalette palette;
    const std::pair<QPalette::ColorRole, QString> roles[] = {
        {QPalette::Window, t.window},
        {QPalette::WindowText, t.text},
        {QPalette::Base, t.input_background},
        {QPalette::AlternateBase, t.surface_subtle},
        {QPalette::ToolTipBase, t.popup_background},
        {QPalette::ToolTipText, t.text},
        {QPalette::Text, t.text},
        {QPalette::Button, t.surface_strong},
        {QPalette::ButtonText, t.text},
        {QPalette::BrightText, "#FFFFFF"},
        {QPalette::Link, t.accent},
        {QPalette::Highlight, t.accent},
        {QPalette::HighlightedText, t.selected_text},
        {QPalette::PlaceholderText, t.text_muted},
        {QPalette::Mid, t.border_hover},
        {QPalette::Dark, t.window_border},
    };
    for (const auto group : {QPalette::Normal, QPalette::Inactive, QPalette::Disabled})
        for (const auto &[role, value] : roles) palette.setColor(group, role, QColor::fromString(value));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::Disabled, role, QColor::fromString(t.text_disabled));
    return palette;
}

QString UIThemeManager::build_global_stylesheet(const UIThemeTokens &t) {
    // Kept small; feature windows with their own theme override it locally.
    return QString(R"(
            QToolTip {
                color: %1;
                background-color: %2;
                border: 1px solid %3;
                padding: 4px 7px;
            }
            QMenu {
                color: %1;
                background-color: %2;
                border: 1px solid %4;
                border-radius: 6px;
                padding: 4px;
            }
            QMenu::item {
                color: %1;
                background-color: transparent;
                padding: 6px 28px 6px 26px;
                margin: 1px 0;
            }
            QMenu::item:selected {
                color: %1;
                background-color: %5;
            }
            QMenu::item:disabled {
                color: %6;
                background-color: transparent;
            }
            QMenu::separator {
                height: 1px;
                background: %7;
                margin: 4px 7px;
            }
        )").arg(t.text, t.popup_background, t.border_hover, t.border, t.popup_hover, t.text_disabled, t.separator);
}

void apply_style_sheet(QWidget *widget, const QString &sheet) {
    if (widget->styleSheet() != sheet) widget->setStyleSheet(sheet);
}

void set_own_style(QWidget *widget, const QString &declarations) {
    // Qt style sheets spell a namespaced class "ns--Class".
    const auto name = QString::fromLatin1(widget->metaObject()->className()).replace("::", "--");
    apply_style_sheet(widget, QString(".%1 { %2 }").arg(name, declarations));
}

UIThemeManager *get_ui_theme() {
    static UIThemeManager *manager = new UIThemeManager;
    return manager;
}
}
