#include "core/save.h"
#include "core/file_names.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <algorithm>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>
#include <QPageLayout>
#include <QPdfWriter>
#include <QThreadPool>
#include <QDebug>
#include <QRunnable>
#include <stdexcept>
#include <filesystem>
#include <system_error>
#include <cerrno>
#include <fcntl.h>
#ifdef Q_OS_WIN
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace jietuba {
SaveOptions SaveOptions::from_settings(const ToolSettingsManager &config) {
    SaveOptions options;
    options.quality = std::clamp(config.get_screenshot_quality(), 1, 100);
    const auto png = config.get_png_compression();
    options.png = png == "low" ? PngCompression::Low : png == "high" ? PngCompression::High : PngCompression::Medium;
    const auto page = config.get_pdf_page_size();
    options.pdf_page = page == "a4_portrait" ? PdfPageSize::A4Portrait : page == "a4_landscape" ? PdfPageSize::A4Landscape : PdfPageSize::Image;
    return options;
}

namespace {
// QImageWriter takes a PNG compression of c as zlib level c * 9 / 91, not as the level itself: these give levels 1, 6 and 9.
int png_compression_argument(SaveOptions::PngCompression compression) {
    switch (compression) {
    case SaveOptions::PngCompression::Low: return 11;
    case SaveOptions::PngCompression::High: return 91;
    case SaveOptions::PngCompression::Medium: break;
    }
    return 61;
}

bool write_raster(QImageWriter &writer, const QImage &image, const QString &format, const SaveOptions &options) {
    if (format == "PNG") writer.setCompression(png_compression_argument(options.png));
    else if (SaveService::format_has_quality(format)) writer.setQuality(options.quality);
    return writer.write(image);
}

constexpr double POINTS_PER_MM = 72.0 / 25.4;
constexpr double A4_SHORT_SIDE = 210 * POINTS_PER_MM;
constexpr double A4_LONG_SIDE = 297 * POINTS_PER_MM;
constexpr double A4_MARGIN = 10 * POINTS_PER_MM;
// Images are placed on an A4 page as they would show at 96 dpi.
constexpr double POINTS_PER_PIXEL = 72.0 / 96.0;
constexpr double LAYOUT_EPSILON = 1e-6;
}

QString SaveService::get_default_directory() const { return config_manager.get_screenshot_save_path(); }

// Given only a file name, the dialog opens in the process working directory (the install directory when packaged).
QString SaveService::suggest_path(const QString &filename) const {
    const auto directory = get_default_directory();
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(directory.toStdWString()), error);
    if (error) {
        log_warning(T("无法创建保存目录: {directory} ({e})", {{"directory", directory}, {"e", QString::fromLocal8Bit(error.message())}}), "Save");
        return filename;
    }
    return QDir(directory).filePath(filename);
}
QString SaveService::normalize_format(QString format) {
    if (format.isEmpty()) format = "PNG";
    format = format.trimmed();
    while (format.startsWith('.')) format.remove(0, 1);
    format = format.toUpper();
    return format == "JPEG" ? "JPG" : format;
}
QString SaveService::format_for_size(QString format, int width, int height, const SaveOptions &options) {
    format = normalize_format(format);
    // An A4 page scales and splits the image, so only an image-sized page has a longest side.
    const bool image_sized_pdf = format == "PDF" && options.pdf_page == SaveOptions::PdfPageSize::Image;
    const qint64 limit = format == "JPG" ? 65500 : format == "WEBP" ? 16383 : image_sized_pdf ? qint64(200) * options.pdf_dpi : -1;
    return limit != -1 && std::max(width, height) > limit ? "PNG" : format;
}
QString SaveService::claim_first_free(const QString &stem, const QString &ext) {
    for (int index = 0; index < 10000; ++index) {
        const auto path = stem + (index ? "_" + QString::number(index) : "") + ext;
        int fd = -1;
#ifdef Q_OS_WIN
        const int error = _wsopen_s(&fd, reinterpret_cast<const wchar_t *>(path.utf16()),
            _O_CREAT | _O_EXCL | _O_WRONLY, _SH_DENYNO, _S_IREAD | _S_IWRITE);
#else
        fd = ::open(path.toUtf8().constData(), O_CREAT | O_EXCL | O_WRONLY, 0777);
        const int error = fd < 0 ? errno : 0;
#endif
        if (fd >= 0) {
#ifdef Q_OS_WIN
            _close(fd);
#else
            ::close(fd);
#endif
            return path;
        }
        if (error != EEXIST) throw std::runtime_error("Unable to reserve screenshot filename");
    }
    throw std::runtime_error("No free screenshot filename after 10000 attempts");
}
QString SaveService::compose_path(QString directory, const QString &prefix, const QString &suffix, const QString &format) const {
    if (directory.isEmpty() || !ToolSettingsManager::is_absolute_save_path(directory)) directory = get_default_directory();
    if (!QDir().mkpath(directory)) throw std::runtime_error("Unable to create screenshot directory");
    const auto kind = prefix == "pinned" ? file_names::Pin : prefix.isEmpty() ? file_names::Screenshot : file_names::LongScreenshot;
    const auto filename = file_names::stem(config_manager, kind, prefix) + (suffix.isEmpty() ? QString() : "_" + suffix) + "." + normalize_format(format).toLower();
    const int dot = filename.lastIndexOf('.');
    return claim_first_free(QDir(directory).filePath(filename.left(dot)), filename.mid(dot));
}
QString SaveService::reserve_save_path(QString directory, QString prefix, QString suffix, QString format) const {
    // The only entry point that still throws: the clipboard delivery reserves a name up front and
    // catches to degrade to "no file reference". Everything below turns the same failures into a
    // plain false, because a throw from inside a Qt slot ends the process.
    return compose_path(directory, prefix, suffix, format);
}
void SaveService::cleanup_failed_placeholder(const QString &path) {
    const QFileInfo file(path);
    if (file.exists() && file.size() == 0) QFile::remove(path);
}
QImage SaveService::flatten_for_pdf(const QImage &image) {
    if (!image.hasAlphaChannel()) return image.convertToFormat(QImage::Format_RGB32);
    QImage flattened(image.size(), QImage::Format_RGB32);
    flattened.fill(0xffffffff);
    QPainter painter(&flattened);
    painter.drawImage(0, 0, image);
    painter.end();
    return flattened;
}
QList<PdfPageSlice> SaveService::pdf_layout(const QSize &size, const SaveOptions &options) {
    if (size.isEmpty()) return {};
    if (options.pdf_page == SaveOptions::PdfPageSize::Image) {
        const double dpi = std::max(72, options.pdf_dpi);
        const QSizeF page(size.width() * 72.0 / dpi, size.height() * 72.0 / dpi);
        return {{page, QRectF(QPointF(0, 0), page), 0, size.height()}};
    }
    const bool landscape = options.pdf_page == SaveOptions::PdfPageSize::A4Landscape;
    const QSizeF page(landscape ? A4_LONG_SIDE : A4_SHORT_SIDE, landscape ? A4_SHORT_SIDE : A4_LONG_SIDE);
    const double printable_width = page.width() - 2 * A4_MARGIN, printable_height = page.height() - 2 * A4_MARGIN;
    const double scale = std::min(1.0, printable_width / (size.width() * POINTS_PER_PIXEL));
    const double points_per_pixel = POINTS_PER_PIXEL * scale;
    const double width = size.width() * points_per_pixel;
    const double left = A4_MARGIN + (printable_width - width) / 2;
    const double height = size.height() * points_per_pixel;
    if (height <= printable_height + LAYOUT_EPSILON) {
        const double top = A4_MARGIN + (printable_height - height) / 2;
        return {{page, QRectF(left, top, width, height), 0, size.height()}};
    }
    const int rows_per_page = std::max(1, static_cast<int>(std::floor(printable_height / points_per_pixel + LAYOUT_EPSILON)));
    QList<PdfPageSlice> pages;
    for (int row = 0; row < size.height(); row += rows_per_page) {
        const int rows = std::min(rows_per_page, size.height() - row);
        pages.append({page, QRectF(left, A4_MARGIN, width, rows * points_per_pixel), row, rows});
    }
    return pages;
}
bool SaveService::save_pdf(const QImage &image, QIODevice *device, const SaveOptions &options) {
    const auto flattened = flatten_for_pdf(image);
    const auto pages = pdf_layout(flattened.size(), options);
    if (pages.isEmpty()) return false;
    QPdfWriter writer(device);
    const int dpi = std::max(72, options.pdf_dpi);
    const bool image_sized = options.pdf_page == SaveOptions::PdfPageSize::Image;
    writer.setResolution(dpi);
    if (image_sized) {
        writer.setPageSize(QPageSize(pages.first().page, QPageSize::Point, "Screenshot"));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
    } else {
        const bool landscape = options.pdf_page == SaveOptions::PdfPageSize::A4Landscape;
        writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), landscape ? QPageLayout::Landscape : QPageLayout::Portrait, QMarginsF(0, 0, 0, 0)));
    }
    writer.setCreator("jietuba");
    QPainter painter(&writer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    // Without it a colour image is embedded as JPEG, which blurs the text of a screenshot.
    painter.setRenderHint(QPainter::LosslessImageRendering);
    const double dots_per_point = dpi / 72.0;
    for (int index = 0; index < pages.size(); ++index) {
        if (index > 0) writer.newPage();
        const auto &slice = pages.at(index);
        const QRectF target = image_sized ? QRectF(0, 0, writer.width(), writer.height())
                                          : QRectF(slice.target.x() * dots_per_point, slice.target.y() * dots_per_point,
                                                   slice.target.width() * dots_per_point, slice.target.height() * dots_per_point);
        painter.drawImage(target, flattened, QRectF(0, slice.source_y, flattened.width(), slice.source_rows));
    }
    painter.end();
    return true;
}
bool SaveService::save_qimage_to_path(const QImage &image, const QString &path, QString format, const SaveOptions &options) {
    if (image.isNull()) return false;
    if (format.isEmpty()) {
        const auto dot = QFileInfo(path).fileName().lastIndexOf('.');
        format = dot < 0 ? "PNG" : QFileInfo(path).fileName().mid(dot);
    }
    format = normalize_format(format);
    if (format == "PDF") {
        QFile file(path);
        const bool success = file.open(QIODevice::WriteOnly) && save_pdf(image, &file, options);
        file.close();
        if (!success) cleanup_failed_placeholder(path);
        return success;
    }
    bool success;
    {
        // Closed before the cleanup below, which cannot remove a file the writer still holds.
        QImageWriter writer(path, format.toLatin1());
        success = write_raster(writer, image, format, options);
    }
    if (!success) cleanup_failed_placeholder(path);
    return success;
}
QByteArray SaveService::encode_image(const QImage &image, QString format, const SaveOptions &options) {
    if (image.isNull()) return {};
    format = normalize_format(format);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    bool success;
    if (format == "PDF") {
        success = save_pdf(image, &buffer, options);
    } else {
        QImageWriter writer(&buffer, format.toLatin1());
        success = write_raster(writer, image, format, options);
    }
    buffer.close();
    return success ? bytes : QByteArray();
}
bool SaveService::format_has_quality(QString format) {
    format = normalize_format(format);
    return format == "JPG" || format == "WEBP";
}
std::pair<int, QByteArray> SaveService::encode_within(const QImage &image, QString format, qint64 max_bytes, const SaveOptions &options) {
    int low = 1, high = 100, best = -1;
    SaveOptions probe = options;
    QByteArray best_bytes, lowest_bytes;
    while (low <= high) {
        const int quality = (low + high) / 2;
        probe.quality = quality;
        auto bytes = encode_image(image, format, probe);
        if (bytes.isEmpty()) return {quality, {}};
        if (bytes.size() <= max_bytes) {
            best = quality;
            best_bytes = std::move(bytes);
            low = quality + 1;
        } else {
            if (quality == 1) lowest_bytes = std::move(bytes);
            high = quality - 1;
        }
    }
    if (best > 0) return {best, best_bytes};
    probe.quality = 1;
    return {1, lowest_bytes.isEmpty() ? encode_image(image, format, probe) : lowest_bytes};
}
QImage SaveService::scale_for_save(const QImage &image, const QSize &size) {
    if (image.isNull() || size.isEmpty() || size == image.size()) return image;
    return image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
std::pair<bool, std::optional<QString>> SaveService::save_qimage(const QImage &image, QString directory,
    QString prefix, QString suffix, QString format, const SaveOptions &options, QString target) const {
    if (image.isNull()) return {false, std::nullopt};
    if (target.isEmpty()) {
        // An unwritable folder or an exhausted name pool is a failed save, not a crash:
        // finish() runs synchronously from the confirm button and nothing above catches.
        try {
            target = compose_path(directory, prefix, suffix, format);
        } catch (const std::exception &error) {
            log_warning(T("保存失败: 无法确定文件名 ({e})", {{"e", QString::fromUtf8(error.what())}}), "Save");
            return {false, std::nullopt};
        }
    }
    return {save_qimage_to_path(image, target, format, options), target};
}
std::optional<QString> SaveService::save_qimage_async(const QImage &image, QString directory, QString prefix,
    QString suffix, QString format, const SaveOptions &options, std::function<void(bool, QString)> callback) const {
    if (image.isNull()) return std::nullopt;
    auto copy = image.copy();
    QString path;
    try {
        path = compose_path(directory, prefix, suffix, format);
    } catch (const std::exception &error) {
        // No path, no worker: the caller treats an empty return as "the save did not start".
        log_warning(T("保存失败: 无法确定文件名 ({e})", {{"e", QString::fromUtf8(error.what())}}), "Save");
        return std::nullopt;
    }
    // As in Python, callback executes on the worker thread. No borrowed service
    // or config pointer crosses the boundary; cancellation/lifecycle remains explicit.
    QThreadPool::globalInstance()->start(QRunnable::create([copy = std::move(copy), path, format, options, callback = std::move(callback)] {
        try {
            const auto success = save_qimage_to_path(copy, path, format, options);
            if (callback) callback(success, path);
        } catch (...) {
            // Python's background-task wrapper captures worker exceptions.
            qWarning() << "Screenshot save worker/callback failed";
        }
    }));
    return path;
}
std::optional<QString> SaveService::save_beside(const QImage &image, const QString &saved, const QString &tag, const SaveOptions &options) {
    if (image.isNull() || saved.isEmpty()) return std::nullopt;
    const auto filename = QFileInfo(saved).fileName();
    const int dot = filename.lastIndexOf('.');
    const bool has_ext = dot > 0;
    const auto stem = has_ext ? saved.left(saved.size() - filename.size() + dot) : saved;
    const auto ext = has_ext ? filename.mid(dot) : QString();
    QString target;
    try {
        target = claim_first_free(stem + "_" + tag, ext);
    } catch (const std::exception &error) {
        // The original is a side copy of a save that already succeeded; never fail the caller over it.
        log_warning(T("保存失败: 无法为原图确定文件名 ({e})", {{"e", QString::fromUtf8(error.what())}}), "Save");
        return std::nullopt;
    }
    return save_qimage_to_path(image, target, {}, options) ? std::optional<QString>(target) : std::nullopt;
}
}
