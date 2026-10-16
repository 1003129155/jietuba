#include "tools/pen.h"
#include "canvas/items/stroke_item.h"
#include "canvas/scene.h"
#include "settings/tool_settings.h"
#include <utility>

namespace jietuba {
void commit_stroke(ToolContext &ctx, StrokeItem *item, FreehandStroke &stroke) {
    if (stroke.finish()) item->extend_path(stroke.path, stroke.take_dirty_from());
    item->finish_path();
    ctx.undo_stack->push(new AddItemCommand(ctx.scene, ItemReference(item)));
}

bool PenTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    // The previous stroke never got its release (focus stolen while the button was down): keep it as released.
    finish(ctx);
    drawing = true;
    if (ctx.settings_manager)
        line_style = ctx.settings_manager->get_tool_settings("pen")->get("line_style", "solid").toString();
    stroke.emplace(pos, shift_line_angle(ctx), true);
    QPen pen(color_with_opacity(ctx.color, ctx.opacity), ctx.stroke_width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    apply_line_style(pen, line_style);
    current_item = new StrokeItem(stroke->path, pen, false);
    ctx.scene->addItem(current_item);
    return true;
}

void PenTool::on_move(const QPointF &pos, ToolContext &) {
    if (!(drawing && current_item && stroke)) return;
    if (stroke->extend(pos, shift_held())) current_item->extend_path(stroke->path, stroke->take_dirty_from());
}

void PenTool::on_release(const QPointF &, ToolContext &ctx) { finish(ctx); }

void PenTool::on_deactivate(ToolContext &ctx) {
    finish(ctx);
    Tool::on_deactivate(ctx);
}

void PenTool::finish(ToolContext &ctx) {
    auto *item = std::exchange(current_item, nullptr);
    auto finished = std::exchange(stroke, std::nullopt);
    drawing = false;
    if (item && finished) commit_stroke(ctx, item, *finished);
}
}
