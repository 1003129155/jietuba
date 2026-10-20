#pragma once
#include "ocr/snipping_tool_ocr.h"
#include <QPointF>
#include <QString>
#include <QTransform>
#include <QVector>

namespace jietuba {
// A line's reading frame in source pixels: x runs from origin along the reading direction for
// length, y across the line for thickness. The corners need not form a rectangle; the frame is
// the parallelogram of the two edges leaving the origin, with the opposite edges averaged in.
struct OcrLineFrame {
    QPointF origin, along, across;
    double length = 0, thickness = 0;
    bool valid() const { return length > 0 && thickness > 0; }
    // Line coordinates to source pixels.
    QTransform to_source() const;
};

// The frame of four corners from top-left clockwise: from the top-left corner right, or from
// the top-right corner down when vertical. Within a degree of the axes it is the corners'
// bounding box. Invalid without four corners or with an empty side.
OcrLineFrame line_frame(const QVector<QPointF> &box, bool vertical);

// Positions estimated from character widths alone (wide : narrow = 2 : 1, half-width punctuation
// next to a wide character counting as wide), for lines whose engine gave none.
QVector<OcrCharSpan> estimate_char_spans(const QString &text);

// One oneocr word: its text and four corners in source pixels.
struct OcrWord {
    QString text;
    QVector<QPointF> box;
};

// Whether words run down rather than across: the direction from the first word to the last,
// or with fewer than two words, a line at least 1.5 times as tall as wide.
bool words_run_vertically(const QVector<QPointF> &box, const QVector<OcrWord> &words);

// Character positions from word boxes laid along the frame. Each word must appear in the line's
// text in order, with only whitespace between; the word's extent is split among its characters
// by width, wide ones as above and narrow ones proportional or fixed, whichever the words'
// extents fit better; whitespace fills the gaps. In a line across holding wide characters each
// box moves a tenth of the line height along the line: the engine's boxes there stop short of
// the characters' far sides. Empty when the words do not match the text.
QVector<OcrCharSpan> spans_from_words(const QString &line_text, const OcrLineFrame &frame, const QVector<OcrWord> &words);
}
