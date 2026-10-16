#pragma once
// Mouse gestures the view owns end to end, each with its phases in one class.
// Plain ItemIsMovable drags stay with Qt; these are the ones it cannot handle.
// Both item drags take over undo: one snapshot on entry, and on release
// SmartEditController.finalize_move_edit pushes a single EditItemCommand
// (none if nothing moved).
#include <QPoint>
#include <QPointF>
#include <optional>

class QGraphicsTextItem;
class QMouseEvent;

namespace jietuba {
class CanvasView;

// Dragging out the capture selection before it is confirmed. A press only
// counts as a drag past DRAG_THRESHOLD, so a click does not collapse the selection.
class SelectionDrag {
public:
    static constexpr double DRAG_THRESHOLD = 10;
    explicit SelectionDrag(CanvasView *view) : view_(view) {}
    void begin(const QPointF &scene_pos);
    void perform(const QPointF &scene_pos);
    void end();
    bool active = false;
    bool dragging = false;
    QPointF start_pos;
private:
    CanvasView *view_;
};

// One stroke of the current tool; what it produces is entirely up to the tool.
class DrawingStroke {
public:
    explicit DrawingStroke(CanvasView *view) : view_(view) {}
    void begin(const QPointF &scene_pos, Qt::MouseButton button);
    void perform(const QPointF &scene_pos);
    void end(const QPointF &scene_pos);
    bool active = false;
private:
    CanvasView *view_;
};

// Text being edited moves only by its border; the inside belongs to the caret.
class TextEdgeDrag {
public:
    explicit TextEdgeDrag(CanvasView *view) : view_(view) {}
    // The edge test is the item's own (TextItem::is_on_edge), in local coordinates.
    static bool is_point_on_edge(QGraphicsTextItem *item, const QPointF &scene_pos);
    void set_cursor(bool active);
    void update_hover(const QPointF &scene_pos);
    void begin(QGraphicsTextItem *item, const QPointF &scene_pos);
    void perform(const QPointF &scene_pos);
    // Settles undo first; finalize_move_edit skips a press without movement.
    void end();
    // Cancels without settling undo (edit ended, scene cleared).
    void reset();
    bool active = false;
    QGraphicsTextItem *item = nullptr;
    QGraphicsTextItem *hover_item = nullptr;
private:
    void clear();
    CanvasView *view_;
    std::optional<QPointF> last_scene_pos_;
    bool cursor_active_ = false;
};

// Clicking selected text enters editing on release, not press, because the
// press may start a drag; moving past the tolerance cancels it.
class PendingTextEdit {
public:
    static constexpr int MOVE_TOLERANCE = 5;
    explicit PendingTextEdit(CanvasView *view) : view_(view) {}
    void arm(QMouseEvent *event, const QPointF &scene_pos);
    void track(QMouseEvent *event);
    void settle(QMouseEvent *event, const QPointF &scene_pos);
    void clear();
    QGraphicsTextItem *item = nullptr;
private:
    CanvasView *view_;
    std::optional<QPoint> press_pos_;
    bool moved_ = false;
};

// An item drag the view owns when the controller deliberately selected an item
// below the topmost one (Qt would send the moves to the top item).
class ManualItemDrag {
public:
    explicit ManualItemDrag(CanvasView *view) : view_(view) {}
    void begin(const QPointF &scene_pos);
    // Moves by the step since the last position, not since the gesture start,
    // so crossing the drag threshold does not make the item jump.
    void perform(QMouseEvent *event, const QPointF &scene_pos);
    void finish(bool commit);
    bool active = false;
private:
    CanvasView *view_;
    std::optional<QPointF> last_scene_pos_;
};
}
