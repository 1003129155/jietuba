#pragma once
#include "core/hooks.h"
#include <QGraphicsScene>
#include <QImage>
#include <QPixmap>
#include "canvas/selection_model.h"

namespace jietuba {
// Exports the selection with or without annotations. The three-reference
// constructor serves tests that build a bare scene; references do not own items.
class CanvasScene;
class ExportService {
public:
    explicit ExportService(CanvasScene *scene);
    ExportService(QGraphicsScene &scene, SelectionModel &selection, QGraphicsItem *background)
        : scene(scene), selection_model(selection), background(background) {}
    QGraphicsScene &scene;
    SelectionModel &selection_model;
    QGraphicsItem *background;
    QPixmap get_result_pixmap();
    // The selection image; export_hooks then post-process it in order.
    QImage export_image(const QRectF &selection_rect);
    // (image, selection rect) -> image, e.g. rounded corners, border or shadow.
    HookList<QImage(const QImage &, const QRectF &)> export_hooks;
    QImage export_base_image_only(const QRectF &selection_rect);
};
}
