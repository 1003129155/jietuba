#pragma once
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <optional>

namespace jietuba::clipboard {
class ClipboardManager;

// What reading a file costs beyond the disk. Neither kind is read to find out.
enum class FileHazard {
    None,
    // A UNC path or a network drive: the read waits on the network and may not return for a long time.
    Remote,
    // The data is not on this machine (a cloud placeholder): reading it starts a download.
    CloudOnly,
};
// A missing file is None: opening it fails by itself.
FileHazard file_hazard(const QString &path);

// One image to prepare for the hover preview: a stored picture or a file, scaled down to fit `bound`.
struct PreviewImageRequest {
    ClipboardManager *manager = nullptr;
    // Set for a stored picture.
    QString image_id;
    // Set for a file; the picture is whatever its content decodes as.
    QString file_path;
    // data URL of the stored thumbnail, used when the picture itself cannot be loaded.
    QString thumbnail;
    QSize bound;
};

// Loads and decodes preview pictures off the UI thread. Only the newest request counts: older
// ones are dropped from the queue, stop at their next checkpoint when running, and are never reported.
class PreviewImageLoader : public QObject {
    Q_OBJECT
public:
    explicit PreviewImageLoader(QObject *parent = nullptr);
    ~PreviewImageLoader() override;

    // The serial `loaded` reports the result under.
    quint64 request(PreviewImageRequest request);
    void cancel();
    // Cached pictures are scaled for one bound; the cache is dropped when the scale changes or the window closes.
    void clear_cache();

signals:
    // A null image: nothing to show as a picture (not an image, unreadable, remote or cloud-only).
    void loaded(quint64 serial, const QImage &image);

private:
    struct Job {
        quint64 serial = 0;
        PreviewImageRequest request;
    };
    void start(Job job);
    // On a worker thread.
    QImage load(const Job &job);
    void finish(quint64 serial, const QImage &image);

    // Recently shown pictures, most recent first; reached from the UI thread and the workers.
    class Cache {
    public:
        QImage find(const QString &key);
        void insert(const QString &key, const QImage &image);
        void clear();

    private:
        QMutex mutex_;
        QList<QString> recent_;
        QHash<QString, QImage> images_;
    };

    QThreadPool pool_;
    Cache cache_;
    std::atomic<quint64> latest_{0};
    quint64 last_serial_ = 0;
    int running_ = 0;
    std::optional<Job> queued_;
};

// Test seams: the file check, and a point each worker passes just before it reads (blocking there
// holds a load in flight). Empty functions restore the real behavior.
struct PreviewImageTestHooks {
    std::function<FileHazard(const QString &)> file_hazard;
    std::function<void()> before_read;
};
void set_preview_image_test_hooks(PreviewImageTestHooks hooks);
// Pictures read from storage since the process started.
int preview_image_reads_for_test();
}
