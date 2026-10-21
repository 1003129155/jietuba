#include "pin/ocr_text_layout.h"
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace jietuba {
namespace {
// Blocks within the tolerance of the current line's centre join it with spaces.
QString join_lines(const std::vector<std::tuple<QString, double, double>> &blocks, double tolerance_ratio, const QString &separator) {
    double total_height = 0;
    for (const auto &block : blocks) total_height += std::get<2>(block);
    const double tolerance = total_height / blocks.size() * tolerance_ratio;
    QStringList lines, current;
    std::optional<double> current_y;
    for (const auto &[text, center_y, height] : blocks) {
        if (!current_y) {
            current = {text};
            current_y = center_y;
        } else if (std::abs(center_y - *current_y) <= tolerance) {
            current.append(text);
        } else {
            lines.append(current.join(' '));
            current = {text};
            current_y = center_y;
        }
    }
    if (!current.isEmpty()) lines.append(current.join(' '));
    return lines.join(separator);
}

std::optional<OcrLayoutLine> make_line(const OcrLine &source, const QTransform &to_image) {
    if (source.text.isEmpty() || source.box.size() < 4) return std::nullopt;
    QVector<QPointF> box;
    for (const auto &point : source.box) box.append(to_image.map(point));
    auto frame = line_frame(box, source.vertical);
    if (!frame.valid()) {
        // Degenerate corners: the bounding box stands in.
        const QRectF rect = QPolygonF(box).boundingRect();
        frame = line_frame({rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()}, rect.height() >= rect.width() * 1.5);
        if (!frame.valid()) return std::nullopt;
    }
    OcrLayoutLine line;
    line.text = source.text;
    line.chars = source.text.toUcs4();
    line.frame = frame;
    line.to_line = frame.to_source().inverted();
    line.estimated = source.chars.size() != line.chars.size();
    const auto spans = line.estimated ? estimate_char_spans(source.text) : source.chars;
    for (const auto &span : spans) {
        line.starts.push_back(span.start * frame.length);
        line.centers.push_back(span.center * frame.length);
        line.ends.push_back(span.end * frame.length);
    }
    const auto to_source = frame.to_source();
    line.outline = QPolygonF({to_source.map(QPointF(0, 0)), to_source.map(QPointF(frame.length, 0)), to_source.map(QPointF(frame.length, frame.thickness)),
                              to_source.map(QPointF(0, frame.thickness))});
    line.bounds = line.outline.boundingRect();
    line.reading_bounds = QPolygonF(source.box).boundingRect();
    return line;
}
}

QString OcrLayoutLine::slice(int first, int last) const {
    first = std::clamp(first, 0, length());
    last = std::clamp(last, first, length());
    const auto part = chars.mid(first, last - first);
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(part.constData()), part.size());
}

OcrTextLayout::OcrTextLayout(const OcrResult &result, const QTransform &to_image) {
    if (result.code != 100) return;
    for (const auto &source : result.data) {
        auto line = make_line(source, to_image);
        if (!line) continue;
        bounds_ = bounds_.isNull() ? line->bounds : bounds_.united(line->bounds);
        lines_.push_back(std::move(*line));
    }
    selected_.assign(lines_.size(), {0, 0});
}

std::optional<int> OcrTextLayout::line_at(const QPointF &point, double slack) const {
    if (!bounds_.adjusted(-slack, -slack, slack, slack).contains(point)) return std::nullopt;
    for (int index = 0; index < static_cast<int>(lines_.size()); ++index) {
        const auto &line = lines_[index];
        if (!line.bounds.adjusted(-slack, -slack, slack, slack).contains(point)) continue;
        const auto local = line.to_line.map(point);
        if (local.x() >= -slack && local.x() <= line.frame.length + slack && local.y() >= -slack && local.y() <= line.frame.thickness + slack) return index;
    }
    return std::nullopt;
}

void OcrTextLayout::select_between(const QPointF &from, const QPointF &to, double slack) {
    constexpr double infinity = std::numeric_limits<double>::infinity();
    selected_.assign(lines_.size(), {0, 0});
    // Points whose positions along a line tell whether a line between the ends overlaps them.
    QVector<QPointF> reach{from, to};
    for (const auto end : {line_at(from, slack), line_at(to, slack)}) {
        if (!end) continue;
        const auto &frame = lines_[*end].frame;
        reach << frame.to_source().map(QPointF(0, 0)) << frame.to_source().map(QPointF(frame.length, 0));
    }
    for (size_t index = 0; index < lines_.size(); ++index) {
        const auto &line = lines_[index];
        const double length = line.frame.length, thickness = line.frame.thickness;
        auto a = line.to_line.map(from), b = line.to_line.map(to);
        // a comes before b across the line: above it for horizontal text.
        if (b.y() < a.y()) std::swap(a, b);
        const auto beside = [&](const QPointF &p) { return p.y() >= -slack && p.y() <= thickness + slack; };
        const auto on = [&](const QPointF &p) { return beside(p) && p.x() >= -slack && p.x() <= length + slack; };
        double low = 0, high = 0;
        if (beside(a) && beside(b)) {
            low = std::min(a.x(), b.x());
            high = std::max(a.x(), b.x());
        } else if (on(a)) {
            low = a.x();
            high = infinity;
        } else if (on(b)) {
            low = -infinity;
            high = b.x();
        } else if (a.y() < 0 && b.y() > thickness) {
            double first = infinity, last = -infinity;
            for (const auto &point : reach) {
                const double x = line.to_line.map(point).x();
                first = std::min(first, x);
                last = std::max(last, x);
            }
            if (last < 0 || first > length) continue;
            low = -infinity;
            high = infinity;
        } else {
            continue;
        }
        // Centres run in reading order, so the run is one slice.
        const auto begin = std::upper_bound(line.centers.begin(), line.centers.end(), low);
        const auto end = std::lower_bound(begin, line.centers.end(), high);
        if (begin < end) selected_[index] = {static_cast<int>(begin - line.centers.begin()), static_cast<int>(end - line.centers.begin())};
    }
}

void OcrTextLayout::select_line(int index) {
    clear_selection();
    if (index >= 0 && index < static_cast<int>(lines_.size())) selected_[index] = {0, lines_[index].length()};
}

void OcrTextLayout::select_all() {
    for (size_t index = 0; index < lines_.size(); ++index) selected_[index] = {0, lines_[index].length()};
}

void OcrTextLayout::clear_selection() { selected_.assign(lines_.size(), {0, 0}); }

bool OcrTextLayout::has_selection() const {
    return std::any_of(selected_.begin(), selected_.end(), [](const auto &range) { return range.first < range.second; });
}

std::optional<std::pair<int, int>> OcrTextLayout::selection(int index) const {
    if (index < 0 || index >= static_cast<int>(selected_.size()) || selected_[index].first >= selected_[index].second) return std::nullopt;
    return selected_[index];
}

QString OcrTextLayout::selected_text() const {
    std::vector<std::tuple<QString, double, double>> pieces;
    for (int index = 0; index < static_cast<int>(lines_.size()); ++index) {
        const auto range = selection(index);
        if (!range) continue;
        const auto &line = lines_[index];
        pieces.emplace_back(line.slice(range->first, range->second), line.reading_bounds.center().y(), line.reading_bounds.height());
    }
    if (pieces.empty()) return {};
    if (pieces.size() == 1) return std::get<0>(pieces[0]);
    return join_lines(pieces, 0.5, "\n");
}

QString OcrTextLayout::all_text(const QString &separator) const {
    if (lines_.empty()) return {};
    if (lines_.size() == 1) return lines_[0].text;
    std::vector<std::tuple<QString, double, double>> blocks;
    for (const auto &line : lines_) blocks.emplace_back(line.text, line.reading_bounds.center().y(), line.reading_bounds.height());
    return join_lines(blocks, 0.8, separator);
}

QPolygonF OcrTextLayout::selection_outline(int index) const {
    const auto range = selection(index);
    if (!range) return {};
    const auto &line = lines_[index];
    const double start = line.starts[range->first], end = line.ends[range->second - 1];
    const auto to_source = line.frame.to_source();
    return QPolygonF({to_source.map(QPointF(start, 0)), to_source.map(QPointF(end, 0)), to_source.map(QPointF(end, line.frame.thickness)),
                      to_source.map(QPointF(start, line.frame.thickness))});
}

QRectF OcrTextLayout::selection_bounds() const {
    QRectF bounds;
    for (int index = 0; index < static_cast<int>(lines_.size()); ++index) {
        const auto outline = selection_outline(index);
        if (!outline.isEmpty()) bounds = bounds.isNull() ? outline.boundingRect() : bounds.united(outline.boundingRect());
    }
    return bounds;
}
}
