#include "canvas/items/background_item.h"
#include "core/logger.h"
#include <QPixmap>
#include <algorithm>

namespace jietuba {
namespace {
// Box filter with the rounding of PIL's Image.reduce: (sum + n / 2) / n per channel.
QImage reduce(const QImage &rgba, int block) {
    const int w = rgba.width(), h = rgba.height();
    const int out_w = (w + block - 1) / block, out_h = (h + block - 1) / block;
    QImage out(out_w, out_h, QImage::Format_RGBA8888);
    for (int oy = 0; oy < out_h; ++oy) {
        const int y0 = oy * block, y1 = std::min(h, y0 + block);
        uchar *dst = out.scanLine(oy);
        for (int ox = 0; ox < out_w; ++ox) {
            const int x0 = ox * block, x1 = std::min(w, x0 + block);
            quint32 sum[4] = {0, 0, 0, 0};
            for (int y = y0; y < y1; ++y) {
                const uchar *src = rgba.constScanLine(y) + x0 * 4;
                for (int x = x0; x < x1; ++x, src += 4)
                    for (int c = 0; c < 4; ++c) sum[c] += src[c];
            }
            const quint32 count = static_cast<quint32>((x1 - x0) * (y1 - y0));
            for (int c = 0; c < 4; ++c) dst[ox * 4 + c] = static_cast<uchar>((sum[c] + count / 2) / count);
        }
    }
    return out;
}
}

BackgroundItem::BackgroundItem(const QImage &image, const QRectF &scene_rect) : scene_rect_(scene_rect), cached_image_(image) {
    setZValue(0);
    setPixmap(QPixmap::fromImage(image));
    // Scene coordinates via offset; boundingRect() stays in pixmap-local (0, 0, w, h).
    setOffset(scene_rect_.topLeft());
    setAcceptedMouseButtons(Qt::NoButton);
    log_debug(T("背景层创建: scene_rect={scene_rect}, offset={offset}", {{"scene_rect", geometry_text(scene_rect)}, {"offset", geometry_text(scene_rect_.topLeft())}}), "Canvas");
}

QImage BackgroundItem::image() {
    if (image_source_) return image_source_();
    if (has_cached_image_) return cached_image_;
    cached_image_ = pixmap().toImage();
    has_cached_image_ = true;
    return cached_image_;
}

void BackgroundItem::update_image(const QImage &image) { update_image(image, QPixmap::fromImage(image)); }

void BackgroundItem::update_image(const QImage &image, const QPixmap &pixmap) {
    image_source_ = nullptr;
    cached_image_ = image;
    has_cached_image_ = true;
    reduced_cache_key_.reset();
    reduced_cache_image_ = QImage();
    setPixmap(pixmap);
}

void BackgroundItem::set_image_source(std::function<QImage()> source) {
    image_source_ = std::move(source);
    cached_image_ = QImage();
    has_cached_image_ = false;
    reduced_cache_key_.reset();
    reduced_cache_image_ = QImage();
}

void BackgroundItem::set_display_pixmap(const QPixmap &pixmap, const QTransform &transform) {
    setPixmap(pixmap);
    setTransform(transform);
}

QImage BackgroundItem::reduced_image(int block_size) {
    block_size = std::max(2, block_size);
    if (reduced_cache_key_ == block_size && !reduced_cache_image_.isNull()) return reduced_cache_image_;
    const auto source = image();
    if (source.isNull()) return QImage();
    const auto rgba = source.convertToFormat(QImage::Format_RGBA8888);
    const auto reduced = reduce(rgba, block_size);
    // Replicated border: the mosaic tiles this image with bilinear sampling for
    // blur, which would otherwise blend in the opposite edge.
    const int width = reduced.width(), height = reduced.height();
    QImage padded(width + 2, height + 2, QImage::Format_RGBA8888);
    for (int y = 0; y < height + 2; ++y) {
        const int sy = std::clamp(y - 1, 0, height - 1);
        const auto *src = reinterpret_cast<const quint32 *>(reduced.constScanLine(sy));
        auto *dst = reinterpret_cast<quint32 *>(padded.scanLine(y));
        for (int x = 0; x < width + 2; ++x) dst[x] = src[std::clamp(x - 1, 0, width - 1)];
    }
    reduced_cache_key_ = block_size;
    reduced_cache_image_ = padded;
    return padded;
}

void BackgroundItem::release_image_cache() {
    cached_image_ = QImage();
    has_cached_image_ = false;
    reduced_cache_key_.reset();
    reduced_cache_image_ = QImage();
}
}
