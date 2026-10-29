#include "core/drag_out.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QTimer>
#include <QUuid>
#include <QWindowsMimeConverter>
#include <algorithm>
#include <windows.h>
#include <shlobj.h>

namespace jietuba {
const QString DragOutMimeData::FILES_MIME = QStringLiteral("application/x-jietuba-drag-files");

namespace {
const QString IMAGE_MIME = QStringLiteral("application/x-qt-image");
// The preview lets the place it will land show through.
constexpr double PREVIEW_OPACITY = 0.8;

QPixmap translucent(const QPixmap &pixmap) {
    QPixmap result(pixmap.size());
    result.setDevicePixelRatio(pixmap.devicePixelRatio());
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setOpacity(PREVIEW_OPACITY);
    painter.drawPixmap(QPointF(0, 0), pixmap);
    return result;
}

// Hands a drag's files to Windows as CF_HDROP and nothing else; reading drops is left to Qt.
class FileDropConverter : public QWindowsMimeConverter {
public:
    bool canConvertFromMime(const FORMATETC &formatetc, const QMimeData *mime_data) const override {
        return formatetc.cfFormat == CF_HDROP && (formatetc.tymed & TYMED_HGLOBAL) && mime_data->hasFormat(DragOutMimeData::FILES_MIME);
    }

    bool convertFromMime(const FORMATETC &formatetc, const QMimeData *mime_data, STGMEDIUM *medium) const override {
        if (!canConvertFromMime(formatetc, mime_data)) return false;
        const QStringList paths = QString::fromUtf8(mime_data->data(DragOutMimeData::FILES_MIME)).split(u'\n', Qt::SkipEmptyParts);
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
        if (mime_type != DragOutMimeData::FILES_MIME) return {};
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

DragOutMimeData::DragOutMimeData(DragOutContent content, QString temp_root) : content_(std::move(content)), temp_root_(std::move(temp_root)) {
    ensure_file_drop_converter();
}

DragOutMimeData::~DragOutMimeData() {
    if (folder_.isEmpty() || !QCoreApplication::instance()) return;
    qint64 bytes = 0;
    for (const auto &path : written_files()) bytes += QFileInfo(path).size();
    QTimer::singleShot(removal_delay_ms(bytes), QCoreApplication::instance(), [folder = folder_] { QDir(folder).removeRecursively(); });
}

const QStringList &DragOutMimeData::write_files() const {
    if (written_) return *written_;
    written_ = QStringList{};
    if (temp_root_.isEmpty() || content_.made_files.isEmpty()) return *written_;
    remove_stale_folders(temp_root_);
    folder_ = QDir(temp_root_).filePath(QUuid::createUuid().toString(QUuid::Id128).left(16));
    if (!QDir().mkpath(folder_)) return *written_;
    for (const auto &file : content_.made_files) {
        const QString path = QDir(folder_).filePath(file.name);
        if (file.write && file.write(path)) written_->append(path);
    }
    return *written_;
}

QStringList DragOutMimeData::file_paths() const { return content_.files + write_files(); }

QStringList DragOutMimeData::formats() const {
    QStringList result;
    if (content_.has_files()) result.append(FILES_MIME);
    if (!content_.text.isEmpty()) result.append(QStringLiteral("text/plain"));
    if (content_.html.valid()) result.append(QStringLiteral("text/html"));
    if (content_.image.valid()) result.append(IMAGE_MIME);
    for (const auto &[mime_type, bytes] : content_.data)
        if (bytes.valid()) result.append(mime_type);
    return result;
}

bool DragOutMimeData::hasFormat(const QString &mime_type) const { return formats().contains(mime_type); }

QVariant DragOutMimeData::retrieveData(const QString &mime_type, QMetaType type) const {
    Q_UNUSED(type);
    if (mime_type == QLatin1String("text/plain")) return content_.text.isEmpty() ? QVariant() : QVariant(content_.text);
    if (mime_type == QLatin1String("text/html")) return content_.html.valid() ? QVariant(content_.html.get()) : QVariant();
    if (mime_type == IMAGE_MIME) return content_.image.valid() && !content_.image.get().isNull() ? QVariant(content_.image.get()) : QVariant();
    if (mime_type == FILES_MIME) return content_.has_files() ? file_paths().join(u'\n').toUtf8() : QVariant();
    for (const auto &[type, bytes] : content_.data)
        if (type == mime_type && bytes.valid()) return bytes.get();
    return {};
}

int DragOutMimeData::removal_delay_ms(qint64 bytes) { return static_cast<int>(std::min<qint64>(5000 + bytes / 100, 600000)); }

void DragOutMimeData::remove_stale_folders(const QString &temp_root, int max_age_seconds) {
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-max_age_seconds);
    for (const auto &info : QDir(temp_root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (info.lastModified() <= cutoff) QDir(info.absoluteFilePath()).removeRecursively();
}

QString DragOutMimeData::default_temp_root() { return QDir::temp().filePath("jietuba_drag"); }

Qt::DropAction exec_drag_out(QObject *source, DragOutMimeData *data, const QPixmap &preview, const QPoint &hot_spot) {
    auto *drag = new QDrag(source);
    drag->setMimeData(data);
    if (!preview.isNull()) {
        drag->setPixmap(translucent(preview));
        drag->setHotSpot(hot_spot);
    }
    const Qt::DropAction action = drag->exec(Qt::CopyAction, Qt::CopyAction);
    drag->deleteLater();
    return action;
}
}
