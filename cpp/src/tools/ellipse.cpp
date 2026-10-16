#include "tools/ellipse.h"
#include "canvas/items/drawing_items.h"
#include "canvas/scene.h"
#include "core/logger.h"
#include "settings/tool_settings.h"

namespace jietuba {
bool EllipseTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    drawing = true;
    start_pos = pos;
    QPen pen(color_with_opacity(ctx.color, ctx.opacity), ctx.stroke_width);
    if (ctx.settings_manager) line_style = ctx.settings_manager->get_tool_settings("ellipse")->get("line_style", "solid").toString();
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    apply_line_style(pen, line_style);
    current_item = new EllipseItem(QRectF(pos, pos), pen);
    ctx.scene->addItem(current_item);
    log_debug(T("开始绘制: {pos}", {{"pos", py_repr(pos)}}), "EllipseTool");
    return true;
}

void EllipseTool::on_move(const QPointF &pos, ToolContext &) {
    if (drawing && current_item) current_item->setRect(drag_rect(start_pos, pos, shift_held()));
}

void EllipseTool::on_release(const QPointF &, ToolContext &ctx) {
    if (!drawing) return;
    drawing = false;
    if (current_item) {
        const auto rect = current_item->rect();
        if (rect.width() < MIN_SIZE || rect.height() < MIN_SIZE) {
            ctx.scene->removeItem(current_item);
            delete current_item;
            current_item = nullptr;
            log_debug(T("绘制取消：尺寸过小 ({width:.1f}x{height:.1f} < {min_size})",
                        {{"width", rect.width()}, {"height", rect.height()}, {"min_size", static_cast<int>(MIN_SIZE)}}), "EllipseTool");
            return;
        }
        ctx.scene->removeItem(current_item);
        ctx.undo_stack->push(new AddItemCommand(ctx.scene, ItemReference(current_item)));
        auto *item_to_select = current_item;
        current_item = nullptr;
        emit ctx.scene->item_auto_select_requested(item_to_select);
    }
    log_debug(T("完成绘制"), "EllipseTool");
}
}
