#pragma once
#include <QColor>
#include <QPoint>
#include <QRect>
#include <QWidget>

// wingdi.h defines PASSTHROUGH as a printer escape code.
#ifdef PASSTHROUGH
#undef PASSTHROUGH
#endif

// The selection border: blue while preparing, red while recording, yellow while
// paused; click-through except while its edges can be dragged.
namespace jietuba {
enum class OverlayMode {
    // Fully click-through.
    PASSTHROUGH,
    // The four edges can be dragged to resize.
    RESIZE,
};

class CaptureOverlay : public QWidget {
    Q_OBJECT
public:
    explicit CaptureOverlay(const QRect &rect, QWidget *parent = nullptr);
    void set_mode(OverlayMode mode);
    // Red while recording, yellow while paused, blue otherwise.
    void set_recording(bool recording, bool paused = false);
    void update_rect(const QRect &rect);
    QRect capture_rect() const { return rect_; }
signals:
    void rect_changed(const QRect &rect);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
private:
    void set_passthrough(bool enable);
    QString hit_edge(const QPoint &pos) const;
    void apply_resize(const QPoint &delta);
    QRect outer_geom() const;
    QRect rect_;
    OverlayMode mode_ = OverlayMode::PASSTHROUGH;
    QColor color_;
    QString drag_edge_;
    QPoint drag_origin_;
    QRect drag_rect_origin_;
};
}
