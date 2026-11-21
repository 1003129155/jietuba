#pragma once
#include <QPoint>
#include <QRect>
#include <QString>
#include <functional>
#include <optional>

class QEvent;
class QObject;
class QWidget;

namespace jietuba::clipboard {
// Dragging and eight-edge resizing for a frameless widget. The owner installs itself as
// an event filter and passes events to handle_event before its own handling.
class FramelessMixin {
public:
    // window must outlive this helper; areas where drag_area returns true move the window.
    void init_frameless(QWidget *window, int edge_margin = 8);
    std::function<bool(QObject *widget, const QPoint &local_pos)> is_draggable_area = [](QObject *, const QPoint &) { return false; };
    // The edge under local_pos: "left", "topright", ...; empty when none.
    QString get_edge(const QPoint &local_pos) const;
    void reset();
    // Mouse tracking and this window's event filter on the widget and all its children.
    void setup_mouse_tracking_recursive(QWidget *widget);
    // True when the event was consumed.
    bool handle_event(QObject *obj, QEvent *event);
    bool is_dragging() const { return is_dragging_; }
    const QString &resize_edge() const { return resize_edge_; }

private:
    void update_cursor(const QString &edge);
    void do_resize(const QPoint &global_pos);

    QWidget *window_ = nullptr;
    int edge_margin_ = 8;
    bool is_dragging_ = false;
    std::optional<QPoint> drag_pos_;
    QString resize_edge_;
    std::optional<QPoint> resize_start_pos_;
    std::optional<QRect> resize_start_geometry_;
};
}
