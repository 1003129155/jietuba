#include "clipboard/ui/widgets/item_drag.h"
#include "clipboard/core/manager.h"
#include <QDateTime>
#include <QDir>
#include <QFile>

namespace jietuba::clipboard {
namespace {
QString image_path(const QString &images_dir, const ClipboardItem &item) {
    return item.image_id && !item.image_id->isEmpty() && !images_dir.isEmpty() ? QDir(images_dir).filePath(*item.image_id + ".png") : QString();
}

// For an image whose library file is missing: the PNG the manager rebuilds from the stored clipboard data.
std::optional<QByteArray> rebuilt_image(const ClipboardItem &item) {
    if (!item.image_id || item.image_id->isEmpty()) return std::nullopt;
    auto data = ClipboardManager::instance().get_image_data(*item.image_id);
    if (!data || data->isEmpty()) return std::nullopt;
    return data;
}

bool write_bytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool write_image_item(const QString &path, const QString &source, const ClipboardItem &item) {
    if (!QFile::exists(source)) {
        const auto data = rebuilt_image(item);
        return data && write_bytes(path, *data);
    }
    // The library file is copied, never handed out: a target that moved it would break the history item.
    if (!QFile::copy(source, path)) return false;
    // The copy keeps the library file's date; the dropped file should carry the time it was made.
    QFile copy(path);
    if (copy.open(QIODevice::ReadWrite)) copy.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    return true;
}
}

ItemDragMimeData::ItemDragMimeData(const QList<ClipboardItem> &items, const QString &images_dir, QString temp_root)
    : DragOutMimeData(content(items, images_dir), std::move(temp_root)) {}

DragOutContent ItemDragMimeData::content(const QList<ClipboardItem> &items, const QString &images_dir) {
    DragOutContent content;
    QStringList texts;
    QList<ClipboardItem> images;
    for (const auto &item : items) {
        if (item.content_type == "text") {
            texts.append(item.content);
            content.made_files.append({file_name(item), [text = item.content](const QString &path) { return write_bytes(path, text.toUtf8()); }});
        } else if (item.content_type == "image") {
            images.append(item);
            content.made_files.append({file_name(item), [source = image_path(images_dir, item), item](const QString &path) { return write_image_item(path, source, item); }});
        } else if (item.content_type == "file") {
            content.files.append(item_paths(item));
        }
    }
    content.text = texts.join('\n');
    if (items.size() == 1 && items.front().content_type == "text" && items.front().html_content && !items.front().html_content->isEmpty())
        content.html = ready_value(*items.front().html_content);
    // Decoded off the UI thread while the cursor travels to the target.
    if (images.size() == 1) {
        const QString path = image_path(images_dir, images.front());
        if (QFile::exists(path)) content.image = std::async(std::launch::async, [path] { return QImage(path); }).share();
        else
            content.image = std::async(std::launch::async, [item = images.front()] {
                                const auto data = rebuilt_image(item);
                                return data ? QImage::fromData(*data) : QImage();
                            }).share();
    }
    return content;
}

bool ItemDragMimeData::can_drag(const ClipboardItem &item, const QString &images_dir) {
    if (item.content_type == "text") return !item.content.isEmpty();
    if (item.content_type == "image") return QFile::exists(image_path(images_dir, item)) || rebuilt_image(item).has_value();
    if (item.content_type == "file") return !item_paths(item).isEmpty();
    return false;
}

QString ItemDragMimeData::file_name(const ClipboardItem &item) {
    const QString stamp = item.created_at.isValid() ? item.created_at.toString("yyyyMMdd_HHmmss_") : QString();
    return QString("clipboard_%1_%2%3.%4").arg(item.content_type, stamp).arg(item.id).arg(item.content_type == "image" ? "png" : "txt");
}
}
