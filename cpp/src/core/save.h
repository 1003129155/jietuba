#pragma once
#include <QImage>
#include <QString>
#include <functional>
#include <optional>

namespace jietuba {
class ToolSettingsManager;
// QImage branch of main/core/save.py. Pillow adapters remain pending.
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
    static bool save_qimage_to_path(const QImage &image, const QString &target_path, QString image_format = {}, int pdf_dpi = DEFAULT_PDF_DPI);
    static QImage flatten_for_pdf(const QImage &image);
    static QString normalize_format(QString image_format);
    static QString claim_first_free(const QString &stem, const QString &ext);
    static void cleanup_failed_placeholder(const QString &path);
private:
    QString compose_path(QString directory, const QString &prefix, const QString &suffix, const QString &format) const;
    static QString build_filename(const QString &prefix, const QString &suffix, const QString &format);
    static bool save_pdf(const QImage &image, const QString &path, int dpi);
};
}
