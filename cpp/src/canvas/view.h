#pragma once
#include "canvas/undo.h"
#include "canvas/gestures.h"
#include "canvas/items/drawing_items.h"
#include <QColor>
#include <QGraphicsView>
#include <QMetaObject>
#include <QPointer>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class QGraphicsTextItem;

namespace jietuba {
class CanvasScene;
class CursorManager;
class HandleOverlayWidget;
class MagnifierOverlay;
class SmartEditController;
class SmartSelectionAnimator;
class TextItem;
class Toolbar;
class ToolController;
class MSAAElementFinder;
class WindowFinder;

// The screenshot mask, which draws the full-screen crosshair (see set_fullscreen_crosshair).
class CrosshairSurface : public QWidget {
public:
    using QWidget::QWidget;
    virtual void set_crosshair_position(const std::optional<QPoint> &position) = 0;
};

// Rect as [x1, y1, x2, y2] in physical screen pixels.
using SmartRect = std::array<int, 4>;
// Adjacent levels within this many pixels on every side count as one level;
// a one-logical-pixel border is two physical pixels on high-DPI screens.
constexpr int LEVEL_TOLERANCE_PX = 2;
// Appends to a fine-to-coarse nesting chain, skipping levels that are not
// really coarser (border/padding differences, or clipped rects that shrank).
void append_level(std::vector<SmartRect> &chain, const SmartRect &rect);

// The view of a CanvasScene: routes mouse input between selection, drawing,
// item editing and text editing, and hosts smart selection.
class CanvasView : public QGraphicsView, public DrawingEditContext, public NumberRefreshContext {
    Q_OBJECT
public:
    CanvasView(CanvasScene *scene, QWidget *parent = nullptr, bool confirm_on_double_click = false, bool cross_tool_select = false);
    ~CanvasView() override;

    // DrawingEditContext, for the hover and selection frames of drawing items.
    QGraphicsItem *selected_drawing_item() const override;
    bool can_show_hover_cursor(QGraphicsItem *item) const override;
    virtual ToolController *tool_controller() const;
    // NumberRefreshContext: the next number shown on the host window's toolbar.
    void refresh_number_toolbar(int next_number) override;
    // The scene owns the cursor refresh.
    void refresh_number_cursor_if_active(bool) override {}

    void request_handles_repaint();
    // Sets the cursor on the viewport too; Qt otherwise applies it only to the parent.
    void setCursor(const QCursor &cursor);
    // While selecting, replaces the plain cross cursor by a full-screen crosshair drawn on the mask; once the
    // selection is confirmed the drawing tools use the plain cross cursor again.
    void set_fullscreen_crosshair(bool enabled, CrosshairSurface *overlay = nullptr);
    // Disconnects session signals so a closing view ignores late callbacks.
    void cleanup();
    // Tool preview cursors apply only while the edit state machine does not own the cursor.
    bool can_apply_tool_cursor() const;

    // "off", "window" or "element" (element adds the control under the pointer, found through MSAA, below windows).
    void enable_smart_selection(const QString &mode);
    bool prepare_initial_selection();
    QRectF get_smart_selection_rect(const QPointF &scene_pos);
    void apply_smart_selection_rect(const QRectF &rect, bool animate);

    // Applies one style to a selected item of another tool without touching
    // tool defaults. nullopt: not a cross-tool selection; false: unsupported property.
    std::optional<bool> apply_cross_tool_selection_style(const std::optional<QColor> &color = std::nullopt,
                                                         const std::optional<double> &width = std::nullopt,
                                                         const std::optional<double> &opacity = std::nullopt);
    void invalidate_double_click_candidate() { double_click_candidate_.reset(); }

    bool is_text_editing() const { return get_active_text_item() != nullptr; }
    QGraphicsTextItem *get_active_text_item() const;
    void apply_size_change_to_selection(double scale);
    void apply_opacity_change_to_selection(double opacity);
    void apply_line_style_change_to_selection(const QString &style);
    void update_magnifier_overlay(const QPointF &scene_pos);
    void clear_magnifier_overlay();
    void apply_tool_cursor();
    void update_edit_handles();
    void enter_text_edit_mode(TextItem *item);
    // Ends editing: focus-out lets the item settle its content (commit, discard
    // or undoable clear), then view-side state is cleared.
    void end_text_edit(QGraphicsTextItem *text_item);
    void export_and_close();

    CanvasScene *canvas_scene = nullptr;
    bool confirm_on_double_click = false;
    bool smart_selection_enabled = false;
    bool smart_selection_drills_to_elements = false;
    bool smart_selection_animated = false;
    std::unique_ptr<WindowFinder> window_finder;
    std::unique_ptr<MSAAElementFinder> element_finder;
    std::unique_ptr<CursorManager> cursor_manager;
    std::unique_ptr<SmartEditController> smart_edit_controller;
    std::unique_ptr<DrawingStroke> drawing;
    std::unique_ptr<SelectionDrag> selection_drag;
    std::unique_ptr<TextEdgeDrag> text_drag;
    std::unique_ptr<ManualItemDrag> item_drag;
    std::unique_ptr<PendingTextEdit> pending_text_edit;
protected:
    void resizeEvent(QResizeEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void leaveEvent(QEvent *event) override;
    // A drawing just made is selected, so it can be adjusted at once.
    virtual void on_item_auto_select_requested(QGraphicsItem *item);
    bool is_closed_ = false;
private:
    struct DoubleClickCandidate {
        QPointF scene_pos;
        QRectF selection_rect;
        QString tool_id;
        int count, index;
        bool had_redo, dragged = false;
    };
    void follow_scene_for_handles(bool follow);
    void on_scene_changed_for_handles();
    void sync_fullscreen_crosshair();
    std::vector<SmartRect> smart_selection_chain(int x, int y, const SmartRect &capture_rect);
    std::vector<SmartRect> element_rects(quintptr hwnd, const SmartRect &window_rect, int x, int y, const SmartRect &capture_rect);
    std::optional<SmartRect> pick_smart_selection_level(const std::vector<SmartRect> &chain);
    bool adjust_smart_selection_level(int delta, const QPointF &scene_pos);
    void on_element_cache_updated(quintptr hwnd);
    void set_selection_rect_now(const QRectF &rect);
    void on_cursor_tool_update_requested(const QString &tool_id, bool force);
    void on_editing_cleanup();
    void on_tool_changed_for_edit(const QString &tool_id);
    void on_edit_cursor_change(const QString &cursor_type);
    void on_cross_tool_switch_requested(const QString &tool_id);
    void on_edit_selection_changed(QGraphicsItem *item);
    void sync_highlighter_panel_mode();
    Toolbar *get_active_toolbar() const;
    void sync_selection_style_to_toolbar(QGraphicsItem *item);
    void show_panel_for_selection(QGraphicsItem *item, Toolbar *toolbar);
    static std::optional<double> extract_selection_width(QGraphicsItem *item);
    static std::optional<double> extract_selection_opacity(QGraphicsItem *item);
    bool route_capture_right_button(QMouseEvent *event, bool dispatch);
    std::optional<DoubleClickCandidate> build_double_click_candidate(const QPointF &scene_pos, QMouseEvent *event);
    bool consume_double_click_candidate(const QPointF &scene_pos, QMouseEvent *event);
    void handle_text_drag_move(const QPointF &scene_pos);
    void handle_edit_mode_move(QMouseEvent *event, const QPointF &scene_pos);
    bool handle_edit_handle_drag(const QPointF &scene_pos);
    bool handle_edit_handle_hover(QMouseEvent *event, const QPointF &scene_pos);
    bool handle_selected_item_drag(QMouseEvent *event, const QPointF &scene_pos);
    void handle_edit_hover_detection(QMouseEvent *event, const QPointF &scene_pos);
    void handle_hover_preview(const QPointF &scene_pos);
    QGraphicsItem *text_switch_target(const QPointF &scene_pos, QGraphicsItem *editing_item, Qt::KeyboardModifiers modifiers);
    bool scale_item_size(QGraphicsItem *item, double scale);
    void scale_text_item(QGraphicsTextItem *item, double scale);
    static double get_text_point_size(QGraphicsTextItem *item);
    void set_text_item_point_size(QGraphicsTextItem *item, double point_size);
    void apply_text_point_size(QGraphicsTextItem *active_item, QGraphicsTextItem *selected_item, double point_size);
    bool update_item_visual_opacity(QGraphicsItem *item, double opacity);
    MagnifierOverlay *get_magnifier_overlay() const;
    void connect_text_geometry_updates(QGraphicsTextItem *item);
    void disconnect_text_geometry_updates(QGraphicsTextItem *item);
    void finalize_text_edit_state(QGraphicsTextItem *text_item);

    std::optional<DoubleClickCandidate> double_click_candidate_;
    int smart_selection_level_ = 0;
    std::optional<SmartRect> smart_selection_level_anchor_;
    std::optional<QPoint> initial_selection_point_;
    std::unique_ptr<SmartSelectionAnimator> smart_selection_anim_;
    QPointer<HandleOverlayWidget> handle_overlay_;
    QPointer<CrosshairSurface> crosshair_surface_;
    bool fullscreen_crosshair_ = false, crosshair_active_ = false, crosshair_inside_ = false, syncing_crosshair_ = false;
    QPoint crosshair_global_pos_;
    QMetaObject::Connection crosshair_confirm_connection_;
    int tool_changed_callback_ = 0;
    std::vector<QMetaObject::Connection> scene_connections_;
    QMetaObject::Connection handles_follow_scene_;
    std::map<QGraphicsTextItem *, QMetaObject::Connection> text_geometry_connections_;
};

}
