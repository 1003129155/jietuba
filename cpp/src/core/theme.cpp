#include "core/theme.h"
#include "settings/tool_settings.h"
#include <algorithm>
#include <cmath>

namespace jietuba {
namespace {
// Size option -> (dot diameter, ring width) in physical pixels.
std::pair<int, int> handle_size_entry(const QString &size) {
    if (size == ThemeManager::HANDLE_SIZE_MEDIUM) return {14, 3};
    if (size == ThemeManager::HANDLE_SIZE_LARGE) return {18, 4};
    return {10, 2};
}
}

ThemeManager &ThemeManager::instance() {
    static ThemeManager manager;
    return manager;
}

void ThemeManager::init(ToolSettingsManager *config_manager) {
    config_manager_ = config_manager;
    load_from_settings();
}

void ThemeManager::load_from_settings() {
    if (!config_manager_) return;
    const QString saved_hex = config_manager_->get_theme_color();
    if (!saved_hex.isEmpty()) {
        const QColor color(saved_hex);
        if (color.isValid()) theme_color_ = color;
    }
    mask_color_ = QColor(config_manager_->get_mask_color_r(), config_manager_->get_mask_color_g(), config_manager_->get_mask_color_b(), 120);
    selection_border_width_ = clamp_border_width(config_manager_->get_selection_border_width());
    const auto style = config_manager_->get_selection_handle_style();
    if (HANDLE_STYLE_OPTIONS.contains(style)) selection_handle_style_ = style;
    const auto size = config_manager_->get_selection_handle_size();
    if (HANDLE_SIZE_OPTIONS.contains(size)) selection_handle_size_ = size;
}

int ThemeManager::clamp_border_width(const QVariant &value) {
    // The config file can be hand-edited, so normalise on read. int() truncates
    // floats and parses only integer strings.
    bool ok = false;
    int width = 0;
    if (value.metaType().id() == QMetaType::Double || value.metaType().id() == QMetaType::Float) {
        const double number = value.toDouble();
        if (!std::isfinite(number)) return DEFAULT_BORDER_WIDTH;
        width = static_cast<int>(std::trunc(number));
        ok = true;
    } else {
        width = value.toString().trimmed().toInt(&ok);
    }
    if (!ok) return DEFAULT_BORDER_WIDTH;
    return std::max(BORDER_WIDTH_OPTIONS.front(), std::min(BORDER_WIDTH_OPTIONS.back(), width));
}

int ThemeManager::selection_handle_diameter() const { return handle_size_entry(selection_handle_size_).first; }
int ThemeManager::selection_handle_ring_width() const { return handle_size_entry(selection_handle_size_).second; }

void ThemeManager::set_theme_color(const QColor &color) {
    theme_color_ = color;
    if (config_manager_) config_manager_->set_theme_color(color.name());
}

void ThemeManager::set_mask_color(const QColor &color) {
    mask_color_ = QColor(color.red(), color.green(), color.blue(), 120);
    if (config_manager_) {
        config_manager_->set_mask_color_r(color.red());
        config_manager_->set_mask_color_g(color.green());
        config_manager_->set_mask_color_b(color.blue());
    }
}

void ThemeManager::set_selection_border_width(const QVariant &width) {
    selection_border_width_ = clamp_border_width(width);
    if (config_manager_) config_manager_->set_selection_border_width(selection_border_width_);
}

void ThemeManager::set_selection_handle_style(QString style) {
    if (!HANDLE_STYLE_OPTIONS.contains(style)) style = HANDLES_ALL;
    selection_handle_style_ = style;
    if (config_manager_) config_manager_->set_selection_handle_style(style);
}

void ThemeManager::set_selection_handle_size(QString size) {
    if (!HANDLE_SIZE_OPTIONS.contains(size)) size = DEFAULT_HANDLE_SIZE;
    selection_handle_size_ = size;
    if (config_manager_) config_manager_->set_selection_handle_size(size);
}

void ThemeManager::reset_to_defaults() {
    set_theme_color(QColor("#40E0D0"));
    set_mask_color(QColor(0, 0, 0, 120));
    set_selection_border_width(DEFAULT_BORDER_WIDTH);
    set_selection_handle_style(HANDLES_ALL);
    set_selection_handle_size(DEFAULT_HANDLE_SIZE);
}

QColor contrast_ink(const QColor &background) {
    const double luminance = 0.299 * background.red() + 0.587 * background.green() + 0.114 * background.blue();
    return luminance > 150 ? QColor(20, 20, 20) : QColor(255, 255, 255);
}
}
