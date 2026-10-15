#pragma once
#include <QColor>
#include <QCursor>
#include <QPointF>
#include <QString>
#include <optional>

class QGraphicsEllipseItem;
class QGraphicsScene;

namespace jietuba {
class CanvasView;
class ToolController;

// Tool cursors (crosshair with a filled dot for pen/highlighter/mosaic, a
// number preview, an I-beam for text, a cross otherwise) and the brush-size indicator.
class CursorManager {
public:
    explicit CursorManager(CanvasView *view);
    // Also lets the selection item take hover and clicks only in cursor mode.
    void set_tool_cursor(const QString &tool_id, bool force = false);
    void show_brush_indicator(const QPointF &pos, double size);
    void update_brush_indicator(const QPointF &pos, double size);
    void hide_brush_indicator();
    std::optional<QCursor> create_tool_cursor_with_size(const QString &tool_id, double brush_size);
    void update_tool_cursor_size(double brush_size);
    void update_tool_cursor_color(const QColor &color);
    void update_tool_cursor_opacity();
    double get_current_brush_size() const;
    // Called when the view zoom changes so cursor sizes match what is drawn.
    void update_view_scale(double scale);
    // Applied to view and viewport; an active override cursor is updated too.
    void apply_cursor(const QCursor &cursor);

    CanvasView *view = nullptr;
    QGraphicsScene *scene = nullptr;
    QGraphicsEllipseItem *brush_indicator = nullptr;
    bool indicator_visible = false;
    QString current_tool_id;
    std::optional<double> current_brush_size;
    std::optional<QCursor> current_cursor;
private:
    ToolController *tool_controller() const;
    QCursor create_crosshair_cursor(double brush_size, const QColor &color) const;
    QCursor create_number_cursor(double brush_size) const;
    double view_scale_ = 1.0;
};
}
