#include "canvas/items/selection_item.h"
#include "canvas/selection_model.h"
#include "core/logger.h"
#include "core/theme.h"
#include <QCursor>
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsTextItem>
#include <QPainter>
#include <algorithm>

namespace jietuba {
namespace {
bool is_corner(int handle) {
    return handle == SelectionItem::HANDLE_TOP_LEFT || handle == SelectionItem::HANDLE_TOP_RIGHT
        || handle == SelectionItem::HANDLE_BOTTOM_LEFT || handle == SelectionItem::HANDLE_BOTTOM_RIGHT;
}
}

SelectionItem::SelectionItem(SelectionModel *model) : TrackedItem(this), model_(model) {
    setZValue(15);
    // Geometry only; the overlay listens to the model for decoration repaints.
    rect_connection_ = QObject::connect(model_, &SelectionModel::rectChanged, [this](QRectF) { update_bounds(); });
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    // Hit area only: with this flag Qt never calls paint(), so the decoration
    // structurally cannot end up in scene.render().
    setFlag(ItemHasNoContents, true);
    log_debug(T("选区框创建"), "Canvas");
}

SelectionItem::~SelectionItem() { QObject::disconnect(rect_connection_); }

int SelectionItem::border_width() const { return get_theme().selection_border_width(); }
int SelectionItem::handle_size() const { return get_theme().selection_handle_diameter(); }
int SelectionItem::handle_ring_width() const { return get_theme().selection_handle_ring_width(); }

QRectF SelectionItem::boundingRect() const {
    if (model_->is_empty()) return {};
    return model_->rect().adjusted(-20, -20, 20, 20);
}

void SelectionItem::request_repaint() {
    if (repaint_requested) repaint_requested();
}

QRectF SelectionItem::visual_bounds() const {
    if (model_->is_empty()) return {};
    // The border spills half its width; handles sit on the edge with their ring.
    const qreal pad = std::max(border_width() / 2.0, handle_size() / 2.0 + 1 + handle_ring_width());
    return model_->rect().adjusted(-pad, -pad, pad, pad);
}

void SelectionItem::render(QPainter *painter) {
    if (render_override && render_override(painter)) return;
    if (model_->is_empty()) return;
    const auto rect = model_->rect();
    // No antialiasing for the axis-aligned border: fractional rects (tweening,
    // locked ratio) would blur a hard edge into transition pixels.
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setPen(QPen(get_theme().theme_color(), border_width(), Qt::SolidLine));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(rect);
    if (handles_visible()) draw_handles(painter, rect);
}

bool SelectionItem::handles_visible() const {
    if (model_->is_empty()) return false;
    if (model_->is_dragging() || !model_->is_confirmed()) return false;
    const auto style = get_theme().selection_handle_style();
    if (style == ThemeManager::HANDLES_NONE) return false;
    // Per side: all handles take two diameters (two half corners plus a middle), corners only one.
    const int occupied = handle_visual_diameter() * (style == ThemeManager::HANDLES_ALL ? 2 : 1);
    const auto rect = model_->rect();
    return occupied <= std::min(rect.width(), rect.height()) * HANDLE_CROWDING_RATIO;
}

void SelectionItem::draw_handles(QPainter *painter, const QRectF &rect, bool skip_corners) {
    const auto style = get_theme().selection_handle_style();
    if (style == ThemeManager::HANDLES_NONE) return;
    painter->setRenderHint(QPainter::Antialiasing, true);
    const auto handles = handle_positions(rect);
    const int outer_r = handle_size() / 2 + 1;
    const int inner_r = handle_size() / 2;
    const QBrush brush_handle(get_theme().theme_color());
    const QPen pen_handle_outer(QColor(255, 255, 255), handle_ring_width());
    for (auto it = handles.begin(); it != handles.end(); ++it) {
        if (is_corner(it.key())) {
            if (skip_corners) continue;
        } else if (style == ThemeManager::HANDLES_CORNERS) {
            continue;
        }
        painter->setPen(pen_handle_outer);
        painter->setBrush(brush_handle);
        painter->drawEllipse(it.value(), outer_r, outer_r);
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(it.value(), inner_r, inner_r);
    }
}

QMap<int, QPointF> SelectionItem::handle_positions(const QRectF &rect) {
    const qreal left = rect.left(), right = rect.right(), top = rect.top(), bottom = rect.bottom();
    const qreal cx = rect.center().x(), cy = rect.center().y();
    return {
        {HANDLE_TOP_LEFT, {left, top}}, {HANDLE_TOP, {cx, top}}, {HANDLE_TOP_RIGHT, {right, top}},
        {HANDLE_LEFT, {left, cy}}, {HANDLE_RIGHT, {right, cy}},
        {HANDLE_BOTTOM_LEFT, {left, bottom}}, {HANDLE_BOTTOM, {cx, bottom}}, {HANDLE_BOTTOM_RIGHT, {right, bottom}},
    };
}

void SelectionItem::update_bounds() {
    // boundingRect is the hit area; the scene index must learn it changed.
    prepareGeometryChange();
}

void SelectionItem::hoverMoveEvent(QGraphicsSceneHoverEvent *event) {
    if (!acceptHoverEvents()) { event->ignore(); return; }
    if (model_->is_empty()) { QGraphicsItem::hoverMoveEvent(event); return; }
    // Text items handle their own editing and get no resize cursors.
    if (should_delegate_to_text(event->scenePos())) {
        unsetCursor();
        event->ignore();
        QGraphicsItem::hoverMoveEvent(event);
        return;
    }
    if (!model_->is_confirmed()) {
        // Smart-selection preview uses the cross cursor throughout.
        setCursor(QCursor(Qt::CrossCursor));
        QGraphicsItem::hoverMoveEvent(event);
        return;
    }
    const int handle = hit_test(event->pos());
    Qt::CursorShape cursor = Qt::ArrowCursor;
    if (handle == HANDLE_TOP_LEFT || handle == HANDLE_BOTTOM_RIGHT) cursor = Qt::SizeFDiagCursor;
    else if (handle == HANDLE_TOP_RIGHT || handle == HANDLE_BOTTOM_LEFT) cursor = Qt::SizeBDiagCursor;
    else if (handle == HANDLE_TOP || handle == HANDLE_BOTTOM) cursor = Qt::SizeVerCursor;
    else if (handle == HANDLE_LEFT || handle == HANDLE_RIGHT) cursor = Qt::SizeHorCursor;
    else if (handle == HANDLE_BODY) cursor = Qt::SizeAllCursor;
    setCursor(QCursor(cursor));
    QGraphicsItem::hoverMoveEvent(event);
}

void SelectionItem::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    if (event->button() != Qt::LeftButton) { event->ignore(); return; }
    if (should_delegate_to_text(event->scenePos())) { event->ignore(); return; }
    active_handle = hit_test(event->pos());
    if (active_handle != HANDLE_NONE) {
        start_pos = event->scenePos();
        start_rect = model_->rect();
        // Lets the toolbar hide while dragging.
        model_->start_dragging();
        event->accept();
    } else {
        event->ignore();
    }
}

void SelectionItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (active_handle == HANDLE_NONE) return;
    const auto current = event->scenePos();
    const qreal dx = current.x() - start_pos.x(), dy = current.y() - start_pos.y();
    QRectF new_rect(start_rect);
    if (active_handle == HANDLE_BODY) {
        new_rect.translate(dx, dy);
    } else {
        if (active_handle == HANDLE_LEFT || active_handle == HANDLE_TOP_LEFT || active_handle == HANDLE_BOTTOM_LEFT)
            new_rect.setLeft(start_rect.left() + dx);
        if (active_handle == HANDLE_RIGHT || active_handle == HANDLE_TOP_RIGHT || active_handle == HANDLE_BOTTOM_RIGHT)
            new_rect.setRight(start_rect.right() + dx);
        if (active_handle == HANDLE_TOP || active_handle == HANDLE_TOP_LEFT || active_handle == HANDLE_TOP_RIGHT)
            new_rect.setTop(start_rect.top() + dy);
        if (active_handle == HANDLE_BOTTOM || active_handle == HANDLE_BOTTOM_LEFT || active_handle == HANDLE_BOTTOM_RIGHT)
            new_rect.setBottom(start_rect.bottom() + dy);
        if (resize_constraint) new_rect = resize_constraint(new_rect, start_rect, active_handle);
    }
    auto normalized = new_rect.normalized();
    model_->set_rect(normalized);
    event->accept();
}

void SelectionItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    if (active_handle != HANDLE_NONE) model_->stop_dragging();
    active_handle = HANDLE_NONE;
    event->accept();
}

int SelectionItem::hit_test(const QPointF &pos) const {
    const auto rect = model_->rect();
    const auto handles = handle_positions(rect);
    for (auto it = handles.begin(); it != handles.end(); ++it)
        if ((pos - it.value()).manhattanLength() < handle_size()) return it.key();
    if (rect.contains(pos)) return HANDLE_BODY;
    return HANDLE_NONE;
}

bool SelectionItem::should_delegate_to_text(const QPointF &scene_pos) const {
    auto *s = scene();
    if (!s) return false;
    for (auto *item : s->items(scene_pos)) {
        if (item == this || !item->isVisible()) continue;
        // TextItem derives from QGraphicsTextItem.
        if (dynamic_cast<QGraphicsTextItem *>(item)) return true;
    }
    return false;
}
}
