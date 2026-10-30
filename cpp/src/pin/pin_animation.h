#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSemaphore>
#include <QString>
#include <QThread>
#include <deque>
#include <functional>
#include <memory>
#include <optional>

class QTimer;

namespace jietuba {
// A copy of an animation's file made for its pin, so the pin plays on after the original is
// moved or deleted without holding the whole file in memory; removed with the last owner.
class AnimationFile {
public:
    explicit AnimationFile(QString path) : path_(std::move(path)) {}
    ~AnimationFile();
    AnimationFile(const AnimationFile &) = delete;
    AnimationFile &operator=(const AnimationFile &) = delete;
    const QString &path() const { return path_; }
private:
    QString path_;
};

struct AnimationSource {
    std::shared_ptr<const AnimationFile> file;
    // The reader format, such as "gif" or "webp".
    QByteArray format;
    // Where it was copied from.
    QString file_path;
    QImage first_frame;
    int first_delay = 0;
    // 0 when the reader cannot tell without decoding every frame.
    int frame_count = 0;
    // As QImageReader::loopCount: -1 forever, 0 once, n repeats after the first play.
    int loop_count = -1;
    // The pin's own copy, named as the original.
    const QString &path() const { return file->path(); }
};

// The file as an animation when it has more than one frame, copied for its pin; nullopt for still or unreadable images.
std::optional<AnimationSource> load_animation(const QString &path);

// Decodes frames in order from a starting frame, never more than the semaphore allows ahead of
// playback; at the end it starts over until the loop count runs out. Frames come out in the
// format the screen draws without converting.
class AnimationDecodeThread : public QThread {
    Q_OBJECT
public:
    AnimationDecodeThread(const AnimationSource &source, int start_frame, int generation, QObject *parent);
    // Lets the decoder read more frames ahead.
    void release_frame(int count = 1) { slots_.release(count); }
    void stop();
signals:
    void frame_decoded(int generation, int index, const QImage &frame, int delay);
    void finished_playing(int generation);
    // While reading up to the starting frame: done of the total frames before it.
    void seek_progress(int generation, int done, int total);
protected:
    void run() override;
private:
    QString path_;
    QByteArray format_;
    int start_frame_, generation_, loop_count_;
    QSemaphore slots_;
};

// Plays an animation: frames come from a decode thread a few frames ahead, and each is handed
// to show for its own delay divided by the speed. Deadlines add up from the start of playback,
// so timer rounding never accumulates into drift. Stepping back has the decoder read up to the
// frame on its own thread, and it shows when ready. Nothing is decoded on the caller's thread.
class PinAnimation : public QObject {
    Q_OBJECT
public:
    static constexpr int BUFFERED_FRAMES = 3;
    // The first frame is already on screen; show gets each later one.
    PinAnimation(AnimationSource source, std::function<void(const QImage &)> show, QObject *parent);
    ~PinAnimation() override;
    const AnimationSource &source() const { return source_; }
    QImage current_frame() const { return frames_.front().image; }
    // 0-based.
    int frame_index() const { return frames_.front().index; }
    // Known once the reader counted the frames or playback reached the end; 0 until then.
    int frame_count() const { return frame_count_; }
    bool paused() const { return paused_; }
    // A frame asked for is being read up to; the one on screen stays until it shows.
    bool seeking() const { return seeking_; }
    void set_paused(bool paused);
    void toggle_paused() { set_paused(!paused_); }
    // Pauses and shows the frame delta frames away, wrapping at either end.
    void step(int delta);
    double speed() const { return SPEEDS[speed_index_]; }
    // +1 faster, -1 slower, 0 back to normal.
    void change_speed(int direction);
    void stop();
    static constexpr double SPEEDS[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
    static constexpr int NORMAL_SPEED = 3;
signals:
    // A frame asked for is read up to from the start: at once with done 0, then as reading goes on.
    void seek_progress(int done, int total);
private:
    struct Frame {
        int index;
        QImage image;
        int delay;
    };
    void restart_decoder(int start_frame);
    // Shows frame index once the decoder has read up to it.
    void seek(int index);
    void on_frame_decoded(int generation, int index, const QImage &frame, int delay);
    void on_finished_playing(int generation);
    void on_seek_progress(int generation, int done, int total);
    // Shows the next frame and schedules the one after.
    void present_next();
    // Shows the frame count frames on, letting the decoder read as many more ahead.
    void advance(int count);
    void schedule_from_now();
    int effective_delay(int delay) const;
    int wrap(int index) const;
    std::function<void(const QImage &)> show_;
    AnimationSource source_;
    QPointer<AnimationDecodeThread> decoder_;
    int generation_ = 0;
    // The frame on screen, then those decoded ahead, which hold the decoder's slots.
    std::deque<Frame> frames_;
    QTimer *timer_;
    QElapsedTimer clock_;
    double deadline_ms_ = 0;
    int frame_count_ = 0;
    int speed_index_ = NORMAL_SPEED;
    bool paused_ = false;
    // The timer fired before the decoder delivered; the next frame shows on arrival.
    bool waiting_ = false;
    // The loop count ran out after the last decoded frame.
    bool ended_ = false;
    // A frame asked for that the decoder is reading up to.
    bool seeking_ = false;
    int seek_target_ = 0;
    // Steps forward taken before the decoder had the frames.
    int pending_steps_ = 0;
};
}
