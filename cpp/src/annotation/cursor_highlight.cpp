#include "annotation/cursor_highlight.h"
#include "annotation/annotation_view.h"
#include "core/input_hub.h"
#include "gif/drawing_view.h"
#include "settings/tool_settings.h"
#include <QGraphicsEllipseItem>
#include <QGraphicsItem>
#include <QCursor>
#include <QGraphicsScene>
#include <QPainter>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
constexpr int FRAME_MS = 16;
constexpr qreal RIPPLE_START = 12;
constexpr qreal RIPPLE_END = 52;
constexpr int RIPPLE_MS = 450;
constexpr qreal ITEM_Z = 1e9 - 1;
const QColor LEFT_COLOR(255, 190, 0);
const QColor RIGHT_COLOR(60, 140, 255);

// The enabled highlight; WinEvent callbacks carry no context of their own.
CursorHighlight *g_cursor_listener = nullptr;

void CALLBACK cursor_event(HWINEVENTHOOK, DWORD, HWND, LONG id_object, LONG, DWORD, DWORD) {
    if (id_object == OBJID_CURSOR && g_cursor_listener) g_cursor_listener->on_cursor_moved();
}
}

// A ring that spreads and thins until it is gone.
class RippleItem : public QGraphicsItem {
public:
    RippleItem(const QPointF &pos, bool right, qint64 born, qreal size) : right_(right), born_(born), start_(RIPPLE_START * size), end_(RIPPLE_END * size) {
        setPos(pos);
        setData(TRANSIENT_ITEM_DATA, true);
        setAcceptedMouseButtons(Qt::NoButton);
        setZValue(ITEM_Z);
    }
    // Repaints for this moment; false once the ring is gone.
    bool step(qint64 now) {
        progress_ = std::clamp(static_cast<qreal>(now - born_) / RIPPLE_MS, 0.0, 1.0);
        update();
        return progress_ < 1.0;
    }
    QRectF boundingRect() const override { return QRectF(-end_ - 4, -end_ - 4, 2 * end_ + 8, 2 * end_ + 8); }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        // Quick at first, settling toward the end.
        const qreal eased = 1.0 - (1.0 - progress_) * (1.0 - progress_);
        const qreal radius = start_ + (end_ - start_) * eased;
        QColor color = right_ ? RIGHT_COLOR : LEFT_COLOR;
        color.setAlphaF(0.85 * (1.0 - progress_));
        QPen pen(color, 4.0 - 3.0 * progress_);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(QPointF(0, 0), radius, radius);
    }
private:
    bool right_;
    qint64 born_;
    qreal start_, end_;
    qreal progress_ = 0;
};

PointerLight::PointerLight(QGraphicsScene *scene) : scene_(scene) {
    disc_ = new QGraphicsEllipseItem(-DISC_RADIUS, -DISC_RADIUS, 2 * DISC_RADIUS, 2 * DISC_RADIUS);
    disc_->setPen(Qt::NoPen);
    disc_->setBrush(QColor(255, 210, 0, 95));
    disc_->setData(TRANSIENT_ITEM_DATA, true);
    disc_->setAcceptedMouseButtons(Qt::NoButton);
    disc_->setZValue(ITEM_Z);
    disc_->setVisible(false);
    scene_->addItem(disc_);
}

PointerLight::~PointerLight() {
    for (auto *ripple : ripples_) {
        scene_->removeItem(ripple);
        delete ripple;
    }
    scene_->removeItem(disc_);
    delete disc_;
}

void PointerLight::set_pressed(bool pressed) {
    if (pressed == pressed_) return;
    pressed_ = pressed;
    disc_->setScale(pressed ? PRESSED_SCALE : 1.0);
    disc_->setBrush(QColor(255, 210, 0, pressed ? 150 : 95));
}

void PointerLight::set_pointer(const std::optional<QPointF> &pos, bool pressed) {
    if (!pos) {
        disc_->setVisible(false);
        return;
    }
    set_pressed(pressed);
    disc_->setPos(*pos);
    disc_->setVisible(true);
}

bool PointerLight::disc_visible() const { return disc_->isVisible(); }

QPointF PointerLight::disc_position() const { return disc_->pos(); }

void PointerLight::set_size(qreal factor) {
    if (factor == size_) return;
    size_ = factor;
    const qreal radius = disc_radius();
    disc_->setRect(-radius, -radius, 2 * radius, 2 * radius);
}

qreal PointerLight::disc_radius() const { return DISC_RADIUS * size_; }

void PointerLight::add_ripple(const QPointF &pos, bool right_button, qint64 now) {
    auto *ripple = new RippleItem(pos, right_button, now, size_);
    scene_->addItem(ripple);
    ripples_.push_back(ripple);
}

void PointerLight::advance(qint64 now) {
    ripples_.erase(std::remove_if(ripples_.begin(), ripples_.end(), [&](RippleItem *ripple) {
        if (ripple->step(now)) return false;
        scene_->removeItem(ripple);
        delete ripple;
        return true;
    }), ripples_.end());
}

CursorHighlight::CursorHighlight(QObject *parent) : QObject(parent) {
    ripple_timer_.setInterval(FRAME_MS);
    connect(&ripple_timer_, &QTimer::timeout, this, &CursorHighlight::animate);
    clock_.start();
}

CursorHighlight::~CursorHighlight() { set_enabled(false); }

InputHub *CursorHighlight::hub() const { return hub_ ? hub_ : input_hub(); }

void CursorHighlight::set_views(const std::vector<AnnotationView *> &views) { views_ = views; }

qreal CursorHighlight::size_factor(const QString &name) {
    if (name == "small") return 0.7;
    if (name == "large") return 1.4;
    return 1.0;
}

void CursorHighlight::set_enabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    if (enabled) {
        auto *config = get_tool_settings_manager();
        size_ = size_factor(config->get_app_setting("screen_annotation_highlight_size").toString());
        ripples_ = config->get_app_setting("screen_annotation_click_ripples", true).toBool();
        auto *source = hub();
        // Queued: the events come from the hub's event thread.
        moved_connection_ = connect(source, &InputHub::pointer_moved, this, [this](const QString &watcher) {
            if (watcher == WATCHER) collect_pointer();
        }, Qt::QueuedConnection);
        button_connection_ = connect(source, &InputHub::mouse_button, this, [this](const QString &watcher, const QString &button, bool pressed, int x, int y) {
            if (watcher == WATCHER) on_button(button, pressed, QPoint(x, y));
        }, Qt::QueuedConnection);
        buttons_down_ = 0;
        source->native->watch_mouse(WATCHER);
        g_cursor_listener = this;
        cursor_events_ = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, cursor_event, 0, 0, WINEVENT_OUTOFCONTEXT);
        // Where the pointer is now, before it first moves.
        on_pointer(QCursor::pos());
        return;
    }
    disconnect(moved_connection_);
    disconnect(button_connection_);
    if (cursor_events_) UnhookWinEvent(static_cast<HWINEVENTHOOK>(cursor_events_));
    cursor_events_ = nullptr;
    if (g_cursor_listener == this) g_cursor_listener = nullptr;
    if (auto *source = hub_ ? hub_ : existing_input_hub()) source->native->unwatch_mouse(WATCHER);
    ripple_timer_.stop();
    for (auto *view : views_) view->remove_pointer_light();
}

void CursorHighlight::collect_pointer() {
    if (!enabled_) return;
    if (const auto position = hub()->native->take_pointer(WATCHER)) on_pointer(*position);
}

void CursorHighlight::show_at(const QPoint &global) {
    last_pos_ = global;
    const bool pressed = buttons_down_ != 0;
    for (auto *view : views_) {
        auto *light = view->pointer_light(true);
        light->set_size(size_);
        const QRect screen = view->geometry();
        const bool inside = screen.contains(global);
        // While a layer takes the mouse it moves the disc itself, in the repaint of the stroke.
        if (inside && !view->passthrough()) {
            light->set_pressed(pressed);
            continue;
        }
        light->set_pointer(inside ? std::optional<QPointF>(QPointF(global - screen.topLeft())) : std::nullopt, pressed);
    }
}

void CursorHighlight::on_pointer(const QPoint &global) { show_at(global); }

void CursorHighlight::on_cursor_moved() {
    if (cursor_read_pending_) return;
    cursor_read_pending_ = true;
    QMetaObject::invokeMethod(this, [this] {
        cursor_read_pending_ = false;
        POINT pos{};
        // The same physical coordinates as the hook's.
        if (enabled_ && GetCursorPos(&pos) && QPoint(pos.x, pos.y) != last_pos_) on_pointer(QPoint(pos.x, pos.y));
    }, Qt::QueuedConnection);
}

void CursorHighlight::on_button(const QString &button, bool pressed, const QPoint &global) {
    const bool left = button == "left", right = button == "right";
    const unsigned bit = left ? 1u : right ? 2u : button == "middle" ? 4u : 0u;
    if (pressed) buttons_down_ |= bit;
    else buttons_down_ &= ~bit;
    show_at(global);
    if (!pressed || !(left || right) || !ripples_) return;
    const qint64 now = clock_.elapsed();
    for (auto *view : views_) {
        const QRect screen = view->geometry();
        if (screen.contains(global)) view->pointer_light(true)->add_ripple(QPointF(global - screen.topLeft()), right, now);
    }
    if (!ripple_timer_.isActive()) ripple_timer_.start();
}

void CursorHighlight::animate() {
    const qint64 now = clock_.elapsed();
    bool any = false;
    for (auto *view : views_) {
        auto *light = view->pointer_light(false);
        if (!light || light->ripple_count() == 0) continue;
        light->advance(now);
        any = any || light->ripple_count() > 0;
    }
    if (!any) ripple_timer_.stop();
}
}
