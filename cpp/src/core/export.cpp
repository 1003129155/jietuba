#include "core/export.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include <QPainter>
#include <QGraphicsItem>
#include <algorithm>

namespace jietuba {
ExportService::ExportService(CanvasScene *canvas) : ExportService(*canvas, *canvas->selection_model, canvas->background) {}
QPixmap ExportService::get_result_pixmap() {
    auto selection_rect = selection_model.rect();
    if (selection_rect.isEmpty()) selection_rect = scene.sceneRect();
    return QPixmap::fromImage(export_image(selection_rect));
}
QImage ExportService::export_image(const QRectF &selection_rect) {
    if (selection_rect.isNull() || selection_rect.isEmpty()) return {};
    const int w = std::max(1, int(selection_rect.width()));
    const int h = std::max(1, int(selection_rect.height()));
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(0);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    scene.render(&painter, QRectF(0, 0, w, h), selection_rect);
    painter.end();
    for (const auto &hook : export_hooks.callbacks()) out = hook(out, selection_rect);
    return out;
}
QImage ExportService::export_base_image_only(const QRectF &selection_rect) {
    if (selection_rect.isNull() || selection_rect.isEmpty()) return {};
    const int w = std::max(1, int(selection_rect.width()));
    const int h = std::max(1, int(selection_rect.height()));
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(0);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QVector<std::pair<QGraphicsItem *, bool>> old_visible_states;
    for (auto *item : scene.items()) if (item != background) {
        old_visible_states.push_back({item, item->isVisible()});
        item->setVisible(false);
    }
    scene.render(&painter, QRectF(0, 0, w, h), selection_rect);
    for (const auto &[item, visible] : old_visible_states) item->setVisible(visible);
    painter.end();
    return out;
}
}
