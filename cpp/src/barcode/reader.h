#pragma once
#include <QImage>
#include <QPolygonF>
#include <QString>
#include <vector>

namespace jietuba {
// Which codes an image holds and where; display lives in result_window.
struct DecodedCode {
    QString text;
    // Human-readable format name such as "QR Code" or "Code 128".
    QString format_name;
    // The code's four corners in image pixels; a skewed code gives a skewed quadrilateral.
    QPolygonF outline;
};

// Every code in image, in reading order; empty when there is none. The image is
// decoded as Grayscale8: decoding only needs luminance, at a quarter of the memory.
std::vector<DecodedCode> read_codes(const QImage &image);
}
