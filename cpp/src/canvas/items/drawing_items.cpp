#include "canvas/items/drawing_items.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/text_item.h"
#include "core/constants.h"
#include "canvas/scene.h"
#include "canvas/undo.h"
#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPolygonF>
#include <QStyleOptionGraphicsItem>
#include <QTextCursor>
#include <QTextDocument>
#include <QFocusEvent>
#include <QCursor>
#include <QtMath>
#include <algorithm>
#include <cmath>

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
QGraphicsItem *annotation_owner(QGraphicsItem *item) {
    while (item && item->parentItem()) item = item->parentItem();
    return item;
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
    : DrawingItemMixin(this), number(value), radius(r), color(c), style(normalize_style(s)), label_font_family_(get_default_text_font_for_language()) {
    setPos(pos);
    setZValue(20);
}
QRectF NumberItem::visualRect() const { return {-radius, -radius, radius * 2, radius * 2}; }
QRectF NumberItem::boundingRect() const {
    auto bounds = visualRect();
    if (const auto pointer = pointer_path(); !pointer.isEmpty()) bounds = bounds.united(pointer.boundingRect());
    return bounds.adjusted(-CLICK_MARGIN, -CLICK_MARGIN, CLICK_MARGIN, CLICK_MARGIN);
}
QPainterPath NumberItem::shape() const {
    QPainterPath path;
    path.addEllipse(visualRect().adjusted(-CLICK_MARGIN, -CLICK_MARGIN, CLICK_MARGIN, CLICK_MARGIN));
    if (const auto pointer = pointer_path(); !pointer.isEmpty())
        path = path.united(pointer).united(stroke_shape(pointer, CLICK_MARGIN * 2, Qt::RoundCap, Qt::RoundJoin));
    return path;
}
QRectF NumberItem::sceneVisualRect() const { return mapToScene(visualRect()).boundingRect(); }
QString NumberItem::normalize_style(const QString &s) { return s == "solid" || s == "hollow_bg" || s == "hollow_all" || s == "no_circle" ? s : QString("solid"); }
void NumberItem::set_style(const QString &s) { const auto normalized = normalize_style(s); if (normalized != style) { style = normalized; update(); } }
qreal NumberItem::ring_width() const { return std::max(2., radius * RING_WIDTH_RATIO); }
QFont NumberItem::number_font() const { QFont font("Arial", std::max(MIN_FONT_SIZE, static_cast<int>(radius * FONT_SCALE))); font.setBold(true); return font; }
QColor NumberItem::solid_text_color() const { return color.red() * 3 + color.green() * 6 + color.blue() > 1280 ? QColor(Qt::black) : QColor(Qt::white); }
void NumberItem::paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) {
    p->setRenderHint(QPainter::Antialiasing); const auto rect = visualRect();
    // The colour can be set straight on the field, so the label, guide and pointer follow it here.
    if (label_ && label_->defaultTextColor() != color) label_->setDefaultTextColor(color);
    if (guide_ && guide_->pen().color() != color) update_guide();
    paint_pointer(p); paint_circle(p, rect, color); paint_number(p, rect, color); paint_selection_frame(p, rect);
}
void NumberItem::paint_pointer(QPainter *p) {
    auto path = pointer_path();
    if (path.isEmpty()) return;
    // Outlined and bare styles keep the inside of the circle clear.
    if (style != STYLE_SOLID) {
        const qreal inner = style == STYLE_NO_CIRCLE ? radius : radius - ring_width() / 2;
        QPainterPath disk; disk.addEllipse(QPointF(), inner, inner);
        path = path.subtracted(disk);
    }
    p->setPen(Qt::NoPen); p->setBrush(color); p->drawPath(path);
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
void NumberItem::set_stroke_width(qreal width) { set_radius(width * RADIUS_SCALE); }
bool NumberItem::scale_stroke_width(qreal scale) { set_radius(radius * scale); return true; }
void NumberItem::set_radius(qreal r) {
    prepareGeometryChange();
    const qreal factor = std::max(4., r) / radius;
    radius = std::max(4., r);
    invalidate_pointer();
    // The label grows with the circle; the pointer's tip stays on what it points at.
    if (label_) { label_->setFont(label_font()); label_->set_anchor(label_->anchor() * factor); }
    update_guide();
    update();
}
void NumberItem::invalidate_pointer() { pointer_cache_.reset(); }
bool NumberItem::set_visual_opacity(qreal opacity) { color.setAlphaF(opacity_value(opacity)); setOpacity(1.); update(); return true; }
std::optional<qreal> NumberItem::get_stroke_width() const { return radius / RADIUS_SCALE; }
std::optional<qreal> NumberItem::get_visual_opacity() const { const auto direct = opacity_value(this->opacity()); return direct < .999 ? direct : color.alphaF(); }

void NumberItem::set_arrow_tip(const std::optional<QPointF> &tip) {
    if (tip == arrow_tip_) return;
    prepareGeometryChange();
    arrow_tip_ = tip;
    invalidate_pointer();
    update();
}
void NumberItem::toggle_arrow() {
    if (arrow_tip_) {
        parked_arrow_tip_ = arrow_tip_;
        set_arrow_tip(std::nullopt);
        return;
    }
    if (parked_arrow_tip_) {
        set_arrow_tip(parked_arrow_tip_);
        return;
    }
    const bool label_on_right = label_ && label_->isVisible() && label_->anchor().x() > 0;
    set_arrow_tip(QPointF((label_on_right ? -1 : 1) * radius * ARROW_DEFAULT_LENGTH, 0));
}
QPainterPath NumberItem::pointer_path() const {
    if (pointer_cache_) return *pointer_cache_;
    QPainterPath path;
    if (!arrow_tip_) return *(pointer_cache_ = path);
    const auto tip = *arrow_tip_;
    const qreal length = std::hypot(tip.x(), tip.y());
    if (length <= radius) return *(pointer_cache_ = path);
    const QPointF along(tip.x() / length, tip.y() / length);
    if (length <= radius * ARROW_TAIL_REACH) {
        // A bubble tail from two points of the circle.
        const qreal cos_spread = std::cos(qDegreesToRadians(30.)), sin_spread = std::sin(qDegreesToRadians(30.));
        const auto side = [&](qreal sign) {
            return QPointF(along.x() * cos_spread - along.y() * sin_spread * sign, along.x() * sin_spread * sign + along.y() * cos_spread) * radius;
        };
        path.addPolygon(QPolygonF({side(1), tip, side(-1)}));
        path.closeSubpath();
        return *(pointer_cache_ = path);
    }
    // Further out it is the arrow tool's arrow, starting under the circle so no seam shows at its edge.
    const ArrowItem arrow(along * (radius * .7), tip, QPen(color, std::max(2., radius * ARROW_WIDTH_RATIO)), arrow_style_);
    return *(pointer_cache_ = arrow.path());
}

QString NumberItem::label_text() const { return label_ && label_->isVisible() ? label_->toPlainText() : QString(); }
QFont NumberItem::label_font() const {
    QFont font(label_font_family_);
    font.setPixelSize(std::max(8, qRound(radius * LABEL_SIZE_LEVELS[static_cast<size_t>(label_size_level_)])));
    return font;
}
void NumberItem::restyle_label() {
    if (label_) {
        label_->setFont(label_font());
        label_->update();
    }
    update_guide();
}
void NumberItem::update_guide() {
    const bool shown = label_guide_ && label_ && label_->isVisible() && !label_->toPlainText().isEmpty();
    if (!shown) {
        if (guide_) guide_->hide();
        return;
    }
    if (!guide_) {
        guide_ = new QGraphicsPathItem(this);
        // Under the circle, which hides where it starts, and under the label.
        guide_->setFlag(ItemStacksBehindParent);
        guide_->setAcceptedMouseButtons(Qt::NoButton);
    }
    const qreal margin = label_->document()->documentMargin();
    const auto text = label_->mapRectToParent(label_->boundingRect().adjusted(margin, margin, -margin, -margin));
    const auto edge = [&](const QPointF &toward) {
        const qreal length = std::hypot(toward.x(), toward.y());
        return length > 1e-6 ? toward * (radius / length) : QPointF(radius, 0);
    };
    QPainterPath path;
    if (text.left() >= 0 || text.right() <= 0) {
        // Beside the circle: to the near end of the text's bottom edge, then under the text.
        const bool right = text.left() >= 0;
        const auto near_end = right ? text.bottomLeft() : text.bottomRight(), far_end = right ? text.bottomRight() : text.bottomLeft();
        path.moveTo(edge(near_end));
        path.lineTo(near_end);
        path.lineTo(far_end);
    } else {
        // Above or below: straight to the nearest point of the facing edge; text above is also underlined.
        const bool below = text.top() >= 0;
        const QPointF target(std::clamp(0., text.left(), text.right()), below ? text.top() : text.bottom());
        path.moveTo(edge(target));
        path.lineTo(target);
        if (!below) {
            path.moveTo(text.bottomLeft());
            path.lineTo(text.bottomRight());
        }
    }
    QPen pen(color, std::max(1.5, radius * GUIDE_WIDTH_RATIO));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    guide_->setPen(pen);
    guide_->setPath(path);
    guide_->show();
}
QPointF NumberItem::default_label_anchor() const {
    const qreal x = radius * (1 + LABEL_GAP);
    // Off the line of a pointer that already goes to the right, and clear of the corner buttons.
    if (!arrow_tip_ || arrow_tip_->x() <= 0 || pointer_path().isEmpty()) return {x, 0};
    return {x, (arrow_tip_->y() > 0 ? -1.5 : 1.5) * radius};
}
void NumberItem::set_label(const QString &text, const std::optional<QPointF> &anchor) {
    const bool blank = text.trimmed().isEmpty();
    if (blank && !label_) return;
    if (!label_) {
        label_ = new NumberLabel(this);
        label_->set_anchor(default_label_anchor());
    }
    if (anchor) label_->set_anchor(*anchor);
    const auto shown = blank ? QString() : text;
    if (label_->toPlainText() != shown) label_->setPlainText(shown);
    // A label being typed in stays, empty or not, until typing ends.
    if (!label_->is_editing()) label_->setVisible(!blank);
    update_guide();
}
NumberLabel *NumberItem::begin_label_edit() {
    if (!label_) label_ = new NumberLabel(this);
    // A label with nothing in it starts over at the default place.
    if (label_text().isEmpty() && !label_->is_editing()) {
        label_->setPlainText(QString());
        label_->set_anchor(default_label_anchor());
        label_->show();
    }
    label_->begin_edit();
    return label_;
}
void NumberItem::finish_label_edit(const QVariantMap &before) {
    if (!label_) return;
    if (label_->toPlainText().trimmed().isEmpty()) {
        label_->setPlainText(QString());
        label_->hide();
    }
    update_guide();
    const auto after = label_state();
    if (after == before) return;
    auto *context = dynamic_cast<TextUndoContext *>(scene());
    if (auto *stack = context ? context->text_undo_stack() : nullptr) stack->push(new EditItemCommand(ItemReference(this), before, after, "Edit Number Label"));
}
QVariantMap NumberItem::label_state() const {
    QVariantMap state{{"label_text", label_text()}};
    if (label_) state["label_anchor"] = label_->anchor();
    return state;
}
QVariantMap NumberItem::extras_state() const {
    auto state = label_state();
    state["arrow_tip"] = arrow_tip_ ? QVariant(*arrow_tip_) : QVariant();
    state["label_font"] = label_font_family_;
    state["label_size"] = label_size_level_;
    state["label_outline"] = label_outline_;
    state["label_guide"] = label_guide_;
    state["arrow_style"] = arrow_style_;
    return state;
}
void NumberItem::apply_extras_state(const QVariantMap &state) {
    const auto point = [](const QVariant &value) {
        return value.metaType().id() == QMetaType::QPointF ? std::optional<QPointF>(value.toPointF()) : std::nullopt;
    };
    if (state.contains("arrow_tip")) set_arrow_tip(point(state.value("arrow_tip")));
    bool restyle = false;
    if (const auto family = state.value("label_font").toString(); !family.isEmpty() && family != label_font_family_) {
        label_font_family_ = family;
        restyle = true;
    }
    if (state.contains("label_size")) {
        const int level = std::clamp(state.value("label_size").toInt(), 0, static_cast<int>(LABEL_SIZE_LEVELS.size()) - 1);
        restyle = restyle || level != label_size_level_;
        label_size_level_ = level;
    }
    if (state.contains("label_outline")) {
        restyle = restyle || state.value("label_outline").toBool() != label_outline_;
        label_outline_ = state.value("label_outline").toBool();
    }
    if (state.contains("label_guide")) {
        restyle = restyle || state.value("label_guide").toBool() != label_guide_;
        label_guide_ = state.value("label_guide").toBool();
    }
    if (const auto style = state.value("arrow_style").toString(); ARROW_STYLES.contains(style) && style != arrow_style_) {
        prepareGeometryChange();
        arrow_style_ = style;
        invalidate_pointer();
        update();
    }
    // Style first: a label shown below is laid out in the font it keeps.
    if (restyle) restyle_label();
    if (state.contains("label_text")) set_label(state.value("label_text").toString(), point(state.value("label_anchor")));
}
void NumberItem::copy_extras_from(const NumberItem &other) {
    parked_arrow_tip_ = other.parked_arrow_tip_;
    apply_extras_state(other.extras_state());
}

NumberLabel::NumberLabel(NumberItem *number) : QGraphicsTextItem(number), number_(number) {
    setFont(number->label_font());
    setDefaultTextColor(number->color);
    setAcceptHoverEvents(true);
    QObject::connect(document(), &QTextDocument::contentsChanged, this, [this] { place(); });
}
void NumberLabel::set_anchor(const QPointF &anchor) {
    anchor_ = anchor;
    place();
}
void NumberLabel::place() {
    setPos(anchor_.x(), anchor_.y() - QGraphicsTextItem::boundingRect().height() / 2);
    number_->update_guide();
}
void NumberLabel::begin_edit() {
    setTextInteractionFlags(Qt::TextEditorInteraction);
    setFocus(Qt::MouseFocusReason);
    auto cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
    update_cursor();
    update();
}
void NumberLabel::update_cursor() {
    if (is_editing()) setCursor(Qt::IBeamCursor);
    else if (number_->hover_allowed()) setCursor(Qt::SizeAllCursor);
    else unsetCursor();
}
void NumberLabel::paint(QPainter *p, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    auto cleaned = *option;
    cleaned.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus);
    if (number_->label_outline()) {
        // Stroked under the glyphs, the same way as text items.
        p->setRenderHint(QPainter::Antialiasing);
        QPen pen(number_->label_outline_color(), 2 * NumberItem::LABEL_OUTLINE_RATIO * font().pixelSize());
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p->strokePath(text_glyph_path(*this), pen);
    }
    QGraphicsTextItem::paint(p, &cleaned, widget);
    if (!is_editing()) return;
    QPen pen(QColor(0, 180, 255, 230), 1, Qt::DashLine);
    pen.setCosmetic(true);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    p->drawRect(boundingRect());
}
void NumberLabel::focusInEvent(QFocusEvent *event) {
    // Back from another window the same edit goes on, undoing to where it started.
    if (!window_away_) state_before_edit_ = number_->label_state();
    window_away_ = false;
    QGraphicsTextItem::focusInEvent(event);
}
void NumberLabel::focusOutEvent(QFocusEvent *event) {
    QGraphicsTextItem::focusOutEvent(event);
    if (event->reason() == Qt::ActiveWindowFocusReason) {
        window_away_ = true;
        return;
    }
    window_away_ = false;
    auto cursor = textCursor();
    cursor.clearSelection();
    setTextCursor(cursor);
    setTextInteractionFlags(Qt::NoTextInteraction);
    update_cursor();
    number_->finish_label_edit(state_before_edit_);
}
void NumberLabel::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    if (is_editing()) { QGraphicsTextItem::mousePressEvent(event); return; }
    if (event->button() != Qt::LeftButton) { event->ignore(); return; }
    press_pos_ = event->scenePos();
    press_anchor_ = anchor_;
    dragged_ = false;
    event->accept();
}
void NumberLabel::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (is_editing() || !press_pos_) { QGraphicsTextItem::mouseMoveEvent(event); return; }
    if (!dragged_ && (event->screenPos() - event->buttonDownScreenPos(Qt::LeftButton)).manhattanLength() < QApplication::startDragDistance()) return;
    // Dragging the label moves it alone; the number stays.
    dragged_ = true;
    set_anchor(*press_anchor_ + number_->mapFromScene(event->scenePos()) - number_->mapFromScene(*press_pos_));
}
void NumberLabel::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    if (is_editing() || !press_pos_) { QGraphicsTextItem::mouseReleaseEvent(event); return; }
    const bool clicked = !dragged_;
    press_pos_.reset();
    press_anchor_.reset();
    dragged_ = false;
    if (clicked) begin_edit();
}
void NumberLabel::hoverEnterEvent(QGraphicsSceneHoverEvent *event) {
    number_->set_hovered(true);
    update_cursor();
    if (is_editing()) QGraphicsTextItem::hoverEnterEvent(event);
}
void NumberLabel::hoverMoveEvent(QGraphicsSceneHoverEvent *event) {
    number_->set_hovered(true);
    update_cursor();
    if (is_editing()) QGraphicsTextItem::hoverMoveEvent(event);
}
void NumberLabel::hoverLeaveEvent(QGraphicsSceneHoverEvent *event) {
    number_->set_hovered(false);
    if (is_editing()) QGraphicsTextItem::hoverLeaveEvent(event);
}
}
