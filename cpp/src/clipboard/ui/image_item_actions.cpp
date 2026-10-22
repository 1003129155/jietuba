#include "clipboard/ui/image_item_actions.h"
#include "clipboard/core/manager.h"
#include "core/save.h"
#include "ui/dialogs.h"
#include "ui/save_image_dialog.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>

namespace jietuba::clipboard {
namespace {
// The texts belong to the ClipboardWindow translation context.
QString tr_window(const char *text) { return QCoreApplication::translate("ClipboardWindow", text); }

// The file name's last extension with its dot.
QString extension_of(const QString &path) {
    const QString name = QFileInfo(path).fileName();
    qsizetype dot = name.lastIndexOf(u'.');
    // Leading dots do not start an extension.
    qsizetype first = 0;
    while (first < name.size() && name[first] == u'.') ++first;
    if (dot < first) return {};
    return name.mid(dot);
}
}

QImage load_item_image(ClipboardManager &manager, const std::optional<ClipboardItem> &item) {
    if (!item || item->content_type != "image" || !item->image_id || item->image_id->isEmpty()) return {};
    const auto data = manager.get_image_data(*item->image_id);
    if (!data || data->isEmpty()) return {};
    QImage image;
    return image.loadFromData(*data) ? image : QImage();
}

void save_image_item_as(QWidget *parent, ClipboardManager &manager, const std::optional<ClipboardItem> &item) {
    if (!item || item->content_type != "image" || !item->image_id || item->image_id->isEmpty()) return;
    const QImage image = load_item_image(manager, item);
    if (image.isNull()) {
        show_warning_dialog(parent, tr_window("Save Failed"), tr_window("Image data is unavailable."));
        return;
    }
    const QString base_name = item->created_at.isValid() ? QString("clipboard_image_%1").arg(item->created_at.toString("yyyyMMdd_HHmmss"))
                                                         : QString("clipboard_image_%1").arg(item->id);
    if (use_modern_save_dialog(nullptr)) {
        const auto outcome = SaveImageDialog::run(parent, image, base_name, nullptr).first;
        if (outcome != SaveImageDialog::Outcome::CLASSIC) return;
    }
    const QString default_name = base_name + ".png";
    auto [file_path, selected_filter] = get_save_file_name(parent, tr_window("Save as"), default_name,
                                                           tr_window("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg);;Bitmap Image (*.bmp);;WebP Image (*.webp);;PDF (*.pdf)"));
    if (file_path.isEmpty()) return;
    const QString image_format = format_from_save_filter(file_path, selected_filter);
    if (extension_of(file_path).isEmpty()) file_path = file_path + "." + image_format.toLower();
    if (!SaveService::save_qimage_to_path(image, file_path, image_format)) show_warning_dialog(parent, tr_window("Save Failed"), tr_window("Failed to save image."));
}

QString format_from_save_filter(const QString &file_path, const QString &selected_filter) {
    const QString ext = extension_of(file_path).mid(1);
    if (!ext.isEmpty()) return ext.toUpper();
    // The first extension in the filter, e.g. "JPEG (*.jpg *.jpeg)" -> "jpg".
    static const QRegularExpression pattern(R"(\*\.(\w+))", QRegularExpression::UseUnicodePropertiesOption);
    const auto match = pattern.match(selected_filter);
    if (match.hasMatch()) return match.captured(1).toUpper();
    return "PNG";
}
}
