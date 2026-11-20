#include "translation/line_grouping.h"
#include <QtTest>
#include <algorithm>

using namespace jietuba;

namespace {
GroupingLine line(const QString &text, double x, double y, double width, double height = 14) { return {text, QRectF(x, y, width, height)}; }

// Groups come in no particular order: compare them ordered by their first index.
QVector<QVector<int>> grouped(const QVector<GroupingLine> &lines) {
    auto groups = group_lines_into_paragraphs(lines);
    std::sort(groups.begin(), groups.end(), [](const QVector<int> &a, const QVector<int> &b) { return a.first() < b.first(); });
    return groups;
}

using Groups = QVector<QVector<int>>;
}

class LineGroupingTests : public QObject {
    Q_OBJECT
private slots:
    void emptyInputGivesNoGroups() { QVERIFY(group_lines_into_paragraphs({}).isEmpty()); }

    void aWrappedParagraphIsOneGroup() {
        // Gaps of 2 px against a reach of 7 px (half the 14 px line height); the third line is 2 px off the margin.
        QCOMPARE(grouped({line("The quick brown fox", 10, 10, 300), line("jumps over the lazy dog", 10, 26, 280), line("and keeps running.", 12, 42, 260)}),
                 (Groups{{0, 1, 2}}));
    }

    void linesComeBackTopToBottomWhateverOrderTheyWereGivenIn() {
        // Index 1 is the top line, 2 the middle one, 0 the bottom one.
        QCOMPARE(grouped({line("c", 10, 42, 300), line("a", 10, 10, 300), line("b", 10, 26, 300)}), (Groups{{1, 2, 0}}));
    }

    void linesFarApartAreSeparate() { QCOMPARE(grouped({line("Username", 10, 10, 300), line("Enter the name you signed up with.", 10, 60, 300)}), (Groups{{0}, {1}})); }

    void sideBySideCellsStayApart() { QCOMPARE(grouped({line("Name", 10, 10, 100), line("Value", 400, 10, 100)}), (Groups{{0}, {1}})); }

    void aColumnBesideAParagraphDoesNotBreakIt() {
        // The caption's lines fall between the paragraph's in height. Taken in top-down order the two interleave; each stays whole here.
        const auto groups = grouped({
            line("one one one one one", 10, 10, 300), line("caption a", 400, 18, 100), line("two two two two two", 10, 26, 300),
            line("caption b", 400, 34, 100), line("three three three three", 10, 42, 300), line("caption c", 400, 50, 100),
            line("four four four", 10, 58, 200),
        });
        QCOMPARE(groups, (Groups{{0, 2, 4, 6}, {1, 3, 5}}));
    }

    void centredLinesBelongTogether() {
        // Centres all at x = 200; the left and right edges differ by 15 to 30 px against a reach of 7.
        QCOMPARE(grouped({line("a", 100, 10, 200), line("b", 130, 26, 140), line("c", 115, 42, 170)}), (Groups{{0, 1, 2}}));
    }

    void rightAlignedLinesBelongTogether() {
        // Right edges all at x = 310; left edges 50 to 70 px apart, centres 10 to 25 px apart.
        QCOMPARE(grouped({line("a", 210, 10, 100), line("b", 160, 26, 150), line("c", 230, 42, 80)}), (Groups{{0, 1, 2}}));
    }

    void anIndentedFirstLineJoinsTheLinesBelowIt() {
        // Raggedly set: the first line starts 30 px (2.1 line heights) right of the others and ends 10 px past them.
        QCOMPARE(grouped({line("It was the best of times, it was the", 40, 10, 260), line("worst of times, it was the age of", 10, 26, 280), line("wisdom.", 10, 42, 150)}),
                 (Groups{{0, 1, 2}}));
        // Justified: the first line is flush with the others on the right.
        QCOMPARE(grouped({line("It was the best of times, it was the", 40, 10, 270), line("worst of times, it was the age of", 10, 26, 300), line("wisdom, it was the age of foolishness", 10, 42, 300)}),
                 (Groups{{0, 1, 2}}));
    }

    void aLeftLabelIsNotTheIndentOfALongLine() {
        // 30 px right of the line below, but less than 0.6 of its width: a label.
        QCOMPARE(grouped({line("Title", 40, 10, 60), line("The text under the title runs a good deal wider.", 10, 26, 300)}), (Groups{{0}, {1}}));
    }

    void aLineIndentedBelowAnotherIsNotItsContinuation() {
        // 50 px right of the line above, and neither edge nor centre meets.
        QCOMPARE(grouped({line("Username", 10, 10, 70), line("Enter the name you signed up with.", 60, 26, 250)}), (Groups{{0}, {1}}));
    }

    void aHeadingStaysApartFromTheTextBelow() {
        // 28 px against 14 px: twice as tall.
        QCOMPARE(grouped({line("Installation", 10, 10, 180, 28), line("Run the installer and pick a folder.", 10, 44, 300)}), (Groups{{0}, {1}}));
    }

    void listItemsAreCutAndAContinuationStaysWithItsItem() {
        QCOMPARE(grouped({
                     line("1. Open the settings page.", 10, 10, 300),
                     line("2. Choose a save folder.", 10, 26, 280),
                     line("which is used from then on", 10, 42, 290),
                     line(QString::fromUtf8("3、保存"), 10, 58, 80),
                     line(QString::fromUtf8("• Pin the picture"), 10, 74, 200),
                     line("- Close it", 10, 90, 120),
                 }),
                 (Groups{{0}, {1, 2}, {3}, {4}, {5}}));
    }

    void aParagraphEndsOnAShortLastLine() {
        // 180 px is under 0.85 of the 400 px the paragraph spans, and the line ends on a full stop.
        QCOMPARE(grouped({line("The first paragraph runs the full width of the", 10, 10, 400), line("column and ends here.", 10, 26, 180),
                          line("The second one starts at the same margin.", 10, 42, 380)}),
                 (Groups{{0, 1}, {2}}));
        // A line that ends on a full stop but fills the width does not end it.
        QCOMPARE(grouped({line("the paragraph begins here and", 10, 10, 400), line("a sentence ends right here.", 10, 26, 390), line("and the next one runs on", 10, 42, 380)}),
                 (Groups{{0, 1, 2}}));
        // A single short line does not either: it can be the first line of a longer paragraph.
        QCOMPARE(grouped({line("Short.", 10, 10, 60), line("and then the paragraph goes on for a while.", 10, 26, 300)}), (Groups{{0, 1}}));
    }

    void closeMenuLabelsStayApartAndProseStillJoins() {
        QCOMPARE(grouped({line("Share", 20, 16, 40), line("Copy link", 20, 34, 66), line("Save", 20, 52, 34), line("Open the", 20, 90, 64), line("settings window.", 20, 108, 115)}),
                 (Groups{{0}, {1}, {2}, {3, 4}}));
    }

    void lowercaseOrLongLinesAreNotLabels() {
        // Menu-like but lowercase, or too long to be a label: same pitch, so one group.
        QCOMPARE(grouped({line("share", 20, 16, 40), line("copy link", 20, 34, 66)}), (Groups{{0, 1}}));
        QCOMPARE(grouped({line("Share this page with a friend", 20, 16, 200), line("Copy the link to the clipboard", 20, 34, 210)}), (Groups{{0, 1}}));
    }

    void shortCjkLabelsStayApart() {
        QCOMPARE(grouped({line(QString::fromUtf8("设置"), 20, 16, 28), line(QString::fromUtf8("关于"), 20, 34, 28), line(QString::fromUtf8("这是一段比较长的说明文字。"), 20, 52, 160)}),
                 (Groups{{0}, {1, 2}}));
    }

    void aParagraphIsCappedAtThirtyLines() {
        QVector<GroupingLine> lines;
        for (int index = 0; index < 40; ++index) lines.append(line(QString("line %1").arg(index), 10, 10 + index * 16, 200));
        const auto groups = grouped(lines);
        QCOMPARE(groups.size(), 2);
        QCOMPARE(groups[0].size(), 30);
        QCOMPARE(groups[1].size(), 10);
        QCOMPARE(groups[0].first(), 0);
        QCOMPARE(groups[1].first(), 30);
    }

    void everyLineIsInExactlyOneGroup() {
        // A mixed page: a heading, two paragraphs, a list, a side column and a lone caption.
        QVector<GroupingLine> lines{line("Heading", 10, 10, 120, 24),     line("first paragraph line one", 10, 44, 300), line("first paragraph line two.", 10, 60, 200),
                                    line("second paragraph", 10, 90, 300), line("1. item", 10, 120, 100),                 line("2. item", 10, 136, 100),
                                    line("side", 500, 44, 60),             line("note", 500, 60, 60),                      line("caption", 250, 300, 90)};
        QVector<int> seen(lines.size(), 0);
        for (const auto &group : group_lines_into_paragraphs(lines)) {
            QVERIFY(!group.isEmpty());
            for (const int index : group) ++seen[index];
        }
        QCOMPARE(seen, QVector<int>(lines.size(), 1));
    }
};

QTEST_GUILESS_MAIN(LineGroupingTests)
#include "test_line_grouping.moc"
