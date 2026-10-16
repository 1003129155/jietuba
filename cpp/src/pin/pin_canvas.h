#pragma once
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QSize>
#include <QVariantMap>
#include <optional>

class QGraphicsItem;
class QPainter;

namespace jietuba {
class CanvasScene;
class CanvasView;
class CommandUndoStack;
class PinToolbar;
class PinWindow;
class ToolController;

// The pin canvas: a full CanvasScene as in the capture window, so undo, tools,
// layers and styles all come from there.
class PinCanvas : public QObject {
    Q_OBJECT
public:
    PinCanvas(PinWindow *parent_window, const QSize &base_size, const QImage &background_image);
    // Takes over the capture's vector annotations, moved by -selection_offset;
    // number_next syncs the number counter (nullopt: no numbers to sync).
    void initialize_from_items(const QList<QGraphicsItem *> &drawing_items, const QPoint &selection_offset, std::optional<int> number_next = std::nullopt);
    // Draws the scene into target_rect (window coordinates).
    void render_to_painter(QPainter *painter, const QRectF &target_rect);
    // Enters editing; the only criterion is whether this scene registered the tool.
    bool activate_tool(const QString &tool_name);
    void deactivate_tool();
    // Toolbar signals routed into the canvas and view.
    void connect_toolbar(PinToolbar *toolbar, CanvasView *view);
    // All three go through scene.update_style, which also emits the cursor refresh signals.
    void set_color(const QColor &color);
    void set_stroke_width(int width);
    void set_opacity(double opacity);
    QImage export_to_image(const QSize &size, double dpr = 1.0);
    // Background plus vector annotations.
    QImage get_current_image(double dpr = 1.0) { return export_to_image(base_size, dpr); }
    void cleanup();
    // Forces a scene repaint.
    void invalidate_cache();
    bool is_editing() const { return is_editing_; }
    PinWindow *parent_window;
    QSize base_size;
    CanvasScene *scene = nullptr;
    CommandUndoStack *undo_stack = nullptr;
    ToolController *tool_controller = nullptr;
private:
    void sync_number_next(int number_next);
    QGraphicsItem *clone_graphics_item(QGraphicsItem *item);
    // Position, Z value and opacity from drawing time.
    void apply_static_item_state(QGraphicsItem *source, QGraphicsItem *cloned, double offset_x, double offset_y);
    static QVariantMap capture_item_state(QGraphicsItem *item);
    static QVariantMap build_final_state(QGraphicsItem *source, const QVariantMap &base_state);
    void initialize_selection();
    void notify_editing_changed();
    void on_tool_changed(const QString &tool_name, PinToolbar *toolbar, CanvasView *view);
    void on_stroke_width_changed(int width, CanvasView *view);
    void on_opacity_changed(int opacity_int, CanvasView *view);
    void on_arrow_style_changed(const QString &style, CanvasView *view);
    void on_watermark_requested(PinToolbar *toolbar);
    void on_watermark_style_changed(const QString &key, const QVariant &value);
    void on_watermark_text_changed(const QString &template_text, PinToolbar *toolbar);
    static QVariantMap capture_arrow_state(QGraphicsItem *item);
    bool is_editing_ = false;
    bool is_drawing_ = false;
};
}
