#include "clipboard/ui/widgets/item_drag.h"
#include "clipboard/core/manager.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTimer>
#include <QUuid>
#include <QWindowsMimeConverter>
#include <algorithm>
#include <windows.h>
#include <shlobj.h>

namespace jietuba::clipboard {
const QString ItemDragMimeData::FILES_MIME = QStringLiteral("application/x-jietuba-drag-files");

namespace {
const QString IMAGE_MIME = QStringLiteral("application/x-qt-image");

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

// Hands a drag's files to Windows as CF_HDROP and nothing else; reading drops is left to Qt.
class FileDropConverter : public QWindowsMimeConverter {
public:
    bool canConvertFromMime(const FORMATETC &formatetc, const QMimeData *mime_data) const override {
        return formatetc.cfFormat == CF_HDROP && (formatetc.tymed & TYMED_HGLOBAL) && mime_data->hasFormat(ItemDragMimeData::FILES_MIME);
    }

    bool convertFromMime(const FORMATETC &formatetc, const QMimeData *mime_data, STGMEDIUM *medium) const override {
        if (!canConvertFromMime(formatetc, mime_data)) return false;
        const QStringList paths = QString::fromUtf8(mime_data->data(ItemDragMimeData::FILES_MIME)).split(u'\n', Qt::SkipEmptyParts);
        if (paths.isEmpty()) return false;
        const QByteArray bytes = drop_files_bytes(paths);
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes.size()));
        if (!global) return false;
        memcpy(GlobalLock(global), bytes.constData(), static_cast<size_t>(bytes.size()));
        GlobalUnlock(global);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        medium->pUnkForRelease = nullptr;
        return true;
    }

    QList<FORMATETC> formatsForMime(const QString &mime_type, const QMimeData *) const override {
        if (mime_type != ItemDragMimeData::FILES_MIME) return {};
        return {FORMATETC{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
    }

    bool canConvertToMime(const QString &, IDataObject *) const override { return false; }
    QVariant convertToMime(const QString &, IDataObject *, QMetaType) const override { return {}; }
    QString mimeForFormat(const FORMATETC &) const override { return {}; }
};

// Registered with Qt on first use and released while the application still exists.
FileDropConverter *file_drop_converter = nullptr;

void ensure_file_drop_converter() {
    // Qt's converter assumes the Windows platform plugin; other platforms have no system drag and drop to feed.
    if (file_drop_converter || QGuiApplication::platformName() != QLatin1String("windows")) return;
    file_drop_converter = new FileDropConverter;
    qAddPostRoutine([] {
        delete file_drop_converter;
        file_drop_converter = nullptr;
    });
}
}

QByteArray drop_files_bytes(const QStringList &paths) {
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    QByteArray bytes(reinterpret_cast<const char *>(&header), sizeof(header));
    for (const auto &path : paths) {
        const QString native = QDir::toNativeSeparators(path);
        bytes.append(reinterpret_cast<const char *>(native.utf16()), (native.size() + 1) * qsizetype(sizeof(char16_t)));
    }
    bytes.append(qsizetype(sizeof(char16_t)), '\0');
    return bytes;
}

ItemDragMimeData::ItemDragMimeData(QList<ClipboardItem> items, QString images_dir, QString temp_root)
    : items_(std::move(items)), images_dir_(std::move(images_dir)), temp_root_(std::move(temp_root)) {
    ensure_file_drop_converter();
    QStringList texts;
    QList<ClipboardItem> images;
    for (const auto &item : items_) {
        if (item.content_type == "text") texts.append(item.content);
        else if (item.content_type == "image") images.append(item);
        else if (item.content_type == "file") item_files_.append(item_paths(item));
    }
    text_ = texts.join('\n');
    if (items_.size() == 1 && items_.front().content_type == "text" && items_.front().html_content && !items_.front().html_content->isEmpty())
        html_ = items_.front().html_content;
    // Decoded off the UI thread while the cursor travels to the target.
    if (images.size() == 1) {
        const QString path = image_path(images_dir_, images.front());
        if (QFile::exists(path)) image_ = std::async(std::launch::async, [path] { return QImage(path); }).share();
        else
            image_ = std::async(std::launch::async, [item = images.front()] {
                         const auto data = rebuilt_image(item);
                         return data ? QImage::fromData(*data) : QImage();
                     }).share();
    }
}

ItemDragMimeData::~ItemDragMimeData() {
    if (folder_.isEmpty() || !QCoreApplication::instance()) return;
    qint64 bytes = 0;
    for (const auto &path : written_files()) bytes += QFileInfo(path).size();
    QTimer::singleShot(removal_delay_ms(bytes), QCoreApplication::instance(), [folder = folder_] { QDir(folder).removeRecursively(); });
}

const QStringList &ItemDragMimeData::write_files() const {
    if (written_) return *written_;
    written_ = QStringList{};
    const bool generates = std::any_of(items_.begin(), items_.end(), [](const ClipboardItem &item) { return item.content_type == "text" || item.content_type == "image"; });
    if (temp_root_.isEmpty() || !generates) return *written_;
    remove_stale_folders(temp_root_);
    folder_ = QDir(temp_root_).filePath(QUuid::createUuid().toString(QUuid::Id128).left(16));
    if (!QDir().mkpath(folder_)) return *written_;
    for (const auto &item : items_) {
        const QString path = QDir(folder_).filePath(file_name(item));
        if (item.content_type == "text") {
            const QByteArray bytes = item.content.toUtf8();
            QFile file(path);
            if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) written_->append(path);
        } else if (item.content_type == "image") {
            // The library file is copied, never handed out: a target that moved it would break the history item.
            const QString source = image_path(images_dir_, item);
            if (QFile::exists(source)) {
                if (!QFile::copy(source, path)) continue;
                // The copy keeps the library file's date; the dropped file should carry the time it was made.
                QFile copy(path);
                if (copy.open(QIODevice::ReadWrite)) copy.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
            } else {
                const auto data = rebuilt_image(item);
                QFile file(path);
                if (!data || !file.open(QIODevice::WriteOnly) || file.write(*data) != data->size()) continue;
            }
            written_->append(path);
        }
    }
    return *written_;
}

QStringList ItemDragMimeData::file_paths() const { return item_files_ + write_files(); }

QStringList ItemDragMimeData::formats() const {
    QStringList result{FILES_MIME};
    if (!text_.isEmpty()) result.append(QStringLiteral("text/plain"));
    if (html_) result.append(QStringLiteral("text/html"));
    if (image_.valid()) result.append(IMAGE_MIME);
    return result;
}

bool ItemDragMimeData::hasFormat(const QString &mime_type) const { return formats().contains(mime_type); }

QVariant ItemDragMimeData::retrieveData(const QString &mime_type, QMetaType type) const {
    Q_UNUSED(type);
    if (mime_type == QLatin1String("text/plain")) return text_.isEmpty() ? QVariant() : QVariant(text_);
    if (mime_type == QLatin1String("text/html")) return html_ ? QVariant(*html_) : QVariant();
    if (mime_type == IMAGE_MIME) return image_.valid() && !image_.get().isNull() ? QVariant(image_.get()) : QVariant();
    if (mime_type == FILES_MIME) return file_paths().join(u'\n').toUtf8();
    return {};
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

int ItemDragMimeData::removal_delay_ms(qint64 bytes) { return static_cast<int>(std::min<qint64>(5000 + bytes / 100, 600000)); }

void ItemDragMimeData::remove_stale_folders(const QString &temp_root, int max_age_seconds) {
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-max_age_seconds);
    for (const auto &info : QDir(temp_root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (info.lastModified() <= cutoff) QDir(info.absoluteFilePath()).removeRecursively();
}

QString ItemDragMimeData::default_temp_root() { return QDir::temp().filePath("jietuba_drag"); }
}
