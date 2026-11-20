#pragma once
#include <QChar>
#include <QRectF>
#include <QString>
#include <QVector>

namespace jietuba {
// One recognized line as paragraph grouping sees it: its text and the horizontal box it occupies.
struct GroupingLine {
    QString text;
    QRectF rect;
};

// Scripts written without spaces between words, and their punctuation.
bool is_cjk(QChar c);

// The lines gathered into paragraphs, each a list of indices into lines running top to bottom.
// The paragraphs come in no particular order. A line belongs to the paragraph of the lines it
// stands directly above or below, whatever order they were recognized in, so a sidebar or a
// picture caption beside a paragraph never breaks it. Boxes must be at least 2 px each way.
QVector<QVector<int>> group_lines_into_paragraphs(const QVector<GroupingLine> &lines);
}
