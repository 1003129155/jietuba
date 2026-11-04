#include "text_recognition/table.h"
#include <QPainter>
#include <QTest>

using namespace jietuba;
using namespace jietuba::text_table;

// Expected tables are written out by hand from the drawn layouts.
namespace {
constexpr double H = 20;

struct W {
    QString text;
    double x0, x1;
};

// One recognized line at height y: its words with their x ranges, the text joined by spaces.
Line line(std::initializer_list<W> words, double y, double h = H) {
    Line out;
    QStringList texts;
    out.x0 = 1e9;
    out.x1 = -1e9;
    for (const auto &w : words) {
        out.words.append({w.text, w.x0, w.x1, y, y + h});
        texts << w.text;
        out.x0 = std::min(out.x0, w.x0);
        out.x1 = std::max(out.x1, w.x1);
    }
    out.text = texts.join(' ');
    out.y0 = y;
    out.y1 = y + h;
    return out;
}

QImage blank(int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::white);
    return image;
}

// Character positions are floats; their pixel positions agree to well under a pixel.
bool near(double a, double b) { return std::abs(a - b) < 1e-3; }

QString text_at(const Table &table, int row, int column) {
    for (const auto &c : table.cells)
        if (c.row == row && c.column == column) return c.text;
    return "<none>";
}

QStringList grid(const Table &table) {
    QStringList rows;
    for (int r = 0; r < table.rows; ++r) {
        QStringList row;
        for (int c = 0; c < table.columns; ++c) row << text_at(table, r, c);
        rows << row.join('|');
    }
    return rows;
}

// Name | Qty | Price over three rows, columns parted by whitespace only.
QVector<Line> borderless_lines(double top = 10) {
    return {line({{"Name", 10, 60}}, top), line({{"Qty", 150, 180}}, top), line({{"Price", 290, 340}}, top),
            line({{"Apple", 10, 62}}, top + 40), line({{"12", 150, 170}}, top + 40), line({{"3.50", 290, 330}}, top + 40),
            line({{"Pear", 10, 50}}, top + 80), line({{"7", 150, 160}}, top + 80), line({{"12.00", 290, 340}}, top + 80)};
}

Table sample() {
    // A | B B
    // A | c d
    // e | f g
    return {3, 3, {{0, 0, 2, 1, "A"}, {0, 1, 1, 2, "B"}, {1, 1, 1, 1, "c"}, {1, 2, 1, 1, "d"}, {2, 0, 1, 1, "e"}, {2, 1, 1, 1, "f"}, {2, 2, 1, 1, "g"}}};
}
}

class TextTableTests : public QObject {
    Q_OBJECT
private slots:
    void whitespaceSeparatesColumns() {
        const auto table = build_table(blank(400, 130), borderless_lines());
        QCOMPARE(grid(table), QStringList({"Name|Qty|Price", "Apple|12|3.50", "Pear|7|12.00"}));
        QVERIFY(is_table(table));
    }

    void aTitleAboveIsLeftOut() {
        auto lines = borderless_lines(50);
        lines.prepend(line({{"Quarterly", 10, 100}, {"report", 106, 160}}, 6, 26));
        const auto table = build_table(blank(400, 170), lines);
        QCOMPARE(grid(table), QStringList({"Name|Qty|Price", "Apple|12|3.50", "Pear|7|12.00"}));
    }

    void ruledCellsWithoutALineBetweenMerge() {
        // Rules every 120 px across and every 50 px down; the header row has no line at x = 240.
        auto image = blank(400, 160);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        for (int y : {0, 50, 100, 150}) painter.drawLine(0, y, 360, y);
        for (int x : {0, 120, 360}) painter.drawLine(x, 0, x, 150);
        painter.drawLine(240, 50, 240, 150);
        painter.end();
        const QVector<Line> lines = {line({{"Region", 10, 70}}, 15), line({{"Sales", 140, 190}}, 15),
                                     line({{"East", 10, 50}}, 65), line({{"120", 140, 170}}, 65), line({{"135", 260, 290}}, 65),
                                     line({{"West", 10, 52}}, 115), line({{"98", 140, 160}}, 115), line({{"101", 260, 290}}, 115)};
        const auto table = build_table(image, lines);
        QCOMPARE(table.rows, 3);
        QCOMPARE(table.columns, 3);
        const auto *sales = cell_at(table, 0, 2);
        QVERIFY(sales);
        QCOMPARE(sales->text, QString("Sales"));
        QCOMPARE(sales->column, 1);
        QCOMPARE(sales->column_span, 2);
        QCOMPARE(text_at(table, 1, 2), QString("135"));
        QCOMPARE(text_at(table, 2, 1), QString("98"));
    }

    void aWrappedCellStaysInItsRow() {
        // The note wraps onto two lines; the other cells sit centred between them.
        const QVector<Line> lines = {line({{"Item", 10, 50}}, 10), line({{"Notes", 150, 200}}, 10), line({{"Qty", 330, 360}}, 10),
                                     line({{"Arrives", 150, 220}}, 45), line({{"Lamp", 10, 52}}, 57), line({{"3", 330, 340}}, 57),
                                     line({{"by", 150, 168}, {"Friday", 174, 215}}, 70),
                                     line({{"Desk", 10, 50}}, 110), line({{"Spare", 150, 200}}, 110), line({{"8", 330, 340}}, 110)};
        const auto table = build_table(blank(400, 140), lines);
        QCOMPARE(grid(table), QStringList({"Item|Notes|Qty", "Lamp|Arrives by Friday|3", "Desk|Spare|8"}));
    }

    void linesBetweenTheSameRulesShareARow() {
        // Rules under every row; the meaning of a+b wraps with more space between its lines than
        // whitespace alone would join. Without the rule at y = 140 the two rows stay apart.
        for (const bool missed : {false, true}) {
            auto image = blank(400, 250);
            QPainter painter(&image);
            painter.setPen(Qt::black);
            for (int y : {0, 40, 140, 190, 240})
                if (!(missed && y == 140)) painter.drawLine(0, y, 380, y);
            for (int x : {0, 120, 260, 380}) painter.drawLine(x, 0, x, 240);
            painter.end();
            const QVector<Line> lines = {line({{"Op", 10, 40}}, 10), line({{"Meaning", 130, 200}}, 10), line({{"Note", 270, 310}}, 10),
                                         line({{"a+b", 10, 45}}, 55), line({{"Adds", 130, 170}, {"two", 176, 205}}, 55), line({{"ok", 270, 290}}, 55),
                                         line({{"numbers", 130, 200}}, 91),
                                         line({{"a-b", 10, 45}}, 155), line({{"Subtracts", 130, 210}}, 155), line({{"ok", 270, 290}}, 155),
                                         line({{"a*b", 10, 45}}, 205), line({{"Multiplies", 130, 215}}, 205), line({{"no", 270, 290}}, 205)};
            const auto table = build_table(image, lines);
            QCOMPARE(grid(table), QStringList({"Op|Meaning|Note", "a+b|Adds two numbers|ok", "a-b|Subtracts|ok", "a*b|Multiplies|no"}));
        }
    }

    void aBoxAroundInlineCodeSeparatesNothing() {
        // Each line of the wrapped example is boxed as code; the boxes' edges lie between its lines.
        auto image = blank(400, 250);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        for (int y : {0, 40, 140, 190, 240}) painter.drawLine(0, y, 380, y);
        for (int x : {0, 160, 380}) painter.drawLine(x, 0, x, 240);
        painter.drawRect(168, 52, 48, 25);
        painter.drawRect(168, 88, 48, 25);
        painter.end();
        const QVector<Line> lines = {line({{"Use", 10, 45}}, 10), line({{"Example", 170, 240}}, 10),
                                     line({{"Assign", 10, 70}}, 55), line({{"x", 172, 182}, {"=", 188, 196}, {"1", 202, 210}}, 55),
                                     line({{"y", 172, 182}, {"=", 188, 196}, {"2", 202, 210}}, 91),
                                     line({{"Print", 10, 55}}, 155), line({{"show", 170, 210}}, 155),
                                     line({{"Exit", 10, 45}}, 205), line({{"quit", 170, 205}}, 205)};
        const auto table = build_table(image, lines);
        QCOMPARE(grid(table), QStringList({"Use|Example", "Assign|x = 1 y = 2", "Print|show", "Exit|quit"}));
    }

    void aSecondHeaderRowUnderAGroupKeepsItsRow() {
        // Name and Status span both header rows; Sales heads Stock and Date. One filled band holds
        // the whole header, with no rule between its two rows.
        auto image = blank(440, 210);
        QPainter painter(&image);
        painter.setPen(Qt::black);
        for (int y : {0, 80, 120, 160, 200}) painter.drawLine(0, y, 420, y);
        painter.end();
        const QVector<Line> lines = {line({{"Sales", 160, 210}}, 8), line({{"Name", 10, 60}}, 30), line({{"Status", 320, 380}}, 30),
                                     line({{"Stock", 110, 160}}, 52), line({{"Date", 220, 260}}, 52),
                                     line({{"Ann", 10, 45}}, 90), line({{"12", 110, 130}}, 90), line({{"May", 220, 255}}, 90), line({{"Open", 320, 365}}, 90),
                                     line({{"Bo", 10, 35}}, 130), line({{"7", 110, 120}}, 130), line({{"June", 220, 260}}, 130), line({{"Shut", 320, 360}}, 130),
                                     line({{"Cy", 10, 35}}, 170), line({{"30", 110, 130}}, 170), line({{"July", 220, 258}}, 170), line({{"Open", 320, 365}}, 170)};
        const auto table = build_table(image, lines);
        QCOMPARE(table.rows, 5);
        QCOMPARE(text_at(table, 1, 1), QString("Stock"));
        QCOMPARE(text_at(table, 1, 2), QString("Date"));
        QCOMPARE(text_at(table, 2, 0), QString("Ann"));
    }

    void aLineCutByTheSelectionEdgeIsLeftOut() {
        // The bottom half of a line above the table shows at the top edge; a tight crop through
        // the header leaves it at full height.
        auto lines = borderless_lines();
        lines.prepend(line({{"Prices", 10, 70}, {"of", 76, 92}, {"fruit", 98, 140}}, 0, 9));
        QCOMPARE(grid(build_table(blank(400, 130), lines)), QStringList({"Name|Qty|Price", "Apple|12|3.50", "Pear|7|12.00"}));
        QCOMPARE(grid(build_table(blank(400, 110), borderless_lines(0))), QStringList({"Name|Qty|Price", "Apple|12|3.50", "Pear|7|12.00"}));
    }

    void cellsReadAsOneLineArePartedAtTheColumnGap() {
        const QVector<Line> lines = {line({{"Name", 10, 50}}, 10), line({{"Qty", 72, 100}}, 10),
                                     line({{"Apple", 10, 60}, {"12", 72, 90}}, 50),
                                     line({{"Pear", 10, 45}}, 90), line({{"7", 72, 80}}, 90)};
        const auto table = build_table(blank(200, 130), lines);
        QCOMPARE(grid(table), QStringList({"Name|Qty", "Apple|12", "Pear|7"}));
    }

    void proseIsNotATable() {
        QVector<Line> lines;
        for (int k = 0; k < 3; ++k) {
            const double y = 10 + k * 28;
            lines << line({{"The", 10, 40}, {"quick", 46, 90}, {"brown", 96, 145}, {"fox", 151, 175}, {"jumps", 181, 230}, {"over", 236, 270},
                           {"the", 276, 300}, {"lazy", 306, 340}, {"dog", 346, 375}},
                          y);
        }
        QVERIFY(!is_table(build_table(blank(400, 100), lines)));
        QVERIFY(build_table(blank(10, 10), {}).cells.isEmpty());
    }

    void wordsComeFromTheEngineOrFromCharacterPositions() {
        OcrLine engine;
        engine.text = "Apple 12";
        engine.box = {QPointF(10, 0), QPointF(90, 0), QPointF(90, 20), QPointF(10, 20)};
        engine.words = {{"Apple", {QPointF(10, 1), QPointF(60, 1), QPointF(60, 19), QPointF(10, 19)}},
                        {"12", {QPointF(72, 2), QPointF(90, 2), QPointF(90, 18), QPointF(72, 18)}}};
        OcrLine spans;
        spans.text = QString::fromUtf8("ab 汉字");
        spans.box = {QPointF(0, 30), QPointF(100, 30), QPointF(100, 50), QPointF(0, 50)};
        spans.chars = {{0, 0.05f, 0.1f}, {0.1f, 0.15f, 0.2f}, {0.2f, 0.25f, 0.3f}, {0.3f, 0.45f, 0.6f}, {0.6f, 0.75f, 0.9f}};
        OcrLine vertical;
        vertical.text = "Up";
        vertical.vertical = true;
        vertical.box = {QPointF(200, 0), QPointF(220, 0), QPointF(220, 60), QPointF(200, 60)};
        const auto lines = table_lines({engine, spans, vertical, OcrLine{}});
        QCOMPARE(lines.size(), 3);
        QCOMPARE(lines[0].words.size(), 2);
        QCOMPARE(lines[0].words[1].text, QString("12"));
        QCOMPARE(lines[0].words[1].x0, 72.0);
        QCOMPARE(lines[0].words[1].y0, 2.0);
        // Latin letters join into words at spaces; each CJK character is a word.
        QCOMPARE(lines[1].words.size(), 3);
        QCOMPARE(lines[1].words[0].text, QString("ab"));
        QVERIFY(near(lines[1].words[0].x0, 0));
        QVERIFY(near(lines[1].words[0].x1, 20));
        QCOMPARE(lines[1].words[1].text, QString::fromUtf8("汉"));
        QVERIFY(near(lines[1].words[1].x0, 30));
        QVERIFY(near(lines[1].words[2].x1, 90));
        QCOMPARE(lines[1].words[2].y0, 30.0);
        QCOMPARE(lines[2].words.size(), 1);
        QCOMPARE(lines[2].words[0].text, QString("Up"));
        QCOMPARE(lines[2].words[0].y1, 60.0);
    }

    void editsKeepMergedCells() {
        const auto table = sample();
        QCOMPARE(cell_at(table, 1, 0)->text, QString("A"));
        QCOMPARE(cell_at(table, 0, 2)->text, QString("B"));
        QVERIFY(!cell_at(table, 3, 0));
        QCOMPARE(cell_at(with_text(table, 1, 0, "x"), 0, 0)->text, QString("x"));

        // Without the first row, A spans one row and B, only in it, is gone.
        const auto no_row = without_row(table, 0);
        QCOMPARE(no_row.rows, 2);
        QCOMPARE(grid(no_row), QStringList({"A|c|d", "e|f|g"}));
        QCOMPARE(cell_at(no_row, 0, 0)->row_span, 1);
        // Without the middle column, B spans one column.
        const auto no_column = without_column(table, 1);
        QCOMPARE(no_column.columns, 2);
        QCOMPARE(grid(no_column), QStringList({"A|B", "<none>|d", "e|g"}));
        QCOMPARE(cell_at(no_column, 1, 0)->text, QString("A"));
        QCOMPARE(cell_at(no_column, 0, 1)->column_span, 1);
        QVERIFY(without_row(table, 5) == table);
        QVERIFY(without_column(table, -1) == table);

        // A part cuts merged cells to it and keeps their text.
        QCOMPARE(grid(part(table, 1, 1, 2, 2)), QStringList({"c|d", "f|g"}));
        const auto corner = part(table, 0, 0, 1, 1);
        QCOMPARE(grid(corner), QStringList({"A|B", "<none>|c"}));
        QCOMPARE(cell_at(corner, 1, 0)->row_span, 2);
        QCOMPARE(part(table, 0, 2, 0, 2).cells.size(), 1);
        QCOMPARE(part(table, 0, 2, 0, 2).cells[0].text, QString("B"));
        QVERIFY(part(table, 2, 2, 1, 1).cells.isEmpty());

        const auto upper = map_text(table, [](const QString &t) { return t.toUpper(); });
        QCOMPARE(text_at(upper, 1, 2), QString("D"));
    }

    void insertedRowsAndColumnsGrowTheMergedCellsTheyPass() {
        const auto table = sample();
        // Between the two rows A spans: A grows, the other columns get empty cells.
        const auto inside = with_row_inserted(table, 1);
        QCOMPARE(inside.rows, 4);
        QCOMPARE(grid(inside), QStringList({"A|B|<none>", "<none>||", "<none>|c|d", "e|f|g"}));
        QCOMPARE(cell_at(inside, 2, 0)->row_span, 3);
        QCOMPARE(grid(with_row_inserted(table, 0)), QStringList({"||", "A|B|<none>", "<none>|c|d", "e|f|g"}));
        QCOMPARE(grid(with_row_inserted(table, 3)).last(), QString("||"));
        // B spans columns 1-2, so a column before 2 widens it.
        const auto column = with_column_inserted(table, 2);
        QCOMPARE(column.columns, 4);
        QCOMPARE(grid(column), QStringList({"A|B|<none>|<none>", "<none>|c||d", "e|f||g"}));
        QCOMPARE(cell_at(column, 0, 3)->column_span, 3);
        QCOMPARE(grid(with_column_inserted(table, 0)), QStringList({"|A|B|<none>", "|<none>|c|d", "|e|f|g"}));
    }

    void mergingKeepsEveryTextAndSplittingKeepsItTopLeft() {
        const auto table = sample();
        QVERIFY(can_merge(table, 1, 1, 2, 2));
        QVERIFY(can_merge(table, 0, 0, 2, 0));
        // A rectangle cutting B, or holding one cell, does not merge.
        QVERIFY(!can_merge(table, 0, 0, 1, 1));
        QVERIFY(!can_merge(table, 0, 1, 0, 2));
        QVERIFY(!can_merge(table, 2, 2, 2, 2));
        QVERIFY(!can_merge(table, 2, 2, 3, 3));
        QVERIFY(merged(table, 0, 0, 1, 1) == table);

        const auto block = merged(table, 1, 1, 2, 2);
        QCOMPARE(grid(block), QStringList({"A|B|<none>", "<none>|c d f g|<none>", "e|<none>|<none>"}));
        QCOMPARE(cell_at(block, 2, 2)->row_span, 2);
        QCOMPARE(block.cells.size(), 4);
        // No space where CJK text meets; empty cells add nothing.
        const Table words{1, 4, {{0, 0, 1, 1, "Total"}, {0, 1, 1, 1, " "}, {0, 2, 1, 1, "amount"}, {0, 3, 1, 1, "金额"}}};
        QCOMPARE(merged(words, 0, 0, 0, 3).cells[0].text, QString::fromUtf8("Total amount金额"));

        QCOMPARE(grid(split(table, 0, 0, 0, 0)), QStringList({"A|B|<none>", "|c|d", "e|f|g"}));
        QCOMPARE(grid(split(table, 0, 0, 2, 2)), QStringList({"A|B|", "|c|d", "e|f|g"}));
        QVERIFY(split(table, 2, 0, 2, 2) == table);
    }

    void pastedTextFillsFromACellAndGrowsTheTable() {
        QCOMPARE(parse_tsv("a\tb\r\nc\t\"x\ny\"\n"), QVector<QStringList>({{"a", "b"}, {"c", "x\ny"}}));
        QCOMPARE(parse_tsv("\"say \"\"hi\"\"\""), QVector<QStringList>({{"say \"hi\""}}));
        QCOMPARE(parse_tsv("a\n\nb"), QVector<QStringList>({{"a"}, {""}, {"b"}}));
        QCOMPARE(parse_tsv("a\"b"), QVector<QStringList>({{"a\"b"}}));
        QVERIFY(parse_tsv("").isEmpty());

        const auto table = sample();
        const auto grown = pasted(table, 2, 1, {{"1", "2", "3"}, {"4"}});
        QCOMPARE(grown.rows, 4);
        QCOMPARE(grown.columns, 4);
        QCOMPARE(grid(grown), QStringList({"A|B|<none>|", "<none>|c|d|", "e|1|2|3", "|4||"}));
        // Only the merged cell's top-left takes a value.
        const auto over = pasted(table, 0, 1, {{"x", "y"}});
        QCOMPARE(cell_at(over, 0, 2)->text, QString("x"));
        QCOMPARE(over.columns, 3);
        QVERIFY(pasted(table, -1, 0, {{"x"}}) == table);
    }

    void cellsKeepTheirPlaceInTheImageUntilTheirTextChanges() {
        const QRectF a(0, 0, 10, 10), c(0, 20, 10, 10), d(20, 20, 10, 10);
        const Table table{2, 2, {{0, 0, 1, 1, "a", {a}}, {0, 1, 1, 1, "", {}}, {1, 0, 1, 1, "c", {c}}, {1, 1, 1, 1, "d", {d}}}};
        // Merging gathers every box; splitting leaves them with the top-left.
        const auto bottom = merged(table, 1, 0, 1, 1);
        QCOMPARE(cell_at(bottom, 1, 1)->boxes, QVector<QRectF>({c, d}));
        QCOMPARE(cell_at(split(bottom, 1, 0, 1, 1), 1, 0)->boxes, QVector<QRectF>({c, d}));
        QVERIFY(cell_at(split(bottom, 1, 0, 1, 1), 1, 1)->boxes.isEmpty());
        // New text points nowhere; the same text keeps its place.
        QVERIFY(cell_at(with_text(table, 1, 1, "x"), 1, 1)->boxes.isEmpty());
        QCOMPARE(cell_at(with_text(table, 1, 1, "d"), 1, 1)->boxes, QVector<QRectF>({d}));
        QVERIFY(cell_at(pasted(table, 1, 0, {{"y"}}), 1, 0)->boxes.isEmpty());
        QCOMPARE(cell_at(pasted(table, 1, 0, {{"c"}}), 1, 0)->boxes, QVector<QRectF>({c}));
        QCOMPARE(cell_at(with_row_inserted(table, 0), 1, 0)->boxes, QVector<QRectF>({a}));
        QCOMPARE(cell_at(part(table, 1, 1, 1, 1), 0, 0)->boxes, QVector<QRectF>({d}));
        // Cells built from recognized words carry where those words sit.
        const auto built = build_table(blank(400, 140), borderless_lines());
        QCOMPARE(cell_at(built, 1, 0)->boxes, QVector<QRectF>({QRectF(QPointF(10, 50), QPointF(62, 70))}));
    }

    void clipboardFormats() {
        const Table table{2, 2, {{0, 0, 1, 2, "a"}, {1, 0, 1, 1, "b<"}, {1, 1, 1, 1, "c\td"}}};
        QCOMPARE(table_html(table), QString("<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" style=\"border-collapse:collapse\">"
                                            "<tr><td colspan=\"2\">a</td></tr><tr><td>b&lt;</td><td>c\td</td></tr></table>"));
        // A merged cell's text at its top-left; tabs inside a cell would start a new column.
        QCOMPARE(table_tsv(table), QString("a\t\nb<\tc d"));
        const Table tall{2, 1, {{0, 0, 2, 1, "x"}}};
        QCOMPARE(table_html(tall), QString("<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" style=\"border-collapse:collapse\">"
                                           "<tr><td rowspan=\"2\">x</td></tr><tr></tr></table>"));
    }

    void onlyRowsOfSeveralCellsMakeATable() {
        QVERIFY(is_table({2, 2, {{0, 0, 1, 1, "a"}, {0, 1, 1, 1, "b"}, {1, 0, 1, 1, "c"}, {1, 1, 1, 1, "d"}}}));
        // One full row of three is not most rows.
        QVERIFY(!is_table({3, 2, {{0, 0, 1, 1, "a"}, {0, 1, 1, 1, "b"}, {1, 0, 1, 1, "c"}, {1, 1, 1, 1, ""}, {2, 0, 1, 1, "e"}, {2, 1, 1, 1, ""}}}));
        QVERIFY(!is_table({2, 1, {{0, 0, 1, 1, "a"}, {1, 0, 1, 1, "b"}}}));
    }

    void columnsAreLetteredAsInASpreadsheet() {
        QCOMPARE(column_name(0), QString("A"));
        QCOMPARE(column_name(25), QString("Z"));
        QCOMPARE(column_name(26), QString("AA"));
        QCOMPARE(column_name(51), QString("AZ"));
        QCOMPARE(column_name(52), QString("BA"));
        QCOMPARE(column_name(701), QString("ZZ"));
        QCOMPARE(column_name(702), QString("AAA"));
    }

    void cellsReadAsNumbersTheWaySpreadsheetsReadThem() {
        const auto value = [](const QString &text) { return parse_number(text).value_or(Number{-9999, -1, false}); };
        QCOMPARE(value("42").value, 42.0);
        QCOMPARE(value(" 1,234.50 ").value, 1234.5);
        QCOMPARE(value("1,234.50").decimals, 2);
        QCOMPARE(value("-3").value, -3.0);
        QCOMPARE(value("+3").value, 3.0);
        QCOMPARE(value("−3").value, -3.0);
        QCOMPARE(value("(1,200)").value, -1200.0);
        QCOMPARE(value("$5").value, 5.0);
        QCOMPARE(value("-$5").value, -5.0);
        QCOMPARE(value("¥-5").value, -5.0);
        QCOMPARE(value("€ 7.5").value, 7.5);
        // Full-width digits, currency and percent, as recognition gives them in CJK text.
        QCOMPARE(value("￥１２３").value, 123.0);
        QVERIFY(value("５０％").percent);
        QCOMPARE(value("12.5%").value, 12.5);
        QVERIFY(value("12.5%").percent);
        for (const char *text : {"", "abc", "12a", "1,23", "12,34.5", "1.2.3", "$", "1.", "--5", "١٢", "5 kg"})
            QVERIFY2(!parse_number(QString::fromUtf8(text)), text);
    }

    void aSelectionSumsAndAveragesItsNumbers() {
        auto s = summarize({"10", "20.5", "", "n/a", " "});
        QCOMPARE(s.count, 3);
        QCOMPARE(s.numbers, 2);
        QCOMPARE(s.sum, 30.5);
        QCOMPARE(s.decimals, 1);
        QVERIFY(!s.percent);
        s = summarize({"10%", "２０％"});
        QVERIFY(s.percent);
        QCOMPARE(s.sum, 30.0);
        // Text only: counted, nothing summed, not percent.
        s = summarize({"a", "b"});
        QCOMPARE(s.count, 2);
        QCOMPARE(s.numbers, 0);
        QVERIFY(!s.percent);
        QCOMPARE(format_number(1234567.891, 2), QString("1,234,567.89"));
        QCOMPARE(format_number(-1234, 0), QString("-1,234"));
        QCOMPARE(format_number(999, 0), QString("999"));
        QCOMPARE(format_number(0.1 + 0.2, 1), QString("0.3"));
        QCOMPARE(format_number(-0.001, 2), QString("0.00"));
    }
};

QTEST_MAIN(TextTableTests)
#include "test_text_table.moc"
