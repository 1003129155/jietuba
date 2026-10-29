#pragma once
#include "pin/ocr_text_layout.h"
#include <QElapsedTimer>
#include <QPointer>

class QKeyEvent;
class QMouseEvent;
class QPainter;

namespace jietuba {
class PinCanvasView;

// Selecting a pin's recognized text: the I-beam over text, dragging, a double click for a word and a
// triple click for a paragraph, Ctrl+A,
// and the highlight. Everything is in image pixels and the view paints it over the scene, so
// zooming, rotating and flipping the pin need nothing here; the highlight is neither in the
// scene's items nor in its exported image.
class PinTextSelection {
public:
    explicit PinTextSelection(PinCanvasView *view);
    void set_layout(OcrTextLayout layout);
    const OcrTextLayout &layout() const { return layout_; }
    bool has_text() const { return !layout_.empty(); }
    // Off while drawing, in thumbnail mode or with text selection turned off; turning off clears the selection.
    void set_active(bool active);
    bool is_active() const { return active_; }
    // Mouse events the view gets outside editing; true when they were used here. A press away
    // from text clears the selection and is left to the view.
    bool mouse_press(QMouseEvent *event);
    bool mouse_move(QMouseEvent *event);
    bool mouse_release(QMouseEvent *event);
    bool mouse_double_click(QMouseEvent *event);
    // Ctrl+A selects all text.
    bool key_press(QKeyEvent *event);
    // Paints the highlight; the painter is in image pixels (the view's drawForeground).
    void paint(QPainter *painter) const;
    bool has_selection() const { return active_ && layout_.has_selection(); }
    QString selected_text() const { return has_selection() ? layout_.selected_text() : QString(); }
    // False when there is nothing selected.
    bool copy_selected_text();
    // True when there was a selection to clear.
    bool clear_selection();
private:
    // Repaints what the selection covered before and covers after.
    template <class Change> void change_selection(Change &&change);
    // How close to text, in image pixels, still counts as on it: a few pixels on screen.
    double slack() const;
    QPointF image_pos(const QMouseEvent *event) const;
    void show_beam(bool on);
    QPointer<PinCanvasView> view_;
    OcrTextLayout layout_;
    bool active_ = true;
    bool selecting_ = false;
    // The release that ends a double or triple click on text belongs here too.
    bool swallow_release_ = false;
    // A press soon after a double click on text, and near it, is the third click.
    QElapsedTimer double_clicked_;
    QPointF double_click_pos_;
    bool beam_ = false;
    QPointF anchor_;
};
}
