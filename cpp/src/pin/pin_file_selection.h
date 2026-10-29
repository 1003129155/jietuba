#pragma once
#include <QPointer>
#include <QRectF>
#include <QStringList>
#include <vector>

class QKeyEvent;
class QPainter;

namespace jietuba {
class PinFileCard;
class PinWindow;

// Picking some of a file card's files: Ctrl+click adds or drops one, Shift+click selects the run from
// the last one Ctrl+clicked, Ctrl+A selects all, and Enter opens them. A plain click is how a pin is
// focused or moved, so it only clears the selection; a click only counts when the button comes up
// where it went down. The view paints the marks over the scene, outside its exported image; while
// drawing and in thumbnail mode they are hidden and the selection has no effect.
class PinFileSelection {
public:
    PinFileSelection(PinWindow *pin, const PinFileCard *card);
    // index is the file under the pointer, -1 off the cards and on the count cell.
    void click(int index, Qt::KeyboardModifiers modifiers);
    void clear();
    // What a right click or a drag on file index acts on: the selection when index is in it, otherwise
    // index alone. Neither changes the selection.
    QStringList files_for(int index) const;
    void select_all();
    // After drawing starts or stops: the marks show or hide.
    void refresh();
    // Ctrl+A and Enter.
    bool key_press(QKeyEvent *event);
    bool is_selected(int index) const;
    // In the card's order; empty while hidden.
    QStringList selected_files() const;
    // The selected cards in image pixels, with the count cell when files it stands for are selected.
    QRectF selected_bounds() const;
    bool in_effect() const;
    // The painter is in image pixels (the view's drawForeground).
    void paint(QPainter *painter) const;
private:
    void select_only(int index);
    void repaint() const;
    // The cell drawn for index: its card, or the count cell for a file not shown.
    QRectF cell(int index) const;
    QPointer<PinWindow> pin_;
    const PinFileCard *card_;
    std::vector<bool> selected_;
    int anchor_ = -1;
};
}
