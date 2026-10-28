#pragma once
#include "gif/gifrecorder.h"
#include <QEventLoop>
#include <QFrame>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QThread>
#include <atomic>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

// GIF composition with a progress overlay. FrameStore::export_gif decodes, scales,
// overlays the cursor and encodes in parallel natively, reporting progress by callback.
namespace jietuba {
class WaitBar;
constexpr int DEFAULT_GIF_WIDTH = 0;

// A file path, the GIF bytes, or nothing (cancelled or failed).
using ComposeResult = std::variant<std::monostate, QString, QByteArray>;

struct ComposeOptions {
    std::optional<QString> path;
    std::shared_ptr<gifrecorder::FrameStore> store;
    int gif_width = DEFAULT_GIF_WIDTH;
    int frame_start = 0;
    int frame_end = 0;
    std::optional<gifrecorder::CursorSprites> cursor_sprites;
    std::optional<std::vector<std::optional<gifrecorder::CursorInfo>>> cursor_infos;
    double speed_multiplier = 1.0;
};

class ComposeWorker : public QObject {
    Q_OBJECT
public:
    explicit ComposeWorker(ComposeOptions options);
    void cancel();
    void run();
    // Drops the store and cursor data early so the native store is released promptly.
    void release_inputs();
signals:
    void progress(int done, int total);
    // (ok, result); result "gifrecorder_not_found" when export is impossible.
    void finished(bool ok, const jietuba::ComposeResult &result, bool unavailable);
private:
    ComposeResult compose();
    bool do_compose(const QString &out_path);
    ComposeOptions options_;
    std::atomic_bool cancel_ = false;
};

// A frameless progress bar matching the end-of-recording wait bar, centred on the
// parent; the local loop ends when composition finishes or is cancelled.
class ComposerProgressDialog : public QFrame {
    Q_OBJECT
public:
    explicit ComposerProgressDialog(QWidget *parent = nullptr);
    void start(ComposeOptions options);
    // Whether a composition thread is running (blocks updates).
    bool exporting() const { return thread_ && thread_->isRunning(); }
    // Shows the bar and blocks until composition ends; the path / bytes, or nothing
    // when cancelled or failed. center_pos takes precedence over parent; frame_start /
    // frame_end are inclusive, 0 meaning no limit.
    static ComposeResult run_compose(ComposeOptions options, QWidget *parent = nullptr, std::optional<QPoint> center_pos = std::nullopt);
private:
    void center_on(QWidget *parent, std::optional<QPoint> center_pos);
    void on_finished(bool ok, const ComposeResult &result, bool unavailable);
    ComposeResult result_;
    bool ok_ = false;
    QEventLoop loop_;
    WaitBar *bar_;
    QPointer<QThread> thread_;
    QPointer<ComposeWorker> worker_;
};
}

Q_DECLARE_METATYPE(jietuba::ComposeResult)
