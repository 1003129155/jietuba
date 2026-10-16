#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include <QFont>
#include <QMetaMethod>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <limits>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jietuba {
namespace {
double number(const QVariant &value, bool *ok) {
    if (!value.isValid() || value.isNull() || value.metaType().id() == QMetaType::QVariantList ||
        value.metaType().id() == QMetaType::QVariantMap || value.metaType().id() == QMetaType::QStringList) {
        *ok = false;
        return 0;
    }
    return value.toDouble(ok);
}
int scaled_pixel(double base, double factor) {
    if (base == 0) return 0;
    const auto value = python_round(base * factor);
    return int(base > 0 ? std::max(qint64(1), value) : std::min(qint64(-1), value));
}
}
qint64 python_round(double value) {
    if (!std::isfinite(value)) throw std::overflow_error("Cannot convert non-finite Python round result to integer");
    if (value >= double(std::numeric_limits<qint64>::max()) || value < double(std::numeric_limits<qint64>::min())) throw std::overflow_error("Python integer exceeds C++ qint64");
    const auto lower = std::floor(value);
    const auto fraction = value - lower;
    auto result = lower;
    if (fraction > .5 || (fraction == .5 && std::fmod(lower, 2.0) != 0)) result += 1;
    return qint64(result);
}
UIScaleManager::UIScaleManager(QString setting, QObject *parent) : QObject(parent), setting_(std::move(setting)) {}
UIScaleManager &UIScaleManager::instance() { static UIScaleManager instance; return instance; }
DialogScaleManager &DialogScaleManager::instance() { static DialogScaleManager instance; return instance; }
void UIScaleManager::init(ToolSettingsManager *config) {
    config_ = config;
    percent_ = normalize_percent(config ? config->get_app_setting(setting_) : QVariant());
}
int UIScaleManager::normalize_percent(const QVariant &input) {
    bool ok = false;
    const auto value = number(input, &ok);
    if (!ok || std::isnan(value)) return 100;
    if (std::isinf(value)) throw std::overflow_error("Cannot round infinity");
    const auto rounded = value > 1000 ? 1000 : value < -1000 ? -1000 : python_round(value);
    const auto &options = PERCENT_OPTIONS;
    int best = options[0];
    for (int option : options) if (std::abs(option - rounded) < std::abs(best - rounded)) best = option;
    return best;
}
int UIScaleManager::px(double base) const { return scaled_pixel(base, factor()); }
bool UIScaleManager::set_percent(const QVariant &value) {
    const auto percent = normalize_percent(value);
    const bool changed = percent != percent_;
    percent_ = percent;
    if (config_) config_->set_app_setting(setting_, percent);
    if (changed) emit scale_changed();
    return changed;
}
int recommended_scale_percent(QVariant system_dpi) {
    if (!system_dpi.isValid()) {
#ifdef Q_OS_WIN
        system_dpi = double(GetDpiForSystem());
#else
        system_dpi = 96.0;
#endif
    }
    bool ok = false;
    const auto dpi = number(system_dpi, &ok);
    double percent = ok ? dpi / 96.0 * 100.0 : 100;
    if (!std::isfinite(percent) || percent <= 0) percent = 100;
    if (percent > 150) return 150;
    if (percent > 100) return 125;
    return 100;
}
std::optional<int> apply_first_run_scale_defaults(ToolSettingsManager &config, QVariant dpi) {
    if (!config.is_first_run()) return std::nullopt;
    const auto recommended = recommended_scale_percent(dpi);
    for (const auto &key : {QString("ui_scale_percent"), QString("dialog_scale_percent")}) {
        if (!config.qsettings.contains("app/" + key)) config.set_app_setting(key, recommended);
    }
    return recommended;
}
UIScaleManager &get_ui_scale() { return UIScaleManager::instance(); }
DialogScaleManager &get_dialog_scale() { return DialogScaleManager::instance(); }
int scaled(double base) { return get_ui_scale().px(base); }
double scaled_f(double base) { return get_ui_scale().pxf(base); }
double scale_factor() { return get_ui_scale().factor(); }
int dialog_scaled(double base) { return get_dialog_scale().px(base); }
double dialog_scaled_f(double base) { return get_dialog_scale().pxf(base); }
int widget_scaled(const QWidget *widget, double base) {
    double factor = 1;
    if (widget) {
        const auto property = widget->property("dialog_scale_factor");
        bool ok = false;
        const auto value = number(property, &ok);
        // Python evaluates `property or 1.0` BEFORE float conversion: the
        // nonempty string "0" stays zero; numeric zero falls back to one.
        const bool nonemptyString = property.metaType().id() == QMetaType::QString && !property.toString().isEmpty();
        if (ok && (value != 0 || nonemptyString)) factor = value;
    }
    return scaled_pixel(base, factor);
}
void scale_dialog_font(QWidget &widget) {
    const auto factor = get_dialog_scale().factor();
    if (factor == 1) return;
    auto font = widget.font();
    if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * factor);
    else if (font.pixelSize() > 0) font.setPixelSize(dialog_scaled(font.pixelSize()));
    widget.setFont(font);
}
void configure_dialog_control(QWidget &widget) {
    widget.setProperty("dialog_scale_factor", get_dialog_scale().factor());
    if (widget.metaObject()->indexOfMethod("_apply_theme()") >= 0) QMetaObject::invokeMethod(&widget, "_apply_theme", Qt::DirectConnection);
}
void configure_dialog_controls(QWidget &root) {
    configure_dialog_control(root);
    for (auto *widget : root.findChildren<QWidget *>()) {
        if (widget->metaObject()->indexOfMethod("_apply_theme()") >= 0) configure_dialog_control(*widget);
    }
}
}
