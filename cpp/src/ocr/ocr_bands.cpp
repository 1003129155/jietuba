#include "ocr/ocr_bands.h"
#include <algorithm>

namespace jietuba {
QVector<OcrBand> split_into_bands(int height, int max_height, int margin) {
    if (height <= max_height || max_height <= 0) return {OcrBand{0, height, 0, height}};
    margin = std::clamp(margin, 0, max_height / 4);
    const int core = max_height - 2 * margin;
    const int count = (height + core - 1) / core;
    QVector<OcrBand> bands;
    for (int i = 0; i < count; ++i) {
        const int own_top = static_cast<int>(static_cast<long long>(height) * i / count);
        const int own_bottom = static_cast<int>(static_cast<long long>(height) * (i + 1) / count);
        bands.append(OcrBand{std::max(0, own_top - margin), std::min(height, own_bottom + margin), own_top, own_bottom});
    }
    return bands;
}

QVector<OcrLine> recognize_in_bands(const QImage &image, int max_height, int margin,
                                    const std::function<QVector<OcrLine>(const QImage &)> &recognize,
                                    const std::atomic_bool *cancelled) {
    const auto bands = split_into_bands(image.height(), max_height, margin);
    if (bands.size() == 1) return recognize(image);
    QVector<OcrLine> lines;
    for (const auto &band : bands) {
        if (cancelled && cancelled->load()) return {};
        for (auto line : recognize(image.copy(0, band.top, image.width(), band.bottom - band.top))) {
            if (line.box.isEmpty()) continue;
            double centre = 0;
            for (auto &point : line.box) {
                point.ry() += band.top;
                centre += point.y();
            }
            centre /= line.box.size();
            if (centre >= band.own_top && centre < band.own_bottom) lines.append(line);
        }
    }
    return lines;
}
}
