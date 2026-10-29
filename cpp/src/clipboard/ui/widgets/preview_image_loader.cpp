#include "clipboard/ui/widgets/preview_image_loader.h"
#include "clipboard/core/manager.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QRunnable>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif
#endif

namespace jietuba::clipboard {
namespace {
constexpr int CACHE_SIZE = 20;
constexpr int WORKER_THREADS = 2;

QMutex hooks_mutex;
PreviewImageTestHooks test_hooks;
std::atomic<int> reads{0};

PreviewImageTestHooks current_hooks() {
    QMutexLocker locker(&hooks_mutex);
    return test_hooks;
}

// The format comes from the content: the reader gets the device, so a file name's suffix is never consulted.
QImage decode(QIODevice &device, const QSize &bound) {
    QImageReader reader(&device);
    reader.setAutoTransform(true);
    // The scaled size applies to the stored orientation, which a quarter turn swaps.
    QSize box = bound;
    if (reader.transformation() & QImageIOHandler::TransformationRotate90) box.transpose();
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > box.width() || size.height() > box.height())) reader.setScaledSize(size.scaled(box, Qt::KeepAspectRatio));
    QImage image = reader.read();
    // A format that cannot report its size comes back whole.
    if (image.width() > bound.width() || image.height() > bound.height()) image = image.scaled(bound, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

QImage thumbnail_image(const QString &data_url, const QSize &bound) {
    if (!data_url.startsWith("data:image")) return {};
    QImage image;
    image.loadFromData(QByteArray::fromBase64(data_url.mid(data_url.indexOf(u',') + 1).toLatin1()));
    // The thumbnail is tiny; it is enlarged to the size a full picture would have.
    return image.isNull() ? image : image.scaled(bound, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
}

FileHazard file_hazard(const QString &path) {
#ifdef Q_OS_WIN
    QString native = QDir::toNativeSeparators(path);
    if (native.startsWith("\\\\")) {
        // \\?\C:\dir is a local path in its long form; any other name behind two backslashes is a server or a device.
        if (!native.startsWith("\\\\?\\") || native.mid(4).startsWith("UNC\\", Qt::CaseInsensitive)) return FileHazard::Remote;
        native = native.mid(4);
    }
    const std::wstring wide = native.toStdWString();
    wchar_t root[MAX_PATH + 1] = {};
    if (GetVolumePathNameW(wide.c_str(), root, MAX_PATH) && GetDriveTypeW(root) == DRIVE_REMOTE) return FileHazard::Remote;
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    constexpr DWORD CLOUD_ONLY = FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & CLOUD_ONLY)) return FileHazard::CloudOnly;
#else
    Q_UNUSED(path);
#endif
    return FileHazard::None;
}

void set_preview_image_test_hooks(PreviewImageTestHooks hooks) {
    QMutexLocker locker(&hooks_mutex);
    test_hooks = std::move(hooks);
}

int preview_image_reads_for_test() { return reads.load(); }

QImage PreviewImageLoader::Cache::find(const QString &key) {
    QMutexLocker locker(&mutex_);
    const auto found = images_.constFind(key);
    if (found == images_.constEnd()) return {};
    recent_.removeOne(key);
    recent_.prepend(key);
    return *found;
}

void PreviewImageLoader::Cache::insert(const QString &key, const QImage &image) {
    QMutexLocker locker(&mutex_);
    recent_.removeOne(key);
    recent_.prepend(key);
    images_.insert(key, image);
    while (recent_.size() > CACHE_SIZE) images_.remove(recent_.takeLast());
}

void PreviewImageLoader::Cache::clear() {
    QMutexLocker locker(&mutex_);
    recent_.clear();
    images_.clear();
}

PreviewImageLoader::PreviewImageLoader(QObject *parent) : QObject(parent) {
    pool_.setMaxThreadCount(WORKER_THREADS);
}

PreviewImageLoader::~PreviewImageLoader() {
    cancel();
    pool_.waitForDone();
}

quint64 PreviewImageLoader::request(PreviewImageRequest request) {
    const quint64 serial = ++last_serial_;
    latest_.store(serial);
    Job job{serial, std::move(request)};
    if (running_ < WORKER_THREADS) start(std::move(job));
    else queued_ = std::move(job);
    return serial;
}

void PreviewImageLoader::cancel() {
    latest_.store(++last_serial_);
    queued_.reset();
}

void PreviewImageLoader::clear_cache() { cache_.clear(); }

void PreviewImageLoader::start(Job job) {
    ++running_;
    pool_.start(QRunnable::create([this, job = std::move(job)] {
        const QImage image = load(job);
        QMetaObject::invokeMethod(this, [this, serial = job.serial, image] { finish(serial, image); }, Qt::QueuedConnection);
    }));
}

void PreviewImageLoader::finish(quint64 serial, const QImage &image) {
    --running_;
    if (queued_) {
        Job next = std::move(*queued_);
        queued_.reset();
        start(std::move(next));
    }
    if (serial == latest_.load()) emit loaded(serial, image);
}

QImage PreviewImageLoader::load(const Job &job) {
    const PreviewImageRequest &request = job.request;
    const auto hooks = current_hooks();
    const auto superseded = [&] { return latest_.load() != job.serial; };
    const auto hazard_of = [&](const QString &path) { return hooks.file_hazard ? hooks.file_hazard(path) : file_hazard(path); };
    // The last place a request that was overtaken stops before it touches storage.
    const auto may_read = [&] {
        if (hooks.before_read) hooks.before_read();
        if (superseded()) return false;
        ++reads;
        return true;
    };
    const QString bound_key = QString("%1x%2").arg(request.bound.width()).arg(request.bound.height());
    if (superseded()) return {};

    if (!request.file_path.isEmpty()) {
        // A file on the network or still in the cloud is not read; its entry stays a text preview.
        if (hazard_of(request.file_path) != FileHazard::None) return {};
        const QFileInfo info(request.file_path);
        if (!info.isFile()) return {};
        const QString key = QString("file:%1|%2|%3|%4").arg(request.file_path).arg(info.lastModified().toMSecsSinceEpoch()).arg(info.size()).arg(bound_key);
        QImage image = cache_.find(key);
        if (image.isNull() && may_read()) {
            QFile file(request.file_path);
            if (file.open(QIODevice::ReadOnly)) image = decode(file, request.bound);
            if (!image.isNull()) cache_.insert(key, image);
        }
        return image;
    }

    QImage image;
    if (request.manager && !request.image_id.isEmpty()) {
        const QString key = QString("image:%1|%2").arg(request.image_id, bound_key);
        image = cache_.find(key);
        if (image.isNull()) {
            // The pictures folder is often a synced one: a placeholder in it is not read, the thumbnail stands in.
            const QString folder = request.manager->get_images_dir().value_or(QString());
            const bool cloud_only = !folder.isEmpty() && hazard_of(QDir(folder).filePath(request.image_id + ".png")) == FileHazard::CloudOnly;
            if (!cloud_only) {
                if (!may_read()) return {};
                if (auto data = request.manager->get_image_data(request.image_id); data && !data->isEmpty()) {
                    QBuffer buffer(&*data);
                    buffer.open(QIODevice::ReadOnly);
                    image = decode(buffer, request.bound);
                }
                if (!image.isNull()) cache_.insert(key, image);
            }
        }
    }
    return image.isNull() ? thumbnail_image(request.thumbnail, request.bound) : image;
}
}
