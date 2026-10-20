#include "canvas/items/drawing_items.h"
#include "canvas/scene.h"
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QCursor>
#include <algorithm>

namespace jietuba {
namespace {
// Ordered max/min preserve Python's treatment of NaN as the second argument.
qreal opacity_value(qreal value) { return std::max(0., std::min(1., value)); }
QPainterPath stroke_shape(const QPainterPath &path, qreal width, Qt::PenCapStyle cap, Qt::PenJoinStyle join) {
    QPainterPathStroker stroker;
    stroker.setWidth(width); stroker.setCapStyle(cap); stroker.setJoinStyle(join);
    return stroker.createStroke(path);
}
template<class T> void set_width(T *item, qreal width) {
    auto pen = item->pen(); pen.setWidthF(std::max(1., width)); item->setPen(pen); item->update();
}
template<class T> void pen_opacity(T *item, qreal opacity) {
    auto pen = item->pen(); auto color = pen.color(); color.setAlphaF(opacity_value(opacity));
    pen.setColor(color); item->setPen(pen); item->setOpacity(1.); item->update();
}
template<class T> qreal visual_opacity(const T *item) {
    const auto direct = opacity_value(item->opacity());
    return direct < .999 ? direct : item->pen().color().alphaF();
}
}
DrawingItemMixin::DrawingItemMixin(QGraphicsItem *item) : TrackedItem(item), drawing_item_(item) {
    item->setFlag(QGraphicsItem::ItemIsMovable, true); item->setAcceptHoverEvents(true);
}
DrawingEditContext *DrawingItemMixin::edit_controller() const {
    auto *scene = drawing_item_->scene();
    if (!scene || scene->views().isEmpty()) return nullptr;
    return dynamic_cast<DrawingEditContext *>(scene->views().front());
}
bool DrawingItemMixin::can_show_hover() const {
    auto *controller = edit_controller();
    if (!controller) return false;
    try { return controller->can_show_hover_cursor(drawing_item_); } catch (...) { return false; }
}
bool DrawingItemMixin::is_edit_target() const {
    auto *controller = edit_controller();
    return controller && controller->selected_drawing_item() == drawing_item_;
}
void DrawingItemMixin::set_hovered(bool hovered) {
    hovered = hovered && can_show_hover();
    if (hovered == hovered_) return;
    hovered_ = hovered; drawing_item_->update();
}
void DrawingItemMixin::update_hover_cursor(QGraphicsSceneHoverEvent *event) {
    if (can_show_hover()) drawing_item_->setCursor(Qt::SizeAllCursor); else drawing_item_->unsetCursor();
    if (event) event->accept();
}
void DrawingItemMixin::hover_enter(QGraphicsSceneHoverEvent *event) { set_hovered(true); update_hover_cursor(event); }
void DrawingItemMixin::hover_move(QGraphicsSceneHoverEvent *event) { set_hovered(true); update_hover_cursor(event); }
void DrawingItemMixin::hover_leave(QGraphicsSceneHoverEvent *event) {
    set_hovered(false); drawing_item_->unsetCursor(); if (event) event->accept();
}
std::optional<Qt::PenStyle> DrawingItemMixin::selection_frame_style() const {
    if (!shows_selection_frame()) return {};
    if (is_edit_target()) return Qt::SolidLine;
    if (hovered_ && can_show_hover()) return Qt::DashLine;
    return {};
}
std::optional<QPen> DrawingItemMixin::selection_frame_pen() const {
    const auto style = selection_frame_style(); if (!style) return {};
    QPen pen(QColor(0, 180, 255, 230), 2, *style); pen.setCosmetic(true); return pen;
}
std::optional<qreal> DrawingItemMixin::get_visual_opacity() const { return opacity_value(drawing_item_->opacity()); }

bool begin_highlighter_paint(QPainter *painter, const QGraphicsItem *item) {
    auto *scene = dynamic_cast<CanvasScene *>(item->scene());
    if (!scene || scene->highlighters_multiply) {
        painter->setCompositionMode(QPainter::CompositionMode_Multiply);
        return false;
    }
    painter->setOpacity(painter->opacity() * TRANSLUCENT_HIGHLIGHTER_ALPHA);
    return true;
}

RectItem::RectItem(const QRectF &rect, const QPen &pen, qreal radius)
    : QGraphicsRectItem(rect), DrawingItemMixin(this), corner_radius_(std::max(0., radius)) {
    setPen(pen); setBrush(Qt::NoBrush); setZValue(20);
}
void RectItem::setPen(const QPen &pen) { QGraphicsRectItem::setPen(pen); shape_cache_.reset(); }
void RectItem::setRect(const QRectF &rect) { QGraphicsRectItem::setRect(rect); shape_cache_.reset(); }
void RectItem::set_corner_radius(qreal radius) { corner_radius_ = std::max(0., radius); shape_cache_.reset(); update(); }
QPainterPath RectItem::shape() const {
    if (shape_cache_) return *shape_cache_;
    QPainterPath path;
    if (corner_radius_ > 0) path.addRoundedRect(rect(), corner_radius_, corner_radius_); else path.addRect(rect());
    shape_cache_ = is_highlighter_rect ? path : stroke_shape(path, pen().widthF() + CLICK_MARGIN * 2, Qt::SquareCap, Qt::MiterJoin);
    return *shape_cache_;
}
QRectF RectItem::boundingRect() const {
    const auto extra = std::max(0., pen().widthF() / 2.) + CLICK_MARGIN;
    return QGraphicsRectItem::boundingRect().adjusted(-extra, -extra, extra, extra);
}
void RectItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    p->setRenderHint(QPainter::Antialiasing);
    auto draw = [&](const QRectF &r, qreal radius) { if (radius > 0) p->drawRoundedRect(r, radius, radius); else p->drawRect(r); };
    const qreal item_opacity = p->opacity();
    if (is_highlighter && begin_highlighter_paint(p, this) && is_highlighter_rect) {
        // Fill and border as one shape: a translucent border over the fill would show as a darker frame.
        const qreal half = pen().widthF() / 2;
        p->setPen(Qt::NoPen); p->setBrush(brush());
        draw(rect().adjusted(-half, -half, half, half), corner_radius_ + half);
        p->setOpacity(item_opacity);
    } else {
        auto pen = this->pen();
        if (pen.style() == Qt::DashLine || pen.style() == Qt::CustomDashLine) { pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin); }
        p->setPen(pen); p->setBrush(brush());
        draw(rect(), corner_radius_);
    }
    if (auto selection = selection_frame_pen()) { p->setBrush(Qt::NoBrush); p->setPen(*selection); draw(rect(), corner_radius_); }
}
void RectItem::set_stroke_width(qreal width) { set_width(this, width); }
bool RectItem::scale_stroke_width(qreal scale) { set_width(this, pen().widthF() * scale); return true; }
bool RectItem::set_visual_opacity(qreal opacity) {
    pen_opacity(this, opacity);
    if (is_highlighter_rect) { auto brush = this->brush(); auto color = brush.color(); color.setAlphaF(opacity_value(opacity)); brush.setColor(color); setBrush(brush); }
    setOpacity(1.); update(); return true;
}
std::optional<qreal> RectItem::get_stroke_width() const { return pen().widthF(); }
std::optional<qreal> RectItem::get_visual_opacity() const { return visual_opacity(this); }

EllipseItem::EllipseItem(const QRectF &rect, const QPen &pen) : QGraphicsEllipseItem(rect), DrawingItemMixin(this) {
    setPen(pen); setBrush(Qt::NoBrush); setZValue(20);
}
void EllipseItem::setPen(const QPen &pen) { QGraphicsEllipseItem::setPen(pen); shape_cache_.reset(); }
void EllipseItem::setRect(const QRectF &rect) { QGraphicsEllipseItem::setRect(rect); shape_cache_.reset(); }
QPainterPath EllipseItem::shape() const {
    if (shape_cache_) return *shape_cache_;
    QPainterPath path; path.addEllipse(rect());
    shape_cache_ = stroke_shape(path, pen().widthF() + CLICK_MARGIN * 2, Qt::RoundCap, Qt::RoundJoin); return *shape_cache_;
}
QRectF EllipseItem::boundingRect() const {
    const auto extra = std::max(0., pen().widthF() / 2.) + CLICK_MARGIN;
    return QGraphicsEllipseItem::boundingRect().adjusted(-extra, -extra, extra, extra);
}
void EllipseItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    p->setRenderHint(QPainter::Antialiasing); p->setPen(pen()); p->setBrush(brush()); p->drawEllipse(rect());
    if (auto selection = selection_frame_pen()) { p->setBrush(Qt::NoBrush); p->setPen(*selection); p->drawEllipse(rect()); }
}
void EllipseItem::set_stroke_width(qreal width) { set_width(this, width); }
bool EllipseItem::scale_stroke_width(qreal scale) { set_width(this, pen().widthF() * scale); return true; }
bool EllipseItem::set_visual_opacity(qreal opacity) { pen_opacity(this, opacity); return true; }
std::optional<qreal> EllipseItem::get_stroke_width() const { return pen().widthF(); }
std::optional<qreal> EllipseItem::get_visual_opacity() const { return visual_opacity(this); }

NumberItem::NumberItem(int value, const QPointF &pos, qreal r, const QColor &c, const QString &s)
    : DrawingItemMixin(this), number(value), radius(r), color(c), style(normalize_style(s)) { setPos(pos); setZValue(20); }
QRectF NumberItem::visualRect() const { return {-radius, -radius, radius * 2, radius * 2}; }
QRectF NumberItem::boundingRect() const { return visualRect().adjusted(-CLICK_MARGIN, -CLICK_MARGIN, CLICK_MARGIN, CLICK_MARGIN); }
QPainterPath NumberItem::shape() const { QPainterPath path; path.addEllipse(boundingRect()); return path; }
QRectF NumberItem::sceneVisualRect() const { return mapToScene(visualRect()).boundingRect(); }
QString NumberItem::normalize_style(const QString &s) { return s == "solid" || s == "hollow_bg" || s == "hollow_all" || s == "no_circle" ? s : QString("solid"); }
void NumberItem::set_style(const QString &s) { const auto normalized = normalize_style(s); if (normalized != style) { style = normalized; update(); } }
qreal NumberItem::ring_width() const { return std::max(2., radius * RING_WIDTH_RATIO); }
QFont NumberItem::number_font() const { QFont font("Arial", std::max(MIN_FONT_SIZE, static_cast<int>(radius * FONT_SCALE))); font.setBold(true); return font; }
QColor NumberItem::solid_text_color() const { return color.red() * 3 + color.green() * 6 + color.blue() > 1280 ? QColor(Qt::black) : QColor(Qt::white); }
void NumberItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    p->setRenderHint(QPainter::Antialiasing); const auto rect = visualRect();
    paint_circle(p, rect, color); paint_number(p, rect, color); paint_selection_frame(p, rect);
}
void NumberItem::paint_circle(QPainter *p, const QRectF &rect, const QColor &c) {
    if (style == "no_circle") return;
    if (!is_hollow()) { p->setPen(Qt::NoPen); p->setBrush(c); p->drawEllipse(rect); return; }
    const auto inset = ring_width() / 2.; QPen pen(c, ring_width()); pen.setJoinStyle(Qt::RoundJoin);
    p->setPen(pen); p->setBrush(Qt::NoBrush); p->drawEllipse(rect.adjusted(inset, inset, -inset, -inset));
}
void NumberItem::paint_number(QPainter *p, const QRectF &rect, const QColor &c) {
    const auto font = number_font(); const auto text = QString::number(number);
    if (style != "hollow_all") { p->setPen(is_hollow() ? c : solid_text_color()); p->setFont(font); p->drawText(rect, Qt::AlignCenter, text); return; }
    QPainterPath path; path.addText(QPointF(0, 0), font, text);
    const auto ink = path.boundingRect(); path.translate(rect.center().x() - ink.center().x(), rect.center().y() - ink.center().y());
    QPen pen(c, std::max(1., radius * GLYPH_OUTLINE_RATIO)); pen.setJoinStyle(Qt::RoundJoin);
    p->setPen(pen); p->setBrush(Qt::NoBrush); p->drawPath(path);
}
void NumberItem::paint_selection_frame(QPainter *p, const QRectF &rect) {
    if (auto selection = selection_frame_pen()) { p->setBrush(Qt::NoBrush); p->setPen(*selection); p->drawRect(rect); }
}
void NumberItem::set_stroke_width(qreal width) { prepareGeometryChange(); radius = std::max(4., width * RADIUS_SCALE); update(); }
bool NumberItem::scale_stroke_width(qreal scale) { prepareGeometryChange(); radius = std::max(4., radius * scale); update(); return true; }
bool NumberItem::set_visual_opacity(qreal opacity) { color.setAlphaF(opacity_value(opacity)); setOpacity(1.); update(); return true; }
std::optional<qreal> NumberItem::get_stroke_width() const { return radius / RADIUS_SCALE; }
std::optional<qreal> NumberItem::get_visual_opacity() const { const auto direct = opacity_value(this->opacity()); return direct < .999 ? direct : color.alphaF(); }
}
