#include "core/save.h"
#include "core/file_names.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
QString SaveService::format_for_size(QString format, int width, int height, int dpi) {
    format = normalize_format(format);
    const qint64 limit = format == "JPG" ? 65500 : format == "WEBP" ? 16383 : format == "PDF" ? qint64(200) * dpi : -1;
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
bool SaveService::save_pdf(const QImage &image, QIODevice *device, int dpi) {
    const auto flattened = flatten_for_pdf(image);
    QPdfWriter writer(device);
    dpi = std::max(72, dpi);
    writer.setResolution(dpi);
    writer.setPageSize(QPageSize(QSizeF(flattened.width() * 72.0 / dpi, flattened.height() * 72.0 / dpi), QPageSize::Point, "Screenshot"));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);
    writer.setCreator("jietuba");
    QPainter painter(&writer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QRectF(0, 0, writer.width(), writer.height()), flattened, QRectF(0, 0, flattened.width(), flattened.height()));
    painter.end();
    return true;
}
bool SaveService::save_qimage_to_path(const QImage &image, const QString &path, QString format, int dpi, int quality) {
    if (image.isNull()) return false;
    if (format.isEmpty()) {
        const auto dot = QFileInfo(path).fileName().lastIndexOf('.');
        format = dot < 0 ? "PNG" : QFileInfo(path).fileName().mid(dot);
    }
    format = normalize_format(format);
    if (format == "PDF") {
        QFile file(path);
        const bool success = file.open(QIODevice::WriteOnly) && save_pdf(image, &file, dpi);
        file.close();
        if (!success) cleanup_failed_placeholder(path);
        return success;
    }
    const bool success = image.save(path, format.toLatin1().constData(), format_has_quality(format) ? quality : -1);
    if (!success) cleanup_failed_placeholder(path);
    return success;
}
QByteArray SaveService::encode_image(const QImage &image, QString format, int quality, int dpi) {
    if (image.isNull()) return {};
    format = normalize_format(format);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    const bool success = format == "PDF" ? save_pdf(image, &buffer, dpi)
                                         : image.save(&buffer, format.toLatin1().constData(), format_has_quality(format) ? quality : -1);
    buffer.close();
    return success ? bytes : QByteArray();
}
bool SaveService::format_has_quality(QString format) {
    format = normalize_format(format);
    return format == "JPG" || format == "WEBP";
}
std::pair<int, QByteArray> SaveService::encode_within(const QImage &image, QString format, qint64 max_bytes) {
    int low = 1, high = 100, best = -1;
    QByteArray best_bytes, lowest_bytes;
    while (low <= high) {
        const int quality = (low + high) / 2;
        auto bytes = encode_image(image, format, quality);
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
    return {1, lowest_bytes.isEmpty() ? encode_image(image, format, 1) : lowest_bytes};
}
QImage SaveService::scale_for_save(const QImage &image, const QSize &size) {
    if (image.isNull() || size.isEmpty() || size == image.size()) return image;
    return image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}
std::pair<bool, std::optional<QString>> SaveService::save_qimage(const QImage &image, QString directory,
    QString prefix, QString suffix, QString format, int dpi, QString target) const {
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
    return {save_qimage_to_path(image, target, format, dpi), target};
}
std::optional<QString> SaveService::save_qimage_async(const QImage &image, QString directory, QString prefix,
    QString suffix, QString format, int dpi, std::function<void(bool, QString)> callback) const {
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
    QThreadPool::globalInstance()->start(QRunnable::create([copy = std::move(copy), path, format, dpi, callback = std::move(callback)] {
        try {
            const auto success = save_qimage_to_path(copy, path, format, dpi);
            if (callback) callback(success, path);
        } catch (...) {
            // Python's background-task wrapper captures worker exceptions.
            qWarning() << "Screenshot save worker/callback failed";
        }
    }));
    return path;
}
std::optional<QString> SaveService::save_beside(const QImage &image, const QString &saved, const QString &tag) {
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
    return save_qimage_to_path(image, target) ? std::optional<QString>(target) : std::nullopt;
}
}
