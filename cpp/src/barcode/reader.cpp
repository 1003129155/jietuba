#include "barcode/reader.h"
#include <ReadBarcode.h>
#include <algorithm>

namespace jietuba {
namespace {
QPolygonF outline(const ZXing::Position &position) {
    QPolygonF polygon;
    for (const auto &corner : {position.topLeft(), position.topRight(), position.bottomRight(), position.bottomLeft()}) polygon << QPointF(corner.x, corner.y);
    return polygon;
}

// Top to bottom, left to right within a row, so the numbers read 1, 2, 3. The
// decoder's order is unrelated to position, and a row's top edges rarely align
// exactly, so a code overlapping the current row vertically joins that row.
std::vector<DecodedCode> reading_order(std::vector<DecodedCode> codes) {
    std::stable_sort(codes.begin(), codes.end(), [](const DecodedCode &a, const DecodedCode &b) {
        return a.outline.boundingRect().top() < b.outline.boundingRect().top();
    });
    // (lowest bottom edge of the row so far, its codes)
    std::vector<std::pair<double, std::vector<DecodedCode>>> rows;
    for (auto &code : codes) {
        const auto rect = code.outline.boundingRect();
        if (!rows.empty() && rect.top() < rows.back().first) {
            rows.back().first = std::max(rows.back().first, rect.bottom());
            rows.back().second.push_back(std::move(code));
        } else {
            rows.push_back({rect.bottom(), {std::move(code)}});
        }
    }
    std::vector<DecodedCode> ordered;
    for (auto &[bottom, row] : rows) {
        std::stable_sort(row.begin(), row.end(), [](const DecodedCode &a, const DecodedCode &b) {
            return a.outline.boundingRect().left() < b.outline.boundingRect().left();
        });
        for (auto &code : row) ordered.push_back(std::move(code));
    }
    return ordered;
}
}

std::vector<DecodedCode> read_codes(const QImage &image) {
    const auto gray = image.convertToFormat(QImage::Format_Grayscale8);
    const ZXing::ImageView view(gray.constBits(), gray.width(), gray.height(), ZXing::ImageFormat::Lum, static_cast<int>(gray.bytesPerLine()));
    // The reference binding's defaults.
    const auto options = ZXing::ReaderOptions()
                             .tryRotate(true)
                             .tryDownscale(true)
                             .tryInvert(true)
                             .textMode(ZXing::TextMode::HRI)
                             .binarizer(ZXing::Binarizer::LocalAverage)
                             .isPure(false)
                             .maxNumberOfSymbols(0xff)
                             .eanAddOnSymbol(ZXing::EanAddOnSymbol::Ignore)
                             .returnErrors(false);
    std::vector<DecodedCode> codes;
    for (const auto &barcode : ZXing::ReadBarcodes(view, options))
        codes.push_back({QString::fromStdString(barcode.text()), QString::fromStdString(ZXing::ToString(barcode.format())), outline(barcode.position())});
    return reading_order(std::move(codes));
}
}
