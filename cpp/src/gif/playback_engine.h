#pragma once
#include "gif/frame_recorder.h"
#include <QImage>
#include <QObject>
#include <QTimer>
#include <memory>
#include <optional>
#include <vector>

// Playback driven by the native gifrecorder: speed, trimmed range and looping.
// Playing starts FrameStore::start_decoder (JPEG to RGB24 ahead of time on a native
// thread); each timer tick takes one frame, builds a QImage and emits frame_ready.
// Seeking decodes from the target frame on, since every frame is its own JPEG.
namespace jietuba {
namespace gifrecorder {
class FrameDecoder;
class FrameStore;
}

enum class PlayState { IDLE, PLAYING, PAUSED };

class PlaybackEngine : public QObject {
    Q_OBJECT
public:
    explicit PlaybackEngine(QObject *parent = nullptr);
    ~PlaybackEngine() override;
    // frames is the recorder's list, shared as in the reference (not copied).
    void load(std::vector<FrameData> *frames, int fps, std::shared_ptr<gifrecorder::FrameStore> store = nullptr);
    void set_display_size(int width, int height);
    PlayState state() const { return state_; }
    bool is_playing() const { return state_ == PlayState::PLAYING; }
    int frame_count() const { return frames_ ? static_cast<int>(frames_->size()) : 0; }
    int current_index() const { return current_; }
    int trim_start() const { return trim_start_; }
    int trim_end() const { return trim_end_; }
    void play();
    void pause();
    void stop();
    // Stops only the timer (GUI thread), before close_all starts its background thread.
    void stop_timer() { timer_.stop(); }
    // Releases the native resources; may run off the Qt thread, so the timer must already be stopped.
    void cleanup(bool blocking = false);
    void seek(int index);
    void set_speed(double speed);
    void set_trim(int start, int end);
    FrameData &get_frame(int index) { return (*frames_)[static_cast<size_t>(index)]; }
    // Decodes one frame on demand, for seek previews and the first frame.
    QImage get_frame_image(int index, int display_width, int display_height) const;
signals:
    // A new frame image and its index.
    void frame_ready(const QImage &image, int index);
    // Index only, for older connections.
    void frame_changed(int index);
    // Playback reached trim_end.
    void playback_finished();
private:
    int interval_ms() const;
    void stop_decoder();
    void restart_decoder_from(int index);
    // Timer tick: takes a frame from the decoder and builds a QImage.
    void advance();

    std::vector<FrameData> *frames_ = nullptr;
    std::shared_ptr<gifrecorder::FrameStore> store_;
    int fps_ = 16;
    double speed_ = 1.0;
    int current_ = 0;
    int trim_start_ = 0;
    int trim_end_ = 0;
    int display_w_ = 0;
    int display_h_ = 0;
    PlayState state_ = PlayState::IDLE;
    std::unique_ptr<gifrecorder::FrameDecoder> decoder_;
    QTimer timer_;
};
}
