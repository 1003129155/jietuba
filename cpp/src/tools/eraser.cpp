#include "tools/eraser.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/background_item.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/selection_item.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include "canvas/scene.h"
#include "core/logger.h"
#include "tools/number.h"
#include <QPainterPathStroker>
#include <algorithm>

namespace jietuba {
bool EraserTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    erasing = true;
    last_pos = pos;
    erased_items_.clear();
    erased_set_.clear();
    number_next_before = NumberTool::get_next_number(ctx.scene);
    erase_at_position(pos, ctx);
    return true;
}

void EraserTool::on_move(const QPointF &pos, ToolContext &ctx) {
    if (!(erasing && last_pos)) return;
    QPainterPath path;
    path.moveTo(*last_pos);
    path.lineTo(pos);
    QPainterPathStroker stroker;
    stroker.setWidth(std::max(ctx.stroke_width, 10.0));
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    const auto eraser_area = stroker.createStroke(path);
    for (auto *item : ctx.scene->items(eraser_area)) try_erase_item(item);
    last_pos = pos;
}

void EraserTool::on_release(const QPointF &pos, ToolContext &ctx) {
    if (!erasing) return;
    erase_at_position(pos, ctx);
    if (!erased_items_.empty()) {
        const auto count = static_cast<int>(erased_items_.size());
        ctx.undo_stack->push_command(new BatchRemoveCommand(ctx.scene, erased_items_, QString("Erase %1 items").arg(count), number_next_before));
        log_info(T("删除了 {count} 个图元", {{"count", count}}), "Eraser");
    }
    erased_items_.clear();
    erased_set_.clear();
    erasing = false;
    last_pos.reset();
    number_next_before.reset();
}

void EraserTool::erase_at_position(const QPointF &pos, ToolContext &ctx) {
    const double radius = std::max(ctx.stroke_width / 2, 5.0);
    QPainterPath path;
    path.addEllipse(pos, radius, radius);
    for (auto *item : ctx.scene->items(path)) try_erase_item(item);
}

void EraserTool::try_erase_item(QGraphicsItem *item) {
    if (dynamic_cast<BackgroundItem *>(item) || dynamic_cast<SelectionItem *>(item)) return;
    const bool erasable = dynamic_cast<StrokeItem *>(item) || dynamic_cast<RectItem *>(item) || dynamic_cast<EllipseItem *>(item)
        || dynamic_cast<ArrowItem *>(item) || dynamic_cast<TextItem *>(item) || dynamic_cast<NumberItem *>(item)
        || dynamic_cast<MosaicItem *>(item);
    if (!erasable || erased_set_.contains(item)) return;
    erased_set_.insert(item);
    erased_items_.emplace_back(dynamic_cast<TrackedItem *>(item));
    // Removed at once for visual feedback.
    if (auto *scene = item->scene()) scene->removeItem(item);
}

void EraserTool::on_activate(ToolContext &ctx) {
    Tool::on_activate(ctx);
    log_debug(T("已激活"), "Eraser");
}

void EraserTool::on_deactivate(ToolContext &ctx) {
    Tool::on_deactivate(ctx);
    // Commit erasures that were never released.
    if (!erased_items_.empty()) {
        ctx.undo_stack->push_command(new BatchRemoveCommand(ctx.scene, erased_items_, "Remove Items", number_next_before));
        erased_items_.clear();
        erased_set_.clear();
    }
    erasing = false;
    last_pos.reset();
    number_next_before.reset();
    log_debug(T("已停用"), "Eraser");
}
}
