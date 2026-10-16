#include "ui/mask_overlay.h"
#include "canvas/selection_model.h"
#include "core/qt_utils.h"
#include "core/theme.h"
#include <QPainter>
#include <QRegion>

namespace jietuba {
MaskOverlayWidget::MaskOverlayWidget(QWidget *parent, SelectionModel *selection_model) : CrosshairSurface(parent), model_(selection_model) {
    mask_color_ = get_theme().mask_color();
    // Mouse events pass through; this blocks no interaction.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(false);
    connect(model_, &SelectionModel::rectChanged, this, &MaskOverlayWidget::on_rect_changed);
    show();
    raise();
}

void MaskOverlayWidget::set_crosshair_position(const std::optional<QPoint> &position) {
    if (position == crosshair_position_) return;
    QRegion dirty;
    for (const auto &point : {crosshair_position_, position}) {
        if (point) {
            dirty |= QRegion(0, point->y() - 2, width(), 5);
            dirty |= QRegion(point->x() - 2, 0, 5, height());
        }
    }
    crosshair_position_ = position;
    // Flush the small strips now rather than queueing behind detection or annotation work.
    repaint(dirty);
}

void MaskOverlayWidget::paint_crosshair(QPainter &painter) const {
    if (!crosshair_position_) return;
    const int x = crosshair_position_->x(), y = crosshair_position_->y();
    const std::pair<QColor, int> strokes[] = {{QColor(0, 0, 0, 150), 3}, {QColor(255, 255, 255, 230), 1}};
    for (const auto &[color, width] : strokes) {
        painter.setPen(QPen(color, width));
        painter.drawLine(0, y, this->width() - 1, y);
        painter.drawLine(x, 0, x, height() - 1);
    }
}

void MaskOverlayWidget::set_mask_color(const QColor &color) {
    mask_color_ = color;
    update();
}

QRect MaskOverlayWidget::scene_to_local(const QRectF &sel) const {
    if (auto *parent = parentWidget())
        return QRect(static_cast<int>(sel.x() - parent->x()), static_cast<int>(sel.y() - parent->y()), static_cast<int>(sel.width()), static_cast<int>(sel.height()));
    return sel.toAlignedRect();
}

void MaskOverlayWidget::on_rect_changed(const QRectF &rect) {
    // Only the union of old and new selections, with a margin against trails.
    const auto new_local = !rect.isEmpty() ? scene_to_local(rect) : QRect();
    const auto old_local = last_local_sel_;
    if (old_local.isEmpty() && new_local.isEmpty()) return;
    const int margin = 5;
    update(old_local.united(new_local).adjusted(-margin, -margin, margin, margin));
}

void MaskOverlayWidget::paintEvent(QPaintEvent *event) {
    paint_mask();
    for (const auto &hook : paint_hooks.callbacks()) hook(event);
}

void MaskOverlayWidget::paint_mask() {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto full = rect();
    if (!model_ || model_->is_empty()) {
        painter.fillRect(full, mask_color_);
        last_local_sel_ = QRect();
        paint_crosshair(painter);
        painter.end();
        return;
    }
    // Four bands outside the selection: top, bottom, then left and right at the selection's rows.
    const auto local_sel = scene_to_local(model_->rect());
    last_local_sel_ = local_sel;
    const auto &color = mask_color_;
    const int fw = full.width(), fh = full.height();
    const int sx = local_sel.x(), sy = local_sel.y(), sw = local_sel.width(), sh = local_sel.height();
    if (sy > 0) painter.fillRect(0, 0, fw, sy, color);
    const int bottom = sy + sh;
    if (bottom < fh) painter.fillRect(0, bottom, fw, fh - bottom, color);
    if (sx > 0) painter.fillRect(0, sy, sx, sh, color);
    const int right = sx + sw;
    if (right < fw) painter.fillRect(right, sy, fw - right, sh, color);
    paint_crosshair(painter);
    painter.end();
}
}
