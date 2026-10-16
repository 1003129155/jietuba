#include "barcode/reader.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace jietuba;

// Expected values come from the Python reference (scripts/generate_barcode_fixtures.py).
class BarcodeTests : public QObject {
    Q_OBJECT
private slots:
    void readCodesMatchesReference() {
        QFile file(JIETUBA_BARCODE_FIXTURE_DIR "/barcodes.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto expected = QJsonDocument::fromJson(file.readAll()).array();
        QVERIFY(!expected.isEmpty());
        const QImage image(JIETUBA_BARCODE_FIXTURE_DIR "/barcodes.png");
        QVERIFY(!image.isNull());
        const auto codes = read_codes(image);
        QCOMPARE(static_cast<qsizetype>(codes.size()), expected.size());
        for (qsizetype index = 0; index < expected.size(); ++index) {
            const auto item = expected[index].toObject();
            QCOMPARE(codes[index].text, item["text"].toString());
            QCOMPARE(codes[index].format_name, item["format_name"].toString());
            const auto outline = item["outline"].toArray();
            QCOMPARE(codes[index].outline.size(), outline.size());
            for (qsizetype corner = 0; corner < outline.size(); ++corner) {
                QCOMPARE(codes[index].outline[corner].x(), outline[corner].toArray()[0].toDouble());
                QCOMPARE(codes[index].outline[corner].y(), outline[corner].toArray()[1].toDouble());
            }
        }
    }
    void emptyImageHasNoCodes() {
        QImage blank(64, 64, QImage::Format_ARGB32_Premultiplied);
        blank.fill(Qt::white);
        QVERIFY(read_codes(blank).empty());
    }
};

QTEST_MAIN(BarcodeTests)
#include "test_barcode.moc"
