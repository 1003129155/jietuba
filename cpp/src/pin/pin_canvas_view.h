#pragma once
#include "canvas/view.h"

namespace jietuba {
class PinCanvas;
class PinTextSelection;
class PinWindow;

// The pin window's only content renderer: shows the CanvasScene through the view's
// own (GPU-accelerated, incremental) painting, rounded by a viewport mask, on a
// transparent background. Outside editing, dragging on text selects it and dragging
// anywhere else moves the window; on a file card a press that stays put is a click on a file.
class PinCanvasView : public CanvasView {
    Q_OBJECT
public:
    PinCanvasView(CanvasScene *scene, PinWindow *pin_window, PinCanvas *pin_canvas, bool cross_tool_select = false);
    void set_corner_radius(double radius);
    void update_viewport_mask();
    PinWindow *pin_window;
    PinCanvas *pin_canvas;
    // Set by the OCR manager once recognition starts; owned there.
    PinTextSelection *text_selection = nullptr;
    bool window_dragging = false;
    // The image painted over the annotations, which stay in the scene untouched.
    bool annotations_hidden = false;
protected:
    void resizeEvent(QResizeEvent *event) override;
    // Arrow outside editing; the tool cursor while editing.
    void enterEvent(QEnterEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    // Hidden annotations under the image again, and the text and file selections' highlights; all over
    // the scene and outside its exported image.
    void drawForeground(QPainter *painter, const QRectF &rect) override;
    // Outside editing the window zooms instead.
    void wheelEvent(QWheelEvent *event) override;
    // Pin shortcuts go through the ShortcutManager; this is the fallback when it does not take the key.
    void keyPressEvent(QKeyEvent *event) override;
private:
    double corner_radius_ = 8;
    QPoint press_global_;
    // The release that ends a double click opening a file.
    bool swallow_release_ = false;
};
}
