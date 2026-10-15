#pragma once
#include <QRect>
#include <QWidget>

class QRegion;

namespace jietuba {
class CanvasView;
class LayerEditor;

// Transparent layer above the viewport where LayerEditor draws its handles,
// outside the scene: invalidation comes from LayerEditor::screen_bounds() instead
// of a guessed margin, and scene.render() never bakes handles into exports.
// It always covers the whole viewport (a geometry following the handles would
// go stale on any unnotified transform change and clip them), but it never
// invalidates as a whole: being translucent, that would repaint everything below.
class HandleOverlayWidget : public QWidget {
public:
    // Handle stroke and antialiasing spill slightly past screen_rect().
    static constexpr int EDGE_ALLOWANCE = 3;
    explicit HandleOverlayWidget(CanvasView *view);
    // Invalidates the new handle area united with what was last painted.
    void refresh();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    LayerEditor *editor() const;
    QRect handles_viewport_rect() const;
    // What remains on screen after a repaint: a passive repaint that did not cover
    // the old area keeps it pending, so the next invalidation clears it too.
    QRect still_on_screen(const QRegion &repainted, const QRect &drawn) const;
    CanvasView *view_;
    QRect painted_;
};
}
