#include "tools/arrow.h"
#include "canvas/items/arrow_item.h"
#include "canvas/scene.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <cmath>

namespace jietuba {
bool ArrowTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    drawing = true;
    start_pos = pos;
    if (ctx.settings_manager)
        arrow_style = ArrowItem::normalize_style(ctx.settings_manager->get_tool_settings("arrow")->get("arrow_style").toString());
    const QPen pen(color_with_opacity(ctx.color, ctx.opacity), ctx.stroke_width);
    current_item = new ArrowItem(pos, pos, pen, arrow_style);
    ctx.scene->addItem(current_item);
    log_debug(T("开始绘制: {pos}, 样式: {arrow_style}", {{"pos", geometry_text(pos)}, {"arrow_style", arrow_style}}), "ArrowTool");
    return true;
}

void ArrowTool::on_move(const QPointF &pos, ToolContext &) {
    if (drawing && current_item) current_item->set_positions(start_pos, pos);
}

void ArrowTool::on_release(const QPointF &pos, ToolContext &ctx) {
    if (!drawing) return;
    drawing = false;
    if (current_item) {
        const auto delta = pos - start_pos;
        const double length = std::pow(delta.x() * delta.x() + delta.y() * delta.y(), .5);
        if (length < MIN_LENGTH) {
            ctx.scene->removeItem(current_item);
            delete current_item;
            current_item = nullptr;
            log_debug(T("绘制取消：长度过短 ({length:.1f} < {min_length})", {{"length", length}, {"min_length", static_cast<int>(MIN_LENGTH)}}), "ArrowTool");
            return;
        }
        ctx.scene->removeItem(current_item);
        ctx.undo_stack->push(new AddItemCommand(ctx.scene, ItemReference(current_item)));
        auto *item_to_select = current_item;
        current_item = nullptr;
        emit ctx.scene->item_auto_select_requested(item_to_select);
    }
    log_debug(T("完成绘制"), "ArrowTool");
}
}
