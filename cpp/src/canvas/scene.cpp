#include "canvas/scene.h"
#include "canvas/items/background_item.h"
#include "canvas/items/selection_item.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/items/watermark_item.h"
#include "canvas/selection_model.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include "tools/arrow.h"
#include "tools/controller.h"
#include "tools/cursor.h"
#include "tools/ellipse.h"
#include "tools/eraser.h"
#include "tools/highlighter.h"
#include "tools/mosaic.h"
#include "tools/number.h"
#include "tools/pen.h"
#include "tools/rect.h"
#include "tools/spotlight.h"
#include "tools/text.h"
#include "tools/watermark.h"
#include <QGraphicsEllipseItem>
#include <QHash>
#include <algorithm>

namespace jietuba {
CanvasScene::CanvasScene(const QImage &background_image, const QRectF &rect, bool enable_mosaic) : scene_rect(rect) {
    selection_model = new SelectionModel(this);
    background = new BackgroundItem(background_image, scene_rect);
    selection_item = new SelectionItem(selection_model);
    // The mask is a widget overlay on the screenshot window, not a scene item.
    background->setZValue(0);
    selection_item->setZValue(101);
    addItem(background);
    addItem(selection_item);
    // The background uses setOffset, so its boundingRect does not give the scene rect.
    setSceneRect(scene_rect);
    log_debug(T("场景创建完成: scene_rect={scene_rect}, sceneRect()={scene_rect_actual}",
                {{"scene_rect", py_repr(scene_rect)}, {"scene_rect_actual", py_repr(sceneRect())}}), "CanvasScene");
    undo_stack = new CommandUndoStack(this);
    // Refreshes the number cursor after undo and redo.
    connect(undo_stack, &QUndoStack::indexChanged, this, [this](int) { on_undo_stack_changed(); });
    // The watermark covers the selection and follows it.
    connect(selection_model, &SelectionModel::rectChanged, this, [this](QRectF r) { sync_watermark_rect(r); });
    tool_settings_manager = get_tool_settings_manager();
    // Initial style from the pen settings rather than hard-coded values.
    ToolContext ctx;
    ctx.scene = this;
    ctx.selection = selection_model;
    ctx.undo_stack = undo_stack;
    ctx.color = tool_settings_manager->get_color("pen");
    ctx.stroke_width = tool_settings_manager->get_stroke_width("pen");
    ctx.opacity = tool_settings_manager->get_opacity("pen");
    ctx.settings_manager = tool_settings_manager;
    tool_controller = std::make_unique<ToolController>(ctx);
    tool_controller->add_tool_changed_callback([this](const QString &id) { on_tool_changed(id); });
    // The cursor tool means no drawing tool; SmartEditController handles selection and editing.
    tool_controller->register_tool(std::make_unique<CursorTool>());
    tool_controller->register_tool(std::make_unique<PenTool>());
    tool_controller->register_tool(std::make_unique<RectTool>());
    tool_controller->register_tool(std::make_unique<EllipseTool>());
    tool_controller->register_tool(std::make_unique<ArrowTool>());
    tool_controller->register_tool(std::make_unique<TextTool>());
    tool_controller->register_tool(std::make_unique<NumberTool>());
    tool_controller->register_tool(std::make_unique<HighlighterTool>());
    if (enable_mosaic) tool_controller->register_tool(std::make_unique<MosaicTool>());
    tool_controller->register_tool(std::make_unique<SpotlightTool>());
    tool_controller->register_tool(std::make_unique<WatermarkTool>());
    tool_controller->register_tool(std::make_unique<EraserTool>());
    tool_controller->activate("cursor");
}

CanvasScene::~CanvasScene() {
    // Items may hold commands' leases; clear the undo stack while the scene still exists.
    undo_stack->clear();
    clear();
}

void CanvasScene::confirm_selection() {
    selection_model->confirm();
    // The frame stays visible after confirmation so the selection can still be adjusted.
    selection_item->show();
    emit selectionConfirmed();
}

void CanvasScene::preset_selection(const QRectF &rect) {
    selection_model->initialize_confirmed_rect(rect);
    selection_item->show();
    emit selectionConfirmed();
}

void CanvasScene::activate_tool(const QString &tool_id) { tool_controller->activate(tool_id); }

void CanvasScene::update_style(const std::optional<QColor> &color, const std::optional<double> &width, const std::optional<double> &opacity) {
    tool_controller->update_style(color, width, opacity);
    if (color) emit cursor_color_update_requested(*color);
    if (opacity) emit cursor_opacity_update_requested();
}

void CanvasScene::on_tool_changed(const QString &tool_id) { emit cursor_tool_update_requested(tool_id, false); }

void CanvasScene::sync_watermark_rect(const QRectF &rect) {
    if (auto *watermark = WatermarkItem::find(this)) watermark->set_scene_rect(rect);
}

void CanvasScene::on_undo_stack_changed() {
    if (tool_controller && tool_controller->current_tool && tool_controller->current_tool->id() == "number")
        emit cursor_tool_update_requested("number", true);
}

void CanvasScene::refresh_number_cursor_if_active(bool force) {
    // Only for the active number tool, so erasing numbers with another tool keeps its cursor.
    if (force && tool_controller && tool_controller->current_tool && tool_controller->current_tool->id() == "number")
        emit cursor_tool_update_requested("number", true);
}

QList<QGraphicsItem *> CanvasScene::get_drawing_items_in_rect(const QRectF &rect) {
    QList<QGraphicsItem *> drawing_items;
    // The selection item paints nothing but still takes mouse events, so it is excluded explicitly.
    for (auto *item : items(rect, Qt::IntersectsItemBoundingRect)) {
        if (item == background || item == selection_item) continue;
        // The brush indicator (z >= 10000).
        if (dynamic_cast<QGraphicsEllipseItem *>(item) && item->zValue() >= 10000) continue;
        // Derived from spotlights; a cloned spotlight grows its own curtain.
        if (dynamic_cast<SpotlightCurtain *>(item)) continue;
        drawing_items.append(item);
    }
    // items() is stacking order (top first), which is not the order of adding: text stays on top
    // by default. Pins rebuild their undo stack in this order, so the live add commands decide it,
    // and items without one go last.
    QHash<QGraphicsItem *, int> added_at;
    for (int index = 0; index < undo_stack->index(); ++index) {
        const auto *command = undo_stack->command(index);
        // RemoveItemCommand derives from AddItemCommand.
        if (dynamic_cast<const RemoveItemCommand *>(command)) continue;
        if (const auto *add = dynamic_cast<const AddItemCommand *>(command); add && !added_at.contains(add->item().get()))
            added_at.insert(add->item().get(), index);
    }
    std::reverse(drawing_items.begin(), drawing_items.end());
    std::stable_sort(drawing_items.begin(), drawing_items.end(), [&added_at, end = undo_stack->index()](QGraphicsItem *a, QGraphicsItem *b) {
        return added_at.value(a, end) < added_at.value(b, end);
    });
    log_debug(T("选区内绘制项目: {item_count} 个（已按绘制顺序排列）", {{"item_count", static_cast<int>(drawing_items.size())}}), "Scene");
    return drawing_items;
}
}
