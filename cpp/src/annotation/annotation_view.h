#pragma once
#include "canvas/canvas_host.h"
#include "gif/drawing_view.h"
#include <QColor>
#include <QPointer>
#include <memory>
#include <optional>

class QGraphicsRectItem;
class QScreen;

namespace jietuba {
class PointerLight;
class ScreenAnnotation;

// One screen of the screen annotation: the GIF drawing layer stretched over the whole screen. As the
// window hosting the canvas it hands the shared toolbar to CanvasView, so selecting a drawing fills the
// toolbar's panels as in a capture. Keys are handled by the session, not per layer.
class AnnotationView : public GifDrawingView, public CanvasHost {
    Q_OBJECT
public:
    AnnotationView(QScreen *screen, ScreenAnnotation *owner);
    ~AnnotationView() override;
    QScreen *target_screen() const { return screen_; }
    // Follows the screen after a resolution or position change; drawings keep their scene positions.
    void fit_screen();
    Toolbar *canvas_toolbar() const override;
    CanvasView *canvas_view() const override { return const_cast<AnnotationView *>(this); }
    // A solid board over the screen, below the drawings; nullopt takes it away.
    void set_board(const std::optional<QColor> &color);
    std::optional<QColor> board() const;
    // The pointer light of the cursor highlight: made on first use, removed when the highlight ends.
    PointerLight *pointer_light(bool create);
    void remove_pointer_light();
signals:
    // A press on this layer: keys and toolbar syncing follow the layer being drawn on.
    void pressed(AnnotationView *view);
protected:
    // Only a new text is selected, to type into: selection handles on a shape would show on the screen
    // and in recordings. A drawing is still selected by clicking it.
    void on_item_auto_select_requested(QGraphicsItem *item) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
private:
    QPointer<QScreen> screen_;
    std::unique_ptr<PointerLight> light_;
    QGraphicsRectItem *board_item_ = nullptr;
    ScreenAnnotation *owner_;
};
}
