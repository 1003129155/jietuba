#include "core/save.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
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
QString SaveService::build_filename(const QString &prefix, const QString &suffix, const QString &format) {
    QStringList parts;
    for (const auto &part : {prefix, suffix, QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")}) if (!part.isEmpty()) parts.append(part);
    return parts.join('_') + '.' + normalize_format(format).toLower();
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
    const auto filename = build_filename(prefix, suffix, format);
    const int dot = filename.lastIndexOf('.');
    return claim_first_free(QDir(directory).filePath(filename.left(dot)), filename.mid(dot));
}
QString SaveService::reserve_save_path(QString directory, QString prefix, QString suffix, QString format) const {
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
bool SaveService::save_pdf(const QImage &image, const QString &path, int dpi) {
    const auto flattened = flatten_for_pdf(image);
    QPdfWriter writer(path);
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
bool SaveService::save_qimage_to_path(const QImage &image, const QString &path, QString format, int dpi) {
    if (image.isNull()) return false;
    if (format.isEmpty()) {
        const auto dot = QFileInfo(path).fileName().lastIndexOf('.');
        format = dot < 0 ? "PNG" : QFileInfo(path).fileName().mid(dot);
    }
    format = normalize_format(format);
    if (format == "PDF") return save_pdf(image, path, dpi);
    const bool success = image.save(path, format.toLatin1().constData());
    if (!success) cleanup_failed_placeholder(path);
    return success;
}
std::pair<bool, std::optional<QString>> SaveService::save_qimage(const QImage &image, QString directory,
    QString prefix, QString suffix, QString format, int dpi, QString target) const {
    if (image.isNull()) return {false, std::nullopt};
    if (target.isEmpty()) target = compose_path(directory, prefix, suffix, format);
    return {save_qimage_to_path(image, target, format, dpi), target};
}
std::optional<QString> SaveService::save_qimage_async(const QImage &image, QString directory, QString prefix,
    QString suffix, QString format, int dpi, std::function<void(bool, QString)> callback) const {
    if (image.isNull()) return std::nullopt;
    auto copy = image.copy();
    const auto path = compose_path(directory, prefix, suffix, format);
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
    const auto filename = QFileInfo(saved).fileName();
    const int dot = filename.lastIndexOf('.');
    const bool has_ext = dot > 0;
    const auto stem = has_ext ? saved.left(saved.size() - filename.size() + dot) : saved;
    const auto ext = has_ext ? filename.mid(dot) : QString();
    const auto target = claim_first_free(stem + "_" + tag, ext);
    return save_qimage_to_path(image, target) ? std::optional<QString>(target) : std::nullopt;
}
}
