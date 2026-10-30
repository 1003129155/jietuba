#include "canvas/smart_edit_controller.h"
#include "canvas/canvas_host.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include "canvas/scene.h"
#include "canvas/undo.h"
#include "settings/tool_settings.h"
#include "tools/number.h"
#include "ui/arrow_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/toolbar.h"
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <cmath>

namespace jietuba {
namespace {
bool is_drawable(QGraphicsItem *item) {
    return dynamic_cast<StrokeItem *>(item) || dynamic_cast<RectItem *>(item) || dynamic_cast<EllipseItem *>(item)
        || dynamic_cast<ArrowItem *>(item) || dynamic_cast<TextItem *>(item) || dynamic_cast<NumberItem *>(item)
        || dynamic_cast<MosaicItem *>(item);
}

std::optional<ItemType> expected_type(const QString &tool_id, bool include_paths) {
    if (include_paths && (tool_id == "pen" || tool_id == "highlighter")) return ItemType::PATH;
    if (tool_id == "rect" || tool_id == "ellipse") return ItemType::SHAPE;
    if (tool_id == "arrow") return ItemType::ARROW;
    if (tool_id == "text") return ItemType::TEXT;
    if (tool_id == "number") return ItemType::NUMBER;
    return std::nullopt;
}
}

SmartEditController::SmartEditController(QGraphicsScene *s) : scene(s), layer_editor(std::make_unique<LayerEditor>()) {
    // Undo/redo resyncs handles with the restored geometry.
    if (auto *canvas = dynamic_cast<CanvasScene *>(scene))
        undo_connection_ = connect(canvas->undo_stack, &QUndoStack::indexChanged, this, [this](int) { on_undo_index_changed(); });
    // Typing in a number's label ends on focus change; the panel folds if nothing was typed.
    if (scene) connect(scene, &QGraphicsScene::focusItemChanged, this, [this] { if (scene) sync_number_panel_state(); });
}

void SmartEditController::cleanup() {
    QObject::disconnect(undo_connection_);
    set_selected_item(nullptr);
    hovered_item.reset();
    active_click_handle_.reset();
    move_initial_state.reset();
    if (layer_editor) layer_editor->stop_edit();
    layer_editor.reset();
    scene = nullptr;
}

void SmartEditController::set_tool(const QString &tool_id) {
    current_tool_id = tool_id;
    // A tool switch must not block the next drawing.
    if (selected_item()) clear_selection(true);
}

ItemType SmartEditController::get_item_type(QGraphicsItem *item) const {
    if (!item) return ItemType::OTHER;
    if (dynamic_cast<StrokeItem *>(item)) return ItemType::PATH;
    if (dynamic_cast<RectItem *>(item) || dynamic_cast<EllipseItem *>(item)) return ItemType::SHAPE;
    if (dynamic_cast<ArrowItem *>(item)) return ItemType::ARROW;
    if (dynamic_cast<TextItem *>(item)) return ItemType::TEXT;
    if (dynamic_cast<NumberItem *>(item)) return ItemType::NUMBER;
    // Box mosaics behave like highlighter rectangles (SHAPE); freehand ones like strokes (PATH).
    if (auto *mosaic = dynamic_cast<MosaicItem *>(item)) return mosaic->fill_mode() ? ItemType::SHAPE : ItemType::PATH;
    return ItemType::OTHER;
}

std::optional<QString> SmartEditController::get_item_tool_id(QGraphicsItem *item) const {
    if (auto *stroke = dynamic_cast<StrokeItem *>(item)) return stroke->is_highlighter ? QString("highlighter") : QString("pen");
    if (dynamic_cast<SpotlightItem *>(item)) return QString("spotlight");
    if (auto *rect = dynamic_cast<RectItem *>(item)) return rect->is_highlighter_rect ? QString("highlighter") : QString("rect");
    if (dynamic_cast<EllipseItem *>(item)) return QString("ellipse");
    if (dynamic_cast<ArrowItem *>(item)) return QString("arrow");
    if (dynamic_cast<TextItem *>(item)) return QString("text");
    if (dynamic_cast<NumberItem *>(item)) return QString("number");
    if (dynamic_cast<MosaicItem *>(item)) return QString("mosaic");
    return std::nullopt;
}

bool SmartEditController::is_cross_tool_selection() const {
    if (!cross_tool_select_enabled || !selected_item()) return false;
    const auto owner = get_item_tool_id(selected_item());
    return owner && *owner != current_tool_id;
}

bool SmartEditController::can_select_item(QGraphicsItem *item, Qt::KeyboardModifiers modifiers) const {
    const auto item_type = get_item_type(item);
    const bool ctrl = modifiers & Qt::ControlModifier;
    // With the host's permission, Ctrl+click temporarily selects any known editable item.
    if (cross_tool_select_enabled && ctrl && item_type != ItemType::OTHER) return true;
    // Spotlight holes are RectItems but belong only to the spotlight tool.
    if (dynamic_cast<SpotlightItem *>(item)) return current_tool_id == "spotlight";
    if (item_type == ItemType::PATH) return ctrl;
    if (current_tool_id == "highlighter" && item_type == ItemType::SHAPE) {
        auto *rect = dynamic_cast<RectItem *>(item);
        return rect && rect->is_highlighter_rect;
    }
    if (current_tool_id == "mosaic" && item_type == ItemType::SHAPE) {
        auto *mosaic = dynamic_cast<MosaicItem *>(item);
        return mosaic && mosaic->fill_mode();
    }
    if (current_tool_id.isEmpty() || current_tool_id == "cursor") return false;
    return expected_type(current_tool_id, true) == item_type;
}

bool SmartEditController::can_show_hover_cursor(QGraphicsItem *item) const {
    const auto item_type = get_item_type(item);
    if (dynamic_cast<SpotlightItem *>(item)) return current_tool_id == "spotlight";
    if (item_type == ItemType::PATH) return false;
    if (current_tool_id.isEmpty() || current_tool_id == "cursor") return false;
    if (current_tool_id == "highlighter" && item_type == ItemType::SHAPE) {
        auto *rect = dynamic_cast<RectItem *>(item);
        return rect && rect->is_highlighter_rect;
    }
    if (current_tool_id == "mosaic" && item_type == ItemType::SHAPE) {
        auto *mosaic = dynamic_cast<MosaicItem *>(item);
        return mosaic && mosaic->fill_mode();
    }
    return expected_type(current_tool_id, false) == item_type;
}

bool SmartEditController::handle_hover(const QPointF &, const QPointF &scene_pos) {
    if (!scene) return false;
    QGraphicsItem *first = nullptr;
    for (auto *item : scene->items(scene_pos))
        if (auto *target = annotation_owner(item); is_drawable(target)) { first = target; break; }
    if (first) {
        if (can_show_hover_cursor(first)) {
            if (hovered_item.get() != first) {
                hovered_item.reset(first);
                emit hover_changed(first);
            }
            return true;
        }
        if (hovered_item) {
            hovered_item.reset();
            emit hover_changed(nullptr);
        }
        return false;
    }
    if (hovered_item) {
        hovered_item.reset();
        emit hover_changed(nullptr);
    }
    return false;
}

bool SmartEditController::handle_press(const QPointF &, const QPointF &scene_pos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    if (button != Qt::LeftButton) return false;
    press_requires_manual_dispatch = false;
    drag_start_pos = scene_pos;
    is_dragging = false;
    QList<QGraphicsItem *> drawable_items;
    for (auto *item : scene->items(scene_pos))
        if (auto *target = annotation_owner(item); is_drawable(target) && !drawable_items.contains(target)) drawable_items.append(target);
    if (!drawable_items.isEmpty()) {
        // Ctrl hits the first editable item; a plain click keeps looking down for
        // one the current tool accepts, so top text does not hide shapes below.
        for (qsizetype index = 0; index < drawable_items.size(); ++index) {
            auto *item = drawable_items[index];
            if (selected_item() == item) {
                press_requires_manual_dispatch = index > 0;
                capture_press_state();
                return true;
            }
            if (can_select_item(item, modifiers)) {
                const auto owner = get_item_tool_id(item);
                // Switch tools first: switching clears the selection.
                if (cross_tool_select_enabled && (modifiers & Qt::ControlModifier) && owner && *owner != current_tool_id)
                    emit tool_switch_requested(*owner);
                select_item(item);
                press_requires_manual_dispatch = index > 0;
                capture_press_state();
                return true;
            }
        }
    } else if (selected_item()) {
        clear_selection();
    }
    return false;
}

void SmartEditController::capture_press_state() {
    // Items follow the mouse from its first pixel, before a drag is told from a click.
    move_initial_state = capture_layer_state(selected_item());
}

bool SmartEditController::handle_move(const QPointF &, const QPointF &scene_pos) {
    if (drag_start_pos && !is_dragging) {
        const auto delta = scene_pos - *drag_start_pos;
        const double distance = std::pow(delta.x() * delta.x() + delta.y() * delta.y(), .5);
        if (distance > drag_threshold) {
            is_dragging = true;
            if (selected_item()) {
                mode = SelectionMode::DRAGGING_MOVE;
                if (!move_initial_state) move_initial_state = capture_layer_state(selected_item());
                // Hides the rotate handle while moving.
                if (layer_editor) layer_editor->set_is_moving_item(true);
            }
        }
    }
    if (is_dragging && selected_item() && !move_initial_state) move_initial_state = capture_layer_state(selected_item());
    return false;
}

bool SmartEditController::handle_release(const QPointF &, const QPointF &, Qt::MouseButton button) {
    if (button != Qt::LeftButton) return false;
    if (is_dragging) {
        is_dragging = false;
        if (mode == SelectionMode::DRAGGING_MOVE) {
            finalize_move_edit();
            mode = SelectionMode::SELECTED;
            if (layer_editor) layer_editor->set_is_moving_item(false);
        }
    }
    drag_start_pos.reset();
    if (mode != SelectionMode::DRAGGING_MOVE) move_initial_state.reset();
    return false;
}

void SmartEditController::repaint_handles() {
    // Handles live on an overlay, which must be told explicitly.
    if (layer_editor) layer_editor->request_repaint();
}

void SmartEditController::select_item(QGraphicsItem *item, bool auto_select) {
    if (selected_item() == item) return;
    auto *previous = selected_item();
    set_selected_item(item);
    if (previous) previous->update();
    item->update();
    mode = SelectionMode::SELECTED;
    move_initial_state.reset();
    is_auto_selected_ = auto_select;
    emit selection_changed(item);
    if (layer_editor) {
        layer_editor->start_edit(item);
        repaint_handles();
    }
    if (dynamic_cast<ArrowItem *>(item)) sync_arrow_panel_state(item);
    sync_number_panel_state();
}

void SmartEditController::clear_selection(bool suppress_block) {
    active_click_handle_.reset();
    auto *previous = selected_item();
    if (!previous) return;
    // Clearing an auto-selected item (just drawn) allows drawing again at once.
    const bool was_auto_selected = is_auto_selected_;
    set_selected_item(nullptr);
    // The frame depends on selected_item, so repaint explicitly.
    previous->update();
    mode = SelectionMode::NONE;
    move_initial_state.reset();
    is_auto_selected_ = false;
    if (!was_auto_selected && !suppress_block) just_cleared_selection = true;
    emit selection_changed(nullptr);
    if (layer_editor) {
        layer_editor->stop_edit();
        layer_editor->set_is_moving_item(false);
        repaint_handles();
    }
    sync_number_panel_state();
}

void SmartEditController::delete_selected(bool suppress_block, bool renumber_numbers) {
    auto *item = selected_item();
    if (!item || !item->scene()) return;
    auto *item_scene = item->scene();
    auto *canvas = dynamic_cast<CanvasScene *>(item_scene);
    auto *tracked = dynamic_cast<TrackedItem *>(item);
    // Typing in the number's label ends first, so the text is recorded before the number goes.
    if (auto *number = dynamic_cast<NumberItem *>(item); number && number->label() && number->label()->hasFocus()) number->label()->clearFocus();
    QUndoCommand *command = nullptr;
    if (dynamic_cast<NumberItem *>(item)) {
        command = renumber_numbers ? static_cast<QUndoCommand *>(new RemoveNumberAndRenumberCommand(item_scene, ItemReference(tracked)))
                                   : new RemoveNumberCommand(item_scene, ItemReference(tracked));
    } else {
        command = new RemoveItemCommand(item_scene, ItemReference(tracked));
    }
    clear_selection(suppress_block);
    if (canvas && canvas->undo_stack) {
        canvas->undo_stack->push_command(command);
    } else {
        command->redo();
        delete command;
    }
}

bool SmartEditController::handle_edit_press(const QPointF &scene_pos, const QPointF &, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    if (button != Qt::LeftButton) return false;
    if (!selected_item() || !layer_editor) return false;
    const auto hit = layer_editor->hit_test(scene_pos);
    if (!hit) return false;
    auto *canvas = dynamic_cast<CanvasScene *>(scene);
    if (LayerEditor::is_number_adjust_handle(hit)) {
        layer_editor->set_hovered_handle(hit);
        if (layer_editor->adjust_number_with_handle(*hit, canvas ? canvas->undo_stack : nullptr)) {
            active_click_handle_ = hit;
            mode = SelectionMode::CLICKING_HANDLE;
            scene->update();
            repaint_handles();
            return true;
        }
    }
    if (LayerEditor::is_number_action_handle(hit)) {
        layer_editor->set_hovered_handle(hit);
        if (layer_editor->run_number_action(*hit, canvas ? canvas->undo_stack : nullptr)) {
            active_click_handle_ = hit;
            mode = SelectionMode::CLICKING_HANDLE;
            scene->update();
            repaint_handles();
            sync_number_panel_state();
            return true;
        }
    }
    if (LayerEditor::is_delete_handle(hit)) {
        layer_editor->set_hovered_handle(hit);
        // Only deleting a number renumbers the rest; deleting text leaves numbers alone.
        delete_selected(true, hit->handle_type == HandleType::NUMBER_DELETE);
        scene->update();
        repaint_handles();
        return true;
    }
    mode = SelectionMode::DRAGGING_HANDLE;
    const bool keep = modifiers & Qt::ShiftModifier;
    layer_editor->start_drag(*hit, scene_pos);
    keep_ratio = keep;
    return true;
}

bool SmartEditController::handle_edit_move(const QPointF &scene_pos) {
    if (mode == SelectionMode::CLICKING_HANDLE && layer_editor) {
        layer_editor->set_hovered_handle(active_click_handle_);
        return true;
    }
    if (mode != SelectionMode::DRAGGING_HANDLE || !layer_editor) return false;
    // Only the item's own swept area is invalidated; handles belong to the overlay.
    const auto old_rect = layer_editor->get_scene_rect(selected_item());
    layer_editor->drag_to(scene_pos, keep_ratio);
    const auto new_rect = layer_editor->get_scene_rect(selected_item());
    if (old_rect && new_rect) scene->update(old_rect->united(*new_rect));
    else scene->update();
    repaint_handles();
    return true;
}

bool SmartEditController::handle_edit_release(const QPointF &scene_pos, Qt::MouseButton button) {
    if (button != Qt::LeftButton) return false;
    if (mode == SelectionMode::CLICKING_HANDLE) {
        active_click_handle_.reset();
        mode = SelectionMode::SELECTED;
        if (layer_editor) layer_editor->update_hover(scene_pos);
        repaint_handles();
        return true;
    }
    if (mode != SelectionMode::DRAGGING_HANDLE || !layer_editor) return false;
    auto *canvas = dynamic_cast<CanvasScene *>(scene);
    const bool radius_drag = layer_editor->dragging_handle && layer_editor->dragging_handle->handle_type == HandleType::CORNER_RADIUS;
    layer_editor->end_drag(canvas ? canvas->undo_stack : nullptr);
    mode = SelectionMode::SELECTED;
    keep_ratio = false;
    // The next rectangle starts with the radius just dragged; highlighter and spotlight
    // rects share the item class but not the setting.
    if (radius_drag && !is_cross_tool_selection())
        if (auto *rect = dynamic_cast<RectItem *>(selected_item()); rect && get_item_tool_id(rect) == "rect")
            get_tool_settings_manager()->update_settings("rect", {{"corner_radius", qint64(std::llround(rect->get_corner_radius()))}});
    scene->update();
    repaint_handles();
    // Dragging the text scale handle keeps the selection, so the panel needs an explicit refresh.
    if (dynamic_cast<QGraphicsTextItem *>(selected_item())) sync_text_panel_state(selected_item());
    return true;
}

std::optional<QVariantMap> SmartEditController::capture_layer_state(QGraphicsItem *item) const {
    if (!item || !layer_editor) return std::nullopt;
    return layer_editor->capture_state(item);
}

void SmartEditController::finalize_move_edit() {
    if (!layer_editor || !selected_item()) {
        move_initial_state.reset();
        return;
    }
    if (!move_initial_state) return;
    const auto new_state = layer_editor->capture_state(selected_item());
    if (new_state == move_initial_state || !new_state || new_state->isEmpty()) {
        move_initial_state.reset();
        return;
    }
    auto *canvas = dynamic_cast<CanvasScene *>(scene);
    if (canvas && canvas->undo_stack)
        if (auto *tracked = dynamic_cast<TrackedItem *>(selected_item()))
            canvas->undo_stack->push_command(new EditItemCommand(ItemReference(tracked), *move_initial_state, *new_state));
    if (auto *mosaic = dynamic_cast<MosaicItem *>(selected_item())) mosaic->edit_finished();
    layer_editor->start_edit(selected_item());
    repaint_handles();
    move_initial_state.reset();
}

void SmartEditController::on_undo_index_changed() {
    auto *item = selected_item();
    if (!item) return;
    if (!item->scene()) {
        // The item was undone out of the scene.
        set_selected_item(nullptr);
        mode = SelectionMode::NONE;
        move_initial_state.reset();
        emit selection_changed(nullptr);
        if (layer_editor) layer_editor->stop_edit();
        if (scene) scene->update();
        repaint_handles();
    } else if (layer_editor) {
        layer_editor->start_edit(item);
        // The undone value may be what the panel shows (arrow style, font size).
        if (dynamic_cast<ArrowItem *>(item)) sync_arrow_panel_state(item);
        else if (dynamic_cast<TextItem *>(item)) sync_text_panel_state(item);
        repaint_handles();
    }
    sync_number_panel_state();
}

void SmartEditController::sync_arrow_panel_state(QGraphicsItem *item) {
    // Pin windows may hide panels, so a missing one is silently skipped.
    auto *host = canvas_host_of_scene(scene);
    auto *toolbar = host ? host->canvas_toolbar() : nullptr;
    if (toolbar && toolbar->arrow_panel) toolbar->arrow_panel->set_arrow_style(static_cast<ArrowItem *>(item)->_arrow_style);
}

void SmartEditController::sync_number_panel_state() {
    auto *host = canvas_host_of_scene(scene);
    auto *toolbar = host ? host->canvas_toolbar() : nullptr;
    auto *panel = toolbar ? toolbar->number_panel : nullptr;
    if (!panel) return;
    if (connected_number_panel_ != panel) {
        connected_number_panel_ = panel;
        connect(panel, &NumberSettingsPanel::label_font_changed, this, [this](const QString &family) { on_number_extras_changed({{"label_font", family}}); });
        connect(panel, &NumberSettingsPanel::label_size_changed, this, [this](int level) { on_number_extras_changed({{"label_size", level}}); });
        connect(panel, &NumberSettingsPanel::label_outline_changed, this, [this](bool on) { on_number_extras_changed({{"label_outline", on}}); });
        connect(panel, &NumberSettingsPanel::label_guide_changed, this, [this](bool on) { on_number_extras_changed({{"label_guide", on}}); });
        connect(panel, &NumberSettingsPanel::arrow_style_changed, this, [this](const QString &style) { on_number_extras_changed({{"arrow_style", style}}); });
    }
    auto *number = dynamic_cast<NumberItem *>(selected_item());
    const bool shown = number && (number->arrow_tip() || !number->label_text().isEmpty() || (number->label() && number->label()->is_editing()));
    toolbar->show_number_extras(shown ? std::optional<QVariantMap>(number->extras_state()) : std::nullopt);
}

void SmartEditController::on_number_extras_changed(const QVariantMap &change) {
    if (auto *number = dynamic_cast<NumberItem *>(selected_item())) {
        const auto before = number->extras_state();
        number->apply_extras_state(change);
        const auto after = number->extras_state();
        if (before != after)
            if (auto *canvas = dynamic_cast<CanvasScene *>(scene); canvas && canvas->undo_stack)
                canvas->undo_stack->push_command(new EditItemCommand(ItemReference(number), before, after, "Number Label Style"));
    }
    // A temporary cross-tool edit changes only the item, never the number tool's defaults.
    if (!is_cross_tool_selection()) NumberTool::remember_extras(change);
}

void SmartEditController::sync_text_panel_state(QGraphicsItem *item) {
    auto *host = canvas_host_of_scene(scene);
    auto *toolbar = host ? host->canvas_toolbar() : nullptr;
    if (toolbar && toolbar->text_panel) toolbar->text_panel->set_state_from_item(dynamic_cast<QGraphicsTextItem *>(item));
}

void SmartEditController::on_text_font_changed(const QFont &font) {
    if (auto *text = dynamic_cast<TextItem *>(selected_item())) {
        text->setFont(font);
        text->update();
        if (layer_editor) layer_editor->start_edit(text);
    }
}

void SmartEditController::on_text_color_changed(const QColor &color) {
    if (auto *text = dynamic_cast<TextItem *>(selected_item())) {
        text->setDefaultTextColor(color);
        text->update();
    }
}

void SmartEditController::on_text_outline_changed(bool enabled, const QColor &color, const QVariant &width) {
    if (auto *text = dynamic_cast<TextItem *>(selected_item())) {
        text->set_outline(enabled, color, width);
        text->update();
    }
}

void SmartEditController::on_text_shadow_changed(bool enabled, const QColor &color) {
    if (auto *text = dynamic_cast<TextItem *>(selected_item())) {
        text->set_shadow(enabled, color);
        text->update();
    }
}

void SmartEditController::on_text_background_changed(bool enabled, const QColor &color, int opacity) {
    if (auto *text = dynamic_cast<TextItem *>(selected_item())) {
        text->set_background(enabled, color, opacity);
        text->update();
    }
}
}
