#include "tools/rect.h"
#include "canvas/items/drawing_items.h"
#include "canvas/scene.h"
#include "core/logger.h"
#include "settings/tool_settings.h"

namespace jietuba {
bool RectTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    drawing = true;
    start_pos = pos;
    QPen pen(color_with_opacity(ctx.color, ctx.opacity), ctx.stroke_width);
    if (ctx.settings_manager) {
        auto *settings = ctx.settings_manager->get_tool_settings("rect");
        line_style = settings->get("line_style", "solid").toString();
        corner_radius = settings->get("corner_radius", 0).toDouble();
    }
    // Round caps and joins keep dashes from looking like blocks.
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    apply_line_style(pen, line_style);
    current_item = new RectItem(QRectF(pos, pos), pen, corner_radius);
    ctx.scene->addItem(current_item);
    log_debug(T("开始绘制: {pos}", {{"pos", py_repr(pos)}}), "RectTool");
    return true;
}

void RectTool::on_move(const QPointF &pos, ToolContext &) {
    if (drawing && current_item) current_item->setRect(drag_rect(start_pos, pos, shift_held()));
}

void RectTool::on_release(const QPointF &, ToolContext &ctx) {
    if (!drawing) return;
    drawing = false;
    if (current_item) {
        // Checked on the drawn item: releasing Shift before the mouse keeps the square.
        const auto rect = current_item->rect();
        if (rect.width() < MIN_SIZE || rect.height() < MIN_SIZE) {
            ctx.scene->removeItem(current_item);
            delete current_item;
            current_item = nullptr;
            log_debug(T("绘制取消：尺寸过小 ({width:.1f}x{height:.1f} < {min_size})",
                        {{"width", rect.width()}, {"height", rect.height()}, {"min_size", static_cast<int>(MIN_SIZE)}}), "RectTool");
            return;
        }
        ctx.scene->removeItem(current_item);
        ctx.undo_stack->push(new AddItemCommand(ctx.scene, ItemReference(current_item)));
        auto *item_to_select = current_item;
        current_item = nullptr;
        emit ctx.scene->item_auto_select_requested(item_to_select);
    }
    log_debug(T("完成绘制"), "RectTool");
}
}
