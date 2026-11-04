#pragma once
#include "pin/pin_text_selection.h"
#include <QGraphicsView>
#include <QImage>
#include <QWidget>
#include <memory>

class QGraphicsPixmapItem;

namespace jietuba {
class CaptionLabel;
class TransparentToolButton;

// The recognition window's original image: fitted, at 1:1, or zoomed by the wheel around the
// cursor; dragged where there is no text. Recognized text on it is selected as on a pin, and
// what the result side selects is shaded over it.
class OriginalImageView : public QWidget {
    Q_OBJECT
public:
    // Width the pane opens at, before window scaling.
    static constexpr int WIDTH = 506;
    // Displayed scale, image pixels to screen pixels.
    static constexpr double MIN_SCALE = 0.1, MAX_SCALE = 20.0;
    enum class Fit { Window, Actual, Custom };

    explicit OriginalImageView(const QImage &image, QWidget *parent = nullptr);
    ~OriginalImageView() override;
    // The recognized text, in the image's pixels.
    void set_text(OcrTextLayout layout);
    // Areas in image pixels shaded as the result side's selection; scrolled to when all of them
    // are out of view.
    void set_highlight(QVector<QPolygonF> areas);
    const QVector<QPolygonF> &highlight() const { return highlight_; }
    Fit fit() const { return fit_; }
    void set_fit(Fit fit);
    // Image pixels per screen pixel: 1 at 1:1, whatever the screen scaling.
    double scale() const;
    // Zooms by factor keeping the point (viewport coordinates) still.
    void zoom_by(double factor, const QPointF &anchor);
    QGraphicsView *view() const;
    PinTextSelection *text_selection() const { return selection_.get(); }

    CaptionLabel *size_label;
    TransparentToolButton *zoom_out_button;
    CaptionLabel *scale_label;
    TransparentToolButton *zoom_in_button;
    // Switches between fitting the window and 1:1.
    TransparentToolButton *fit_button;
    TransparentToolButton *copy_button;
private:
    class View;
    friend class View;
    void apply_theme();
    void set_view_scale(double scale, const QPointF &anchor);
    void apply_fit();
    void update_toolbar();
    // Displayed scale to the view's own, which works in device-independent pixels.
    double device_ratio() const;

    QImage image_;
    View *view_;
    QGraphicsPixmapItem *pixmap_item_;
    std::unique_ptr<PinTextSelection> selection_;
    QVector<QPolygonF> highlight_;
    Fit fit_ = Fit::Window;
};
}
