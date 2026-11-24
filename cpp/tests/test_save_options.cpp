#include "core/clipboard_utils.h"
#include "core/save.h"
#include "settings/tool_settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QSemaphore>
#include <QSettings>
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

SaveOptions with_quality(int quality) {
    SaveOptions options;
    options.quality = quality;
    return options;
}

QByteArray file_bytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The only file in folder, or an empty path.
QString only_file(const QString &folder) {
    const auto names = QDir(folder).entryList(QDir::Files);
    return names.size() == 1 ? QDir(folder).filePath(names.first()) : QString();
}
}

class SaveOptionsTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ToolSettingsManager *config_ = nullptr;
private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        config_ = get_tool_settings_manager(settings_);
    }

    void init() {
        settings_->clear();
        QDir(dir_.filePath("shots")).removeRecursively();
        config_->set_screenshot_save_path(dir_.filePath("shots"));
    }

    // A colour image embedded as JPEG turns the text of a screenshot into smears.
    void pdfEmbedsImagesWithoutJpegCompression() {
        const auto pdf = SaveService::encode_image(screenshot_like(), "PDF", {});
        QVERIFY(pdf.startsWith("%PDF-"));
        QVERIFY2(!pdf.contains("/DCTDecode"), "the image is stored as JPEG");
        QVERIFY(pdf.contains("/FlateDecode"));
    }

    void optionsReadTheScreenshotQualityAndClampIt() {
        QCOMPARE(SaveOptions::from_settings(*config_).quality, 85);
        config_->set_screenshot_quality(40);
        QCOMPARE(SaveOptions::from_settings(*config_).quality, 40);
        QCOMPARE(SaveService(*config_).options().quality, 40);
        // A hand-edited file never hands the encoder a value it rejects.
        settings_->setValue("app/screenshot_quality", 500);
        QCOMPARE(SaveOptions::from_settings(*config_).quality, 100);
        settings_->setValue("app/screenshot_quality", 0);
        QCOMPARE(SaveOptions::from_settings(*config_).quality, 1);
    }

    // The dialog's quality opens as the screenshot quality and then goes its own way.
    void saveDialogQualityStartsFromTheScreenshotQualityAndStaysApart() {
        QCOMPARE(config_->get_save_dialog_quality(), 85);
        config_->set_screenshot_quality(60);
        QCOMPARE(config_->get_save_dialog_quality(), 60);
        config_->set_save_dialog_quality(30);
        QCOMPARE(config_->get_save_dialog_quality(), 30);
        QCOMPARE(config_->get_screenshot_quality(), 60);
        config_->set_screenshot_quality(70);
        QCOMPARE(config_->get_save_dialog_quality(), 30);
        config_->set_save_dialog_quality(500);
        QCOMPARE(config_->get_save_dialog_quality(), 100);
        config_->set_save_dialog_quality(-3);
        QCOMPARE(config_->get_save_dialog_quality(), 1);
    }

    // Auto-save writes JPG with the quality the settings hold, not Qt's default.
    void autoSaveUsesTheSettingsQuality() {
        const auto image = screenshot_like();
        config_->set_screenshot_save_enabled(true);
        config_->set_screenshot_format("JPG");
        QByteArray written[2];
        int index = 0;
        for (const int quality : {20, 90}) {
            QDir(dir_.filePath("shots")).removeRecursively();
            config_->set_screenshot_quality(quality);
            const auto delivery = deliver_screenshot(image, config_, false, "capture");
            QVERIFY(delivery);
            QVERIFY(delivery->join(10.0));
            const auto path = only_file(dir_.filePath("shots"));
            QVERIFY(!path.isEmpty());
            QVERIFY(path.endsWith(".jpg"));
            written[index] = file_bytes(path);
            QCOMPARE(written[index], SaveService::encode_image(image, "JPG", with_quality(quality)));
            ++index;
        }
        QVERIFY(written[0].size() < written[1].size());
    }

    // The same for the original saved beside a capture.
    void autoSavedOriginalUsesTheSettingsQuality() {
        const auto image = screenshot_like();
        config_->set_screenshot_save_enabled(true);
        config_->set_screenshot_save_original_enabled(true);
        config_->set_screenshot_format("WEBP");
        config_->set_screenshot_quality(35);
        const auto delivery = deliver_screenshot(image, config_, false, "capture", {}, image);
        QVERIFY(delivery);
        QVERIFY(delivery->join(10.0));
        const auto names = QDir(dir_.filePath("shots")).entryList(QDir::Files, QDir::Name);
        QCOMPARE(names.size(), 2);
        for (const auto &name : names) QCOMPARE(file_bytes(dir_.filePath("shots/" + name)), SaveService::encode_image(image, "WEBP", with_quality(35)));
    }

    // The quick save and the long screenshot save through the service with the settings' options.
    void serviceSavesTakeTheSettingsQuality() {
        const auto image = screenshot_like();
        config_->set_screenshot_quality(25);
        const SaveService service(*config_);
        const auto quick = service.save_qimage(image, dir_.filePath("shots"), "", "", "JPG", service.options());
        QVERIFY(quick.first && quick.second);
        QCOMPARE(file_bytes(*quick.second), SaveService::encode_image(image, "JPG", with_quality(25)));
        QSemaphore done;
        const auto async_path = service.save_qimage_async(image, dir_.filePath("shots"), "long", "", "WEBP", service.options(),
                                                          [&done](bool, QString) { done.release(); });
        QVERIFY(async_path);
        QVERIFY(done.tryAcquire(1, 10000));
        QCOMPARE(file_bytes(*async_path), SaveService::encode_image(image, "WEBP", with_quality(25)));
    }
};

QTEST_MAIN(SaveOptionsTests)
#include "test_save_options.moc"
