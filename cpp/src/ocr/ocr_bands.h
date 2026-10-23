#pragma once
#include "ocr/snipping_tool_ocr.h"
#include <QImage>
#include <QVector>
#include <atomic>
#include <functional>

namespace jietuba {
// One band of a tall image: rows [top, bottom) go to the engine, and the band keeps the lines whose
// vertical centre falls in [own_top, own_bottom), so every line comes from exactly one band.
struct OcrBand {
    int top = 0, bottom = 0;
    int own_top = 0, own_bottom = 0;
};

// Equal bands down an image height rows tall, none taller than max_height. Neighbours overlap by
// 2 * margin rows and split ownership in the middle, so a line up to 2 * margin tall that crosses a
// split lies whole in the band that keeps it. One band when the image fits.
QVector<OcrBand> split_into_bands(int height, int max_height, int margin);

// Recognizes image band by band with recognize and returns the lines in image coordinates; an image
// that fits goes through whole. Stops with no lines once cancelled is set.
QVector<OcrLine> recognize_in_bands(const QImage &image, int max_height, int margin,
                                    const std::function<QVector<OcrLine>(const QImage &)> &recognize,
                                    const std::atomic_bool *cancelled = nullptr);
}
