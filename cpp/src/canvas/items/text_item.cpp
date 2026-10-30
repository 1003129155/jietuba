#include "canvas/items/text_item.h"
#include "canvas/undo.h"
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QGlyphRun>
#include <QRawFont>
#include <QTextCursor>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QClipboard>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMimeData>
#include <algorithm>
#include <cmath>

namespace jietuba {
namespace {
bool python_blank(const QString &value) {
    // Python str.strip additionally considers U+001C..U+001F whitespace.
    for (auto ch : value) if (!ch.isSpace() && !(ch.unicode() >= 0x1c && ch.unicode() <= 0x1f)) return false;
    return true;
}
}
TextItem::TextItem(const QString &text, const QPointF &pos, const QFont &font, const QColor &color, bool on_top, bool provisional)
    : QGraphicsTextItem(text), DrawingItemMixin(this), _provisional(provisional), _text_before_edit(text) {
    setTextWidth(-1); setPos(pos); setFont(font); setDefaultTextColor(color);
    setTextInteractionFlags(Qt::TextEditorInteraction); setZValue(on_top ? ANNOTATION_Z_VALUE : NORMAL_ANNOTATION_Z_VALUE);
    document()->setDocumentMargin(TEXT_PADDING);
}
qreal TextItem::font_point_size() const { auto size = font().pointSizeF(); if (size <= 0) size = font().pixelSize(); return std::max(size, MIN_POINT_SIZE); }
void TextItem::set_font_point_size(qreal size) { auto f = font(); f.setPointSizeF(std::max(MIN_POINT_SIZE, std::min(MAX_POINT_SIZE, size))); setFont(f); }
std::vector<EditHandle> TextItem::get_edit_handles(double units_per_pixel) const {
    const ButtonSlot rotate{0, 0}, remove{-1, 0}, scale{-1, -1};
    const auto frame = LayerEditor::button_frame(interaction_rect(), {rotate, remove, scale}, units_per_pixel);
    const auto at = [&](const ButtonSlot &slot) { return mapToScene(LayerEditor::button_anchor(frame, slot)); };
    return {{HANDLE_ROTATE, HandleType::ROTATE, at(rotate), QCursor(Qt::SizeAllCursor), LayerEditor::FUNCTIONAL_HANDLE_SIZE, 8, {}},
        {HANDLE_DELETE, HandleType::ITEM_DELETE, at(remove), QCursor(Qt::PointingHandCursor), LayerEditor::FUNCTIONAL_HANDLE_SIZE, 2, {}},
        {HANDLE_SCALE, HandleType::TEXT_SCALE, at(scale), QCursor(Qt::SizeFDiagCursor), SCALE_HANDLE_SIZE, 8, {}}};
}
qreal TextItem::normalize_outline_width(const QVariant &raw) {
    bool ok = false; const auto value = raw.toDouble(&ok); if (!ok || !raw.isValid() || raw.isNull()) return DEFAULT_OUTLINE_WIDTH;
    auto best = OUTLINE_WIDTH_LEVELS.front(), distance = std::abs(best - value);
    for (const auto level : OUTLINE_WIDTH_LEVELS) {
        const auto next_distance = std::abs(level - value);
        if (next_distance < distance || (next_distance == distance && level > best)) { best = level; distance = next_distance; }
    }
    return best;
}
qreal TextItem::outline_extent() const { return has_outline ? outline_width * font_point_size() : 0.; }
qreal TextItem::shadow_distance() const { return has_shadow ? SHADOW_OFFSET_RATIO * font_point_size() : 0.; }
void TextItem::set_outline(bool enabled, std::optional<QColor> color, std::optional<QVariant> width) {
    prepareGeometryChange(); has_outline = enabled; if (color) outline_color = *color; if (width) outline_width = normalize_outline_width(*width); update();
}
void TextItem::set_shadow(bool enabled, std::optional<QColor> color) { prepareGeometryChange(); has_shadow = enabled; if (color) shadow_color = *color; update(); }
QRectF TextItem::content_rect() const { const auto outline = outline_extent(), far_side = outline + shadow_distance(); return QGraphicsTextItem::boundingRect().adjusted(-outline, -outline, far_side, far_side); }
QRectF TextItem::interaction_rect() const { auto rect = content_rect().adjusted(-FRAME_SIDE_GAP, 0, FRAME_SIDE_GAP, 0); if (rect.width() < MIN_INTERACTION_WIDTH) rect.setWidth(MIN_INTERACTION_WIDTH); return rect; }
QRectF TextItem::hit_rect() const { return interaction_rect().adjusted(-CLICK_MARGIN, -CLICK_MARGIN, CLICK_MARGIN, CLICK_MARGIN); }
QRectF TextItem::boundingRect() const { return hit_rect(); }
QPainterPath TextItem::shape() const { QPainterPath path; path.addRect(hit_rect()); return path; }
bool TextItem::contains(const QPointF &pos) const { return hit_rect().contains(pos); }
QPainterPath TextItem::glyph_path() const { return text_glyph_path(*this); }
QPainterPath text_glyph_path(const QGraphicsTextItem &item) {
    QPainterPath path; path.setFillRule(Qt::WindingFill);
    for (auto block = item.document()->begin(); block.isValid(); block = block.next()) {
        const auto *layout = block.layout(); const auto origin = layout->position();
        for (const auto &run : layout->glyphRuns(0, block.length())) {
            const auto raw_font = run.rawFont(); const auto indexes = run.glyphIndexes(); const auto positions = run.positions();
            for (qsizetype i = 0; i < std::min(indexes.size(), positions.size()); ++i) path.addPath(raw_font.pathForGlyph(indexes[i]).translated(origin + positions[i]));
            TextItem::add_decoration_lines(path, run, origin);
        }
    }
    return path;
}
void TextItem::add_decoration_lines(QPainterPath &path, const QGlyphRun &run, const QPointF &origin) {
    const auto positions = run.positions(); if (positions.isEmpty()) return;
    const auto raw = run.rawFont(); const auto baseline = origin.y() + positions.front().y(), thickness = raw.lineThickness();
    const auto span = run.boundingRect().translated(origin);
    for (const auto &line : {std::make_pair(run.underline(), raw.underlinePosition()), std::make_pair(run.strikeOut(), -raw.ascent() / 3), std::make_pair(run.overline(), -raw.ascent())})
        if (line.first) path.addRect(QRectF(span.left(), baseline + line.second - thickness / 2, span.width(), thickness));
}
QPen TextItem::outline_pen() const { QPen pen(outline_color, 2 * outline_extent()); pen.setJoinStyle(Qt::RoundJoin); pen.setCapStyle(Qt::RoundCap); return pen; }
void TextItem::paint_shadow(QPainter *p, const QPainterPath &glyphs, const std::optional<QPen> &outline) {
    const auto distance = shadow_distance(); p->save(); p->translate(distance, distance); p->fillPath(glyphs, shadow_color);
    if (outline) { QPainterPath outside; outside.addRect(boundingRect()); outside.addPath(glyphs); p->setClipPath(outside, Qt::IntersectClip); auto pen = *outline; pen.setColor(shadow_color); p->strokePath(glyphs, pen); }
    p->restore();
}
void TextItem::paint(QPainter *p, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    p->setRenderHint(QPainter::Antialiasing); p->setRenderHint(QPainter::TextAntialiasing);
    if (has_background) { p->save(); p->setPen(Qt::NoPen); p->setBrush(background_color); const auto rect = content_rect(); const auto radius = std::min({BACKGROUND_RADIUS, std::max(0., rect.width() / 2.), std::max(0., rect.height() / 2.)}); p->drawRoundedRect(rect, radius, radius); p->restore(); }
    if (has_outline || has_shadow) { const auto glyphs = glyph_path(); const std::optional<QPen> pen = has_outline ? std::optional<QPen>(outline_pen()) : std::nullopt; if (has_shadow) paint_shadow(p, glyphs, pen); if (pen) p->strokePath(glyphs, *pen); }
    const auto cleaned = text_paint_option(*option); QGraphicsTextItem::paint(p, &cleaned, widget); paint_interaction_frame(p);
}
bool TextItem::is_editing() const {
    if (!(textInteractionFlags() & Qt::TextEditorInteraction)) return false;
    return hasFocus() || (scene() && scene()->focusItem() == this);
}
QStyleOptionGraphicsItem TextItem::text_paint_option(const QStyleOptionGraphicsItem &option) { auto cleaned = option; cleaned.state &= ~(QStyle::State_Selected | QStyle::State_HasFocus); return cleaned; }
bool TextItem::is_edit_target() const { return DrawingItemMixin::is_edit_target() || is_editing(); }
void TextItem::paint_interaction_frame(QPainter *p) {
    if (auto pen = selection_frame_pen()) { p->save(); p->setBrush(Qt::NoBrush); p->setPen(*pen); p->drawRect(interaction_rect().adjusted(FRAME_INSET, FRAME_INSET, -FRAME_INSET, -FRAME_INSET)); p->restore(); }
}
void TextItem::set_background(bool enabled, std::optional<QColor> color, std::optional<qreal> opacity) { has_background = enabled; if (color) background_color = *color; if (opacity) background_color.setAlpha(static_cast<int>(std::max(0., std::min(255., *opacity)))); update(); }
void TextItem::focusInEvent(QFocusEvent *event) {
    if (_is_closed) return;
    try {
        // Back from another window the same edit goes on: clearing it still undoes to the text it started from.
        if (!_window_away) _text_before_edit = toPlainText();
        _window_away = false;
        QGraphicsTextItem::focusInEvent(event);
    } catch (const std::exception &e) { qWarning("TextItem.focusInEvent: %s", e.what()); }
}
void TextItem::focusOutEvent(QFocusEvent *event) {
    if (_is_closed) return;
    try {
        // Picking from another window (the clipboard history over a capture) or switching programs does
        // not end typing: an empty new box and the selection are kept for when the window is active again.
        if (event->reason() == Qt::ActiveWindowFocusReason) {
            _window_away = true;
            QGraphicsTextItem::focusOutEvent(event);
            return;
        }
        _window_away = false;
        QGraphicsTextItem::focusOutEvent(event); auto cursor = textCursor(); cursor.clearSelection(); setTextCursor(cursor);
        auto *s = scene(); auto *context = dynamic_cast<TextUndoContext *>(s); auto *stack = context ? context->text_undo_stack() : nullptr;
        if (python_blank(toPlainText())) {
            if (!s) return;
            // A provisional text never reached the undo stack: once out of the scene nothing owns it.
            if (_provisional) { s->removeItem(this); deleteLater(); }
            else if (!stack) s->removeItem(this);
            else stack->push(new ClearTextItemCommand(s, ItemReference(this), _text_before_edit));
            return;
        }
        setTextInteractionFlags(Qt::NoTextInteraction);
        if (_provisional) { _provisional = false; if (stack) stack->push(new AddItemCommand(s, ItemReference(this))); }
    } catch (const std::exception &e) { qWarning("TextItem.focusOutEvent: %s", e.what()); }
}
void TextItem::keyPressEvent(QKeyEvent *event) {
    if (_is_closed) return;
    if (!event->matches(QKeySequence::Paste) || !(textInteractionFlags() & Qt::TextEditable)) {
        QGraphicsTextItem::keyPressEvent(event);
        return;
    }
    const auto *mime = QGuiApplication::clipboard()->mimeData();
    if (mime && mime->hasText()) {
        auto cursor = textCursor();
        cursor.insertText(mime->text());
        setTextCursor(cursor);
    }
    event->accept();
}

void TextItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) {
    if (_is_closed) return;
    try { if (textInteractionFlags() == Qt::NoTextInteraction) { setTextInteractionFlags(Qt::TextEditorInteraction); setFocus(); } QGraphicsTextItem::mouseDoubleClickEvent(event); } catch (const std::exception &e) { qWarning("TextItem.mouseDoubleClickEvent: %s", e.what()); }
}
bool TextItem::is_on_edge(const QPointF &point) const {
    if (!boundingRect().contains(point)) return false;
    const auto margin = document()->documentMargin(); const auto inner = QGraphicsTextItem::boundingRect().adjusted(margin, margin, -margin, -margin);
    if (inner.width() <= 0 || inner.height() <= 0) return true;
    return !inner.contains(point);
}
void TextItem::apply_edit_cursor(const QPointF &point) { setCursor(is_on_edge(point) ? Qt::SizeAllCursor : Qt::IBeamCursor); }
void TextItem::hoverEnterEvent(QGraphicsSceneHoverEvent *event) { if (is_editing()) { apply_edit_cursor(event->pos()); event->accept(); return; } hover_enter(event); }
void TextItem::hoverMoveEvent(QGraphicsSceneHoverEvent *event) { if (is_editing()) { apply_edit_cursor(event->pos()); event->accept(); return; } hover_move(event); }
bool TextItem::scale_stroke_width(qreal scale) { auto f = font(); auto size = f.pointSizeF(); if (size <= 0) size = f.pointSize() ? f.pointSize() : 12; f.setPointSizeF(std::max(6., size * scale)); setFont(f); update(); return true; }
bool TextItem::set_visual_opacity(qreal opacity) { setOpacity(std::max(0., std::min(1., opacity))); update(); return true; }
std::optional<qreal> TextItem::get_visual_opacity() const { return std::max(0., std::min(1., opacity())); }
}
