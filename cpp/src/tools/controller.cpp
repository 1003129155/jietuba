#include "tools/controller.h"
#include "core/logger.h"
#include <exception>

namespace jietuba {
ToolController::ToolController(const ToolContext &context) : ctx(context) { log_debug(T("初始化"), "ToolController"); }

int ToolController::add_tool_changed_callback(Callback callback) {
    const int handle = next_callback_handle_++;
    tool_changed_callbacks_.emplace_back(handle, std::move(callback));
    return handle;
}

void ToolController::remove_tool_changed_callback(int handle) {
    for (auto it = tool_changed_callbacks_.begin(); it != tool_changed_callbacks_.end(); ++it)
        if (it->first == handle) { tool_changed_callbacks_.erase(it); return; }
}

void ToolController::register_tool(std::unique_ptr<Tool> tool) {
    const auto id = tool->id();
    if (tools.count(id) && current_tool == tools[id].get()) current_tool = nullptr;
    tools[id] = std::move(tool);
}

Tool *ToolController::get_tool(const QString &tool_id) const {
    const auto found = tools.find(tool_id);
    return found == tools.end() ? nullptr : found->second.get();
}

void ToolController::activate(const QString &tool_id) {
    auto *new_tool = get_tool(tool_id);
    if (!new_tool) {
        log_warning(T("工具不存在: {tool_id}", {{"tool_id", tool_id}}), "ToolController");
        return;
    }
    if (current_tool) {
        current_tool->on_deactivate(ctx);
        // Cleared first so a failed activation never leaves the deactivated tool current.
        current_tool = nullptr;
    }
    new_tool->on_activate(ctx);
    current_tool = new_tool;
    // Copied: a callback may unregister itself.
    const auto callbacks = tool_changed_callbacks_;
    for (const auto &[handle, callback] : callbacks) {
        try {
            callback(tool_id);
        } catch (const std::exception &e) {
            log_error(T("工具切换回调错误: {e}", {{"e", exception_text(e)}}), "ToolController");
        }
    }
}

bool ToolController::on_press(const QPointF &pos, Qt::MouseButton button) {
    if (current_tool) return current_tool->on_press(pos, button, ctx);
    return false;
}

void ToolController::on_move(const QPointF &pos) {
    if (current_tool) current_tool->on_move(pos, ctx);
}

void ToolController::on_release(const QPointF &pos) {
    if (current_tool) current_tool->on_release(pos, ctx);
}

void ToolController::update_style(const std::optional<QColor> &color, const std::optional<double> &width, const std::optional<double> &opacity) {
    if (color) ctx.color = *color;
    // The only clamping point: the current tool decides what width is valid.
    if (width) ctx.stroke_width = current_tool ? current_tool->clamp_width(*width) : *width;
    if (opacity) ctx.opacity = *opacity;
    if (current_tool && ctx.settings_manager) current_tool->save_settings(ctx);
}
}
