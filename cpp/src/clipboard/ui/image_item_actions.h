#pragma once
#include "clipboard/core/models.h"
#include <QImage>
#include <optional>

class QWidget;

// Image item actions shared by the clipboard window and the manage window.
namespace jietuba::clipboard {
class ClipboardManager;

// The item's original image (not the list thumbnail); null when unavailable.
QImage load_item_image(ClipboardManager &manager, const std::optional<ClipboardItem> &item);
void save_image_item_as(QWidget *parent, ClipboardManager &manager, const std::optional<ClipboardItem> &item);
// The image format from the file's extension, else the filter's first extension, else PNG.
QString format_from_save_filter(const QString &file_path, const QString &selected_filter);
}
