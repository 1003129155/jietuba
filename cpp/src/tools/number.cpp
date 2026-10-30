#include "tools/number.h"
#include "canvas/items/drawing_items.h"
#include "canvas/scene.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/view.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include "tools/cursor_manager.h"

namespace jietuba {
QString NumberTool::get_style(const ToolContext &ctx) {
    // NumberItem.DEFAULT_STYLE is "solid".
    if (!ctx.settings_manager) return NumberItem::normalize_style("solid");
    return NumberItem::normalize_style(ctx.settings_manager->get_setting("number", "style", "solid").toString());
}

bool NumberTool::apply_style_change(const QString &requested, CanvasView *view, QUndoStack *undo_stack) {
    const auto style = NumberItem::normalize_style(requested);
    auto *controller = view ? view->smart_edit_controller.get() : nullptr;
    auto *item = controller ? dynamic_cast<NumberItem *>(controller->selected_item()) : nullptr;
    bool temporary = false;
    if (item) {
        // Decided even when the style is unchanged, or picking the current style would skip it.
        temporary = controller->is_cross_tool_selection();
        if (item->style != style && undo_stack) undo_stack->push(new NumberStyleCommand(ItemReference(item), item->style, style, QString()));
    }
    bool persisted = false;
    if (!temporary) {
        if (auto *manager = get_tool_settings_manager()) {
            manager->update_settings("number", {{"style", style}});
            persisted = true;
        }
    }
    // Only while the number tool is active; a cross-tool edit must not swap another tool's cursor.
    auto *cursor_manager = view ? view->cursor_manager.get() : nullptr;
    if (cursor_manager && cursor_manager->current_tool_id == "number") cursor_manager->set_tool_cursor("number", true);
    return persisted;
}

QVariantMap NumberTool::extras_defaults(const ToolContext &ctx) {
    if (!ctx.settings_manager) return {};
    QVariantMap defaults;
    for (const char *key : EXTRAS_SETTING_KEYS) defaults[key] = ctx.settings_manager->get_setting("number", key);
    return defaults;
}

void NumberTool::remember_extras(const QVariantMap &change) {
    QVariantMap values;
    for (const char *key : EXTRAS_SETTING_KEYS)
        if (change.contains(key)) values[key] = change.value(key);
    if (auto *manager = get_tool_settings_manager(); manager && !values.isEmpty()) manager->update_settings("number", values);
}

bool NumberTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return true;
    const int number = get_next_number(ctx.scene);
    const double radius = get_radius_for_width(ctx.stroke_width);
    log_debug(T("创建前场景中序号数量: {prev_count}, 将创建序号: {number}", {{"prev_count", number - 1}, {"number", number}}), "NumberTool");
    auto *item = new NumberItem(number, pos, radius, color_with_opacity(ctx.color, ctx.opacity), get_style(ctx));
    item->apply_extras_state(extras_defaults(ctx));
    assign_number_order(ctx.scene, item);
    // push() runs redo(), which adds the item to the scene.
    ctx.undo_stack->push(new AddNumberCommand(ctx.scene, ItemReference(item), number));
    emit ctx.scene->item_auto_select_requested(item);
    log_debug(T("创建后场景中序号数量: {count_after}", {{"count_after", NumberSequence::count_numbers(ctx.scene)}}), "NumberTool");
    return true;
}
}
