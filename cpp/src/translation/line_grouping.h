#pragma once
#include <QChar>
#include <QRectF>
#include <QString>
#include <QVector>
#include <array>

namespace jietuba {
// One recognized line as paragraph grouping sees it: its text and the horizontal box it occupies.
struct GroupingLine {
    QString text;
    QRectF rect;
};

// Scripts written without spaces between words, and their punctuation.
bool is_cjk(QChar c);

// A line and a line below it that it may continue into, with the measurements the scoring trees
// read (scripts/paragraph_model trains them from these same values).
constexpr int PARAGRAPH_FEATURE_COUNT = 41;
extern const std::array<const char *, PARAGRAPH_FEATURE_COUNT> PARAGRAPH_FEATURE_NAMES;
struct LinePair {
    int above = 0, below = 0;
    std::array<float, PARAGRAPH_FEATURE_COUNT> features{};
};
QVector<LinePair> paragraph_line_pairs(const QVector<GroupingLine> &lines);
// A short, capitalised line that does not end a sentence: a menu entry or a field label. Two of
// them stacked are never one paragraph, however the trees score them.
bool is_short_label(const GroupingLine &line);
// "1. Item", "2) item", "3、项目" or a bullet: a list item, which always starts a paragraph however
// the trees score it. A number followed by a digit ("1.5 million") is not a marker.
bool opens_list_item(const QString &text);
// Bullets and list numbers recognized as boxes of their own ("•", "2.", "三、"), within a line height
// left of an item's first line: for each line, the index of that item line, or -1. The marker joins
// the item's paragraph, and the item counts as opening a list item.
QVector<int> detached_list_markers(const QVector<GroupingLine> &lines);

// The lines gathered into paragraphs, each a list of indices into lines running top to bottom.
// The paragraphs come in no particular order. Every line is paired with the line it most likely
// continues into below it, as scored by trees trained on screenshots with known paragraphs, so
// the result does not depend on the order the lines were recognized in. Boxes must be at least
// 2 px each way.
QVector<QVector<int>> group_lines_into_paragraphs(const QVector<GroupingLine> &lines);
}
