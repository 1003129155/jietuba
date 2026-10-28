#pragma once
#include "canvas/handle_editor.h"
#include "canvas/items/item_reference.h"
#include "canvas/items/drawing_items.h"
#include <QColor>
#include <QFont>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <memory>
#include <optional>

class QGraphicsScene;

namespace jietuba {
enum class SelectionMode { NONE, HOVER, SELECTED, EDITING, DRAGGING_MOVE, DRAGGING_HANDLE, CLICKING_HANDLE };
enum class ItemType { PATH, SHAPE, ARROW, TEXT, NUMBER, OTHER };

// Which items can be selected and edited with the current tool:
// pen/highlighter strokes need Ctrl+click and show no hover cross; shapes,
// arrows, text and numbers are selected by a plain click of the matching tool;
// with no tool active, drawings are not selectable so the selection moves instead.
class SmartEditController : public QObject {
    Q_OBJECT
public:
    explicit SmartEditController(QGraphicsScene *scene);
    // Releases session references so a stale controller ignores late signals.
    void cleanup();
    void set_tool(const QString &tool_id);
    ItemType get_item_type(QGraphicsItem *item) const;
    // The tool whose defaults the item was created with.
    std::optional<QString> get_item_tool_id(QGraphicsItem *item) const;
    // A host-enabled temporary edit of an item owned by another tool.
    bool is_cross_tool_selection() const;
    bool can_select_item(QGraphicsItem *item, Qt::KeyboardModifiers modifiers) const;
    bool can_show_hover_cursor(QGraphicsItem *item) const;

    bool handle_hover(const QPointF &pos, const QPointF &scene_pos);
    // true intercepts drawing.
    bool handle_press(const QPointF &pos, const QPointF &scene_pos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    bool handle_move(const QPointF &pos, const QPointF &scene_pos);
    bool handle_release(const QPointF &pos, const QPointF &scene_pos, Qt::MouseButton button);
    // selected_item is the single owner of "what is selected"; Qt's setSelected()
    // is not mirrored because Qt also changes it in mouse events.
    void select_item(QGraphicsItem *item, bool auto_select = false);
    void clear_selection(bool suppress_block = false);
    void delete_selected(bool suppress_block = false, bool renumber_numbers = false);
    bool handle_edit_press(const QPointF &scene_pos, const QPointF &view_pos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    bool handle_edit_move(const QPointF &scene_pos);
    bool handle_edit_release(const QPointF &scene_pos, Qt::MouseButton button);
    QGraphicsItem *get_selected_item() const { return selected_item(); }
    std::optional<QVariantMap> capture_layer_state(QGraphicsItem *item) const;
    void finalize_move_edit();

    // Reads as null once the item has been destroyed (undo, eraser, scene cleared).
    QGraphicsItem *selected_item() const { return selected_item_.get(); }
    void set_selected_item(QGraphicsItem *item) { selected_item_.reset(item); }

    void on_text_font_changed(const QFont &font);
    void on_text_color_changed(const QColor &color);
    // Text styling is applied directly without undo commands, like font and colour.
    void on_text_outline_changed(bool enabled, const QColor &color, const QVariant &width);
    void on_text_shadow_changed(bool enabled, const QColor &color);
    void on_text_background_changed(bool enabled, const QColor &color, int opacity = 200);

    QGraphicsScene *scene = nullptr;
    SelectionMode mode = SelectionMode::NONE;
    TrackedPointer hovered_item;
    QString current_tool_id;
    bool cross_tool_select_enabled = false;
    std::optional<QPointF> drag_start_pos;
    double drag_threshold = 5.0;
    bool is_dragging = false;
    bool press_requires_manual_dispatch = false;
    std::unique_ptr<LayerEditor> layer_editor;
    std::optional<QVariantMap> move_initial_state;
    // Set after a manual deselection so that click does not also start drawing.
    bool just_cleared_selection = false;
    bool keep_ratio = false;
signals:
    void selection_changed(QGraphicsItem *item);
    void hover_changed(QGraphicsItem *item);
    void cursor_change_request(const QString &cursor_type);
    // Ctrl-selecting another tool's item switches to that tool permanently
    // (like a toolbar click). Handled before select_item(), since switching tools clears the selection.
    void tool_switch_requested(const QString &tool_id);
private:
    void repaint_handles();
    void on_undo_index_changed();
    void sync_arrow_panel_state(QGraphicsItem *arrow_item);
    // Unfolds the number panel's label and pointer part while the selected number has either, or is being labelled.
    void sync_number_panel_state();
    void on_number_extras_changed(const QVariantMap &change);
    QPointer<QObject> connected_number_panel_;
    void sync_text_panel_state(QGraphicsItem *text_item);
    bool is_auto_selected_ = false;
    std::optional<EditHandle> active_click_handle_;
    TrackedPointer selected_item_;
    QMetaObject::Connection undo_connection_;
};
}
