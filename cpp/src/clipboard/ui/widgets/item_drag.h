#pragma once
#include "clipboard/core/models.h"
#include "core/drag_out.h"

namespace jietuba::clipboard {

// A drag of history items: their text, the HTML of a single text item, the image of a single image item, and the
// items as files. Text and image items become files only when a target asks for them; file items offer their own paths.
class ItemDragMimeData : public DragOutMimeData {
public:
    ItemDragMimeData(const QList<ClipboardItem> &items, const QString &images_dir, QString temp_root);

    static DragOutContent content(const QList<ClipboardItem> &items, const QString &images_dir);
    // Whether the item has anything to offer: text, an image whose file exists, or file paths.
    static bool can_drag(const ClipboardItem &item, const QString &images_dir);
    // clipboard_<type>_<creation time>_<id>.<txt|png>
    static QString file_name(const ClipboardItem &item);
};
}
