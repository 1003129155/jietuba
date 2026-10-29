#pragma once
#include <QImage>
#include <QMimeData>
#include <QPixmap>
#include <QThreadPool>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace jietuba {

// A file a drag writes into its own folder, only once a target asks for the files.
struct DragOutFile {
    QString name;
    // Writes the file at `path`; false when it could not.
    std::function<bool(const QString &path)> write;
};

// What a drag out of the program carries. A target takes what it understands: editors and chat boxes the text,
// HTML or image, Explorer and the desktop the files.
// The parts held as futures are offered when valid: made ready beforehand, prepared off the UI thread while the
// cursor travels, or deferred until a target first asks.
struct DragOutContent {
    QString text;
    std::shared_future<QString> html;
    std::shared_future<QImage> image;
    // Other formats by MIME type, such as an animation's bytes as image/gif.
    QList<std::pair<QString, std::shared_future<QByteArray>>> data;
    // Existing files, handed out as they are.
    QStringList files;
    // Offered after `files`.
    QList<DragOutFile> made_files;

    bool has_files() const { return !files.isEmpty() || !made_files.isEmpty(); }
};

// A part already worked out.
template <typename T> std::shared_future<T> ready_value(T value) {
    std::promise<T> promise;
    promise.set_value(std::move(value));
    return promise.get_future().share();
}

// A part prepared on a worker thread. Unlike std::async, letting go of it never waits for the work to end, so
// a drop soon after the drag starts never holds up the UI thread.
template <typename F> std::shared_future<std::invoke_result_t<F>> prepare_in_background(F work) {
    auto promise = std::make_shared<std::promise<std::invoke_result_t<F>>>();
    auto result = promise->get_future().share();
    QThreadPool::globalInstance()->start([promise, work = std::move(work)]() mutable {
        try {
            promise->set_value(work());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    return result;
}

// The files go out as CF_HDROP alone (see drop_files_bytes). Qt's text/uri-list would add a URL format that Word
// inserts as a file:/// link instead of the picture. Every format is offered from the start of the drag: Explorer
// never sees a format added once the cursor reaches it.
class DragOutMimeData : public QMimeData {
public:
    // The private type the files are offered under.
    static const QString FILES_MIME;

    // Each drag writes into its own folder under `temp_root`.
    explicit DragOutMimeData(DragOutContent content, QString temp_root = default_temp_root());
    // Removes the written files once a target that copies them after the drop has had time to read them.
    ~DragOutMimeData() override;

    QStringList formats() const override;
    bool hasFormat(const QString &mime_type) const override;
    // The files of the drag, writing the made ones on the first call.
    QStringList file_paths() const;
    // The files written so far; empty until a target asked for them.
    QStringList written_files() const { return written_.value_or(QStringList{}); }

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

    DragOutContent content_;
    QString temp_root_;
    mutable QString folder_;
    mutable std::optional<QStringList> written_;
};

// The CF_HDROP payload for the paths: a DROPFILES header followed by the wide paths, each ended by a null, and a final null.
QByteArray drop_files_bytes(const QStringList &paths);

// Drags `data` out of `source` and returns once it is dropped or cancelled. Copy only, Shift included: a move would
// let the target delete what was dragged. The preview follows the cursor at 80% opacity.
Qt::DropAction exec_drag_out(QObject *source, DragOutMimeData *data, const QPixmap &preview = {}, const QPoint &hot_spot = {});
}
