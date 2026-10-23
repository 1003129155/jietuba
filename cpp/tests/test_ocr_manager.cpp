#include "ocr/ocr_manager.h"
#include "ocr/ppocr_models.h"
#include "pin/pin_ocr_manager.h"
#include "settings/tool_settings.h"
#include "text_recognition/recognizer.h"
#include "translation/translation_manager.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QtMath>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>
#include <windows.h>

using namespace jietuba;

namespace {
// "major.minor.build.private" from a file's version resource; empty without one.
QString file_version(const QString &path) {
    const auto file = QDir::toNativeSeparators(path).toStdWString();
    DWORD ignored = 0;
    std::vector<char> data(GetFileVersionInfoSizeW(file.c_str(), &ignored));
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (data.empty() || !GetFileVersionInfoW(file.c_str(), 0, static_cast<DWORD>(data.size()), data.data())
        || !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) || !info)
        return {};
    return QString("%1.%2.%3.%4").arg(HIWORD(info->dwFileVersionMS)).arg(LOWORD(info->dwFileVersionMS)).arg(HIWORD(info->dwFileVersionLS)).arg(LOWORD(info->dwFileVersionLS));
}

// One line of text centred on white, turned clockwise by degrees on a square big enough to hold it.
QImage text_image(const QString &text, int degrees = 0) {
    QImage image(degrees == 0 ? QSize(640, 160) : QSize(760, 760), QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    QFont font("Arial");
    font.setPixelSize(56);
    painter.setFont(font);
    painter.translate(image.width() / 2.0, image.height() / 2.0);
    painter.rotate(degrees);
    painter.drawText(QRectF(-320, -80, 640, 160), Qt::AlignCenter, text);
    painter.end();
    return image;
}

QString recognized_text(const OcrResult &result) {
    QStringList texts;
    for (const auto &line : result.data) texts << line.text;
    return texts.join(' ');
}

// Lines of text in ink on paper, 1.6 sizes apart, turned clockwise by degrees, with room around.
QImage render_text(const QStringList &lines, const QString &family, int pixel_size, const QColor &ink, const QColor &paper, double degrees = 0) {
    QFont font(family);
    font.setPixelSize(pixel_size);
    const QFontMetricsF metrics(font);
    const double step = pixel_size * 1.6;
    double width = 0;
    for (const auto &line : lines) width = std::max(width, metrics.horizontalAdvance(line));
    const double height = step * lines.size();
    const double radians = qDegreesToRadians(std::abs(degrees));
    const double margin = pixel_size * 2.0;
    const QSize size(qCeil(width * std::cos(radians) + height * std::sin(radians) + 2 * margin),
        qCeil(width * std::sin(radians) + height * std::cos(radians) + 2 * margin));
    QImage image(size, QImage::Format_RGB32);
    image.fill(paper);
    QPainter painter(&image);
    painter.setPen(ink);
    painter.setFont(font);
    painter.translate(size.width() / 2.0, size.height() / 2.0);
    painter.rotate(degrees);
    for (qsizetype i = 0; i < lines.size(); ++i)
        painter.drawText(QRectF(-width / 2, -height / 2 + step * i, width, step), Qt::AlignLeft | Qt::AlignVCenter, lines[i]);
    painter.end();
    return image;
}

// Edit distance over the length of the expected text, spaces left out of both.
double char_error_rate(const QString &got, const QString &expected) {
    const auto strip = [](const QString &text) { auto chars = text.toUcs4(); chars.removeIf([](char32_t c) { return QChar::isSpace(c); }); return chars; };
    const auto a = strip(got), b = strip(expected);
    std::vector<qsizetype> row(b.size() + 1);
    for (qsizetype j = 0; j <= b.size(); ++j) row[j] = j;
    for (qsizetype i = 1; i <= a.size(); ++i) {
        qsizetype diagonal = row[0];
        row[0] = i;
        for (qsizetype j = 1; j <= b.size(); ++j) {
            const qsizetype above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal = above;
        }
    }
    return b.isEmpty() ? 0 : double(row[b.size()]) / b.size();
}

// A line box of width x height whose top-left corner is at top_left, rising by degrees to the right:
// corners from the top-left clockwise, as both engines give them.
OcrLine tilted_line(const QString &text, QPointF top_left, double width, double height, double degrees) {
    const double radians = qDegreesToRadians(degrees);
    const QPointF along(std::cos(radians), -std::sin(radians)), down(std::sin(radians), std::cos(radians));
    return OcrLine{{top_left, top_left + along * width, top_left + along * width + down * height, top_left + down * height}, text};
}

// One block per line, laid out by lines spaced line_step apart down the page's own axis.
OcrResult tilted_paragraph(int lines, double width, double degrees, double line_step = 28) {
    const double radians = qDegreesToRadians(degrees);
    const QPointF down(std::sin(radians), std::cos(radians));
    OcrResult result;
    for (int i = 0; i < lines; ++i) result.data.append(tilted_line(QString("line %1").arg(i + 1), QPointF(300, 300) + down * line_step * i, width, 20, degrees));
    return result;
}

QStringList line_texts(const QVector<OcrTextLine> &lines) {
    QStringList texts;
    for (const auto &line : lines) texts << line.text;
    return texts;
}

// A line of text with its baseline starting at a point.
struct PlacedText {
    QPointF at;
    QString text;
};

// Black Arial text on a white page.
QImage draw_page(QSize size, int pixel_size, const QVector<PlacedText> &texts) {
    QImage page(size, QImage::Format_RGB32);
    page.fill(Qt::white);
    QPainter painter(&page);
    painter.setPen(Qt::black);
    QFont font("Arial");
    font.setPixelSize(pixel_size);
    painter.setFont(font);
    for (const auto &placed : texts) painter.drawText(placed.at, placed.text);
    painter.end();
    return page;
}

// A screen full of numbered lines in three columns, three sizes apart.
QVector<PlacedText> screen_lines(QSize size, int pixel_size) {
    QVector<PlacedText> texts;
    for (int y = pixel_size * 2; y < size.height() - pixel_size; y += pixel_size * 3)
        for (int column = 0; column < 3; ++column)
            texts.append({QPointF(20 + size.width() / 3.0 * column, y), QString("Line %1 of the screen").arg(texts.size() + 1)});
    return texts;
}

// A scrolling capture: 280 numbered lines of 16px text 32 pixels apart, 1080 wide.
QVector<PlacedText> long_capture_lines() {
    QVector<PlacedText> texts;
    for (int i = 0; i < 280; ++i) texts.append({QPointF(24, 26 + 32 * i), QString("%1. Every line of the long capture reads back %2").arg(i + 1).arg((i * 37) % 1000)});
    return texts;
}

QSet<QString> read_lines(const OcrResult &result) {
    QSet<QString> read;
    for (const auto &line : result.data) read.insert(line.text);
    return read;
}

// Listing a model only checks that its files exist; without a runtime they are never opened.
void write_placeholder_model(const QString &root) {
    const QDir folder(QDir(root).filePath("placeholder"));
    QVERIFY(QDir().mkpath(folder.path()));
    for (const char *name : {"det.onnx", "rec.onnx"}) {
        QFile file(folder.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }
    QFile manifest(folder.filePath("model.json"));
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(R"({"name": "placeholder", "det": "det.onnx", "rec": "rec.onnx"})");
}
}

// Engine choice, loading, recognition and release. PP-OCR needs its pack (models and
// onnxruntime.dll) next to the executable; JIETUBA_PPOCR_MODELS names a folder to copy it from.
class OcrManagerTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    bool ppocr_ = false;
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        get_tool_settings_manager(settings_);
        QVERIFY(QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf")) >= 0);
        QVERIFY(QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/msyh.ttc")) >= 0);
        const QString source = qEnvironmentVariable("JIETUBA_PPOCR_MODELS");
        const QDir target(QDir(QCoreApplication::applicationDirPath()).filePath("models"));
        if (!source.isEmpty()) {
            QDir().mkpath(target.path());
            for (const char *name : {"PP-OCRv6_det_small.onnx", "PP-OCRv6_rec_small.onnx", "onnxruntime.dll"})
                if (!target.exists(name) && QDir(source).exists(name)) QVERIFY(QFile::copy(QDir(source).filePath(name), target.filePath(name)));
        }
        ppocr_ = get_ppocr_status() == "available";
        // A pack that was named but does not load would skip every PP-OCR case unnoticed.
        QVERIFY2(source.isEmpty() || ppocr_, qPrintable("JIETUBA_PPOCR_MODELS does not hold a usable pack: " + get_ppocr_status()));
    }

    void cleanup() { release_ocr_engine(); }

    void enginesResolveByPreference() {
        auto &ocr = OCRManager::instance();
        const auto engines = get_available_engines();
        QCOMPARE(is_ocr_available(), !engines.isEmpty());
        QCOMPARE(ocr.resolve_engine(OCRManager::ENGINE_AUTO), engines.isEmpty() ? std::nullopt : std::optional<QString>(engines.front()));
        QCOMPARE(ocr.resolve_engine("tesseract"), std::nullopt);
        QVERIFY(!set_ocr_engine("tesseract"));
        // With no engine at all, a missing choice has nothing to fall back to and reports why.
        set_oneocr_available_for_test(false);
        QTemporaryDir models;
        QVERIFY(models.isValid());
        ppocr_models::set_models_root_for_test(models.path());
        const auto restore = qScopeGuard([] {
            ppocr_models::set_models_root_for_test({});
            set_oneocr_available_for_test(std::nullopt);
            set_ocr_engine(OCRManager::ENGINE_AUTO);
        });
        QVERIFY(!set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        QVERIFY(ocr.get_last_error().contains("PP-OCR"));
        QVERIFY(!initialize_ocr(OCRManager::ENGINE_PP_RUST));
        QCOMPARE(recognize_text(text_image("x")).code, -1);
        QVERIFY(!set_ocr_engine(OCRManager::ENGINE_ONEOCR));
        QVERIFY(!ocr.get_last_error().isEmpty());
        QCOMPARE(get_ocr_memory_status(), QString::fromUtf8("未初始化"));
    }

    // A tilted paragraph keeps its lines and their order: lines are told apart across the page's
    // own direction, not by their upright bounding boxes, which grow taller with the tilt.
    void tiltedLinesStayApartInReadingOrder() {
        const QStringList expected{"line 1", "line 2", "line 3", "line 4"};
        QStringList wrong;
        for (const double width : {200.0, 400.0, 800.0}) {
            for (const double degrees : {0.0, 3.0, -3.0, 10.0, -10.0, 30.0, -30.0}) {
                auto result = tilted_paragraph(4, width, degrees);
                std::reverse(result.data.begin(), result.data.end());
                const auto texts = line_texts(group_ocr_lines(result));
                if (texts != expected) wrong << QString("%1px at %2: %3").arg(width).arg(degrees).arg(texts.join(" | "));
            }
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join("; ")));
        // A line found as two blocks, one sitting 3px higher, is one line read from the left.
        for (const double degrees : {0.0, 10.0, -10.0}) {
            OcrResult result;
            const double radians = qDegreesToRadians(degrees);
            const QPointF along(std::cos(radians), -std::sin(radians)), down(std::sin(radians), std::cos(radians));
            result.data = {tilted_line("world", QPointF(300, 300) + along * 220 - down * 3, 180, 20, degrees),
                tilted_line("hello", QPointF(300, 300), 200, 20, degrees), tilted_line("next", QPointF(300, 300) + down * 28, 300, 20, degrees)};
            const auto texts = line_texts(group_ocr_lines(result));
            QVERIFY2(texts == QStringList({"hello world", "next"}), qPrintable(QString("%1: %2").arg(degrees).arg(texts.join(" | "))));
        }
    }

    // The pack's onnxruntime.dll is judged by its version resource, before anything loads it.
    void ppocrRuntimeNeedsOnnxRuntimeOneTwentyFourOrLater() {
        QVERIFY(!ppocr_runtime_supported(dir_.filePath("absent.dll")));
        QFile fake(dir_.filePath("onnxruntime.dll"));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("MZ");
        fake.close();
        QVERIFY(!ppocr_runtime_supported(fake.fileName()));
        // A DLL whose version is not ONNX Runtime 1.x.
        QVERIFY(!ppocr_runtime_supported(QDir(qEnvironmentVariable("WINDIR")).filePath("System32/kernel32.dll")));
        // Windows 11 ships an ONNX Runtime 1.17 with Windows ML.
        const QString system = QDir(qEnvironmentVariable("WINDIR")).filePath("System32/onnxruntime.dll");
        const QString version = QFileInfo::exists(system) ? file_version(system) : QString();
        if (version.startsWith("1.17.")) QVERIFY(!ppocr_runtime_supported(system));
        if (ppocr_) QVERIFY(ppocr_runtime_supported(QDir(QCoreApplication::applicationDirPath()).filePath("models/onnxruntime.dll")));
    }

    void ppocrRecognizesRenderedText() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        auto &ocr = OCRManager::instance();
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        QCOMPARE(get_current_engine(), std::optional<QString>(OCRManager::ENGINE_PP_RUST));
        QVERIFY(ocr.is_engine_loaded());
        QVERIFY(get_ocr_memory_status().contains("ppocr_rust"));
        // Loading again reuses the engine.
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        const auto result = recognize_text(text_image("Hello 2026"));
        QCOMPARE(result.code, 100);
        QVERIFY2(recognized_text(result).contains("Hello"), qPrintable(recognized_text(result)));
        QVERIFY(result.elapse > 0);
        QCOMPARE(result.data.first().box.size(), 4);
        // One position per character, in reading order and inside the line.
        for (const auto &line : result.data) {
            QCOMPARE(line.chars.size(), line.text.toUcs4().size());
            float previous = 0;
            for (const auto &span : line.chars) {
                QVERIFY2(span.start >= previous - 1e-6f && span.start <= span.center && span.center <= span.end && span.end <= 1.0f, qPrintable(line.text));
                previous = span.end;
            }
        }
        QVERIFY(format_ocr_result_text(result).contains("2026"));
        // Formats other than RGB888 are converted; blank and null images find nothing.
        QImage alpha = text_image("Second line").convertToFormat(QImage::Format_ARGB32);
        QVERIFY(recognized_text(recognize_text(alpha)).contains("Second"));
        QImage white(200, 80, QImage::Format_RGB32);
        white.fill(Qt::white);
        const auto blank = recognize_text(white);
        QCOMPARE(blank.code, 100);
        QVERIFY(blank.data.isEmpty());
        QVERIFY(recognize_text(QImage()).data.isEmpty());
        // Releasing frees the engine; the next recognition loads it again.
        release_ocr_engine();
        QVERIFY(!ocr.is_engine_loaded());
        QVERIFY(recognized_text(recognize_text(text_image("Again"))).contains("Again"));
        QVERIFY(ocr.is_engine_loaded());
    }

    // A tilted line reads the same as a level one, both ways up to 60 degrees.
    void ppocrRecognizesTiltedText() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        for (const int degrees : {-60, -45, -30, -15, 15, 30, 45, 60}) {
            const auto text = recognized_text(recognize_text(text_image("Hello 2026", degrees)));
            QVERIFY2(text == "Hello 2026", qPrintable(QString("%1: %2").arg(degrees).arg(text)));
        }
        // A user's screenshot: Chinese rising at 45 degrees.
        const QImage screenshot(JIETUBA_OCR_FIXTURE_DIR "/ocr_tilted.png");
        QVERIFY(!screenshot.isNull());
        QCOMPARE(recognized_text(recognize_text(screenshot)), QString::fromUtf8("滚滚长江东逝水"));
    }

    // Text drawn across sizes, colours, scripts and tilts reads back as drawn. Each dimension takes
    // its usual and edge values around one base case rather than every combination. Small text also
    // fills part of a window-sized page, where it is a small part of what detection sees.
    void ppocrReadsTextAcrossLooks() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        struct Script { const char *name, *family, *text; };
        const Script scripts[] = {{"en", "Arial", "Quick brown fox 42"}, {"zh", "Microsoft YaHei", "今天下午三点开会"}, {"mixed", "Microsoft YaHei", "版本 2.1.8 已发布"}};
        struct Look { const char *name; QColor ink, paper; };
        const Look looks[] = {{"light", Qt::black, Qt::white}, {"dark", QColor(230, 230, 230), QColor(30, 30, 30)}, {"accent", Qt::white, QColor(0, 120, 212)}};
        QStringList wrong;
        const auto check = [&](const QString &label, const QImage &image, const QString &expected) {
            const QString got = format_ocr_result_text(recognize_text(image), "\n");
            if (char_error_rate(got, expected) > 0) wrong << QString("%1: %2").arg(label, got);
        };
        for (const auto &script : scripts) {
            const QString text = QString::fromUtf8(script.text);
            for (const int size : {12, 16, 24, 48})
                for (const auto &look : looks) check(QString("%1 %2px %3").arg(script.name).arg(size).arg(look.name), render_text({text}, script.family, size, look.ink, look.paper), text);
            for (const double degrees : {3.0, -10.0, 30.0, 90.0}) check(QString("%1 %2deg").arg(script.name).arg(degrees), render_text({text}, script.family, 24, Qt::black, Qt::white, degrees), text);
            for (const auto &look : {looks[0], looks[1]}) {
                const QStringList block(6, text);
                QImage page(1280, 800, QImage::Format_RGB32);
                page.fill(look.paper);
                QPainter painter(&page);
                painter.drawImage(40, 40, render_text(block, script.family, 14, look.ink, look.paper));
                painter.end();
                check(QString("%1 14px in a window %2").arg(script.name, look.name), page, block.join("\n"));
            }
        }
        // A paragraph comes back line by line from the top, level or tilted.
        const QStringList paragraph{"First line of text", QString::fromUtf8("第二行是中文"), "Third line 2026"};
        for (const double degrees : {0.0, 3.0, -3.0, 10.0, -10.0, 30.0})
            check(QString("paragraph %1deg").arg(degrees), render_text(paragraph, "Microsoft YaHei", 24, Qt::black, Qt::white, degrees), paragraph.join("\n"));
        QVERIFY2(wrong.isEmpty(), wrong.join("; ").toUtf8().constData());
    }

    // Lengths beyond what one pass of the models takes. Detection works on the image at its own size,
    // cut into tiles once a side reaches 1440; recognition scales each line to a height of 48 with no
    // limit on its width.
    void ppocrReadsLongLinesAndWholeScreens() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        QFont font("Arial");
        font.setPixelSize(14);
        const QFontMetricsF metrics(font);
        // A line of code 20 pixels tall filling the width.
        const auto strip = [&](int width) {
            QString text;
            while (metrics.horizontalAdvance(text) < width) text += "int value = compute(alpha, beta); ";
            while (metrics.horizontalAdvance(text) > width - 8) text.chop(1);
            QImage image(width, 20, QImage::Format_RGB32);
            image.fill(Qt::white);
            QPainter painter(&image);
            painter.setPen(Qt::black);
            painter.setFont(font);
            painter.drawText(QRectF(4, 0, width - 8, 20), Qt::AlignLeft | Qt::AlignVCenter, text);
            painter.end();
            return std::pair{image, text.trimmed()};
        };
        const auto [short_strip, short_text] = strip(640);
        QCOMPARE(format_ocr_result_text(recognize_text(short_strip)), short_text);
        const auto [long_strip, long_text] = strip(2560);
        const QString long_got = format_ocr_result_text(recognize_text(long_strip));
        QVERIFY2(char_error_rate(long_got, long_text) < 0.05, qPrintable(long_got));

        // Whole screens of small text, the smallest at 4K.
        for (const auto &[size, pixel_size] : {std::pair{QSize(2560, 1440), 14}, std::pair{QSize(3840, 2160), 12}}) {
            const auto texts = screen_lines(size, pixel_size);
            QSet<QString> drawn;
            for (const auto &placed : texts) drawn.insert(placed.text);
            const auto read = read_lines(recognize_text(draw_page(size, pixel_size, texts)));
            QVERIFY2(read == drawn, qPrintable(QString("%1x%2 at %3px: %4 of %5 lines read").arg(size.width()).arg(size.height()).arg(pixel_size)
                                                   .arg((read & drawn).size()).arg(drawn.size())));
        }
        // One such line alone on the screen.
        QCOMPARE(format_ocr_result_text(recognize_text(draw_page(QSize(2560, 1440), 14, {{QPointF(40, 60), "Quick brown fox 42"}}))), QString("Quick brown fox 42"));
    }

    // A scrolling capture many screens tall reads back whole, line by line from the top.
    void ppocrReadsALongCaptureLineByLine() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        const auto texts = long_capture_lines();
        QStringList expected;
        for (const auto &placed : texts) expected << placed.text;
        const QStringList got = format_ocr_result_text(recognize_text(draw_page(QSize(1080, 9000), 16, texts)), "\n").split('\n');
        QStringList wrong;
        for (qsizetype i = 0; i < std::max(got.size(), expected.size()); ++i)
            if (got.value(i) != expected.value(i)) wrong << QString("line %1: %2").arg(i + 1).arg(got.value(i));
        QVERIFY2(wrong.isEmpty(), qPrintable(QString("%1 of %2 lines wrong; %3").arg(wrong.size()).arg(expected.size()).arg(wrong.mid(0, 5).join("; "))));
    }

    // Lines crossing the edges between detection tiles, or lying in the strip two tiles share, read
    // back whole. A 4K screen is padded to 3840 x 2176, 8 pixels above, and cut into tiles 960 wide
    // overlapping by 192, with the last one in each direction taking the rest: the overlaps are
    // x = 768..960, 1536..1728, 2304..2496, 3072..3264 and y = 760..952, 1528..1720 on the screen.
    void ppocrReadsLinesAcrossDetectionTiles() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        QVector<PlacedText> texts;
        // Baselines 3 below each horizontal edge put 12px glyphs across it; the rows between them lie
        // in the overlap strips. Rows take turns crossing the near edge of each overlap, crossing its
        // far edge, and holding two short words inside it.
        for (int row = 0, baseline = 27; baseline < 2150; ++row, baseline += 32) {
            texts.append({QPointF(20, baseline), QString("Row %1 starts inside a tile").arg(row + 1)});
            for (const int overlap : {768, 1536, 2304, 3072}) {
                if (row % 3 == 0) {
                    texts.append({QPointF(overlap - 64, baseline), QString("Row %1 runs across the edge at %2").arg(row + 1).arg(overlap)});
                } else if (row % 3 == 1) {
                    texts.append({QPointF(overlap + 128, baseline), QString("Row %1 runs across the edge at %2").arg(row + 1).arg(overlap + 192)});
                } else {
                    texts.append({QPointF(overlap + 12, baseline), QString("%1 in %2").arg(row + 1).arg(overlap)});
                    texts.append({QPointF(overlap + 120, baseline), QString("%1 by %2").arg(row + 1).arg(overlap)});
                }
            }
        }
        QSet<QString> drawn;
        for (const auto &placed : texts) drawn.insert(placed.text);
        const auto read = read_lines(recognize_text(draw_page(QSize(3840, 2160), 12, texts)));
        QStringList missing;
        for (const auto &text : drawn - read) missing << text;
        QVERIFY2(read == drawn, qPrintable(QString("%1 of %2 lines read; missing %3").arg((read & drawn).size()).arg(drawn.size()).arg(missing.mid(0, 6).join("; "))));
    }

    // Large text across the overlap of two detection tiles comes back as one line, read as drawn, and
    // the flat background around it adds nothing. A 1920 x 1080 screen is cut at x 0..960 and
    // 768..1920, a 1080 x 2400 capture at y 0..960, 768..1728 and 1536..2400; each line is moved
    // across the overlap a step at a time.
    void ppocrReadsLargeTextAcrossDetectionTiles() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        struct Look { const char *name; QColor ink, paper; };
        const Look looks[] = {{"light", Qt::black, Qt::white}, {"accent", Qt::white, QColor(0, 120, 212)}};
        QStringList wrong;
        const auto check = [&](const QString &label, QSize size, int pixel_size, QPointF at, const Look &look, const QString &text) {
            QImage page(size, QImage::Format_RGB32);
            page.fill(look.paper);
            QPainter painter(&page);
            painter.setPen(look.ink);
            QFont font("Arial");
            font.setPixelSize(pixel_size);
            painter.setFont(font);
            painter.drawText(at, text);
            painter.end();
            const auto read = read_lines(recognize_text(page));
            if (read != QSet<QString>{text}) wrong << QString("%1 %2: %3").arg(label, look.name, QStringList(read.values()).join(" | "));
        };
        const QString across = "Quick brown fox 42 jumps";
        for (const int pixel_size : {48, 96}) {
            QFont font("Arial");
            font.setPixelSize(pixel_size);
            const int width = qCeil(QFontMetricsF(font).horizontalAdvance(across));
            for (int x = std::max(16, 768 - width + 32); x < std::min(940, 1904 - width); x += 96)
                for (const auto &look : looks) check(QString("%1px at x %2").arg(pixel_size).arg(x), QSize(1920, 1080), pixel_size, QPointF(x, 500), look, across);
        }
        for (int baseline = 790; baseline <= 1010; baseline += 44)
            for (const auto &look : looks) check(QString("110px at y %1").arg(baseline), QSize(1080, 2400), 110, QPointF(40, baseline), look, "Scroll line 2026 ok");
        // Small text on a coloured screen is the faintest for detection near a tile's edge.
        for (const int pixel_size : {12, 16}) {
            QFont font("Arial");
            font.setPixelSize(pixel_size);
            const int width = qCeil(QFontMetricsF(font).horizontalAdvance(across));
            for (int x = 768 - width + 16; x < 940; x += 32) check(QString("%1px at x %2").arg(pixel_size).arg(x), QSize(1920, 1080), pixel_size, QPointF(x, 500), looks[1], across);
        }
        // Wide flat colour around the line, where detection alone finds shapes in the background.
        for (int x = 200; x < 1200; x += 220) check(QString("110px at x %1 on 2560").arg(x), QSize(2560, 1440), 110, QPointF(x, 700), looks[1], "Meeting at 3pm in room 42");
        QVERIFY2(wrong.isEmpty(), wrong.join("; ").toUtf8().constData());
    }

    // A long capture takes seconds to read; cancelling stops it within a tile or a line, with nothing
    // recognized, and one cancelled before it starts does no work. The worker threads of the result
    // window, pins and translation pass their cancel on.
    void ppocrStopsSoonAfterCancelling() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        const QImage capture = draw_page(QSize(1080, 9000), 16, long_capture_lines());
        std::atomic_bool cancelled = true;
        QElapsedTimer clock;
        clock.start();
        QVERIFY(recognize_text(capture, &cancelled).data.isEmpty());
        QVERIFY2(clock.elapsed() < 200, qPrintable(QString("%1 ms").arg(clock.elapsed())));
        cancelled = false;
        OcrResult result;
        std::thread worker([&] { result = recognize_text(capture, &cancelled); });
        QThread::msleep(300);
        cancelled = true;
        clock.restart();
        worker.join();
        QVERIFY2(clock.elapsed() < 500, qPrintable(QString("%1 ms after cancelling").arg(clock.elapsed())));
        QCOMPARE(result.data.size(), 0);

        // Milliseconds from cancelling a running thread to its end; the thread has ended on return.
        const auto stopping_time = [](QThread &thread, const std::function<void()> &cancel) {
            if (!thread.isRunning()) thread.start();
            QThread::msleep(300);
            cancel();
            QElapsedTimer since;
            since.start();
            thread.wait();
            return since.elapsed();
        };
        bool reported = false;
        QObject context;
        auto *recognition = recognize_async(capture, &context, [&](const QVector<OcrTextLine> &, const QString &) { reported = true; });
        const qint64 window_ms = stopping_time(*recognition, [&] { recognition->cancel(); });
        QVERIFY2(window_ms < 500, qPrintable(QString("result window: %1 ms").arg(window_ms)));
        QCoreApplication::processEvents();
        QVERIFY(!reported);
        PinOcrThread pin(capture, QTransform());
        const qint64 pin_ms = stopping_time(pin, [&] { pin.cancel(); });
        QVERIFY2(pin_ms < 500, qPrintable(QString("pin: %1 ms").arg(pin_ms)));
        QVERIFY(pin.result && pin.result->data.isEmpty());
        TranslationOcrThread translation(capture);
        const qint64 translation_ms = stopping_time(translation, [&] { translation.cancel(); });
        QVERIFY2(translation_ms < 500, qPrintable(QString("translation: %1 ms").arg(translation_ms)));
    }

    // The engine follows the chosen model and reloads when it changes; Auto takes the catalog's
    // choice, not the saved model. A deleted model gives way to the automatic choice while the
    // setting stays as it was.
    void switchingModelsReloadsAndAMissingOneFallsBackToAuto() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        set_oneocr_available_for_test(false);
        const auto restore = qScopeGuard([] {
            set_oneocr_available_for_test(std::nullopt);
            set_ppocr_model({});
            set_ocr_engine(OCRManager::ENGINE_AUTO);
        });
        // A chosen Snipping Tool OCR that is not there gives way to PP-OCR.
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_ONEOCR));
        QCOMPARE(get_current_engine(), std::optional<QString>(OCRManager::ENGINE_PP_RUST));
        const QDir root(ppocr_models::models_root());
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        ppocr_models::import_model("copy", root.filePath("PP-OCRv6_det_small.onnx"), root.filePath("PP-OCRv6_rec_small.onnx"));
        set_ppocr_model("copy");
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString("copy"));
        QVERIFY(recognized_text(recognize_text(text_image("Switched"))).contains("Switched"));
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_AUTO));
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString());
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        release_ocr_engine();
        ppocr_models::remove("copy");
        QCOMPARE(get_ppocr_status(), QString("available"));
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString());
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        QVERIFY(recognized_text(recognize_text(text_image("Fallback"))).contains("Fallback"));
    }

    // A model that fails to load is passed over for the automatic one, and tried again once its
    // files are replaced.
    void aBrokenModelFallsBackAndIsRetriedOnceReplaced() {
        if (!ppocr_) QSKIP("PP-OCR pack is not installed; set JIETUBA_PPOCR_MODELS.");
        set_oneocr_available_for_test(false);
        const QDir pack(ppocr_models::models_root());
        const auto model = ppocr_models::import_model("damaged", pack.filePath("PP-OCRv6_det_small.onnx"), pack.filePath("PP-OCRv6_rec_small.onnx"));
        const auto restore = qScopeGuard([] {
            release_ocr_engine();
            ppocr_models::remove("damaged");
            set_oneocr_available_for_test(std::nullopt);
            set_ppocr_model({});
            set_ocr_engine(OCRManager::ENGINE_AUTO);
        });
        const QString det = model.value("det").toString();
        QVERIFY(QFile::remove(det));
        QFile junk(det);
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write("not a model");
        junk.close();
        set_ppocr_model("damaged");
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        QVERIFY(recognized_text(recognize_text(text_image("Fallback"))).contains("Fallback"));
        QVERIFY(OCRManager::instance().get_last_error().contains(QString::fromUtf8("初始化失败")));
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString());
        release_ocr_engine();
        QVERIFY(QFile::remove(det));
        QVERIFY(QFile::copy(pack.filePath("PP-OCRv6_det_small.onnx"), det));
        QCOMPARE(OCRManager::instance().selected_ppocr_model()->value("id").toString(), QString("damaged"));
        QVERIFY(recognized_text(recognize_text(text_image("Repaired"))).contains("Repaired"));
    }

    // Failures carry the reason the result window explains: no model at all, or a model that
    // does not load with nothing else to use.
    void failuresReportTheirReason() {
        set_oneocr_available_for_test(false);
        QTemporaryDir models;
        QVERIFY(models.isValid());
        ppocr_models::set_models_root_for_test(models.path());
        const auto restore = qScopeGuard([] {
            release_ocr_engine();
            ppocr_models::set_models_root_for_test({});
            set_oneocr_available_for_test(std::nullopt);
            set_ppocr_model({});
            set_ocr_engine(OCRManager::ENGINE_AUTO);
        });
        QVERIFY(!set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        auto result = recognize_text(text_image("x"));
        QCOMPARE(result.code, -1);
        QCOMPARE(result.reason, QString("no_model"));
        // A model without the runtime points at the runtime, not at another model.
        write_placeholder_model(models.path());
        QVERIFY(!set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        result = recognize_text(text_image("x"));
        QCOMPARE(result.code, -1);
        QCOMPARE(result.reason, QString("no_runtime"));
        QVERIFY(QDir(QDir(models.path()).filePath("placeholder")).removeRecursively());
        if (!ppocr_) return;
        const QDir pack(QDir(QCoreApplication::applicationDirPath()).filePath("models"));
        QVERIFY(QFile::copy(pack.filePath("onnxruntime.dll"), QDir(models.path()).filePath("onnxruntime.dll")));
        const auto model = ppocr_models::import_model("only", pack.filePath("PP-OCRv6_det_small.onnx"), pack.filePath("PP-OCRv6_rec_small.onnx"));
        QFile det(model.value("det").toString());
        QVERIFY(det.open(QIODevice::WriteOnly | QIODevice::Truncate));
        det.write("not a model");
        det.close();
        set_ppocr_model("only");
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        result = recognize_text(text_image("x"));
        QCOMPARE(result.code, -1);
        QCOMPARE(result.reason, QString("model_broken"));
    }

    // Switching engines closes the one no longer chosen.
    void switchingEnginesReleasesTheOldOne() {
        if (!ppocr_ || !get_available_engines().contains(OCRManager::ENGINE_ONEOCR)) QSKIP("needs both engines");
        auto &ocr = OCRManager::instance();
        QVERIFY(initialize_ocr(OCRManager::ENGINE_PP_RUST));
        QVERIFY(ocr.is_engine_loaded());
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_ONEOCR));
        QVERIFY(!ocr.is_engine_loaded());
        QVERIFY(initialize_ocr());
        QVERIFY(get_ocr_memory_status().contains(QString::fromUtf8("截图工具")));
        QVERIFY(recognized_text(recognize_text(text_image("Snipping"))).contains("Snipping"));
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_PP_RUST));
        QVERIFY(!ocr.is_engine_loaded());
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_AUTO));
    }
};

QTEST_MAIN(OcrManagerTests)
#include "test_ocr_manager.moc"
