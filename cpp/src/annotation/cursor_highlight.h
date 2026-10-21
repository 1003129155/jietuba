#pragma once
#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QTimer>
#include <optional>
#include <vector>

class QGraphicsEllipseItem;
class QGraphicsScene;

namespace jietuba {
class AnnotationView;
class InputHub;
class RippleItem;

// What one layer shows for the pointer: a soft disc around it, smaller while a button is held, and a
// ring that spreads from every click, yellow for the left button and blue for the right. The disc and
// each ring are items of their own, so a repaint stays as small as they are. None of them is an
// annotation (see TRANSIENT_ITEM_DATA).
class PointerLight {
public:
    static constexpr qreal DISC_RADIUS = 26;
    static constexpr qreal PRESSED_SCALE = 0.7;
    explicit PointerLight(QGraphicsScene *scene);
    ~PointerLight();
    PointerLight(const PointerLight &) = delete;
    PointerLight &operator=(const PointerLight &) = delete;
    // The pointer in scene coordinates, or nullopt while it is on another screen.
    void set_pointer(const std::optional<QPointF> &pos, bool pressed);
    // The disc alone, where it is.
    void set_pressed(bool pressed);
    void add_ripple(const QPointF &pos, bool right_button, qint64 now);
    // The disc and the rings as a share of their default size.
    void set_size(qreal factor);
    qreal disc_radius() const;
    // Steps the rings and drops the finished ones.
    void advance(qint64 now);
    int ripple_count() const { return static_cast<int>(ripples_.size()); }
    bool disc_visible() const;
    QPointF disc_position() const;
    bool disc_pressed() const { return pressed_; }
    QGraphicsScene *scene() const { return scene_; }
private:
    QGraphicsScene *scene_;
    QGraphicsEllipseItem *disc_;
    std::vector<RippleItem *> ripples_;
    bool pressed_ = false;
    qreal size_ = 1.0;
};

// Shows the light on the layers from the input hub's mouse events: the position and the buttons come
// from the system hook, so it works while the layers pass the mouse through, nothing runs while the
// pointer rests, and a click shorter than a frame is not missed. A timer runs only while a ring spreads.
// A hook installed after ours can keep the events from it (a live wallpaper does while the desktop has
// the focus), so the system's cursor location events move the light as well.
class CursorHighlight : public QObject {
    Q_OBJECT
public:
    explicit CursorHighlight(QObject *parent = nullptr);
    ~CursorHighlight() override;
    // The layers that show the light; each gets its own PointerLight while enabled.
    void set_views(const std::vector<AnnotationView *> &views);
    // Starts or ends listening to the hub and takes the light off the layers when it ends. Starting reads
    // the size and whether clicks leave rings (screen_annotation_highlight_size, _click_ripples).
    void set_enabled(bool enabled);
    // small, medium or large as a share of the default size.
    static qreal size_factor(const QString &name);
    bool enabled() const { return enabled_; }
    // The hub to listen to: the process's own hub by default. Tests give theirs.
    void set_hub(InputHub *hub) { hub_ = hub; }
    // What the hub's events do; tests call them with their own values.
    void on_pointer(const QPoint &global);
    void on_button(const QString &button, bool pressed, const QPoint &global);
    static constexpr const char *WATCHER = "cursor_highlight";
    // A cursor location event: the pointer is read once for all the events of one pass of the loop.
    void on_cursor_moved();
private:
    void collect_pointer();
    void show_at(const QPoint &global);
    void animate();
    InputHub *hub() const;
    std::vector<AnnotationView *> views_;
    InputHub *hub_ = nullptr;
    bool enabled_ = false;
    QMetaObject::Connection moved_connection_, button_connection_;
    void *cursor_events_ = nullptr;
    bool cursor_read_pending_ = false;
    qreal size_ = 1.0;
    bool ripples_ = true;
    QTimer ripple_timer_;
    QElapsedTimer clock_;
    // The last position and the buttons held, as the events told.
    QPoint last_pos_;
    unsigned buttons_down_ = 0;
};
}
