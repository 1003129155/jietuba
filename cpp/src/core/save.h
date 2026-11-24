#pragma once
#include <QByteArray>
#include <QImage>
#include <QString>
#include <functional>
#include <optional>
#include <utility>

class QIODevice;

namespace jietuba {
class ToolSettingsManager;

// How an image is encoded on its way out. Every way of saving takes one, read from the settings
// on the thread that starts the save: SaveService::options() or SaveOptions::from_settings.
struct SaveOptions {
    static constexpr int DEFAULT_PDF_DPI = 300;
    // QImageWriter's 0-100 for JPG and WebP, -1 for the format's default.
    int quality = -1;
    int pdf_dpi = DEFAULT_PDF_DPI;
    static SaveOptions from_settings(const ToolSettingsManager &config);
};

class SaveService {
public:
    static constexpr int DEFAULT_PDF_DPI = SaveOptions::DEFAULT_PDF_DPI;
    explicit SaveService(ToolSettingsManager &config) : config_manager(config) {}
    ToolSettingsManager &config_manager;
    QString get_default_directory() const;
    QString suggest_path(const QString &filename) const;
    // The options the settings give for the exits that save without asking.
    SaveOptions options() const { return SaveOptions::from_settings(config_manager); }
    std::pair<bool, std::optional<QString>> save_qimage(const QImage &image, QString directory, QString prefix, QString suffix,
        QString image_format, const SaveOptions &options, QString target_path = {}) const;
    std::optional<QString> save_qimage_async(const QImage &image, QString directory, QString prefix, QString suffix,
        QString image_format, const SaveOptions &options, std::function<void(bool, QString)> callback = {}) const;
    QString reserve_save_path(QString directory = {}, QString prefix = {}, QString suffix = {}, QString image_format = "PNG") const;
    static QString format_for_size(QString image_format, int width, int height, const SaveOptions &options);
    static std::optional<QString> save_beside(const QImage &image, const QString &saved_path, const QString &tag, const SaveOptions &options);
    // Formats without a quality ignore options.quality. An empty image_format is taken from the path's suffix.
    static bool save_qimage_to_path(const QImage &image, const QString &target_path, QString image_format, const SaveOptions &options);
    // The bytes save_qimage_to_path writes for the same arguments; empty on failure.
    static QByteArray encode_image(const QImage &image, QString image_format, const SaveOptions &options);
    // JPG and WebP take a quality; PNG and BMP are lossless and PDF has no quality setting.
    static bool format_has_quality(QString image_format);
    // The highest quality whose bytes fit in max_bytes, with those bytes; quality 1 when even
    // that does not fit. Size mostly grows with quality, so a binary search needs about 7 encodes.
    static std::pair<int, QByteArray> encode_within(const QImage &image, QString image_format, qint64 max_bytes, const SaveOptions &options);
    // Resized to size with smooth filtering; the image itself when size is empty or unchanged.
    static QImage scale_for_save(const QImage &image, const QSize &size);
    static QImage flatten_for_pdf(const QImage &image);
    static QString normalize_format(QString image_format);
    static QString claim_first_free(const QString &stem, const QString &ext);
    static void cleanup_failed_placeholder(const QString &path);
private:
    QString compose_path(QString directory, const QString &prefix, const QString &suffix, const QString &format) const;
    static bool save_pdf(const QImage &image, QIODevice *device, const SaveOptions &options);
};
}
