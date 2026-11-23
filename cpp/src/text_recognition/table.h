#pragma once
#include "ocr/snipping_tool_ocr.h"
#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <functional>
#include <optional>

// Rebuilds a table from recognized words and the screenshot's ruling lines, without an
// image model: what a table recognition service returns, at the cost of a pass over the
// OCR result.
//
// Columns come from two kinds of evidence: x ranges where the segments of different rows
// overlap, and whitespace no row covers. The second also parts cells the engine read as
// one line. Rows come from lines that share height, joined into one row where a cell's
// text wraps. Thin lines and the edges of filled rows separate rows and columns, and where
// one is missing between two ruled cells those cells are merged.
//
// The thresholds were tuned on rendered table screenshots with known structure (local
// notes, table_proto). Whether a line continues the row above is then scored by small
// decision trees trained on rendered and real web tables (scripts/table_model), which read
// the rules' own answer among their measurements. Positions are in image pixels.
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
    // Where the recognized text sits in the image; dropped once the text is replaced.
    QVector<QRectF> boxes;
};

// Cells cover the grid once each; a merged cell is listed at its top-left position.
struct Table {
    int rows = 0, columns = 0;
    QVector<Cell> cells;
    // As built: the first row holds the table's title and the last its note, each one cell across.
    int title_rows = 0, note_rows = 0;
    bool operator==(const Table &other) const;
};

// Recognized lines as words: the engine's own word boxes where it gives them, else Latin text
// split at spaces and CJK text into characters along the character positions, as the Snipping
// Tool engine reports its words; a vertical line without word boxes is one word.
QVector<Line> table_lines(const QVector<OcrLine> &lines);

// The table the lines form in image; empty without one.
Table build_table(const QImage &image, const QVector<Line> &lines);

// For training and checking the models (scripts/table_model). A visual row is one line of text
// across the table; a cell that wraps spreads one table row over several visual rows. The builder
// first decides which visual rows belong to the table (titles, captions and notes do not), then,
// for each pair of consecutive visual rows of the table, whether the lower one continues the table
// row of the upper one, from measurements the trees read.
constexpr int ROW_FEATURE_COUNT = 57;
extern const std::array<const char *, ROW_FEATURE_COUNT> ROW_FEATURE_NAMES;
struct RowPair {
    std::array<float, ROW_FEATURE_COUNT> features{};
    bool rules_join = false;  // what the hand-written rules decide
    bool fixed = false;       // next to a row the rules take for text centred over a row span
    bool joined = false;
};
constexpr int EXTENT_FEATURE_COUNT = 42;
extern const std::array<const char *, EXTENT_FEATURE_COUNT> EXTENT_FEATURE_NAMES;
struct ExtentRow {
    std::array<float, EXTENT_FEATURE_COUNT> features{};
    bool rules_in = false;  // inside the run of rows the rules keep
    bool in = false;
};
// Rows just outside the table's extent: above it a title, below it a note, when the trees keep them.
constexpr int CAPTION_FEATURE_COUNT = 30;
extern const std::array<const char *, CAPTION_FEATURE_COUNT> CAPTION_FEATURE_NAMES;
struct CaptionRow {
    int row = 0;  // index into all_rows
    bool above = true;
    std::array<float, CAPTION_FEATURE_COUNT> features{};
    bool kept = false;
};
struct RowTrace {
    QVector<QVector<QRectF>> all_rows;  // every visual row's segment boxes, before the extent is chosen
    QVector<QStringList> all_texts;
    QVector<ExtentRow> extent;          // extent[i]: all_rows[i]
    QVector<CaptionRow> captions;
    QVector<QVector<QRectF>> rows;      // the table's visual rows
    QVector<QStringList> texts;
    QVector<RowPair> pairs;  // pairs[i]: rows i and i + 1
};
struct RowControl {
    bool use_model = true;         // false: the rules alone join rows
    bool use_extent_model = true;  // false: the rules alone choose the table's rows
    bool use_caption_model = true; // false: no title or note rows
    QVector<int> given;            // per pair, 1 joins and 0 parts the rows, overriding model and rules
    QVector<int> given_extent;     // per visual row, 1 keeps and 0 drops it at either end
    QVector<int> given_caption;    // per visual row, 1 keeps it as title or note when it lies just outside
    RowTrace *trace = nullptr;
};
Table build_table(const QImage &image, const QVector<Line> &lines, const RowControl &control);

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
// The cell covering (row, column) given text; it no longer points into the image.
Table with_text(Table table, int row, int column, const QString &text);
// Without one row or column: merged cells across it shrink, cells only in it go.
Table without_row(const Table &table, int row);
Table without_column(const Table &table, int column);
// An empty row or column inserted before index row/column (rows/columns appends); merged
// cells it passes through grow over it.
Table with_row_inserted(const Table &table, int row);
Table with_column_inserted(const Table &table, int column);
// Rows row0..row1 and columns column0..column1, merged cells cut to them with their text.
Table part(const Table &table, int row0, int column0, int row1, int column1);
// Whether the rectangle holds two or more cells and cuts no merged cell.
bool can_merge(const Table &table, int row0, int column0, int row1, int column1);
// The rectangle as one cell. Every cell's text is kept, in reading order: merging mostly
// repairs text the recognition split, so keeping only the top-left would lose it.
Table merged(const Table &table, int row0, int column0, int row1, int column1);
// Merged cells meeting the rectangle split into single cells, each text in its top-left.
Table split(const Table &table, int row0, int column0, int row1, int column1);
// Tab-separated rows, quoted as spreadsheets copy them, parsed into a grid.
QVector<QStringList> parse_tsv(const QString &text);
// grid written from (row, column), the table growing to hold it. A position inside a merged
// cell but not at its top-left is skipped, so the merged cell takes one value.
Table pasted(Table table, int row, int column, const QVector<QStringList> &grid);
// Every cell's text passed through convert.
Table map_text(Table table, const std::function<QString(const QString &)> &convert);

// ---- Reading cells as a spreadsheet does ----
// A, B, ..., Z, AA, AB, ...
QString column_name(int column);
struct Number {
    double value = 0;
    // Digits after the point, as written.
    int decimals = 0;
    bool percent = false;
};
// Digits with optional thousands commas and decimals; a sign or accounting parentheses, a
// leading currency sign and a trailing % are allowed, full-width forms included. 12% reads
// as 12, so a column of percentages adds up in percent.
std::optional<Number> parse_number(const QString &text);
struct Summary {
    // Non-empty cells, and how many of them are numbers.
    int count = 0;
    int numbers = 0;
    double sum = 0;
    int decimals = 0;
    // Every number was a percentage.
    bool percent = false;
};
Summary summarize(const QStringList &texts);
// Thousands separated by commas.
QString format_number(double value, int decimals);
}
