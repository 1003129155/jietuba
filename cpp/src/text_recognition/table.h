#pragma once
#include "ocr/snipping_tool_ocr.h"
#include <QImage>
#include <QString>
#include <QVector>
#include <functional>

// Rebuilds a table from recognized words and the screenshot's ruling lines, without a
// table model: what a table recognition service returns, at the cost of a pass over the
// OCR result.
//
// Columns come from two kinds of evidence: x ranges where the segments of different rows
// overlap, and whitespace no row covers. The second also parts cells the engine read as
// one line. Rows come from lines that share height, joined into one row where a cell's
// text wraps. Thin lines and the edges of filled rows separate rows and columns, and where
// one is missing between two ruled cells those cells are merged.
//
// The thresholds were tuned on rendered table screenshots with known structure (local
// notes, table_proto). Positions are in image pixels.
namespace jietuba::text_table {
// One word, or one CJK character, as it appears in its line's text.
struct Word {
    QString text;
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

struct Line {
    QString text;
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    QVector<Word> words;
};

struct Cell {
    int row = 0, column = 0, row_span = 1, column_span = 1;
    QString text;
};

// Cells cover the grid once each; a merged cell is listed at its top-left position.
struct Table {
    int rows = 0, columns = 0;
    QVector<Cell> cells;
    bool operator==(const Table &other) const;
};

// Recognized lines as words: the engine's own word boxes where it gives them, else Latin text
// split at spaces and CJK text into characters along the character positions, as the Snipping
// Tool engine reports its words; a vertical line without word boxes is one word.
QVector<Line> table_lines(const QVector<OcrLine> &lines);

// The table the lines form in image; empty without one.
Table build_table(const QImage &image, const QVector<Line> &lines);

// Two or more rows and columns, and in most rows two or more filled cells: prose and
// single lists are not offered as tables.
bool is_table(const Table &table);

// The table for the clipboard: HTML keeps merged cells for spreadsheets and documents;
// tab-separated text puts a merged cell's text at its top-left and leaves the rest empty.
QString table_html(const Table &table);
QString table_tsv(const Table &table);

// ---- Edits, each returning the edited table ----
// The cell covering (row, column), or nullptr.
const Cell *cell_at(const Table &table, int row, int column);
// The cell covering (row, column) given text.
Table with_text(Table table, int row, int column, const QString &text);
// Without one row or column: merged cells across it shrink, cells only in it go.
Table without_row(const Table &table, int row);
Table without_column(const Table &table, int column);
// Rows row0..row1 and columns column0..column1, merged cells cut to them with their text.
Table part(const Table &table, int row0, int column0, int row1, int column1);
// Every cell's text passed through convert.
Table map_text(Table table, const std::function<QString(const QString &)> &convert);
}
