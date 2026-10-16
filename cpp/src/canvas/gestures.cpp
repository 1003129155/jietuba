#include "canvas/gestures.h"
#include "canvas/items/text_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/view.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include <QGraphicsTextItem>
#include <QMouseEvent>
#include <cmath>

namespace jietuba {
void SelectionDrag::begin(const QPointF &scene_pos) {
    active = true;
    dragging = false;
    start_pos = scene_pos;
    auto *model = view_->canvas_scene->selection_model;
    model->activate();
    // Hide handles while dragging.
    model->start_dragging();
    // Smart selection snaps at press without tweening: the selection must be a
    // real window rectangle now, or the capture takes an interpolated one.
    if (view_->smart_selection_enabled) view_->apply_smart_selection_rect(view_->get_smart_selection_rect(scene_pos), false);
}

void SelectionDrag::perform(const QPointF &scene_pos) {
    view_->update_magnifier_overlay(scene_pos);
    if (!dragging && (scene_pos - start_pos).manhattanLength() > DRAG_THRESHOLD) dragging = true;
    if (dragging) {
        auto rect = QRectF(start_pos, scene_pos).normalized();
        view_->canvas_scene->selection_model->set_rect(rect);
    }
}

void SelectionDrag::end() {
    active = false;
    dragging = false;
    view_->canvas_scene->selection_model->stop_dragging();
    view_->canvas_scene->confirm_selection();
}

void DrawingStroke::begin(const QPointF &scene_pos, Qt::MouseButton button) {
    active = true;
    // Hide the magnifier at once so its hide and the first stroke frame do not stack up.
    view_->clear_magnifier_overlay();
    // A tool may refuse to start; then this press drew nothing.
    if (!view_->canvas_scene->tool_controller->on_press(scene_pos, button)) active = false;
}

void DrawingStroke::perform(const QPointF &scene_pos) {
    view_->canvas_scene->tool_controller->on_move(scene_pos);
    view_->apply_tool_cursor();
}

void DrawingStroke::end(const QPointF &scene_pos) {
    active = false;
    view_->canvas_scene->tool_controller->on_release(scene_pos);
    view_->update_magnifier_overlay(scene_pos);
}

bool TextEdgeDrag::is_point_on_edge(QGraphicsTextItem *item, const QPointF &scene_pos) {
    auto *text = dynamic_cast<TextItem *>(item);
    return text && text->is_on_edge(item->mapFromScene(scene_pos));
}

void TextEdgeDrag::set_cursor(bool is_active) {
    if (is_active) {
        cursor_active_ = true;
        view_->setCursor(Qt::SizeAllCursor);
        return;
    }
    if (!cursor_active_) return;
    cursor_active_ = false;
    auto *cursor_manager = view_->cursor_manager.get();
    if (view_->is_text_editing()) view_->viewport()->unsetCursor();
    else if (cursor_manager && cursor_manager->current_cursor && cursor_manager->current_tool_id != "cursor") view_->setCursor(*cursor_manager->current_cursor);
    else view_->setCursor(Qt::ArrowCursor);
}

void TextEdgeDrag::update_hover(const QPointF &scene_pos) {
    if (active) return;
    if (!view_->is_text_editing()) {
        if (hover_item) {
            hover_item = nullptr;
            set_cursor(false);
        }
        return;
    }
    auto *editing = view_->get_active_text_item();
    if (editing && is_point_on_edge(editing, scene_pos)) {
        hover_item = editing;
        set_cursor(true);
    } else {
        hover_item = nullptr;
        set_cursor(false);
    }
}

void TextEdgeDrag::begin(QGraphicsTextItem *text_item, const QPointF &scene_pos) {
    view_->pending_text_edit->clear();
    active = true;
    item = text_item;
    last_scene_pos_ = scene_pos;
    set_cursor(true);
    if (auto *controller = view_->smart_edit_controller.get()) {
        controller->select_item(text_item, false);
        // One snapshot per gesture, however far it moves.
        controller->move_initial_state = controller->capture_layer_state(text_item);
    }
}

void TextEdgeDrag::perform(const QPointF &scene_pos) {
    if (!active || !item) return;
    if (!last_scene_pos_) {
        last_scene_pos_ = scene_pos;
        return;
    }
    const auto delta = scene_pos - *last_scene_pos_;
    if (std::abs(delta.x()) < 1e-3 && std::abs(delta.y()) < 1e-3) return;
    item->moveBy(delta.x(), delta.y());
    last_scene_pos_ = scene_pos;
}

void TextEdgeDrag::end() {
    auto *controller = view_->smart_edit_controller.get();
    if (active && controller && controller->selected_item() == item) controller->finalize_move_edit();
    clear();
}

void TextEdgeDrag::reset() {
    hover_item = nullptr;
    clear();
}

void TextEdgeDrag::clear() {
    active = false;
    item = nullptr;
    last_scene_pos_.reset();
    set_cursor(false);
}

void PendingTextEdit::arm(QMouseEvent *event, const QPointF &scene_pos) {
    if (event->button() != Qt::LeftButton) return;
    if (view_->is_text_editing()) return;
    auto *text = dynamic_cast<QGraphicsTextItem *>(view_->smart_edit_controller->selected_item());
    if (!text) return;
    // Only while the press is still on the text.
    if (!text->contains(text->mapFromScene(scene_pos))) return;
    item = text;
    press_pos_ = event->pos();
    moved_ = false;
}

void PendingTextEdit::track(QMouseEvent *event) {
    if (!item || !press_pos_) return;
    if (!(event->buttons() & Qt::LeftButton)) return;
    if ((event->pos() - *press_pos_).manhattanLength() > MOVE_TOLERANCE) moved_ = true;
}

void PendingTextEdit::settle(QMouseEvent *event, const QPointF &scene_pos) {
    auto *text = item;
    if (!text) return;
    const bool should_enter = event->button() == Qt::LeftButton && !moved_ && text->contains(text->mapFromScene(scene_pos));
    clear();
    if (should_enter)
        if (auto *text_item = dynamic_cast<TextItem *>(text)) view_->enter_text_edit_mode(text_item);
}

void PendingTextEdit::clear() {
    item = nullptr;
    press_pos_.reset();
    moved_ = false;
}

void ManualItemDrag::begin(const QPointF &scene_pos) {
    active = true;
    last_scene_pos_ = scene_pos;
}

void ManualItemDrag::perform(QMouseEvent *event, const QPointF &scene_pos) {
    auto *controller = view_->smart_edit_controller.get();
    auto *selected_item = controller->selected_item();
    controller->handle_move(event->pos(), scene_pos);
    if (controller->is_dragging && selected_item) {
        const auto last_pos = last_scene_pos_.value_or(scene_pos);
        const auto delta = scene_pos - last_pos;
        if (!delta.isNull()) selected_item->moveBy(delta.x(), delta.y());
        last_scene_pos_ = scene_pos;
        view_->update_edit_handles();
    }
    view_->setCursor(Qt::SizeAllCursor);
}

void ManualItemDrag::finish(bool commit) {
    auto *controller = view_->smart_edit_controller.get();
    if (!active) {
        if (controller) controller->press_requires_manual_dispatch = false;
        return;
    }
    if (controller) {
        if (commit && controller->is_dragging && controller->selected_item()) controller->finalize_move_edit();
        controller->is_dragging = false;
        controller->drag_start_pos.reset();
        controller->move_initial_state.reset();
        controller->press_requires_manual_dispatch = false;
        if (controller->mode == SelectionMode::DRAGGING_MOVE) controller->mode = SelectionMode::SELECTED;
        if (controller->layer_editor) controller->layer_editor->set_is_moving_item(false);
    }
    active = false;
    last_scene_pos_.reset();
}
}
