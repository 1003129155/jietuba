#pragma once
#include "gif/video_settings.h"
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <vector>

class QFileDialog;

// The GIF recording / playback window: the state machine coordinator. This file holds
// the state machine, window layers, recording control and drawing forwarding;
// PlaybackController holds the playback engine, preview, cursor overlay and export.
namespace jietuba {
class CaptureOverlay;
class FrameRecorder;
class GifDrawingView;
class PlaybackController;
class RecordToolbar;
class VideoRecorder;
struct FrameData;

enum class AppState {
    // Recording idle; every layer passes clicks through.
    IDLE,
    // Adjusting the selection; the overlay takes the mouse.
    RESIZING,
    // Drawing; the drawing layer takes the mouse.
    DRAWING,
    PLAYBACK,
};

// Coordinates the three window layers, the toolbar and the state machine.
class GifRecordWindow : public QObject {
    Q_OBJECT
public:
    explicit GifRecordWindow(const QRect &capture_rect, QObject *parent = nullptr);
    bool video_busy() const;
    bool overlay_visible() const;
    void close_all();
signals:
    void closed();
private:
    struct PendingVideo {
        QString path;
        RecordingOptions options;
        std::chrono::steady_clock::time_point started;
    };
    void reposition_current_toolbar();
    void connect_record_toolbar();
    void enter_state(AppState state);
    void sync_geometry(const QRect &rect, bool reposition = true);
    // Below the selection first, then above, left, right.
    void reposition_toolbar(QWidget *toolbar);
    void on_rect_changed(const QRect &rect);
    // The drag handle moves the selection and the toolbar together.
    void on_move_requested(const QPoint &delta);
    void on_drag_ended();
    void on_fps_changed(int fps);
    void on_mode_changed(const QString &mode);
    // Shows the frame count as elapsed time.
    void on_record_frame_count(int count);
    void on_record_start();
    void start_gif_recording();
    // stop_async waits for the capture thread in the background, then playback starts.
    void on_record_stop();
    // A frameless progress bar, shown late so it is not recorded at the end.
    void show_stop_progress();
    void on_pause_toggled(bool paused);
    void begin_countdown(std::function<void()> action);
    void advance_countdown();
    void cancel_countdown();
    void start_video();
    QFileDialog *take_save_dialog();
    void on_video_path_cancelled();
    void on_video_path_selected();
    void start_pending_video();
    void on_video_ready();
    void on_video_progress(double elapsed);
    void on_video_paused(bool paused);
    void on_video_completed(const QString &path);
    void on_video_failed(const QString &code, const QString &message);
    void reset_video_controls();
    void on_video_finished();
    void use_video_result(const QString &action);
    void rerecord_video();
    void on_drawing_tool_selected(const QString &tool_id);
    // Leaves drawing: back to pass-through.
    void on_drawing_deactivate();
    void on_drawing_color_changed(const QColor &color);
    void on_drawing_width_changed(int width);
    void on_drawing_opacity_changed(int opacity);
    void on_drawing_arrow_style_changed(const QString &style);
    void switch_to_playback(std::vector<FrameData> &frames);
    // Re-record: a full reset from scratch.
    void on_rerecord();

    QRect rect_;
    AppState state_ = AppState::IDLE;
    FrameRecorder *recorder_ = nullptr;
    VideoRecorder *video_ = nullptr;
    bool close_after_video_ = false;
    QPointer<QFileDialog> save_dialog_;
    std::optional<std::shared_future<void>> gif_release_;
    std::optional<PendingVideo> pending_video_;
    std::optional<QString> completed_video_path_;
    std::function<void()> countdown_action_;
    int countdown_remaining_ = 0;
    QTimer countdown_timer_;
    QTimer release_timer_;
    CaptureOverlay *overlay_ = nullptr;
    GifDrawingView *drawing_view_ = nullptr;
    RecordToolbar *record_toolbar_ = nullptr;
    PlaybackController *playback_ = nullptr;
};

// The single recording window, kept here so it outlives the screenshot window that opened it.
QPointer<GifRecordWindow> &active_gif_window();
}
