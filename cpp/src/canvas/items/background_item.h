#pragma once
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QPoint>
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
    // The size of image(), whatever size the display pixmap has.
    QSize image_size() const { return image_size_.isValid() ? image_size_ : pixmap().size(); }
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
    // A layer drawn over the image wherever the scene is shown or exported, at position in
    // image pixels: translations laid over their source text. image() stays the picture
    // underneath, so sampling it never reads the layer.
    void set_overlay(const QImage &layer, const QPoint &position);
    void set_overlay_visible(bool visible);
    bool overlay_visible() const { return overlay_visible_ && !overlay_.isNull(); }
    const QImage &overlay() const { return overlay_; }
    // The layer resampled like the display pixmap of display_size, at position in its pixels. It is
    // drawn while that pixmap is shown; otherwise the layer itself is stretched to the same place.
    void set_display_overlay(const QPixmap &layer, const QPoint &position, const QSize &display_size);
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;
private:
    // When the pixmap took a converted copy of the image, its pixels serve as the image too, so the
    // picture is held once rather than in two formats.
    void hold_pixels_once();
    QRectF scene_rect_;
    QSize image_size_;
    QImage cached_image_;
    bool has_cached_image_ = true;
    std::function<QImage()> image_source_;
    std::optional<int> reduced_cache_key_;
    QImage reduced_cache_image_;
    QImage overlay_;
    QPoint overlay_position_;
    bool overlay_visible_ = true;
    QPixmap display_overlay_;
    QPoint display_overlay_position_;
    QSize display_overlay_for_;
};
}
