#pragma once
#include "tools/base.h"
#include <map>
#include <functional>
#include <memory>
#include <vector>

namespace jietuba {
// Owns the tools, switches between them and forwards mouse events.
class ToolController {
public:
    using Callback = std::function<void(const QString &)>;
    explicit ToolController(const ToolContext &ctx);
    ToolContext ctx;
    ToolContext &context() { return ctx; }
    std::map<QString, std::unique_ptr<Tool>> tools;
    Tool *current_tool = nullptr;
    // "cursor" when no tool is active, i.e. no drawing feature is on.
    QString current_tool_id() const { return current_tool ? current_tool->id() : QStringLiteral("cursor"); }
    // Returns a handle for remove_tool_changed_callback.
    int add_tool_changed_callback(Callback callback);
    void remove_tool_changed_callback(int handle);
    void register_tool(std::unique_ptr<Tool> tool);
    Tool *get_tool(const QString &tool_id) const;
    template<class T> T *get_tool_as(const QString &tool_id) const { return dynamic_cast<T *>(get_tool(tool_id)); }
    void activate(const QString &tool_id);
    bool on_press(const QPointF &pos, Qt::MouseButton button);
    void on_move(const QPointF &pos);
    void on_release(const QPointF &pos);
    void update_style(const std::optional<QColor> &color = std::nullopt, const std::optional<double> &width = std::nullopt,
                      const std::optional<double> &opacity = std::nullopt);
private:
    std::vector<std::pair<int, Callback>> tool_changed_callbacks_;
    int next_callback_handle_ = 1;
};
}
