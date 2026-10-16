#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QVariantList>
#include <future>
#include <memory>
#include <optional>
#include <vector>

// Frame capture controller over the native gifrecorder.
//
// Recording: a native thread grabs the screen, compresses to JPEG and stores frames;
// here a QTimer only tells the UI the frame count and never touches pixels.
// After stopping, store() holds every frame and frames() the per-frame metadata
// (elapsed_ms and annotations); PlaybackEngine decodes images from the store.
namespace jietuba {
class InputHub;
namespace gifrecorder {
class FrameStore;
class RecordSession;
}

enum class RecordState { IDLE, RECORDING, PAUSED, STOPPED };
QString record_state_name(RecordState state);

// One frame's mouse state, relative to the recording region's top-left.
struct CursorSnapshot {
    int x = 0;
    int y = 0;
    // 0 none, 1 left button, 2 right button.
    int press = 0;
    // Whether the cursor is inside the recording region.
    bool visible = true;
    // 0 none, 1 up, -1 down.
    int scroll = 0;
};

// One frame's metadata; images are decoded on demand from the store.
struct FrameData {
    int elapsed_ms = 0;
    // The recording region's size after even alignment.
    int width = 0;
    int height = 0;
    QVariantList annotations;
    std::optional<CursorSnapshot> cursor;
};

// Grabs the screen at the configured rate through a native RecordSession; this side
// only ticks to report frame counts, so it never competes with the capture thread.
class FrameRecorder : public QObject {
    Q_OBJECT
public:
    explicit FrameRecorder(QObject *parent = nullptr);
    ~FrameRecorder() override;
    RecordState state() const { return state_; }
    std::vector<FrameData> &frames() { return frames_; }
    int fps() const { return fps_; }
    // Shared by recording, playback and export.
    std::shared_ptr<gifrecorder::FrameStore> store() const { return store_; }
    int total_duration_ms() const;
    // The recording size (width, height).
    std::pair<int, int> rec_size() const { return {rec_width_, rec_height_}; }
    void set_rect(const QRect &rect) { rect_ = rect; }
    void set_fps(int fps);
    // When the recording window opens or recording restarts: prepares the capture thread
    // in the background so the first frame comes at once. Each display allows one
    // duplication, so the capture session is lent first and given back in release(),
    // both on the capture session thread. DXGI is used exactly when captures use it
    // (uses_hdr_engine); otherwise GDI, without lending.
    void prepare();
    // When the recording window closes: stops prepared or running threads and gives the
    // capture session back, all in the background. Switching to MP4 may wait for the
    // result so the recording process never captures alongside a prepared GIF session.
    std::shared_future<void> release();
    // Starts recording: the native thread grabs (DXGI first, GDI fallback) into the store.
    void start();
    void pause();
    void resume();
    // Stops synchronously.
    std::vector<FrameData> &stop();
    // Stops without blocking: the timer stops now, a worker thread waits for the native
    // capture thread, then stop_finished is emitted.
    void stop_async();
    // Resets the recorder (GUI thread).
    void reset();
    // Releases only the data (store and frames), never the timer; safe off the GUI thread.
    void reset_data();
    int max_duration_s = 60;
signals:
    // The current frame count.
    void frame_captured(int count);
    // "IDLE" / "RECORDING" / ...
    void state_changed(const QString &state);
    // The maximum duration was reached; stopping follows.
    void limit_reached();
    void stop_finished();
private:
    // Stops the native capture session and waits for its thread.
    void stop_session();
    // Takes timestamps from the store into frames and binds the cursor track.
    void sync_frames_from_store();
    void tick();
    void start_scroll_listener();
    void stop_scroll_listener();
    void on_wheel(const QString &watcher, int x, int y, int delta, bool horizontal);
    void sample_cursor();
    void on_stop_worker_done();

    QRect rect_;
    int fps_ = 16;
    std::vector<FrameData> frames_;
    RecordState state_ = RecordState::IDLE;
    QTimer timer_;
    QElapsedTimer clock_;
    double pause_offset_ = 0;
    double pause_start_ = 0;
    std::shared_ptr<gifrecorder::FrameStore> store_;
    std::shared_ptr<gifrecorder::RecordSession> session_;
    std::optional<std::shared_future<std::shared_ptr<gifrecorder::RecordSession>>> prepared_;
    // Whether the capture session is lent to the recording thread.
    bool hdr_lent_ = false;
    int rec_width_ = 0;
    int rec_height_ = 0;
    std::vector<CursorSnapshot> cursor_track_;
    int rec_left_ = 0;
    int rec_top_ = 0;
    // Global vertical wheel while recording, read and cleared each frame.
    int scroll_value_ = 0;
    QPointer<InputHub> input_hub_;
    QMetaObject::Connection wheel_connection_;
};
}
