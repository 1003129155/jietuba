#include "ui/fluent_lite/labels.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/theme.h"
#include <QSizePolicy>

namespace jietuba {
BodyLabel::BodyLabel(const QString &text, QWidget *parent) : QLabel(text, parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void BodyLabel::_apply_theme() {
    set_own_style(this, QString("background: transparent; color: %1; font: %2px %3;").arg(ui_tokens(this).fg).arg(widget_scaled(this, 14)).arg(FONT_FAMILY));
}

CaptionLabel::CaptionLabel(const QString &text, QWidget *parent) : QLabel(text, parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
    setWordWrap(true);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void CaptionLabel::_apply_theme() {
    set_own_style(this, QString("background: transparent; color: %1; font: %2px %3;").arg(ui_tokens(this).fg_muted).arg(widget_scaled(this, 12)).arg(FONT_FAMILY));
}
}
