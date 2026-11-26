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
        t.window_border = "#D5D5D5";
        t.popup_background = "#FFFFFF";
        t.popup_hover = "#F0F0F0";
        t.success = "#3E9B63";
        t.danger = "#CF4F4B";
        t.danger_soft = "#FBECEB";
        t.status_idle = "#C2C2C2";
        t.page_background = "#F3F3F3";
        t.layer = "rgba(255, 255, 255, 0.5)";
        t.layer_border = "rgba(0, 0, 0, 0.0578)";
        t.card = "rgba(255, 255, 255, 0.7)";
        t.card_border = "rgba(0, 0, 0, 0.0578)";
        t.card_body = "rgba(246, 246, 246, 0.5)";
        t.fg = "rgba(0, 0, 0, 0.894)";
        t.fg_muted = "rgba(0, 0, 0, 0.606)";
        t.fg_disabled = "rgba(0, 0, 0, 0.361)";
        t.control = "rgba(255, 255, 255, 0.7)";
        t.control_hover = "rgba(249, 249, 249, 0.5)";
        t.control_pressed = "rgba(249, 249, 249, 0.3)";
        t.control_border = "rgba(0, 0, 0, 0.0578)";
        t.control_border_bottom = "rgba(0, 0, 0, 0.162)";
        t.item_hover = "rgba(0, 0, 0, 0.037)";
        t.divider = "rgba(0, 0, 0, 0.0578)";
        t.switch_off_fill = "rgba(0, 0, 0, 0.024)";
        return t;
    }();
    return tokens;
}

const UIThemeTokens &DARK_TOKENS() {
    static const UIThemeTokens tokens = [] {
        auto t = common_tokens(UIThemeMode::DARK);
        t.accent_soft = "#31404C";
        t.accent_text = "#B9CCDD";
        t.window_border = "#404040";
        t.popup_background = "#2C2C2C";
        t.popup_hover = "#383838";
        t.success = "#4FAE74";
        t.danger = "#E0605B";
        t.danger_soft = "rgba(224, 96, 91, 0.14)";
        t.status_idle = "#5F5F5F";
        t.page_background = "#202020";
        t.layer = "rgba(58, 58, 58, 0.3)";
        t.layer_border = "rgba(0, 0, 0, 0.3)";
        t.card = "rgba(255, 255, 255, 0.051)";
        t.card_border = "rgba(0, 0, 0, 0.1)";
        t.card_body = "rgba(255, 255, 255, 0.03)";
        t.fg = "#FFFFFF";
        t.fg_muted = "rgba(255, 255, 255, 0.786)";
        t.fg_disabled = "rgba(255, 255, 255, 0.363)";
        t.control = "rgba(255, 255, 255, 0.0605)";
        t.control_hover = "rgba(255, 255, 255, 0.0837)";
        t.control_pressed = "rgba(255, 255, 255, 0.0326)";
        t.control_border = "rgba(255, 255, 255, 0.0698)";
        t.control_border_bottom = "rgba(255, 255, 255, 0.0698)";
        t.item_hover = "rgba(255, 255, 255, 0.061)";
        t.divider = "rgba(255, 255, 255, 0.0837)";
        t.switch_off_fill = "rgba(255, 255, 255, 0.03)";
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

QColor css_color(const QString &value) {
    const auto text = value.trimmed();
    if (text.startsWith("rgba(") || text.startsWith("rgb(")) {
        const auto inner = text.mid(text.indexOf('(') + 1).chopped(1);
        const auto channels = inner.split(',');
        QColor color(channels.value(0).trimmed().toInt(), channels.value(1).trimmed().toInt(), channels.value(2).trimmed().toInt());
        if (channels.size() == 4) color.setAlphaF(channels[3].trimmed().toDouble());
        return color;
    }
    return QColor::fromString(text);
}

QColor painted_over(const QColor &top, const QColor &under) {
    const double a = top.alphaF();
    return QColor::fromRgbF(top.redF() * a + under.redF() * (1 - a), top.greenF() * a + under.greenF() * (1 - a),
                            top.blueF() * a + under.blueF() * (1 - a));
}

// Opaque colours: widgets without a sheet of their own may lie on any window background.
QPalette UIThemeManager::build_palette(const UIThemeTokens &t) {
    const QColor page = css_color(t.page_background);
    const auto on_page = [&page](const QString &token) { return painted_over(css_color(token), page); };
    const QColor base = on_page(t.control);
    QPalette palette;
    const std::pair<QPalette::ColorRole, QColor> roles[] = {
        {QPalette::Window, page},
        {QPalette::WindowText, on_page(t.fg)},
        {QPalette::Base, base},
        {QPalette::AlternateBase, painted_over(css_color(t.item_hover), base)},
        {QPalette::ToolTipBase, css_color(t.popup_background)},
        {QPalette::ToolTipText, on_page(t.fg)},
        {QPalette::Text, on_page(t.fg)},
        {QPalette::Button, base},
        {QPalette::ButtonText, on_page(t.fg)},
        {QPalette::BrightText, QColor(Qt::white)},
        {QPalette::Link, css_color(t.accent)},
        {QPalette::Highlight, css_color(t.accent)},
        {QPalette::HighlightedText, css_color(t.selected_text)},
        {QPalette::PlaceholderText, on_page(t.fg_muted)},
        {QPalette::Mid, on_page(t.control_border_bottom)},
        {QPalette::Dark, css_color(t.window_border)},
    };
    for (const auto group : {QPalette::Normal, QPalette::Inactive, QPalette::Disabled})
        for (const auto &[role, value] : roles) palette.setColor(group, role, value);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::Disabled, role, on_page(t.fg_disabled));
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
        )").arg(t.fg, t.popup_background, t.control_border_bottom, t.control_border_bottom, t.popup_hover, t.fg_disabled, t.divider);
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
