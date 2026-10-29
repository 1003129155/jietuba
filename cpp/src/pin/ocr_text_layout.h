#pragma once
#include "ocr/ocr_line_geometry.h"
#include "ocr/ocr_manager.h"
#include <QPolygonF>
#include <QRectF>
#include <QTransform>
#include <optional>
#include <utility>
#include <vector>

namespace jietuba {
// One recognized line in image pixels: its reading frame and where each character sits along it.
struct OcrLayoutLine {
    QString text;
    QList<uint> chars;
    // Line pixels along the reading direction, one entry per character.
    std::vector<double> starts, centers, ends;
    OcrLineFrame frame;
    // Image pixels to line pixels.
    QTransform to_line;
    // The line's corners in image pixels.
    QPolygonF outline;
    QRectF bounds;
    // The corners' bounding box in the recognized image. Text joins into lines by it, in the
    // direction the pin showed when recognized, however the pin is turned.
    QRectF reading_bounds;
    // Positions estimated from character widths: the engine gave none.
    bool estimated = false;
    int length() const { return static_cast<int>(chars.size()); }
    // Characters [first, last).
    QString slice(int first, int last) const;
};

// A pin's recognized text in image pixels and the selection over it. Zooming, rotating and
// flipping the pin happen in the view, so nothing here changes with them.
class OcrTextLayout {
public:
    OcrTextLayout() = default;
    // to_image maps the recognized image's pixels to the pin's image pixels. Lines without
    // text or four corners are left out.
    explicit OcrTextLayout(const OcrResult &result, const QTransform &to_image = {});
    bool empty() const { return lines_.empty(); }
    const std::vector<OcrLayoutLine> &lines() const { return lines_; }
    // The nearest line within slack image pixels of the point.
    std::optional<int> line_at(const QPointF &point, double slack = 0) const;
    // The line whose text lies under the point: inside its box, and not where the character there
    // and those beside it are all whitespace.
    std::optional<int> text_line_at(const QPointF &point) const;
    // What a drag from `from` to `to` covers, judged in each line's own frame: a line holding
    // both ends gives the run between them; a line holding one end, the run from it towards the
    // other; a line lying wholly between the ends counts in full when it overlaps the end lines
    // or the drag along its direction, so short lines inside a paragraph are kept and another
    // column is not. A character counts when its centre falls inside the run.
    void select_between(const QPointF &from, const QPointF &to, double slack = 0);
    void select_line(int index);
    // The character of a line at point along it, or the nearest one; -1 for a line without any.
    int char_at(int line, const QPointF &point) const;
    // The word holding a line's character, as a double click picks one in a text editor.
    void select_word(int line, int index);
    // The lines a paragraph wraps into; without flow, where nothing tells paragraphs apart, the line.
    void select_paragraph(int line);
    void select_all();
    void clear_selection();
    bool has_selection() const;
    // A line's selected characters as [first, last); nullopt when none.
    std::optional<std::pair<int, int>> selection(int index) const;
    // Lines laid out from a document: line i joins line i + 1 with breaks[i], as the text runs,
    // rather than by where they sit. Needs one break fewer than there are lines.
    void set_flow(std::vector<QString> breaks);
    // Selected text in reading order: pieces on one line joined with spaces, lines with newlines.
    QString selected_text() const;
    // All text in reading order: blocks on one line joined with spaces, lines with separator.
    QString all_text(const QString &separator = "\n") const;
    // A line's selected run in image pixels; empty without a selection there.
    QPolygonF selection_outline(int index) const;
    // What the selection covers in image pixels.
    QRectF selection_bounds() const;
    // What the text covers in image pixels.
    QRectF bounds() const { return bounds_; }
private:
    // The line owning the point within slack across lines; within_length also bounds it along them.
    std::optional<int> nearest_line(const QPointF &point, double slack, bool within_length) const;
    std::vector<OcrLayoutLine> lines_;
    // Per line [first, last); first == last when nothing is selected there.
    std::vector<std::pair<int, int>> selected_;
    std::vector<QString> breaks_;
    QRectF bounds_;
};
}
