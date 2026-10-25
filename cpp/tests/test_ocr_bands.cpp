#include "ocr/ocr_bands.h"
#include "ocr/ocr_manager.h"
#include "settings/tool_settings.h"
#include <QDir>
#include <QFontDatabase>
#include <QMap>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>

using namespace jietuba;

namespace {
// Rows of short texts, one every 850 px across, on white.
std::pair<QImage, QSet<QString>> page(int width, int height, int pixels) {
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    QFont font("Arial");
    font.setPixelSize(pixels);
    painter.setFont(font);
    QSet<QString> drawn;
    for (int y = 30, row = 1; y < height - 10; y += pixels * 2, ++row)
        for (int x = 20, column = 1; x + 300 < width; x += 850, ++column) {
            const QString text = QString("Row %1 column %2 sample").arg(row).arg(column);
            painter.drawText(QPointF(x, y), text);
            drawn.insert(text);
        }
    painter.end();
    return {image, drawn};
}

// Lines drawn as filled rectangles whose colour carries their number.
QRgb bar_colour(int id) { return qRgb(id & 0xff, id >> 8, 0); }

QImage draw_bars(int width, int height, const QMap<int, QRect> &bars) {
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(Qt::white);
    for (auto it = bars.cbegin(); it != bars.cend(); ++it)
        for (int y = it.value().top(); y <= it.value().bottom(); ++y)
            for (int x = it.value().left(); x <= it.value().right(); ++x) image.setPixel(x, y, bar_colour(it.key()));
    return image;
}

// A stand-in engine: one line per rectangle it sees, named by its number, with a * when the band's
// edge cuts it, as an engine misreads a line cut in half.
QVector<OcrLine> read_bars(const QImage &band) {
    QMap<int, QRect> seen;
    for (int y = 0; y < band.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(band.constScanLine(y));
        for (int x = 0; x < band.width(); ++x)
            if (qBlue(row[x]) == 0) seen[qRed(row[x]) | qGreen(row[x]) << 8] |= QRect(x, y, 1, 1);
    }
    QVector<OcrLine> lines;
    for (auto it = seen.cbegin(); it != seen.cend(); ++it) {
        const QRect r = it.value();
        OcrLine line;
        line.text = QString::number(it.key()) + (r.top() == 0 || r.bottom() == band.height() - 1 ? "*" : "");
        line.box = {QPointF(r.left(), r.top()), QPointF(r.right() + 1, r.top()), QPointF(r.right() + 1, r.bottom() + 1), QPointF(r.left(), r.bottom() + 1)};
        lines.append(line);
    }
    return lines;
}
}

// Long images go to the Snipping Tool OCR in bands: past about 2500 px along its long side the engine
// shrinks the image and loses small text.
class OcrBandsTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        get_tool_settings_manager(new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this));
        QVERIFY(QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/arial.ttf")) >= 0);
    }

    void cleanupTestCase() { release_ocr_engine(); }

    void bandsShareTheImageOutAndOverlapAtEachSplit() {
        const auto one = split_into_bands(2500, 2500, 100);
        QCOMPARE(one.size(), 1);
        QCOMPARE(std::tuple(one[0].top, one[0].bottom, one[0].own_top, one[0].own_bottom), std::tuple(0, 2500, 0, 2500));
        // 2300 rows of each 2500-row band are its own, so 2501 rows need two: 1250 and 1251 rows.
        const auto two = split_into_bands(2501, 2500, 100);
        QCOMPARE(two.size(), 2);
        QCOMPARE(std::tuple(two[0].top, two[0].bottom, two[0].own_top, two[0].own_bottom), std::tuple(0, 1350, 0, 1250));
        QCOMPARE(std::tuple(two[1].top, two[1].bottom, two[1].own_top, two[1].own_bottom), std::tuple(1150, 2501, 1250, 2501));
        // 9000 rows: four bands owning 2250 each.
        const auto four = split_into_bands(9000, 2500, 100);
        const QVector<std::tuple<int, int, int, int>> expected = {{0, 2350, 0, 2250}, {2150, 4600, 2250, 4500}, {4400, 6850, 4500, 6750}, {6650, 9000, 6750, 9000}};
        QCOMPARE(four.size(), expected.size());
        for (int i = 0; i < four.size(); ++i) QCOMPARE(std::tuple(four[i].top, four[i].bottom, four[i].own_top, four[i].own_bottom), expected[i]);
    }

    // Every line comes back once, whole and in the image's coordinates: those across a split, those
    // a band's edge cuts (centred on 2150, 2350, ...) and one 190 px tall centred on a split.
    void eachLineComesFromTheBandThatHoldsItWhole() {
        QMap<int, QRect> bars;
        int id = 1;
        for (int y = 20; y + 16 < 9000; y += 32) bars[id++] = QRect(10, y, 80, 16);
        for (const int centre : {2150, 2250, 2350, 4400, 4500, 4600, 6650, 6750, 6850}) bars[id++] = QRect(110, centre - 8, 80, 16);
        bars[id] = QRect(210, 4405, 80, 190);
        int calls = 0;
        const auto lines = recognize_in_bands(draw_bars(300, 9000, bars), 2500, 100, [&calls](const QImage &band) {
            ++calls;
            return read_bars(band);
        });
        QCOMPARE(calls, 4);
        QMap<int, QRect> read;
        for (const auto &line : lines) {
            bool ok = false;
            const int number = line.text.toInt(&ok);
            QVERIFY2(ok, qPrintable("a cut line came back: " + line.text));
            QVERIFY2(!read.contains(number), qPrintable("twice: " + line.text));
            read[number] = QPolygonF(line.box).boundingRect().toRect();
        }
        QCOMPARE(read.keys(), bars.keys());
        for (auto it = bars.cbegin(); it != bars.cend(); ++it) QCOMPARE(read[it.key()], it.value());
    }

    void anImageThatFitsGoesThroughWhole() {
        const QImage image = draw_bars(1080, 2500, {{1, QRect(10, 2480, 80, 16)}});
        QVector<qint64> seen;
        const auto lines = recognize_in_bands(image, 2500, 100, [&seen](const QImage &band) {
            seen.append(band.cacheKey());
            return read_bars(band);
        });
        QCOMPARE(seen, QVector<qint64>{image.cacheKey()});
        QCOMPARE(lines.size(), 1);
    }

    void cancellingStopsBeforeTheNextBand() {
        std::atomic_bool cancelled = false;
        int calls = 0;
        const auto lines = recognize_in_bands(draw_bars(200, 9000, {{1, QRect(10, 100, 80, 16)}, {2, QRect(10, 8000, 80, 16)}}), 2500, 100,
                                              [&](const QImage &band) {
                                                  ++calls;
                                                  cancelled = true;
                                                  return read_bars(band);
                                              },
                                              &cancelled);
        QCOMPARE(calls, 1);
        QVERIFY(lines.isEmpty());
    }

    // The real engine reads nothing of these pages whole; in bands it reads every line.
    void snippingToolOcrReadsALongScreenshot() {
        if (!get_available_engines().contains(OCRManager::ENGINE_ONEOCR)) QSKIP("the Snipping Tool OCR is not installed here");
        QVERIFY(set_ocr_engine(OCRManager::ENGINE_ONEOCR));
        for (const int pixels : {12, 14}) {
            const auto [image, drawn] = page(1080, 9000, pixels);
            QSet<QString> read;
            for (const auto &line : recognize_text(image).data) read.insert(line.text);
            QVERIFY2(read == drawn, qPrintable(QString("%1px: %2 of %3 read, %4 extra").arg(pixels).arg((read & drawn).size()).arg(drawn.size()).arg((read - drawn).size())));
        }
        std::atomic_bool cancelled = true;
        QVERIFY(recognize_text(page(1080, 9000, 14).first, &cancelled).data.isEmpty());
    }
};

QTEST_MAIN(OcrBandsTests)
#include "test_ocr_bands.moc"
