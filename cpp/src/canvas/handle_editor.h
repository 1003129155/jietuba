#pragma once
#include "canvas/items/item_reference.h"
#include <QColor>
#include <QCursor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVariantMap>
#include <functional>
#include <optional>
#include <vector>

class QPainter;
class QUndoStack;

namespace jietuba {
enum class HandleType {
    CORNER_TL, CORNER_TR, CORNER_BR, CORNER_BL, EDGE_T, EDGE_R, EDGE_B, EDGE_L,
    ROTATE, ARROW_START, ARROW_END, ARROW_CONTROL, CORNER_RADIUS,
    NUMBER_INCREMENT, NUMBER_DECREMENT, NUMBER_DELETE, ITEM_DELETE, TEXT_SCALE
};
QString handle_type_name(HandleType type);

// Position is the anchor in scene coordinates (content, follows view zoom);
// size, hit padding and offset are screen pixels (interface, constant under zoom).
struct EditHandle {
    int id;
    HandleType handle_type;
    QPointF position;
    QCursor cursor;
    int size = 8, hit_area_padding = 8;
    QPointF offset;
    QRectF rect_at(const QPointF &center) const;
    QRectF hit_rect_at(const QPointF &center) const;
};

// Edit handles of the selected item: generation, hit testing, dragging with
// undo snapshots, and rendering in viewport coordinates. Interaction takes
// scene coordinates; handle sizes and drawing are in screen pixels via screen_transform.
class LayerEditor {
public:
    static constexpr int HANDLE_SIZE = 10;
    static constexpr int HANDLE_BORDER_WIDTH = 2;
    static inline const QColor HANDLE_COLOR = QColor(0, 123, 255);
    static inline const QColor HANDLE_FILL = QColor(255, 255, 255);
    static inline const QColor HOVER_COLOR = QColor(0, 160, 255);
    static inline const QColor HOVER_FILL = QColor(0, 123, 255);
    static inline const QColor ROTATE_HANDLE_COLOR = QColor(0, 200, 83);
    static constexpr int ROTATE_HANDLE_SIZE = 23;
    static inline const QColor ROTATE_LINE_COLOR = QColor(0, 123, 255, 100);
    static constexpr int RADIUS_HANDLE_SIZE = 11;
    static inline const QColor RADIUS_HANDLE_COLOR = QColor(255, 140, 0);
    static inline const QColor RADIUS_HANDLE_HOVER_COLOR = QColor(255, 100, 0);
    // Inset of a corner-radius handle when the radius is 0.
    static constexpr int RADIUS_HANDLE_MIN_OFFSET = 16;
    // Shared side of functional handles (rotate, delete, number +/-).
    static constexpr int FUNCTIONAL_HANDLE_SIZE = 14;
    // Glyph radius as a share of the side; the rest is theme colour.
    static constexpr double FUNCTIONAL_GLYPH_RATIO = .24;
    static constexpr int NUMBER_BUTTON_SIZE = 14;
    static constexpr int NUMBER_BUTTON_GAP = 4;
    static inline const QColor NUMBER_BUTTON_COLOR = QColor(220, 45, 35);
    static inline const QColor NUMBER_BUTTON_HOVER_COLOR = QColor(245, 70, 55);

    LayerEditor();
    static QCursor get_rotate_cursor();

    bool start_edit(QGraphicsItem *layer);
    void stop_edit();
    bool is_editing() const { return active_layer_.get() != nullptr; }
    QGraphicsItem *active_layer() const { return active_layer_.get(); }
    // Recomputes handles from the item's current geometry (typing, font size, number changes).
    void refresh_handles();
    std::vector<EditHandle> generate_handles(QGraphicsItem *layer);
    // Scene bounding rect; number items use their visual rect.
    std::optional<QRectF> get_scene_rect(QGraphicsItem *layer) const;
    // Corners and edge midpoints of the local bounds, each mapped to the scene,
    // so handles follow rotation (the scene bounding box corners do not).
    std::optional<std::vector<std::pair<QString, QPointF>>> scene_anchors(QGraphicsItem *layer) const;

    double scene_units_per_pixel() const;
    QPointF screen_center(const EditHandle &handle) const;
    QRectF screen_rect(const EditHandle &handle) const;
    std::optional<EditHandle> hit_test(const QPointF &pos) const;
    void request_repaint() { if (repaint_requested) repaint_requested(); }

    // Setters notify the overlay, so state change and repaint cannot drift apart.
    const std::optional<EditHandle> &hovered_handle() const { return hovered_handle_; }
    void set_hovered_handle(const std::optional<EditHandle> &handle);
    bool is_moving_item() const { return is_moving_item_; }
    void set_is_moving_item(bool moving);
    bool update_hover(const QPointF &pos);
    QCursor get_cursor(const QPointF &pos) const;
    static bool is_number_adjust_handle(const std::optional<EditHandle> &handle);
    static bool is_delete_handle(const std::optional<EditHandle> &handle);
    bool adjust_number_with_handle(const EditHandle &handle, QUndoStack *undo_stack);

    void start_drag(const EditHandle &handle, const QPointF &pos);
    void drag_to(const QPointF &pos, bool keep_ratio = false);
    // Pushes an EditItemCommand when the state changed; returns (old, new).
    std::pair<std::optional<QVariantMap>, std::optional<QVariantMap>> end_drag(QUndoStack *undo_stack = nullptr);

    // Paired with render(): the screen area it draws.
    QRectF screen_bounds();
    // painter must be in viewport coordinates.
    void render(QPainter *painter);

    std::optional<QVariantMap> capture_state(QGraphicsItem *layer = nullptr) const;

    std::vector<EditHandle> handles;
    // Injected by the view: overlay repaint and scene-to-screen transform.
    std::function<void()> repaint_requested;
    std::function<QTransform()> screen_transform;
    std::optional<EditHandle> dragging_handle;
    std::optional<QPointF> drag_start_pos;
    std::optional<QVariantMap> initial_layer_state;
private:
    std::vector<EditHandle> generate_number_handles(const QRectF &rect) const;
    std::vector<EditHandle> generate_arrow_handles(QGraphicsItem *layer) const;
    std::vector<EditHandle> generate_rect_handles(const QRectF &rect, QGraphicsItem *layer);
    void append_corner_radius_handles(std::vector<EditHandle> &handles, QGraphicsItem *layer) const;
    std::optional<QRectF> local_bounds(QGraphicsItem *layer) const;
    QTransform to_screen() const;
    void apply_handle_drag(QGraphicsItem *layer, const EditHandle &handle, const QPointF &delta_scene, bool keep_ratio);
    static void apply_rect_delta(QRectF &rect, HandleType type, const QPointF &delta);
    void apply_text_scale_drag(QGraphicsItem *layer, const QPointF &delta_scene);
    void apply_corner_radius_drag(QGraphicsItem *layer, const EditHandle &handle, const QPointF &delta_scene);
    void apply_rotation(QGraphicsItem *layer, const QPointF &delta_scene);
    void apply_stroke_item_drag(QGraphicsItem *layer, const EditHandle &handle, const QPointF &delta_scene, bool keep_ratio);
    void capture_arrow_base_geometry(QGraphicsItem *layer);
    void apply_arrow_item_drag(QGraphicsItem *layer, HandleType type, const QPointF &delta_scene);
    void render_functional_handle(QPainter *painter, const EditHandle &handle) const;
    void draw_handle_glyph(QPainter *painter, const EditHandle &handle, const QPointF &center, const QColor &ink) const;
    std::optional<QVariantMap> copy_layer_state(QGraphicsItem *layer) const;
    void restore_base_state(QGraphicsItem *layer);
    static std::optional<QRectF> get_local_rect(QGraphicsItem *layer);
    static void set_local_rect(QGraphicsItem *layer, const QRectF &rect);
    bool is_hovered(const EditHandle &handle) const { return hovered_handle_ && hovered_handle_->id == handle.id; }

    TrackedPointer active_layer_;
    bool number_item_mode_ = false;
    bool is_moving_item_ = false;
    std::optional<EditHandle> hovered_handle_;
    // Every drag step recomputes from these to avoid accumulating error.
    std::optional<QRectF> base_scene_rect_, base_local_rect_;
    std::optional<QPointF> base_pos_;
    std::optional<QTransform> base_transform_;
    std::optional<double> base_rotation_;
    std::optional<QPointF> rotation_origin_local_;
    std::optional<QPointF> arrow_base_start_local_, arrow_base_end_local_, arrow_base_start_scene_, arrow_base_end_scene_;
    std::optional<QPointF> arrow_base_control_local_, arrow_base_control_scene_;
    bool arrow_base_control_modified_ = false;
    std::optional<double> base_corner_radius_, base_font_point_size_;
};
}
