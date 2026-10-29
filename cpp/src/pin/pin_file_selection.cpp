#include "pin/pin_file_selection.h"
#include "core/theme.h"
#include "pin/pin_canvas.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_file_card.h"
#include "pin/pin_window.h"
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>

namespace jietuba {
namespace {
// In image pixels: the outline's distance from the edge of its cell, its corner radius and width,
// and the legs of the corner badge. The badge goes bottom right, where a long name never reaches.
constexpr double INSET = 2, RADIUS = 4, OUTLINE = 1.5, BADGE = 20;
}

PinFileSelection::PinFileSelection(PinWindow *pin, const PinFileCard *card) : pin_(pin), card_(card), selected_(card->files().size(), false) {}

bool PinFileSelection::in_effect() const { return pin_ && !(pin_->canvas && pin_->canvas->is_editing()) && !pin_->thumbnail_mode(); }

bool PinFileSelection::is_selected(int index) const { return index >= 0 && index < static_cast<int>(selected_.size()) && selected_[index]; }

void PinFileSelection::select_only(int index) {
    std::fill(selected_.begin(), selected_.end(), false);
    if (index >= 0) selected_[index] = true;
}

void PinFileSelection::click(int index, Qt::KeyboardModifiers modifiers) {
    if (!in_effect()) return;
    const bool ctrl = modifiers & Qt::ControlModifier, shift = modifiers & Qt::ShiftModifier;
    if (!ctrl && !shift) {
        clear();
        return;
    }
    if (index < 0) return;
    if (shift) {
        const int from = anchor_ >= 0 ? anchor_ : index;
        if (!ctrl) select_only(-1);
        for (int i = std::min(from, index); i <= std::max(from, index); ++i) selected_[i] = true;
    } else {
        selected_[index] = !selected_[index];
        anchor_ = index;
    }
    repaint();
}

void PinFileSelection::clear() {
    select_only(-1);
    anchor_ = -1;
    repaint();
}

QStringList PinFileSelection::files_for(int index) const {
    if (in_effect() && is_selected(index)) return selected_files();
    return {card_->files().value(index)};
}

void PinFileSelection::select_all() {
    std::fill(selected_.begin(), selected_.end(), true);
    repaint();
}

void PinFileSelection::refresh() { repaint(); }

bool PinFileSelection::key_press(QKeyEvent *event) {
    if (!in_effect()) return false;
    if (event->matches(QKeySequence::SelectAll)) {
        select_all();
        event->accept();
        return true;
    }
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    if (!enter || (event->modifiers() & ~Qt::KeypadModifier)) return false;
    const QStringList files = selected_files();
    if (files.isEmpty()) return false;
    event->accept();
    PinFileCard::open(files, pin_);
    return true;
}

QStringList PinFileSelection::selected_files() const {
    QStringList files;
    if (!in_effect()) return files;
    for (int i = 0; i < static_cast<int>(selected_.size()); ++i)
        if (selected_[i]) files.append(card_->files()[i]);
    return files;
}

QRectF PinFileSelection::cell(int index) const { return QRectF(card_->cell_rect(std::min(index, card_->shown()))); }

QRectF PinFileSelection::selected_bounds() const {
    QRectF bounds;
    for (int i = 0; i < static_cast<int>(selected_.size()); ++i)
        if (selected_[i]) bounds = bounds.united(cell(i));
    return bounds;
}

void PinFileSelection::repaint() const {
    if (pin_ && pin_->view) pin_->view->viewport()->update();
}

void PinFileSelection::paint(QPainter *painter) const {
    if (!in_effect()) return;
    // An outline and a checked corner rather than a fill, so the card reads the same; grey on an inactive pin.
    const QColor color = pin_->has_keyboard_focus() ? get_theme().theme_color() : QColor(0xa0, 0xa0, 0xa0);
    const auto draw = [painter, &color](const QRectF &cell) {
        const QRectF r = cell.adjusted(INSET, INSET, -INSET, -INSET);
        QPainterPath outline;
        outline.addRoundedRect(r, RADIUS, RADIUS);
        QPainterPath corner;
        corner.moveTo(r.right(), r.bottom() - BADGE);
        corner.lineTo(r.right(), r.bottom());
        corner.lineTo(r.right() - BADGE, r.bottom());
        corner.closeSubpath();
        painter->fillPath(corner.intersected(outline), color);
        painter->strokePath(outline, QPen(color, OUTLINE));
        // The check sits on the badge's centroid.
        const QPointF c(r.right() - BADGE / 3, r.bottom() - BADGE / 3);
        const double s = BADGE / 7;
        const QPointF check[] = {c + QPointF(-s, 0), c + QPointF(-s / 3, s * 0.7), c + QPointF(s, -s * 0.7)};
        painter->setPen(QPen(Qt::white, OUTLINE, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->drawPolyline(check, 3);
    };
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const int shown = card_->shown();
    bool hidden_selected = false;
    for (int i = 0; i < static_cast<int>(selected_.size()); ++i) {
        if (!selected_[i]) continue;
        if (i >= shown) hidden_selected = true;
        else draw(cell(i));
    }
    if (hidden_selected) draw(cell(shown));
    painter->restore();
}
}
