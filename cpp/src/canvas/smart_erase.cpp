#include "canvas/smart_erase.h"
#include "core/native.h"
#include <algorithm>
#include <vector>

namespace jietuba {
namespace {
// The solver never samples further than this from the hole, so only that window is handed over
// (and converted, for formats it does not read directly).
constexpr int DONOR_REACH = 512;

bool is_cancelled(const void *context) { return static_cast<const std::atomic_bool *>(context)->load(std::memory_order_relaxed); }
}

SmartEraseResult smart_erase(const QImage &source, const QRect &rect, const QImage &mask, const std::atomic_bool *cancelled) {
    if (source.isNull() || rect.isEmpty() || !source.rect().contains(rect) || mask.size() != rect.size()) return {};
    const QRect window = rect.adjusted(-DONOR_REACH, -DONOR_REACH, DONOR_REACH, DONOR_REACH).intersected(source.rect());
    QImage converted;
    const QImage *pixels = &source;
    QRect area = window;
    bool bgra = true;
    switch (source.format()) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
        break;
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
        bgra = false;
        break;
    default:
        // Premultiplied colours would be averaged with their alpha folded in.
        converted = source.copy(window).convertToFormat(QImage::Format_ARGB32);
        pixels = &converted;
        area = QRect(QPoint(0, 0), window.size());
        break;
    }
    if (pixels->isNull()) return {};
    const auto stride = static_cast<size_t>(pixels->bytesPerLine());
    const uchar *first = pixels->constScanLine(area.top()) + static_cast<size_t>(area.left()) * 4;
    const size_t len = stride * static_cast<size_t>(area.height() - 1) + static_cast<size_t>(area.width()) * 4;
    const QImage gray = mask.format() == QImage::Format_Grayscale8 ? mask : mask.convertToFormat(QImage::Format_Grayscale8);
    std::vector<uint8_t> packed(static_cast<size_t>(rect.width()) * static_cast<size_t>(rect.height()));
    for (int y = 0; y < rect.height(); ++y) std::copy_n(gray.constScanLine(y), rect.width(), packed.begin() + static_cast<ptrdiff_t>(y) * rect.width());
    QImage out(rect.size(), bgra ? QImage::Format_ARGB32 : QImage::Format_RGBA8888);
    if (out.isNull()) return {};
    const QPoint hole = rect.topLeft() - window.topLeft();
    const int32_t solver = jb_smart_erase(first, len, static_cast<uint32_t>(area.width()), static_cast<uint32_t>(area.height()), stride, bgra,
                                          static_cast<uint32_t>(hole.x()), static_cast<uint32_t>(hole.y()), static_cast<uint32_t>(rect.width()),
                                          static_cast<uint32_t>(rect.height()), packed.data(), cancelled ? &is_cancelled : nullptr, cancelled,
                                          out.bits(), static_cast<size_t>(out.sizeInBytes()));
    if (solver < 0) return {};
    return {out, static_cast<SmartEraseSolver>(solver)};
}
}
