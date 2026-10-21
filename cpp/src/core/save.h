#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <functional>
#include <optional>

class QIODevice;

namespace jietuba {
class ToolSettingsManager;
class SaveService {
public:
    static constexpr int DEFAULT_PDF_DPI = 300;
    explicit SaveService(ToolSettingsManager &config) : config_manager(config) {}
    ToolSettingsManager &config_manager;
    QString get_default_directory() const;
    QString suggest_path(const QString &filename) const;
    std::pair<bool, std::optional<QString>> save_qimage(const QImage &image, QString directory = {},
        QString prefix = {}, QString suffix = {}, QString image_format = "PNG", int pdf_dpi = DEFAULT_PDF_DPI, QString target_path = {}) const;
    std::optional<QString> save_qimage_async(const QImage &image, QString directory = {}, QString prefix = {},
        QString suffix = {}, QString image_format = "PNG", int pdf_dpi = DEFAULT_PDF_DPI,
        std::function<void(bool, QString)> callback = {}) const;
    QString reserve_save_path(QString directory = {}, QString prefix = {}, QString suffix = {}, QString image_format = "PNG") const;
    static QString format_for_size(QString image_format, int width, int height, int pdf_dpi = DEFAULT_PDF_DPI);
    static std::optional<QString> save_beside(const QImage &image, const QString &saved_path, const QString &tag);
    // quality is QImageWriter's 0-100, -1 for the format's default; formats without one ignore it.
    static bool save_qimage_to_path(const QImage &image, const QString &target_path, QString image_format = {}, int pdf_dpi = DEFAULT_PDF_DPI, int quality = -1);
    // The bytes save_qimage_to_path writes for the same arguments; empty on failure.
    static QByteArray encode_image(const QImage &image, QString image_format, int quality = -1, int pdf_dpi = DEFAULT_PDF_DPI);
    // JPG and WebP take a quality; PNG and BMP are lossless and PDF has no quality setting.
    static bool format_has_quality(QString image_format);
    // Resized to size with smooth filtering; the image itself when size is empty or unchanged.
    static QImage scale_for_save(const QImage &image, const QSize &size);
    static QImage flatten_for_pdf(const QImage &image);
    static QString normalize_format(QString image_format);
    static QString claim_first_free(const QString &stem, const QString &ext);
    static void cleanup_failed_placeholder(const QString &path);
private:
    QString compose_path(QString directory, const QString &prefix, const QString &suffix, const QString &format) const;
    static QString build_filename(const QString &prefix, const QString &suffix, const QString &format);
    static bool save_pdf(const QImage &image, QIODevice *device, int dpi);
};
}
