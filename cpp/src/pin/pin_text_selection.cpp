#include "pin/pin_text_selection.h"
#include "core/logger.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>

namespace jietuba {
namespace {
// Screen pixels across a line that a drag end still counts as on it, so a wobbling drag along a
// line keeps its run. A press counts only on the text itself, so the blank around it drags the pin.
constexpr double HIT_SLACK = 5;
const QColor SELECTION_COLOR(100, 150, 255, 100);

// Edges all horizontal or vertical: such a fill stays crisp without antialiasing.
bool axis_aligned(const QPolygonF &polygon) {
    for (int i = 0; i < polygon.size(); ++i) {
        const auto edge = polygon[(i + 1) % polygon.size()] - polygon[i];
        if (std::abs(edge.x()) > 1e-6 && std::abs(edge.y()) > 1e-6) return false;
    }
    return true;
}
}

PinTextSelection::PinTextSelection(PinCanvasView *view) : view_(view) {}

void PinTextSelection::set_layout(OcrTextLayout layout) {
    selecting_ = false;
    change_selection([&] { layout_ = std::move(layout); });
}

void PinTextSelection::set_active(bool active) {
    if (active_ == active) return;
    if (!active) {
        clear_selection();
        selecting_ = false;
        show_beam(false);
    }
    active_ = active;
}

bool PinTextSelection::mouse_press(QMouseEvent *event) {
    if (!active_ || layout_.empty() || event->button() != Qt::LeftButton) return false;
    swallow_release_ = false;
    const auto pos = image_pos(event);
    const auto line = layout_.text_line_at(pos);
    if (!line) {
        clear_selection();
        return false;
    }
    const bool third_click = double_clicked_.isValid() && double_clicked_.elapsed() < QApplication::doubleClickInterval() &&
                             (event->position() - double_click_pos_).manhattanLength() < QApplication::startDragDistance();
    double_clicked_.invalidate();
    if (third_click) {
        selecting_ = false;
        swallow_release_ = true;
        change_selection([&] { layout_.select_paragraph(*line); });
        event->accept();
        return true;
    }
    anchor_ = pos;
    selecting_ = true;
    change_selection([this] { layout_.clear_selection(); });
    if (view_) view_->setFocus(Qt::MouseFocusReason);
    event->accept();
    return true;
}

bool PinTextSelection::mouse_move(QMouseEvent *event) {
    if (!active_ || layout_.empty()) return false;
    const auto pos = image_pos(event);
    if (selecting_) {
        // The release went elsewhere (a popup took the mouse).
        if (!(event->buttons() & Qt::LeftButton)) {
            selecting_ = false;
            return false;
        }
        change_selection([&] { layout_.select_between(anchor_, pos, slack()); });
        event->accept();
        return true;
    }
    if (event->buttons() != Qt::NoButton) return false;
    const bool on_text = layout_.text_line_at(pos).has_value();
    show_beam(on_text);
    // Over text the view's own hover would replace the I-beam.
    return on_text;
}

bool PinTextSelection::mouse_release(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return false;
    // The release closing a double click on text.
    if (swallow_release_) {
        swallow_release_ = false;
        event->accept();
        return true;
    }
    if (!selecting_) return false;
    selecting_ = false;
    event->accept();
    return true;
}

bool PinTextSelection::mouse_double_click(QMouseEvent *event) {
    if (!active_ || layout_.empty() || event->button() != Qt::LeftButton) return false;
    const auto line = layout_.text_line_at(image_pos(event));
    if (!line) return false;
    selecting_ = false;
    swallow_release_ = true;
    const int index = layout_.char_at(*line, image_pos(event));
    change_selection([&] { layout_.select_word(*line, index); });
    double_clicked_.start();
    double_click_pos_ = event->position();
    event->accept();
    return true;
}

bool PinTextSelection::key_press(QKeyEvent *event) {
    if (!active_ || layout_.empty() || !event->matches(QKeySequence::SelectAll)) return false;
    change_selection([this] { layout_.select_all(); });
    event->accept();
    return true;
}

void PinTextSelection::paint(QPainter *painter) const {
    if (!has_selection()) return;
    painter->save();
    // Drawn in device pixels: an upright run is a rectangle fill, which the raster engine does far
    // faster than a transformed polygon; only slanted runs are filled as smoothed polygons.
    const auto to_device = painter->transform();
    painter->resetTransform();
    painter->setPen(Qt::NoPen);
    painter->setBrush(SELECTION_COLOR);
    painter->setRenderHint(QPainter::Antialiasing, true);
    for (int index = 0; index < static_cast<int>(layout_.lines().size()); ++index) {
        const auto outline = to_device.map(layout_.selection_outline(index));
        if (outline.isEmpty()) continue;
        if (axis_aligned(outline)) painter->fillRect(outline.boundingRect().toAlignedRect(), SELECTION_COLOR);
        else painter->drawPolygon(outline);
    }
    painter->restore();
}

bool PinTextSelection::copy_selected_text() {
    const auto text = selected_text();
    if (text.isEmpty()) return false;
    QApplication::clipboard()->setText(text);
    const auto chars = text.toUcs4();
    auto preview = QString::fromUcs4(reinterpret_cast<const char32_t *>(chars.constData()), std::min<qsizetype>(chars.size(), 50));
    if (chars.size() > 50) preview += "...";
    log_info(T("已复制: {text_preview}", {{"text_preview", preview}}), "PinTextSelection");
    return true;
}

bool PinTextSelection::clear_selection() {
    if (!layout_.has_selection()) return false;
    change_selection([this] { layout_.clear_selection(); });
    return true;
}

template <class Change> void PinTextSelection::change_selection(Change &&change) {
    const QRectF before = layout_.selection_bounds();
    change();
    const QRectF after = layout_.selection_bounds();
    if (!view_ || (before.isNull() && after.isNull())) return;
    const QRectF dirty = before.isNull() ? after : after.isNull() ? before : before.united(after);
    view_->viewport()->update(view_->mapFromScene(dirty).boundingRect().adjusted(-2, -2, 2, 2));
}

double PinTextSelection::slack() const {
    const double scale = view_ ? std::sqrt(std::abs(view_->transform().determinant())) : 1.0;
    return scale > 0 ? HIT_SLACK / scale : HIT_SLACK;
}

bool PinTextSelection::on_selection(const QPoint &global_pos) const {
    if (!view_ || !has_selection()) return false;
    const QPointF pos = view_->viewportTransform().inverted().map(QPointF(view_->viewport()->mapFromGlobal(global_pos)));
    for (int i = 0; i < static_cast<int>(layout_.lines().size()); ++i)
        if (layout_.selection_outline(i).containsPoint(pos, Qt::OddEvenFill)) return true;
    return false;
}

QPointF PinTextSelection::image_pos(const QMouseEvent *event) const {
    return view_ ? view_->viewportTransform().inverted().map(event->position()) : event->position();
}

void PinTextSelection::show_beam(bool on) {
    if (!view_) return;
    const auto shape = view_->viewport()->cursor().shape();
    if (on) {
        if (shape != Qt::IBeamCursor) view_->setCursor(QCursor(Qt::IBeamCursor));
        beam_ = true;
    } else if (beam_) {
        beam_ = false;
        // While editing the tool owns the cursor.
        if (shape == Qt::IBeamCursor && !(view_->pin_canvas && view_->pin_canvas->is_editing())) view_->setCursor(QCursor(Qt::ArrowCursor));
    }
}
}
