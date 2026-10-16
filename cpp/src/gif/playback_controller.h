#pragma once
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <functional>
#include <utility>
#include <vector>

class QLabel;

// The playback controller: engine, preview layer, cursor overlay, toolbar and export.
namespace jietuba {
class CursorOverlay;
struct FrameData;
class FrameRecorder;
class PlaybackEngine;
class PlaybackToolbar;
class RecordToolbar;

class PlaybackController : public QObject {
    Q_OBJECT
public:
    using RepositionFn = std::function<void(QWidget *)>;
    explicit PlaybackController(QObject *parent = nullptr);
    // Injects the record window's shared context.
    void set_context(const QRect &rect, FrameRecorder *recorder, RecordToolbar *record_toolbar, RepositionFn reposition_fn);
    void update_rect(const QRect &rect) { rect_ = rect; }
    PlaybackEngine *engine() const { return engine_; }
    PlaybackToolbar *playback_toolbar() const { return playback_toolbar_; }
    QLabel *preview_label() const { return preview_label_; }
    CursorOverlay *cursor_overlay() const { return cursor_overlay_; }
    // Active while an engine exists.
    bool is_active() const { return engine_ != nullptr; }
    // Creates the engine, preview layer and toolbar and enters playback.
    void start_playback(std::vector<FrameData> &frames);
    // Destroys everything playback created (re-record / close).
    void teardown();
    // Hides all playback UI (close_all hides first, releases asynchronously).
    void hide_all();
    // Disconnects the engine signals so close_all gets no late callbacks.
    void disconnect_engine_signals();
    // Detaches the engine and toolbar for a background cleanup; the caller deletes them later.
    std::pair<PlaybackEngine *, PlaybackToolbar *> detach_for_cleanup();
signals:
    // Export finished or the user closed: the whole window should close.
    void close_requested();
    void rerecord_requested();
private:
    void connect_playback_toolbar();
    void disconnect_and_destroy_playback_toolbar();
    void on_play_pause();
    void on_speed_changed(double speed);
    void on_seek(int index);
    void on_trim_changed(int start, int end);
    void on_rerecord();
    // A still preview of one frame (seek / first frame / after pausing), decoded from the store.
    void render_frame(int index);
    // The resident decoder produced a frame: show it.
    void on_frame_ready(const QImage &image, int index);
    // frame_changed: only the progress (seek / paused).
    void on_playback_frame_index(int index);
    void update_playhead(int index);
    void on_playback_finished();
    void update_cursor_for_frame(int index);
    void on_cursor_toggled(bool visible);
    void destroy_preview_label();
    std::pair<int, int> get_trim_range() const;
    void on_save();
    void on_copy();
    // Shows the progress bar while exporting, then writes the file or copies it.
    void compose_and_export(const QString &output_path, bool copy_to_clipboard);
    // Writes the file to the clipboard as a URL plus the raw bytes.
    static void copy_file_to_clipboard(const QString &path, const QString &mime_type = "image/gif");
    // A GIF path in the screenshot save directory (created if missing).
    static QString make_save_path();

    PlaybackEngine *engine_ = nullptr;
    PlaybackToolbar *playback_toolbar_ = nullptr;
    QLabel *preview_label_ = nullptr;
    CursorOverlay *cursor_overlay_ = nullptr;
    bool cursor_export_enabled_ = true;
    // Export speed, synchronised with the playback speed.
    double export_speed_ = 1.0;
    QRect rect_;
    FrameRecorder *recorder_ = nullptr;
    RecordToolbar *record_toolbar_ = nullptr;
    RepositionFn reposition_fn_;
};
}
