#include "clipboard/ui/forms/form_widgets.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/theme.h"
#include "ui/settings_ui/components.h"
#include <QPainter>

namespace jietuba::clipboard {
QLabel *field_label(const QString &text, bool top_gap) {
    auto *label = new QLabel(text);
    // Spacing through contentsMargins: padding in a style sheet indents the text a few pixels out of line.
    label->setContentsMargins(0, top_gap ? dialog_scaled(8) : 0, 0, 0);
    apply_theme_text_style(label, 12, false, "font-weight: 600;");
    return label;
}

QLabel *field_hint(const QString &text) {
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    apply_theme_text_style(label, 11, false, {}, true);
    return label;
}

OptionCard::OptionCard(const QString &title, const QString &description, QWidget *parent) : QAbstractButton(parent), title_(title), description_(description) {
    setText(title);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

QSize OptionCard::sizeHint() const { return QSize(dialog_scaled(150), dialog_scaled(58)); }

QSize OptionCard::minimumSizeHint() const { return QSize(dialog_scaled(96), dialog_scaled(58)); }

void OptionCard::paintEvent(QPaintEvent *) {
    const auto &tokens = get_ui_theme()->tokens();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF rect = QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double radius = dialog_scaled_f(10);

    const bool checked = isChecked();
    if (checked) {
        painter.setBrush(css_color(tokens.accent_soft));
        painter.setPen(QPen(css_color(tokens.accent), dialog_scaled_f(1.5)));
    } else {
        painter.setBrush(css_color(underMouse() ? tokens.surface_hover : tokens.input_background));
        painter.setPen(QPen(css_color(underMouse() ? tokens.border_hover : tokens.border), 1));
    }
    painter.drawRoundedRect(rect, radius, radius);

    QFont title_font(font());
    title_font.setPixelSize(dialog_scaled(13));
    title_font.setWeight(QFont::DemiBold);
    QFont description_font(font());
    description_font.setPixelSize(dialog_scaled(11));
    const int pad_x = dialog_scaled(12);
    const QRectF inner = rect.adjusted(pad_x, dialog_scaled(10), -pad_x, -dialog_scaled(10));
    const int text_w = static_cast<int>(inner.width());
    const double half = inner.height() / 2;
    painter.setFont(title_font);
    painter.setPen(css_color(checked ? tokens.accent_text : tokens.text));
    painter.drawText(QRectF(inner.left(), inner.top(), text_w, half), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(title_font).elidedText(title_, Qt::ElideRight, text_w));
    painter.setFont(description_font);
    painter.setPen(css_color(tokens.text_muted));
    painter.drawText(QRectF(inner.left(), inner.top() + half, inner.width(), half), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(description_font).elidedText(description_, Qt::ElideRight, static_cast<int>(inner.width())));
}

void OptionCard::enterEvent(QEnterEvent *event) {
    update();
    QAbstractButton::enterEvent(event);
}

void OptionCard::leaveEvent(QEvent *event) {
    update();
    QAbstractButton::leaveEvent(event);
}
}
