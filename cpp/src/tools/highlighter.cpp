#include "tools/highlighter.h"
#include "canvas/items/drawing_items.h"
#include "canvas/items/stroke_item.h"
#include "canvas/scene.h"
#include "settings/tool_settings.h"
#include "tools/pen.h"
#include <utility>
#include <algorithm>

namespace jietuba {
QString HighlighterTool::load_draw_mode(ToolContext &ctx) const {
    if (ctx.settings_manager) return ctx.settings_manager->get_tool_settings("highlighter")->get("draw_mode", MODE_FREEHAND).toString();
    return MODE_FREEHAND;
}

void HighlighterTool::start_rect_draw(const QPointF &pos, ToolContext &ctx) {
    drawing = true;
    start_pos = pos;
    const auto color = color_with_opacity(ctx.color, ctx.opacity);
    QPen pen(color, std::max(1.0, ctx.stroke_width));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    auto *rect = new RectItem(QRectF(pos, pos), pen);
    rect->setBrush(QBrush(color));
    rect->setZValue(10);
    rect->is_highlighter = true;
    rect->is_highlighter_rect = true;
    rect_item = rect;
    ctx.scene->addItem(rect);
}

bool HighlighterTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    // The previous stroke never got its release (focus stolen while the button was down): keep it as released.
    finish_freehand(ctx);
    draw_mode = load_draw_mode(ctx);
    if (draw_mode == MODE_RECT) {
        start_rect_draw(pos, ctx);
        return true;
    }
    drawing = true;
    stroke.emplace(pos, shift_line_angle(ctx), true);
    QPen pen(color_with_opacity(ctx.color, ctx.opacity), ctx.stroke_width * 3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    current_item = new StrokeItem(stroke->path, pen, true);
    ctx.scene->addItem(current_item);
    return true;
}

void HighlighterTool::on_move(const QPointF &pos, ToolContext &) {
    if (draw_mode == MODE_RECT) {
        if (drawing && rect_item && start_pos) rect_item->setRect(QRectF(*start_pos, pos).normalized());
        return;
    }
    if (!(drawing && current_item && stroke)) return;
    if (stroke->extend(pos, shift_held())) current_item->extend_path(stroke->path, stroke->take_dirty_from());
}

void HighlighterTool::on_release(const QPointF &pos, ToolContext &ctx) {
    if (draw_mode == MODE_RECT) {
        if (!drawing) return;
        drawing = false;
        if (rect_item) {
            const auto rect = QRectF(*start_pos, pos).normalized();
            if (rect.width() < MIN_SIZE || rect.height() < MIN_SIZE) {
                ctx.scene->removeItem(rect_item);
                delete rect_item;
                rect_item = nullptr;
                start_pos.reset();
                return;
            }
            ctx.scene->removeItem(rect_item);
            ctx.undo_stack->push(new AddItemCommand(ctx.scene, ItemReference(rect_item)));
            auto *item_to_select = rect_item;
            rect_item = nullptr;
            start_pos.reset();
            emit ctx.scene->item_auto_select_requested(item_to_select);
        }
        return;
    }
    finish_freehand(ctx);
}

void HighlighterTool::on_deactivate(ToolContext &ctx) {
    finish_freehand(ctx);
    Tool::on_deactivate(ctx);
}

void HighlighterTool::finish_freehand(ToolContext &ctx) {
    if (!current_item || !stroke) return;
    auto *item = std::exchange(current_item, nullptr);
    auto finished = std::exchange(stroke, std::nullopt);
    drawing = false;
    commit_stroke(ctx, item, *finished);
}
}
