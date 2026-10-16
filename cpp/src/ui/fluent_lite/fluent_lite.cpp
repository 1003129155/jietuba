#include "ui/fluent_lite/fluent_lite.h"
#include "core/ui_scale.h"
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionButton>

namespace jietuba {
CheckBox::CheckBox(const QString &text, QWidget *parent) : QCheckBox(text, parent) {
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void CheckBox::_apply_theme() {
    const int spacing = widget_scaled(this, LABEL_SPACING);
    apply_style_sheet(this, QString("QCheckBox { color: %1; background: transparent; spacing: %2px; font: %3px %4; }"
                                    "QCheckBox::indicator { width: %5px; height: %5px; }")
                                .arg(ui_tokens(this).text).arg(spacing).arg(widget_scaled(this, 13)).arg(FONT_FAMILY).arg(widget_scaled(this, 16)));
}

void CheckBox::paintEvent(QPaintEvent *) {
    QStyleOptionButton option;
    initStyleOption(&option);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto indicator = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
    auto label_rect = rect();
    const int spacing = widget_scaled(this, LABEL_SPACING);
    int flags = Qt::AlignVCenter | Qt::TextShowMnemonic;
    if (layoutDirection() == Qt::RightToLeft) {
        label_rect.setRight(indicator.left() - spacing - 1);
        flags |= Qt::AlignRight;
    } else {
        label_rect.setLeft(indicator.right() + spacing + 1);
        flags |= Qt::AlignLeft;
    }
    painter.setPen(option.palette.color(QPalette::WindowText));
    painter.drawText(label_rect, flags, option.text);
    const auto &tokens = ui_tokens(this);
    const bool checked = isChecked();
    const auto box = QRectF(indicator).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = widget_scaled(this, 4);
    painter.setPen(QColor::fromString(checked || underMouse() ? ACCENT : tokens.border_hover));
    painter.setBrush(QColor::fromString(checked ? ACCENT : tokens.input_background));
    painter.drawRoundedRect(box, radius, radius);
    if (checked) {
        QPen pen(QColor("#FFFFFF"), std::max(1.5, box.width() / 9));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const double x = box.x(), y = box.y(), w = box.width(), h = box.height();
        QPainterPath tick(QPointF(x + w * 0.26, y + h * 0.52));
        tick.lineTo(x + w * 0.43, y + h * 0.69);
        tick.lineTo(x + w * 0.75, y + h * 0.34);
        painter.drawPath(tick);
    }
}
}
