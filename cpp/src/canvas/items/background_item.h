#pragma once
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QRectF>
#include <functional>
#include <optional>

namespace jietuba {
// The screenshot, z = 0. Keeps the QImage beside the pixmap so the magnifier
// never converts the pixmap back every frame.
class BackgroundItem : public QGraphicsPixmapItem {
public:
    BackgroundItem(const QImage &image, const QRectF &scene_rect);
    QImage image();
    void update_image(const QImage &image);
    // The same with image already made into pixmap, such as on a worker thread.
    void update_image(const QImage &image, const QPixmap &pixmap);
    // image() draws the original with source each time instead of keeping it, for a background that
    // can be drawn again, such as text; update_image() ends it.
    void set_image_source(std::function<QImage()> source);
    // Swaps only the rendered pixmap and its compensating transform (pin zoom
    // resamples for display); image() and reduced_image() keep the original
    // resolution so mosaic blocks stay aligned with scene coordinates.
    void set_display_pixmap(const QPixmap &pixmap, const QTransform &transform);
    // The background shrunk block_size times with one replicated edge pixel on
    // every side, so pixel (1, 1) is the top-left block. Box averaging counts
    // only real pixels in the partial blocks at the right and bottom edges.
    QImage reduced_image(int block_size);
    void release_image_cache();
private:
    QRectF scene_rect_;
    QImage cached_image_;
    bool has_cached_image_ = true;
    std::function<QImage()> image_source_;
    std::optional<int> reduced_cache_key_;
    QImage reduced_cache_image_;
};
}
