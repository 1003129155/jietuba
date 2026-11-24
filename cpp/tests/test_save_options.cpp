#include "core/save.h"
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>

using namespace jietuba;

namespace {
// Flat areas, a gradient and a little noise: the mix of a screenshot, large enough that
// lossy and lossless embeddings differ.
QImage screenshot_like(int width = 320, int height = 200) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(245, 245, 245));
    QRandomGenerator text(5);
    QPainter painter(&image);
    painter.fillRect(0, 0, width, height / 6, QColor(32, 96, 200));
    for (int row = 0; row < height / 10; ++row)
        for (int x = 8; x < width - 8; x += 3)
            if (text.bounded(5) < 3) painter.fillRect(x, height / 6 + 8 + row * 10, 2, 5, QColor(30, 30, 30));
    painter.end();
    QRandomGenerator random(11);
    for (int y = height * 3 / 4; y < height; ++y)
        for (int x = 0; x < width; ++x) image.setPixel(x, y, qRgb(x * 255 / width, y & 0xFF, random.bounded(256)));
    return image;
}
}

class SaveOptionsTests : public QObject {
    Q_OBJECT
private slots:
    // A colour image embedded as JPEG turns the text of a screenshot into smears.
    void pdfEmbedsImagesWithoutJpegCompression() {
        const auto pdf = SaveService::encode_image(screenshot_like(), "PDF", -1, SaveService::DEFAULT_PDF_DPI);
        QVERIFY(pdf.startsWith("%PDF-"));
        QVERIFY2(!pdf.contains("/DCTDecode"), "the image is stored as JPEG");
        QVERIFY(pdf.contains("/FlateDecode"));
    }
};

QTEST_MAIN(SaveOptionsTests)
#include "test_save_options.moc"
