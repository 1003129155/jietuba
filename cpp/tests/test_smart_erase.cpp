#include "canvas/smart_erase.h"
#include <QTest>

using namespace jietuba;

namespace {
const QRgb BACKGROUND = qRgb(0xF0, 0xE6, 0xD2);
const QRgb BUBBLE = qRgb(0x95, 0xEC, 0x69);

// A chat-like picture: flat background, a green bubble at (60, 40, 80, 30) with dark dots as text.
QImage chat_picture(QImage::Format format) {
    QImage image(240, 140, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const bool bubble = x >= 60 && x < 140 && y >= 40 && y < 70;
            image.setPixel(x, y, bubble ? ((x + y) % 5 == 0 ? qRgb(25, 25, 25) : BUBBLE) : BACKGROUND);
        }
    return image.convertToFormat(format);
}

QImage full_mask(const QSize &size) {
    QImage mask(size, QImage::Format_Grayscale8);
    mask.fill(255);
    return mask;
}
}

class SmartEraseTests : public QObject {
    Q_OBJECT
private slots:
    void aBubbleOnAFlatBackgroundDisappears_data() {
        QTest::addColumn<int>("format");
        QTest::newRow("RGB32") << int(QImage::Format_RGB32);
        QTest::newRow("ARGB32") << int(QImage::Format_ARGB32);
        QTest::newRow("ARGB32 premultiplied") << int(QImage::Format_ARGB32_Premultiplied);
        QTest::newRow("RGBA8888") << int(QImage::Format_RGBA8888);
        QTest::newRow("RGB888") << int(QImage::Format_RGB888);
    }
    void aBubbleOnAFlatBackgroundDisappears() {
        QFETCH(int, format);
        const QImage source = chat_picture(static_cast<QImage::Format>(format));
        const QRect rect(54, 34, 92, 42);
        const auto result = smart_erase(source, rect, full_mask(rect.size()));
        QCOMPARE(result.solver, SmartEraseSolver::Surface);
        QCOMPARE(result.image.size(), rect.size());
        for (int y = 0; y < rect.height(); ++y)
            for (int x = 0; x < rect.width(); ++x) QCOMPARE(result.image.pixel(x, y), BACKGROUND);
    }

    void unmaskedPixelsKeepTheirValues() {
        const QImage source = chat_picture(QImage::Format_RGB32);
        const QRect rect(54, 34, 92, 42);
        QImage mask(rect.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        // Only the bubble's left half; its right half must come back untouched.
        for (int y = 0; y < rect.height(); ++y)
            for (int x = 0; x < 46; ++x) mask.setPixel(x, y, 255);
        const auto result = smart_erase(source, rect, mask);
        QVERIFY(!result.image.isNull());
        for (int y = 0; y < rect.height(); ++y)
            for (int x = 46; x < rect.width(); ++x) QCOMPARE(result.image.pixel(x, y), source.pixel(rect.x() + x, rect.y() + y));
    }

    void aHoleFarFromTheOriginLandsInPlace() {
        // The window handed to the solver starts away from (0, 0); a misplaced hole would
        // sample the box instead of the gradient around it.
        QImage source(1600, 900, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y)
            for (int x = 0; x < source.width(); ++x) source.setPixel(x, y, qRgb(x / 8, y / 4, 120));
        for (int y = 500; y < 540; ++y)
            for (int x = 900; x < 960; ++x) source.setPixel(x, y, qRgb(0, 0, 0));
        const QRect rect(890, 490, 80, 60);
        const auto result = smart_erase(source, rect, full_mask(rect.size()));
        QCOMPARE(result.solver, SmartEraseSolver::Surface);
        for (int y = 0; y < rect.height(); y += 7)
            for (int x = 0; x < rect.width(); x += 9) {
                const QColor got(result.image.pixel(x, y));
                QVERIFY2(qAbs(got.red() - (rect.x() + x) / 8) <= 1 && qAbs(got.green() - (rect.y() + y) / 4) <= 1,
                         qPrintable(QString("%1,%2: %3").arg(x).arg(y).arg(got.name())));
            }
    }
    void aHoleAtTheImageCornerIsFilled() {
        QImage source(200, 150, QImage::Format_RGB32);
        source.fill(qRgb(30, 60, 90));
        for (int y = 0; y < 20; ++y)
            for (int x = 180; x < 200; ++x) source.setPixel(x, y, qRgb(250, 0, 0));
        const QRect rect(176, 0, 24, 24);
        const auto result = smart_erase(source, rect, full_mask(rect.size()));
        QCOMPARE(result.solver, SmartEraseSolver::Surface);
        QCOMPARE(result.image.pixel(20, 2), qRgb(30, 60, 90));
    }

    void cancellingReturnsNothing() {
        const QImage source = chat_picture(QImage::Format_RGB32);
        const QRect rect(54, 34, 92, 42);
        const std::atomic_bool cancelled{true};
        QVERIFY(smart_erase(source, rect, full_mask(rect.size()), &cancelled).image.isNull());
    }

    void argumentsThatDoNotFitReturnNothing() {
        const QImage source = chat_picture(QImage::Format_RGB32);
        QVERIFY(smart_erase(source, QRect(220, 10, 40, 40), full_mask(QSize(40, 40))).image.isNull());
        QVERIFY(smart_erase(source, QRect(10, 10, 40, 40), full_mask(QSize(40, 41))).image.isNull());
        QVERIFY(smart_erase(QImage(), QRect(0, 0, 4, 4), full_mask(QSize(4, 4))).image.isNull());
    }
};

QTEST_GUILESS_MAIN(SmartEraseTests)
#include "test_smart_erase.moc"
