#include "ocr/ocr_line_geometry.h"
#include "core/east_asian_width.h"
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>

namespace jietuba {
namespace {
// Advances of printable ASCII in a typical proportional sans-serif face, in half ems, the unit
// in which a wide character is 2.
constexpr double PROPORTIONAL_ASCII[95] = {
    0.55, 0.56, 0.75, 1.15, 1.10, 1.71, 1.47, 0.42, 0.64, 0.64, 0.81, 1.27, 0.49, 0.73, 0.49, 0.67,  //  !"#$%&'()*+,-./
    1.10, 1.10, 1.10, 1.10, 1.10, 1.10, 1.10, 1.10, 1.10, 1.10, 0.49, 0.49, 1.27, 1.27, 1.27, 1.00,  // 0123456789:;<=>?
    1.97, 1.31, 1.24, 1.34, 1.42, 1.17, 1.10, 1.46, 1.43, 0.54, 0.86, 1.25, 1.03, 1.73, 1.47, 1.53,  // @ABCDEFGHIJKLMNO
    1.23, 1.53, 1.32, 1.20, 1.14, 1.41, 1.29, 1.88, 1.26, 1.22, 1.18, 0.58, 0.66, 0.58, 1.15, 0.97,  // PQRSTUVWXYZ[\]^_
    0.60, 1.06, 1.14, 0.96, 1.15, 1.08, 0.59, 1.15, 1.12, 0.46, 0.46, 1.00, 0.46, 1.69, 1.12, 1.14,  // `abcdefghijklmno
    1.14, 1.15, 0.68, 0.92, 0.62, 1.12, 0.98, 1.44, 0.96, 0.98, 0.95, 0.64, 0.50, 0.64, 1.27,        // pqrstuvwxyz{|}~
};

// Narrow characters' widths in a face, in half ems.
using NarrowWidth = double (*)(char32_t);

double fixed_narrow(char32_t) { return 1.0; }

double proportional_narrow(char32_t code_point) { return code_point >= 32 && code_point < 127 ? PROPORTIONAL_ASCII[code_point - 32] : 1.0; }

// Engines may give full-width punctuation in its half-width form; next to a wide character it
// is taken as the full-width mark it most likely is.
bool reads_wide(const QList<uint> &chars, qsizetype index) {
    if (is_east_asian_wide(chars[index])) return true;
    if (chars[index] >= 128 || std::string_view(",;:!?()").find(static_cast<char>(chars[index])) == std::string_view::npos) return false;
    const auto wide = [&](qsizetype at) { return at >= 0 && at < chars.size() && is_east_asian_wide(chars[at]); };
    return wide(index - 1) || wide(index + 1);
}

QVector<double> char_widths(const QList<uint> &chars, NarrowWidth narrow) {
    QVector<double> widths(chars.size());
    for (qsizetype i = 0; i < chars.size(); ++i) widths[i] = reads_wide(chars, i) ? 2.0 : narrow(chars[i]);
    return widths;
}

// A word's characters [first, first + count) and its extent in line pixels.
struct PlacedWord {
    qsizetype first, count;
    double start, end;
};

// The narrow widths that explain the words' extents better: each set is scaled to fit all words
// at once and the smaller mean relative miss wins. Proportional unless the words show otherwise.
// Words with wide characters weigh the same in both and their ink-tight boxes would skew the
// scale, so only the others take part.
NarrowWidth fitting_widths(const QList<uint> &chars, const QVector<PlacedWord> &words) {
    QVector<PlacedWord> narrow_words;
    for (const auto &word : words) {
        bool wide = false;
        for (qsizetype i = word.first; i < word.first + word.count && !wide; ++i) wide = reads_wide(chars, i);
        if (!wide) narrow_words.append(word);
    }
    if (narrow_words.size() < 2) return proportional_narrow;
    NarrowWidth best = proportional_narrow;
    double best_miss = std::numeric_limits<double>::infinity();
    for (const NarrowWidth narrow : {proportional_narrow, fixed_narrow}) {
        QVector<double> sums;
        double xy = 0, xx = 0;
        for (const auto &word : narrow_words) {
            double sum = 0;
            for (qsizetype i = word.first; i < word.first + word.count; ++i) sum += narrow(chars[i]);
            sums.append(sum);
            xy += sum * (word.end - word.start);
            xx += sum * sum;
        }
        const double scale = xy / xx;
        double miss = 0;
        for (qsizetype i = 0; i < narrow_words.size(); ++i) {
            const double extent = narrow_words[i].end - narrow_words[i].start;
            miss += std::abs(extent - scale * sums[i]) / std::max(extent, 1.0);
        }
        if (miss < best_miss) {
            best = narrow;
            best_miss = miss;
        }
    }
    return best;
}

QPointF unit(const QPointF &vector) {
    const double norm = std::hypot(vector.x(), vector.y());
    return norm > 0 ? vector / norm : QPointF();
}

double distance(const QPointF &a, const QPointF &b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

QPointF center_of(const QVector<QPointF> &box) {
    QPointF sum;
    for (const auto &point : box) sum += point;
    return box.isEmpty() ? sum : sum / box.size();
}

// Splits [start, end] (line pixels) among characters [first, last) by width.
void split_by_width(const QVector<double> &widths, qsizetype first, qsizetype last, double start, double end, double length, QVector<OcrCharSpan> &spans) {
    double total = 0;
    for (qsizetype i = first; i < last; ++i) total += widths[i];
    double at = start;
    for (qsizetype i = first; i < last; ++i) {
        const double next = total > 0 ? at + (end - start) * widths[i] / total : at;
        spans.append(OcrCharSpan{static_cast<float>(at / length), static_cast<float>((at + next) / 2 / length), static_cast<float>(next / length)});
        at = next;
    }
}
}

QTransform OcrLineFrame::to_source() const { return QTransform(along.x(), along.y(), across.x(), across.y(), origin.x(), origin.y()); }

OcrLineFrame line_frame(const QVector<QPointF> &box, bool vertical) {
    if (box.size() < 4) return {};
    // The origin, the end of the reading edge, the corner across from the origin and the one opposite.
    const int o = vertical ? 1 : 0, e = vertical ? 2 : 1, a = vertical ? 0 : 3, f = vertical ? 3 : 2;
    OcrLineFrame frame;
    frame.origin = box[o];
    frame.along = unit(box[e] - box[o]);
    frame.across = unit(box[a] - box[o]);
    frame.length = (distance(box[o], box[e]) + distance(box[a], box[f])) / 2;
    frame.thickness = (distance(box[o], box[a]) + distance(box[e], box[f])) / 2;
    // Collinear sides give no frame.
    if (std::abs(frame.along.x() * frame.across.y() - frame.along.y() * frame.across.x()) < 1e-6) return {};
    // Engines box straight text with a pixel or two of lean. Within a degree of the axes the frame
    // becomes the corners' bounding box, so upright text stays upright and fills as a rectangle.
    constexpr double LEAN = 0.0175;  // sin 1°
    const auto upright = [](const QPointF &v) -> std::optional<QPointF> {
        if (std::abs(v.y()) < LEAN) return QPointF(v.x() > 0 ? 1 : -1, 0);
        if (std::abs(v.x()) < LEAN) return QPointF(0, v.y() > 0 ? 1 : -1);
        return std::nullopt;
    };
    const auto along = upright(frame.along), across = upright(frame.across);
    if (along && across && (along->x() != 0) != (across->x() != 0)) {
        const QRectF bounds = QPolygonF(box).boundingRect();
        // The corner the two axes leave from.
        const double x_step = along->x() + across->x(), y_step = along->y() + across->y();
        frame.origin = QPointF(x_step > 0 ? bounds.left() : bounds.right(), y_step > 0 ? bounds.top() : bounds.bottom());
        frame.along = *along;
        frame.across = *across;
        frame.length = along->x() != 0 ? bounds.width() : bounds.height();
        frame.thickness = along->x() != 0 ? bounds.height() : bounds.width();
    }
    return frame;
}

QVector<OcrCharSpan> estimate_char_spans(const QString &text) {
    const auto chars = text.toUcs4();
    QVector<OcrCharSpan> spans;
    split_by_width(char_widths(chars, fixed_narrow), 0, chars.size(), 0, 1, 1, spans);
    return spans;
}

bool words_run_vertically(const QVector<QPointF> &box, const QVector<OcrWord> &words) {
    const auto across = line_frame(box, false), down = line_frame(box, true);
    if (!across.valid() || !down.valid()) return false;
    QVector<QPointF> centers;
    for (const auto &word : words)
        if (word.box.size() >= 4) centers.append(center_of(word.box));
    if (centers.size() >= 2) {
        const QPointF step = centers.back() - centers.front();
        const double sideways = std::abs(QPointF::dotProduct(step, across.along));
        const double downward = std::abs(QPointF::dotProduct(step, down.along));
        if (sideways != downward) return downward > sideways;
    }
    return across.thickness >= across.length * 1.5;
}

QVector<OcrCharSpan> spans_from_words(const QString &line_text, const OcrLineFrame &frame, const QVector<OcrWord> &words) {
    bool invertible = false;
    const auto to_line = frame.to_source().inverted(&invertible);
    if (!frame.valid() || !invertible || words.isEmpty()) return {};
    const auto chars = line_text.toUcs4();
    // In lines across with wide characters the engine's boxes start at the character but stop short
    // of its far side (a wide character's box is a fixed five eighths of the line height), so their
    // centres come about a tenth of the line height early; lines of narrow text alone are off by
    // too little to matter.
    const bool across = std::abs(frame.along.x()) > std::abs(frame.along.y());
    const double lead = across && std::any_of(chars.begin(), chars.end(), [](uint c) { return is_east_asian_wide(c); }) ? 0.1 * frame.thickness : 0;
    QVector<PlacedWord> placed;
    qsizetype at = 0;
    double reached = 0;
    for (const auto &word : words) {
        const auto word_chars = word.text.toUcs4();
        if (word_chars.isEmpty() || word.box.size() < 4) return {};
        while (at < chars.size() && QChar::isSpace(chars[at])) ++at;
        if (at + word_chars.size() > chars.size() || !std::equal(word_chars.begin(), word_chars.end(), chars.begin() + at)) return {};
        double low = frame.length, high = 0;
        for (const auto &corner : word.box) {
            const double x = to_line.map(corner).x() + lead;
            low = std::min(low, x);
            high = std::max(high, x);
        }
        // Kept in reading order and inside the line, so spans never overlap.
        low = std::clamp(low, reached, frame.length);
        high = std::clamp(high, low, frame.length);
        placed.append({at, word_chars.size(), low, high});
        reached = high;
        at += word_chars.size();
    }
    for (qsizetype i = at; i < chars.size(); ++i)
        if (!QChar::isSpace(chars[i])) return {};
    const auto widths = char_widths(chars, fitting_widths(chars, placed));
    QVector<OcrCharSpan> spans;
    spans.reserve(chars.size());
    double previous_end = 0;
    qsizetype next = 0;
    for (const auto &word : placed) {
        split_by_width(widths, next, word.first, previous_end, word.start, frame.length, spans);
        split_by_width(widths, word.first, word.first + word.count, word.start, word.end, frame.length, spans);
        previous_end = word.end;
        next = word.first + word.count;
    }
    split_by_width(widths, next, chars.size(), previous_end, frame.length, frame.length, spans);
    return spans;
}
}
