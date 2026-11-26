// Hard-coded colours in src/ must not grow: colours belong in the theme tokens. The number of colour
// literals per file is compared with tests/fixtures/style_literals_baseline.txt and must match exactly, so
// removing one is recorded in the baseline and adding one fails the test.
// JIETUBA_STYLE_BASELINE_WRITE=<path> writes the current counts there, to regenerate the baseline.
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>
#include <QtTest>
#include <algorithm>
#include <map>

namespace {
// Colour definitions themselves, which are the one place literals belong. Each is a palette.
const QStringList EXEMPT_FILES = {
    // The window palette (UIThemeTokens) and the translation style.
    "core/ui_theme.cpp",
    // Capture selection and mask colours.
    "core/theme.cpp",
    // The user-selectable clipboard panel themes.
    "clipboard/ui/theme/themes.cpp",
    "clipboard/ui/theme/themes.h",
};

constexpr QChar QUOTE(0x22);

// The source with comments replaced by spaces; strings stay, since style sheets are strings.
QString strip_comments(const QString &source) {
    QString out;
    out.reserve(source.size());
    const int size = source.size();
    int i = 0;
    while (i < size) {
        const QChar c = source[i];
        if (c == 'R' && i + 2 < size && source[i + 1] == QUOTE && source[i + 2] == '(' && (i == 0 || !(source[i - 1].isLetterOrNumber() || source[i - 1] == '_'))) {
            const int end = source.indexOf(")\"", i + 3);
            const int stop = end < 0 ? size : end + 2;
            out.append(source.mid(i, stop - i));
            i = stop;
        } else if (c == QUOTE) {
            int j = i + 1;
            while (j < size && source[j] != QUOTE && source[j] != '\n') j += source[j] == '\\' ? 2 : 1;
            j = std::min(j + 1, size);
            out.append(source.mid(i, j - i));
            i = j;
        } else if (c == '\'' && !(i > 0 && source[i - 1].isLetterOrNumber())) {
            int j = i + 1;
            while (j < size && source[j] != '\'' && source[j] != '\n') j += source[j] == '\\' ? 2 : 1;
            j = std::min(j + 1, size);
            out.append(source.mid(i, j - i));
            i = j;
        } else if (c == '/' && i + 1 < size && source[i + 1] == '/') {
            while (i < size && source[i] != '\n') ++i;
            out.append(' ');
        } else if (c == '/' && i + 1 < size && source[i + 1] == '*') {
            const int end = source.indexOf("*/", i + 2);
            i = end < 0 ? size : end + 2;
            out.append(' ');
        } else {
            out.append(c);
            ++i;
        }
    }
    return out;
}

// Colour literals outside comments: #RRGGBB and #RRGGBBAA, rgb( and rgba( in a style sheet, and
// QColor built from numbers.
int count_literals(const QString &source) {
    static const QRegularExpression hex("#[0-9A-Fa-f]{6}(?:[0-9A-Fa-f]{2})?(?![0-9A-Za-z_])");
    static const QRegularExpression css_function("(?<![A-Za-z0-9_])rgba?\\(");
    // A temporary QColor(1, 2, 3) or a declared one, QColor name(1, 2, 3) and QColor name{1, 2, 3}.
    static const QRegularExpression numeric_color("QColor(?:\\s+[A-Za-z_]\\w*)?\\s*[({]\\s*\\d+\\s*,\\s*\\d+\\s*,\\s*\\d+");
    const auto code = strip_comments(source);
    int count = 0;
    for (const auto *expression : {&hex, &css_function, &numeric_color}) {
        auto matches = expression->globalMatch(code);
        while (matches.hasNext()) {
            matches.next();
            ++count;
        }
    }
    return count;
}

// Relative path under the source root (forward slashes) to the number of literals; files without any are left out.
std::map<QString, int> scan_sources() {
    const QDir root(JIETUBA_STYLE_SOURCE_DIR);
    std::map<QString, int> counts;
    QDirIterator files(root.absolutePath(), {"*.cpp", "*.h"}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        const auto relative = root.relativeFilePath(path);
        if (EXEMPT_FILES.contains(relative)) continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const int count = count_literals(QString::fromUtf8(file.readAll()));
        if (count > 0) counts[relative] = count;
    }
    return counts;
}

std::map<QString, int> read_baseline(QStringList *problems) {
    std::map<QString, int> baseline;
    QFile file(JIETUBA_STYLE_BASELINE_PATH);
    if (!file.open(QIODevice::ReadOnly)) {
        problems->append(QString("cannot read the baseline %1").arg(JIETUBA_STYLE_BASELINE_PATH));
        return baseline;
    }
    QString previous;
    for (const auto &raw : QString::fromUtf8(file.readAll()).split('\n')) {
        const auto line = raw.trimmed();
        if (line.isEmpty()) continue;
        const auto space = line.indexOf(' ');
        bool ok = false;
        const int count = space > 0 ? line.left(space).toInt(&ok) : 0;
        if (!ok || count <= 0) {
            problems->append(QString("baseline line is not '<count> <path>': %1").arg(line));
            continue;
        }
        const auto path = line.mid(space + 1).trimmed();
        if (!previous.isEmpty() && !(previous < path)) problems->append(QString("baseline is not sorted by path, or repeats one, at %1").arg(path));
        previous = path;
        baseline[path] = count;
    }
    return baseline;
}
}

class StyleLiteralTests : public QObject {
    Q_OBJECT
private slots:
    void hardCodedColoursStayAtTheBaseline() {
        const auto current = scan_sources();
        const auto write_to = qEnvironmentVariable("JIETUBA_STYLE_BASELINE_WRITE");
        if (!write_to.isEmpty()) {
            QFile out(write_to);
            QVERIFY2(out.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(write_to));
            for (const auto &[path, count] : current) out.write(QString("%1 %2\n").arg(count).arg(path).toUtf8());
            qInfo().noquote() << "style literal baseline written to" << write_to;
            return;
        }
        QStringList problems;
        const auto baseline = read_baseline(&problems);
        QSet<QString> paths;
        for (const auto &entry : current) paths.insert(entry.first);
        for (const auto &entry : baseline) paths.insert(entry.first);
        QStringList sorted(paths.begin(), paths.end());
        sorted.sort();
        for (const auto &path : sorted) {
            const int now = current.count(path) ? current.at(path) : 0;
            const int recorded = baseline.count(path) ? baseline.at(path) : 0;
            if (now > recorded)
                problems.append(QString("src/%1 has %2 hard-coded colours, the baseline has %3: take the colour from the theme tokens instead").arg(path).arg(now).arg(recorded));
            else if (now < recorded)
                problems.append(QString("src/%1 has %2 hard-coded colours, the baseline still has %3: lower the baseline to %2").arg(path).arg(now).arg(recorded));
        }
        QVERIFY2(problems.isEmpty(), qPrintable("\n" + problems.join('\n')));
    }

    void theCountingRules_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<int>("expected");
        QTest::newRow("six digit hex") << "const char *c = \"#6F8FAB\";" << 1;
        QTest::newRow("eight digit hex") << "s = \"color: #6F8FAB80;\"" << 1;
        QTest::newRow("seven digits is not a colour") << "s = \"#6F8FABC\";" << 0;
        QTest::newRow("five digits is not a colour") << "s = \"#6F8FA\";" << 0;
        QTest::newRow("a selector") << "s = \"#SettingsDialog { }\";" << 0;
        QTest::newRow("preprocessor") << "#define A 1\n#include <QString>\n#endif" << 0;
        QTest::newRow("two in one line") << "s = \"#FFFFFF #000000\";" << 2;
        QTest::newRow("css rgb and rgba") << "s = \"rgb(1, 2, 3) rgba(1, 2, 3, 0.5)\";" << 2;
        QTest::newRow("qt colour functions are not css") << "auto a = qRgba(1, 2, 3, 4); auto b = QColor::fromRgb(1, 2, 3); auto c = to_rgba(1);" << 0;
        QTest::newRow("numeric QColor") << "QColor c(1, 2, 3); QColor d = QColor(255, 255, 255, 40);" << 2;
        QTest::newRow("declared QColor constants") << "const QColor RING(255, 255, 255, 190); inline const QColor EDGE{15, 23, 42};" << 2;
        QTest::newRow("QColor from variables or names") << "QColor a(r, g, b); QColor b(Qt::white); QColor c(\"red\");" << 0;
        QTest::newRow("line comment") << "// \"#FFFFFF\" rgba(0,0,0,1) QColor(1,2,3)\nint x;" << 0;
        QTest::newRow("trailing comment") << "int x; // #FFFFFF" << 0;
        QTest::newRow("block comment over lines") << "/* #FFFFFF\n * #000000 */\nint x;" << 0;
        QTest::newRow("code after a block comment") << "/* note */ s = \"#FFFFFF\";" << 1;
        QTest::newRow("comment marks inside a string") << "s = \"// #FFFFFF\"; t = \"/*\"; u = \"#000000\";" << 2;
        QTest::newRow("char literal quote") << "c = '\"'; s = \"#FFFFFF\";" << 1;
        QTest::newRow("raw string with css") << "s = R\"(\n a { color: #FFFFFF; } /* x */\n)\";\nt = \"#000000\";" << 2;
    }

    void theCountingRules() {
        QFETCH(QString, source);
        QFETCH(int, expected);
        QCOMPARE(count_literals(source), expected);
    }
};

QTEST_MAIN(StyleLiteralTests)
#include "test_style_literals.moc"
