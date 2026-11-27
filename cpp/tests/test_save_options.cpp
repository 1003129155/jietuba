#include "core/clipboard_utils.h"
#include "core/save.h"
#include "settings/tool_settings.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSemaphore>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

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

SaveOptions png_options(SaveOptions::PngCompression compression) {
    SaveOptions options;
    options.png = compression;
    return options;
}

SaveOptions pdf_options(SaveOptions::PdfPageSize page, int dpi = SaveOptions::DEFAULT_PDF_DPI) {
    SaveOptions options;
    options.pdf_page = page;
    options.pdf_dpi = dpi;
    return options;
}

// The zlib compression-level flag and the type of the first deflate block of a PNG's image data.
struct DeflateHeader {
    int level_flag = -1;
    int block_type = -1;
};

DeflateHeader deflate_header(const QByteArray &png) {
    qsizetype position = 8;
    while (position + 8 <= png.size()) {
        const auto length = qFromBigEndian<quint32>(png.constData() + position);
        if (png.mid(position + 4, 4) == "IDAT") {
            const auto *data = reinterpret_cast<const uchar *>(png.constData()) + position + 8;
            return {data[1] >> 6, (data[2] >> 1) & 3};
        }
        position += 12 + length;
    }
    return {};
}

// The MediaBox size of every page, in points.
QList<QSizeF> page_sizes(const QByteArray &pdf) {
    static const QRegularExpression box(R"(/MediaBox\s*\[\s*[\d.]+\s+[\d.]+\s+([\d.]+)\s+([\d.]+)\s*\])");
    QList<QSizeF> sizes;
    const auto text = QString::fromLatin1(pdf);
    for (auto it = box.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        sizes.append(QSizeF(match.captured(1).toDouble(), match.captured(2).toDouble()));
    }
    return sizes;
}

// Where each page draws its image: the rectangle in points from the top left of the page, in page order.
// Qt writes a page's content as a scale to points, a scale and an offset in drawing units, then the image's own size.
QList<QRectF> image_rects(const QByteArray &pdf) {
    static const QRegularExpression stream(R"(stream\r?\n)");
    static const QRegularExpression matrix(R"(([-\d.]+) 0 0 ([-\d.]+) 0 [-\d.]+ cm)");
    static const QRegularExpression placed(R"(([-\d.]+) 0 0 ([-\d.]+) ([-\d.]+) ([-\d.]+) cm\s+([-\d.]+) 0 0 ([-\d.]+) 0 [-\d.]+ cm /\w+\s+Do)");
    QList<QRectF> rects;
    qsizetype from = 0;
    while (true) {
        const qsizetype begin = pdf.indexOf("stream", from);
        if (begin < 0) break;
        const qsizetype start = pdf.indexOf('\n', begin) + 1;
        const qsizetype end = pdf.indexOf("endstream", start);
        from = end + 9;
        if (end < 0) break;
        // qUncompress wants the expected size up front; it grows its buffer when that is too small.
        QByteArray packed(4, '\0');
        packed[2] = 0x10;
        packed.append(pdf.mid(start, end - start));
        const auto text = QString::fromLatin1(qUncompress(packed));
        if (!text.contains(" Do")) continue;
        const auto page = matrix.match(text);
        const auto draw = placed.match(text);
        if (!page.hasMatch() || !draw.hasMatch()) continue;
        const double to_points = page.captured(1).toDouble();
        const double scale = draw.captured(1).toDouble() * to_points;
        rects.append(QRectF(draw.captured(3).toDouble() * to_points, draw.captured(4).toDouble() * to_points,
                            draw.captured(5).toDouble() * scale, qAbs(draw.captured(6).toDouble()) * scale));
    }
    return rects;
}

bool near(double a, double b, double tolerance = 0.02) { return qAbs(a - b) <= tolerance; }
bool near(const QRectF &a, const QRectF &b) { return near(a.x(), b.x()) && near(a.y(), b.y()) && near(a.width(), b.width()) && near(a.height(), b.height()); }

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

    // QImageWriter reads a PNG compression c as zlib level c * 9 / 91, so the levels 1, 6 and 9 are asked for as 11, 61 and 91.
    // Asking for 1, 6 and 9 would write every PNG with no compression at all.
    void pngLevelsAreZlibLevels1And6And9() {
        const auto image = screenshot_like(600, 400);
        const auto low = SaveService::encode_image(image, "PNG", png_options(SaveOptions::PngCompression::Low));
        const auto medium = SaveService::encode_image(image, "PNG", png_options(SaveOptions::PngCompression::Medium));
        const auto high = SaveService::encode_image(image, "PNG", png_options(SaveOptions::PngCompression::High));
        // The header zlib writes says which of its level groups was used: fastest (1), default (6), best (7 to 9).
        QCOMPARE(deflate_header(low).level_flag, 0);
        QCOMPARE(deflate_header(medium).level_flag, 2);
        QCOMPARE(deflate_header(high).level_flag, 3);
        // Level 0 stores the data as it is, level 1 does not.
        QVERIFY(deflate_header(low).block_type != 0);
        QByteArray stored_bytes;
        QBuffer buffer(&stored_bytes);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "PNG");
        writer.setCompression(1);
        QVERIFY(writer.write(image));
        QCOMPARE(deflate_header(stored_bytes).block_type, 0);
        QVERIFY(low.size() * 3 < stored_bytes.size());
        QVERIFY(high.size() <= medium.size());
        QVERIFY(medium.size() <= low.size());
        QVERIFY(high.size() < low.size());
        // The default is what saving wrote before the setting existed.
        QByteArray before;
        QBuffer before_buffer(&before);
        before_buffer.open(QIODevice::WriteOnly);
        QVERIFY(image.save(&before_buffer, "PNG"));
        QCOMPARE(medium, before);
        for (const auto &bytes : {low, medium, high}) QCOMPARE(QImage::fromData(bytes, "PNG").convertToFormat(QImage::Format_RGB32), image);
    }

    void pngLevelLeavesTheOtherFormatsAlone() {
        const auto image = screenshot_like();
        SaveOptions low, high;
        low.png = SaveOptions::PngCompression::Low;
        high.png = SaveOptions::PngCompression::High;
        for (const char *format : {"JPG", "WEBP", "BMP"}) QCOMPARE(SaveService::encode_image(image, format, low), SaveService::encode_image(image, format, high));
        // A PDF carries a date and an id, so only what the PNG level could change is compared: the length.
        QCOMPARE(SaveService::encode_image(image, "PDF", low).size(), SaveService::encode_image(image, "PDF", high).size());
    }

    void saveToPathWritesWhatEncodeImageReturns() {
        const auto image = screenshot_like();
        for (const auto level : {SaveOptions::PngCompression::Low, SaveOptions::PngCompression::High}) {
            const auto options = png_options(level);
            const auto path = dir_.filePath(QString("level%1.png").arg(int(level)));
            QVERIFY(SaveService::save_qimage_to_path(image, path, "PNG", options));
            QCOMPARE(file_bytes(path), SaveService::encode_image(image, "PNG", options));
        }
        const auto options = pdf_options(SaveOptions::PdfPageSize::A4Landscape);
        const auto pdf = dir_.filePath("page.pdf");
        QVERIFY(SaveService::save_qimage_to_path(image, pdf, "PDF", options));
        const auto saved = file_bytes(pdf);
        QCOMPARE(page_sizes(saved), page_sizes(SaveService::encode_image(image, "PDF", options)));
        QCOMPARE(saved.size(), SaveService::encode_image(image, "PDF", options).size());
        QVERIFY(!SaveService::save_qimage_to_path(image, dir_.filePath("x.bad"), "INVALID", {}));
    }

    void optionsReadThePngLevelAndThePdfPage() {
        QCOMPARE(SaveOptions::from_settings(*config_).png, SaveOptions::PngCompression::Medium);
        QCOMPARE(SaveOptions::from_settings(*config_).pdf_page, SaveOptions::PdfPageSize::Image);
        config_->set_png_compression("low");
        config_->set_pdf_page_size("a4_portrait");
        QCOMPARE(SaveOptions::from_settings(*config_).png, SaveOptions::PngCompression::Low);
        QCOMPARE(SaveOptions::from_settings(*config_).pdf_page, SaveOptions::PdfPageSize::A4Portrait);
        config_->set_png_compression("HIGH");
        config_->set_pdf_page_size("a4_landscape");
        QCOMPARE(SaveOptions::from_settings(*config_).png, SaveOptions::PngCompression::High);
        QCOMPARE(SaveOptions::from_settings(*config_).pdf_page, SaveOptions::PdfPageSize::A4Landscape);
        // A value no choice matches reads as the default, from a file edited by hand or imported from a newer version.
        settings_->setValue("app/png_compression", "extreme");
        settings_->setValue("app/pdf_page_size", "letter");
        QCOMPARE(config_->get_png_compression(), QString("medium"));
        QCOMPARE(config_->get_pdf_page_size(), QString("image"));
        QCOMPARE(SaveOptions::from_settings(*config_).png, SaveOptions::PngCompression::Medium);
        config_->set_png_compression("extreme");
        config_->set_pdf_page_size("letter");
        QCOMPARE(config_->get_png_compression(), QString("medium"));
        QCOMPARE(config_->get_pdf_page_size(), QString("image"));
    }

    // The page of an image-sized PDF is the image's pixels at the PDF's dpi, in points.
    void imageSizedPageIsThePixelsAtTheDpi() {
        auto pages = SaveService::pdf_layout(QSize(600, 400), pdf_options(SaveOptions::PdfPageSize::Image));
        QCOMPARE(pages.size(), 1);
        QVERIFY(near(pages[0].page.width(), 144) && near(pages[0].page.height(), 96));
        QVERIFY(near(pages[0].target, QRectF(0, 0, 144, 96)));
        QCOMPARE(pages[0].source_y, 0);
        QCOMPARE(pages[0].source_rows, 400);
        pages = SaveService::pdf_layout(QSize(600, 400), pdf_options(SaveOptions::PdfPageSize::Image, 150));
        QVERIFY(near(pages[0].page.width(), 288) && near(pages[0].page.height(), 192));
        // Below 72 dpi the drawing space is still 72.
        pages = SaveService::pdf_layout(QSize(600, 400), pdf_options(SaveOptions::PdfPageSize::Image, 10));
        QVERIFY(near(pages[0].page.width(), 600));
        QVERIFY(SaveService::pdf_layout(QSize(), pdf_options(SaveOptions::PdfPageSize::A4Portrait)).isEmpty());
        QVERIFY(SaveService::pdf_layout(QSize(0, 40), pdf_options(SaveOptions::PdfPageSize::Image)).isEmpty());
    }

    // A4 is 595.28 x 841.89 pt; with a 10 mm (28.35 pt) margin the printable area is 538.58 x 785.20 pt.
    // Images are measured at 96 dpi, 0.75 pt a pixel, and only ever shrunk.
    void smallImageOnA4KeepsItsSizeAndIsCentred() {
        const auto portrait = SaveService::pdf_layout(QSize(100, 50), pdf_options(SaveOptions::PdfPageSize::A4Portrait));
        QCOMPARE(portrait.size(), 1);
        QVERIFY(near(portrait[0].page.width(), 595.276) && near(portrait[0].page.height(), 841.890));
        // (538.583 - 75) / 2 + 28.346 and (785.197 - 37.5) / 2 + 28.346
        QVERIFY(near(portrait[0].target, QRectF(260.138, 402.195, 75, 37.5)));
        QCOMPARE(portrait[0].source_y, 0);
        QCOMPARE(portrait[0].source_rows, 50);
        const auto landscape = SaveService::pdf_layout(QSize(1000, 300), pdf_options(SaveOptions::PdfPageSize::A4Landscape));
        QCOMPARE(landscape.size(), 1);
        QVERIFY(near(landscape[0].page.width(), 841.890) && near(landscape[0].page.height(), 595.276));
        // (785.197 - 750) / 2 + 28.346 and (538.583 - 225) / 2 + 28.346
        QVERIFY(near(landscape[0].target, QRectF(45.945, 185.138, 750, 225)));
    }

    // A page's rows are flattened on their own, onto white, as they are in the whole image.
    void aPageOfRowsFlattensLikeTheWholeImage() {
        QImage image(3, 4, QImage::Format_ARGB32);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 3; ++x) image.setPixel(x, y, qRgba(60 * y, 80 * x, 200, y == 2 ? 0 : 255));
        const auto band = SaveService::flatten_for_pdf(image, 1, 2);
        QCOMPARE(band.size(), QSize(3, 2));
        QCOMPARE(band.format(), QImage::Format_RGB32);
        QCOMPARE(band, SaveService::flatten_for_pdf(image).copy(0, 1, 3, 2));
        QCOMPARE(band.pixel(0, 0), qRgb(60, 0, 200));
        QCOMPARE(band.pixel(1, 1), qRgb(255, 255, 255));
        QImage opaque(3, 4, QImage::Format_RGB888);
        opaque.fill(QColor(10, 20, 30));
        const auto last_row = SaveService::flatten_for_pdf(opaque, 3, 1);
        QCOMPARE(last_row.size(), QSize(3, 1));
        QCOMPARE(last_row.format(), QImage::Format_RGB32);
        QCOMPARE(last_row.pixel(2, 0), qRgb(10, 20, 30));
    }

    void wideImageOnA4ShrinksToThePrintableWidth() {
        const auto pages = SaveService::pdf_layout(QSize(1440, 300), pdf_options(SaveOptions::PdfPageSize::A4Portrait));
        QCOMPARE(pages.size(), 1);
        // 1440 px are 1080 pt; 538.583 / 1080 = 0.49869 of that is 112.20 pt high.
        QVERIFY(near(pages[0].target, QRectF(28.346, 28.346 + (785.197 - 112.205) / 2, 538.583, 112.205)));
    }

    void tallImageOnA4IsCutIntoPagesOfTheSameWidth() {
        // 720 px are 540 pt, shrunk to 538.583: 0.748031 pt a pixel, so 785.197 pt hold 1049 rows.
        const auto pages = SaveService::pdf_layout(QSize(720, 4000), pdf_options(SaveOptions::PdfPageSize::A4Portrait));
        QCOMPARE(pages.size(), 4);
        const int rows[] = {1049, 1049, 1049, 853};
        for (int index = 0; index < 4; ++index) {
            QCOMPARE(pages[index].source_y, 1049 * index);
            QCOMPARE(pages[index].source_rows, rows[index]);
            QVERIFY(near(pages[index].target.x(), 28.346));
            QVERIFY(near(pages[index].target.y(), 28.346));
            QVERIFY(near(pages[index].target.width(), 538.583));
            QVERIFY(near(pages[index].target.height(), rows[index] * 0.748031));
        }
        // 900 px stay 675 pt wide on a landscape page, where 538.583 pt hold 718 rows.
        const auto landscape = SaveService::pdf_layout(QSize(900, 2000), pdf_options(SaveOptions::PdfPageSize::A4Landscape));
        QCOMPARE(landscape.size(), 3);
        QCOMPARE(landscape[2].source_y, 1436);
        QCOMPARE(landscape[2].source_rows, 564);
        QVERIFY(near(landscape[0].target, QRectF(83.445, 28.346, 675, 718 * 0.75)));
        QVERIFY(near(landscape[2].target.height(), 564 * 0.75));
    }

    // 1046 rows are 784.5 pt, which fits the 785.2 pt of a page; one row more does not.
    void theLastRowThatFitsStaysOnOnePage() {
        QCOMPARE(SaveService::pdf_layout(QSize(100, 1046), pdf_options(SaveOptions::PdfPageSize::A4Portrait)).size(), 1);
        const auto two = SaveService::pdf_layout(QSize(100, 1047), pdf_options(SaveOptions::PdfPageSize::A4Portrait));
        QCOMPARE(two.size(), 2);
        QCOMPARE(two[1].source_y, 1046);
        QCOMPARE(two[1].source_rows, 1);
    }

    void pdfFilesHaveTheLaidOutPages() {
        // Qt writes the page size in whole points.
        auto pdf = SaveService::encode_image(screenshot_like(600, 400), "PDF", pdf_options(SaveOptions::PdfPageSize::Image));
        QCOMPARE(page_sizes(pdf), QList<QSizeF>{QSizeF(144, 96)});
        pdf = SaveService::encode_image(screenshot_like(100, 50), "PDF", pdf_options(SaveOptions::PdfPageSize::A4Portrait));
        QCOMPARE(page_sizes(pdf), QList<QSizeF>{QSizeF(595, 842)});
        auto rects = image_rects(pdf);
        QCOMPARE(rects.size(), 1);
        QVERIFY2(near(rects[0], QRectF(260.138, 402.195, 75, 37.5)), qPrintable(QString("%1 %2 %3 %4").arg(rects[0].x()).arg(rects[0].y()).arg(rects[0].width()).arg(rects[0].height())));
        pdf = SaveService::encode_image(screenshot_like(1000, 300), "PDF", pdf_options(SaveOptions::PdfPageSize::A4Landscape));
        QCOMPARE(page_sizes(pdf), QList<QSizeF>{QSizeF(842, 595)});
        rects = image_rects(pdf);
        QCOMPARE(rects.size(), 1);
        QVERIFY(near(rects[0], QRectF(45.945, 185.138, 750, 225)));
        // The long image makes four pages of one size, each with its slice drawn at the top of the margin.
        pdf = SaveService::encode_image(screenshot_like(720, 4000), "PDF", pdf_options(SaveOptions::PdfPageSize::A4Portrait));
        QCOMPARE(page_sizes(pdf), QList<QSizeF>(4, QSizeF(595, 842)));
        rects = image_rects(pdf);
        QCOMPARE(rects.size(), 4);
        for (const auto &rect : rects) QVERIFY(near(rect.x(), 28.346) && near(rect.y(), 28.346) && near(rect.width(), 538.583));
        QVERIFY(near(rects[0].height(), 1049 * 0.748031));
        QVERIFY(near(rects[3].height(), 853 * 0.748031));
        QVERIFY(!pdf.contains("/DCTDecode"));
        // Each page holds its rows, not a second copy of the whole image.
        QVERIFY(pdf.size() < SaveService::encode_image(screenshot_like(720, 4000), "PDF", pdf_options(SaveOptions::PdfPageSize::Image)).size() * 3 / 2);
    }

    // An image-sized page has a longest side the PDF format allows; an A4 page scales and splits, so it has none.
    void onlyImageSizedPdfPagesLimitTheLongestSide() {
        QCOMPARE(SaveService::format_for_size("PDF", 70000, 100, pdf_options(SaveOptions::PdfPageSize::Image)), QString("PNG"));
        QCOMPARE(SaveService::format_for_size("PDF", 60000, 100, pdf_options(SaveOptions::PdfPageSize::Image)), QString("PDF"));
        QCOMPARE(SaveService::format_for_size("PDF", 150, 140000, pdf_options(SaveOptions::PdfPageSize::Image, 150)), QString("PNG"));
        QCOMPARE(SaveService::format_for_size("PDF", 70000, 100, pdf_options(SaveOptions::PdfPageSize::A4Portrait)), QString("PDF"));
        QCOMPARE(SaveService::format_for_size("PDF", 100, 90000, pdf_options(SaveOptions::PdfPageSize::A4Landscape)), QString("PDF"));
        QCOMPARE(SaveService::format_for_size("JPG", 70000, 100, pdf_options(SaveOptions::PdfPageSize::A4Portrait)), QString("PNG"));
        QCOMPARE(SaveService::format_for_size("WEBP", 100, 20000, {}), QString("PNG"));
        QCOMPARE(SaveService::format_for_size("PNG", 100, 200000, {}), QString("PNG"));
    }

    // The auto-save writes the level and the page the settings name.
    void autoSaveUsesThePngLevelAndThePdfPage() {
        const auto image = screenshot_like(600, 400);
        config_->set_screenshot_save_enabled(true);
        config_->set_screenshot_format("PNG");
        config_->set_png_compression("high");
        auto delivery = deliver_screenshot(image, config_, false, "capture");
        QVERIFY(delivery && delivery->join(10.0));
        auto path = only_file(dir_.filePath("shots"));
        QVERIFY(path.endsWith(".png"));
        QCOMPARE(deflate_header(file_bytes(path)).level_flag, 3);
        QDir(dir_.filePath("shots")).removeRecursively();
        config_->set_screenshot_format("PDF");
        config_->set_pdf_page_size("a4_landscape");
        delivery = deliver_screenshot(image, config_, false, "capture");
        QVERIFY(delivery && delivery->join(10.0));
        path = only_file(dir_.filePath("shots"));
        QVERIFY(path.endsWith(".pdf"));
        QCOMPARE(page_sizes(file_bytes(path)), QList<QSizeF>{QSizeF(842, 595)});
    }
};

QTEST_MAIN(SaveOptionsTests)
#include "test_save_options.moc"
