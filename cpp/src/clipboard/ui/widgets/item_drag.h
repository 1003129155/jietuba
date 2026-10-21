#pragma once
#include "clipboard/core/models.h"
#include <QImage>
#include <QMimeData>
#include <future>
#include <optional>

namespace jietuba::clipboard {

// What a drag of history items offers: text, HTML and image data, and the items as files. A target takes what it
// understands: editors and chat boxes the text or image, Explorer and the desktop the files. Text and image items
// become files only when a target asks for them; file items offer their own paths.
//
// The files go out as CF_HDROP alone (see drop_files_bytes). Qt's text/uri-list would add a URL format that Word
// inserts as a file:/// link instead of the picture. They are offered from the start of the drag: Explorer never
// sees a format added once the cursor reaches it.
class ItemDragMimeData : public QMimeData {
public:
    // The private type the files are offered under.
    static const QString FILES_MIME;

    // Each drag writes into its own folder under `temp_root`, and only when a target first asks for the files.
    ItemDragMimeData(QList<ClipboardItem> items, QString images_dir, QString temp_root);
    // Removes the written files once a target that copies them after the drop has had time to read them.
    ~ItemDragMimeData() override;

    QStringList formats() const override;
    bool hasFormat(const QString &mime_type) const override;
    // The files of the drag, writing those of text and image items on the first call.
    QStringList file_paths() const;
    // The files written so far; empty until a target asked for them.
    QStringList written_files() const { return written_.value_or(QStringList{}); }

    // Whether the item has anything to offer: text, an image whose file exists, or file paths.
    static bool can_drag(const ClipboardItem &item, const QString &images_dir);
    // clipboard_<type>_<creation time>_<id>.<txt|png>
    static QString file_name(const ClipboardItem &item);
    // Five seconds plus 100 KB/s of the files' size, at most ten minutes.
    static int removal_delay_ms(qint64 bytes);
    // Removes drag folders older than `max_age_seconds`, left behind when the program exited before removing them:
    // an hour while running, every one at start-up, when no drag of this run can be in progress.
    static void remove_stale_folders(const QString &temp_root, int max_age_seconds = 3600);
    static QString default_temp_root();

protected:
    QVariant retrieveData(const QString &mime_type, QMetaType type) const override;

private:
    const QStringList &write_files() const;

    QList<ClipboardItem> items_;
    QString images_dir_;
    QString temp_root_;
    QString text_;
    std::optional<QString> html_;
    QStringList item_files_;
    std::shared_future<QImage> image_;
    mutable QString folder_;
    mutable std::optional<QStringList> written_;
};

// The CF_HDROP payload for the paths: a DROPFILES header followed by the wide paths, each ended by a null, and a final null.
QByteArray drop_files_bytes(const QStringList &paths);
}
