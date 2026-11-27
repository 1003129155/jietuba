#include "ui/segmented_toggle.h"
#include "core/ui_scale.h"
#include "ui/base_settings_panel.h"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>

namespace jietuba {
SegmentedToggle::SegmentedToggle(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    apply_scale();
}

void SegmentedToggle::set_labels(const QStringList &labels) {
    labels_ = labels;
    current_ = std::clamp(current_, 0, std::max(0, static_cast<int>(labels_.size()) - 1));
    hovered_ = -1;
    apply_scale();
}

void SegmentedToggle::set_current(int index) {
    if (index < 0 || index >= labels_.size() || index == current_) return;
    current_ = index;
    update();
}

void SegmentedToggle::apply_scale() {
    QFont label = font();
    label.setFamilies({"Microsoft YaHei", "SimSun", "Arial"});
    label.setPixelSize(scaled(BASE_FONT));
    setFont(label);
    setFixedSize(sizeHint());
    update();
}

QSize SegmentedToggle::sizeHint() const {
    // Measured in the bold the chosen label is drawn in, so choosing never needs more room.
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics metrics(bold);
    int width = 2 * scaled(BASE_INSET);
    for (const auto &label : labels_) width += metrics.horizontalAdvance(label) + 2 * scaled(BASE_SEGMENT_PADDING);
    return {width, scaled(BASE_HEIGHT)};
}

QRect SegmentedToggle::segment_rect(int index) const {
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics metrics(bold);
    const int inset = scaled(BASE_INSET);
    int x = inset;
    for (int i = 0; i < labels_.size(); ++i) {
        const int width = metrics.horizontalAdvance(labels_[i]) + 2 * scaled(BASE_SEGMENT_PADDING);
        if (i == index) return {x, inset, width, height() - 2 * inset};
        x += width;
    }
    return {};
}

int SegmentedToggle::segment_at(const QPoint &point) const {
    for (int i = 0; i < labels_.size(); ++i)
        if (segment_rect(i).contains(point)) return i;
    return -1;
}

void SegmentedToggle::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QRectF pill(rect());
    painter.setBrush(CONTROL_FILL);
    painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    for (int i = 0; i < labels_.size(); ++i) {
        const QRectF segment(segment_rect(i));
        const double radius = segment.height() / 2;
        if (i == current_) {
            // A faint shadow a little below, then the white the chosen segment sits on.
            painter.setBrush(QColor(0, 0, 0, 18));
            painter.drawRoundedRect(segment.translated(0, 0.8), radius, radius);
            painter.setBrush(Qt::white);
            painter.drawRoundedRect(segment, radius, radius);
        } else if (i == hovered_) {
            painter.setBrush(CONTROL_FILL);
            painter.drawRoundedRect(segment, radius, radius);
        }
        QFont label = font();
        label.setWeight(i == current_ ? QFont::DemiBold : QFont::Normal);
        painter.setFont(label);
        painter.setPen(i == current_ || i == hovered_ ? CONTROL_INK : CONTROL_SUB);
        painter.drawText(segment, Qt::AlignCenter, labels_[i]);
        painter.setPen(Qt::NoPen);
    }
}

void SegmentedToggle::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return QWidget::mousePressEvent(event);
    const int index = segment_at(event->position().toPoint());
    if (index < 0 || index == current_) return;
    current_ = index;
    update();
    emit current_changed(index);
}

void SegmentedToggle::mouseMoveEvent(QMouseEvent *event) {
    const int index = segment_at(event->position().toPoint());
    if (index != hovered_) {
        hovered_ = index;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void SegmentedToggle::leaveEvent(QEvent *event) {
    hovered_ = -1;
    update();
    QWidget::leaveEvent(event);
}
}
