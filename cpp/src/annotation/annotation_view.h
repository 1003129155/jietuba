#pragma once
#include "canvas/canvas_host.h"
#include "gif/drawing_view.h"
#include <QColor>
#include <memory>
#include <optional>

namespace jietuba {
class AnnotationBoard;
class PointerLight;
class ScreenAnnotation;

// The layer of the screen annotation: the GIF drawing layer stretched over the bounding rectangle of
// every screen, as the capture window is, so a drawing can be moved from one screen to another. Its
// scene is in desktop coordinates: a drawing keeps its place on the desktop when screens come and go.
// As the window hosting the canvas it hands the shared toolbar to CanvasView, so selecting a drawing
// fills the toolbar's panels as in a capture. Keys are handled by the session.
class AnnotationView : public GifDrawingView, public CanvasHost {
    Q_OBJECT
public:
    explicit AnnotationView(ScreenAnnotation *owner);
    ~AnnotationView() override;
    // The bounding rectangle of every screen.
    static QRect desktop_rect();
    // Follows the screens after one is added, removed, moved or resized.
    void fit_desktop();
    Toolbar *canvas_toolbar() const override;
    CanvasView *canvas_view() const override { return const_cast<AnnotationView *>(this); }
    // A solid board over the screens: a window of its own, so the session can stack the pins between it
    // and the drawings. nullopt takes it away.
    void set_board(const std::optional<QColor> &color);
    std::optional<QColor> board() const;
    QWidget *board_window() const;
    // The pointer light of the cursor highlight: made on first use, removed when the highlight ends.
    PointerLight *pointer_light(bool create);
    void remove_pointer_light();
signals:
    // A press while drawing: the toolbar follows the tool's style.
    void pressed();
protected:
    // Only a new text is selected, to type into: selection handles on a shape would show on the screen
    // and in recordings. A drawing is still selected by clicking it.
    void on_item_auto_select_requested(QGraphicsItem *item) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    std::unique_ptr<PointerLight> light_;
    std::unique_ptr<AnnotationBoard> board_;
    ScreenAnnotation *owner_;
};
}
