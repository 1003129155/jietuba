#include "pin/pin_animation.h"
#include "core/logger.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace jietuba {
namespace {
// Browsers show frames that ask for 10 ms or less for 100 ms; many GIFs are made with that in mind.
constexpr int SHORT_DELAY_MS = 10;
constexpr int DEFAULT_DELAY_MS = 100;
// A frame shown this late restarts the deadlines instead of rushing to catch up.
constexpr double MAX_LAG_MS = 100;

// Frames read between two reports of progress while reading up to a frame.
constexpr int SEEK_PROGRESS_STEP = 4;

// Reads up to frame index, sequentially when the format cannot jump; going_on gets the count read so
// far before each frame and stops reading by returning false.
bool seek_frame(QImageReader &reader, int index, const std::function<bool(int)> &going_on) {
    if (index <= 0 || reader.jumpToImage(index)) return true;
    for (int i = 0; i < index; ++i)
        if (!going_on(i) || reader.read().isNull()) return false;
    return true;
}

// The format the screen draws as it is, so showing a frame copies nothing.
QImage drawable(QImage frame) {
    return frame.hasAlphaChannel() ? frame.convertToFormat(QImage::Format_ARGB32_Premultiplied) : frame.convertToFormat(QImage::Format_RGB32);
}
}

AnimationFile::~AnimationFile() {
    QFile::remove(path_);
    QDir().rmdir(QFileInfo(path_).absolutePath());
}

std::optional<AnimationSource> load_animation(const QString &path) {
    QImageReader probe(path);
    if (!probe.canRead() || !probe.supportsAnimation()) return std::nullopt;
    AnimationSource source;
    source.format = probe.format();
    source.file_path = path;
    source.first_frame = probe.read();
    if (source.first_frame.isNull()) return std::nullopt;
    source.first_delay = probe.nextImageDelay();
    source.loop_count = probe.loopCount();
    source.frame_count = std::max(0, probe.imageCount());
    // A still image in a format that can animate, such as a one-frame GIF.
    if (source.frame_count == 1 || (source.frame_count == 0 && !probe.canRead())) return std::nullopt;
    const QDir folder(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("jietuba_pins/" + QUuid::createUuid().toString(QUuid::WithoutBraces)));
    const QString copy = folder.filePath(QFileInfo(path).fileName());
    if (!folder.mkpath(".") || !QFile::copy(path, copy)) {
        log_warning(T("动图复制失败: {file_path}", {{"file_path", QDir::toNativeSeparators(path)}}), "PinWindow");
        folder.rmdir(".");
        return std::nullopt;
    }
    source.file = std::make_shared<const AnimationFile>(copy);
    return source;
}

AnimationDecodeThread::AnimationDecodeThread(const AnimationSource &source, int start_frame, int generation, QObject *parent)
    : QThread(parent), path_(source.path()), format_(source.format), start_frame_(start_frame), generation_(generation), loop_count_(source.loop_count),
      slots_(PinAnimation::BUFFERED_FRAMES) {}

void AnimationDecodeThread::stop() {
    requestInterruption();
    // Wakes the loop when it waits for a free slot.
    slots_.release(PinAnimation::BUFFERED_FRAMES + 1);
    wait();
}

void AnimationDecodeThread::run() {
    int index = start_frame_;
    int plays = 0;
    while (!isInterruptionRequested()) {
        QImageReader reader(path_, format_);
        // Reading up to a frame far into a long animation takes seconds: progress meanwhile, and stop()
        // ends it there rather than waiting for it.
        const bool reached = seek_frame(reader, index, [this, index](int done) {
            if (done > 0 && done % SEEK_PROGRESS_STEP == 0) emit seek_progress(generation_, done, index);
            return !isInterruptionRequested();
        });
        if (!reached) return;
        bool decoded = false;
        while (!isInterruptionRequested()) {
            slots_.acquire();
            if (isInterruptionRequested()) return;
            const QImage frame = reader.read();
            if (frame.isNull()) {
                slots_.release();
                break;
            }
            decoded = true;
            emit frame_decoded(generation_, index++, drawable(frame), reader.nextImageDelay());
        }
        // Damaged data that yields no frame would otherwise spin here.
        if (isInterruptionRequested() || !decoded) return;
        if (loop_count_ >= 0 && ++plays > loop_count_) {
            emit finished_playing(generation_);
            return;
        }
        index = 0;
    }
}

PinAnimation::PinAnimation(AnimationSource source, std::function<void(const QImage &)> show, QObject *parent)
    : QObject(parent), show_(std::move(show)), source_(std::move(source)) {
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &PinAnimation::present_next);
    frames_.push_back({0, source_.first_frame, source_.first_delay});
    frame_count_ = source_.frame_count;
    restart_decoder(1);
    schedule_from_now();
}

PinAnimation::~PinAnimation() { stop(); }

void PinAnimation::stop() {
    timer_->stop();
    if (decoder_) {
        decoder_->disconnect(this);
        decoder_->stop();
        delete decoder_.data();
    }
}

int PinAnimation::effective_delay(int delay) const {
    const int ms = delay <= SHORT_DELAY_MS ? DEFAULT_DELAY_MS : delay;
    return std::max(1, static_cast<int>(std::lround(ms / speed())));
}

int PinAnimation::wrap(int index) const {
    int count = frame_count_;
    for (const auto &frame : frames_) count = std::max(count, frame.index + 1);
    index %= count;
    return index < 0 ? index + count : index;
}

void PinAnimation::restart_decoder(int start_frame) {
    stop();
    ended_ = false;
    waiting_ = false;
    if (frame_count_ > 0 && start_frame >= frame_count_) start_frame = 0;
    decoder_ = new AnimationDecodeThread(source_, start_frame, ++generation_, this);
    connect(decoder_, &AnimationDecodeThread::frame_decoded, this, &PinAnimation::on_frame_decoded);
    connect(decoder_, &AnimationDecodeThread::finished_playing, this, &PinAnimation::on_finished_playing);
    connect(decoder_, &AnimationDecodeThread::seek_progress, this, &PinAnimation::on_seek_progress);
    decoder_->start(QThread::LowPriority);
}

void PinAnimation::seek(int index) {
    seeking_ = true;
    seek_target_ = index;
    pending_steps_ = 0;
    // The frame on screen stays until the one asked for arrives.
    restart_decoder(index);
    emit seek_progress(0, index);
}

void PinAnimation::on_seek_progress(int generation, int done, int total) {
    if (generation == generation_ && seeking_) emit seek_progress(done, total);
}

void PinAnimation::on_frame_decoded(int generation, int index, const QImage &frame, int delay) {
    // Frames from a decoder replaced by a seek.
    if (generation != generation_) return;
    frame_count_ = std::max(frame_count_, index + 1);
    if (seeking_) {
        seeking_ = false;
        frames_.clear();
        frames_.push_back({index, frame, delay});
        // On screen now, so no longer ahead of playback.
        decoder_->release_frame();
        if (show_) show_(frame);
        if (!paused_) schedule_from_now();
        return;
    }
    frames_.push_back({index, frame, delay});
    if (pending_steps_ > 0) {
        --pending_steps_;
        advance(1);
    } else if (waiting_ && !paused_) {
        waiting_ = false;
        clock_.start();
        deadline_ms_ = 0;
        present_next();
    }
}

void PinAnimation::on_finished_playing(int generation) {
    if (generation != generation_) return;
    ended_ = true;
    // The last frame had already run out while the end was on its way.
    if (waiting_ && frames_.size() == 1) {
        waiting_ = false;
        paused_ = true;
    }
}

void PinAnimation::advance(int count) {
    for (int i = 0; i < count; ++i) frames_.pop_front();
    if (decoder_) decoder_->release_frame(count);
    if (show_) show_(frames_.front().image);
}

void PinAnimation::present_next() {
    if (paused_ || seeking_) return;
    if (frames_.size() < 2) {
        // The last play ended: the last frame stays, and playing again starts over.
        if (ended_) paused_ = true;
        else waiting_ = true;
        return;
    }
    advance(1);
    deadline_ms_ += effective_delay(frames_.front().delay);
    const double remaining = deadline_ms_ - static_cast<double>(clock_.elapsed());
    if (remaining < -MAX_LAG_MS) {
        schedule_from_now();
        return;
    }
    timer_->start(std::max(1, static_cast<int>(std::ceil(remaining))));
}

void PinAnimation::schedule_from_now() {
    clock_.start();
    deadline_ms_ = effective_delay(frames_.front().delay);
    timer_->start(static_cast<int>(deadline_ms_));
}

void PinAnimation::set_paused(bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    if (paused) {
        timer_->stop();
        waiting_ = false;
        return;
    }
    if (seeking_) return;
    if (ended_ && frames_.size() == 1) {
        // Over again from the first frame, shown as soon as it is decoded.
        seek(0);
        return;
    }
    schedule_from_now();
}

void PinAnimation::step(int delta) {
    set_paused(true);
    if (seeking_) {
        seek(wrap(seek_target_ + delta));
        return;
    }
    const int ahead = static_cast<int>(frames_.size()) - 1;
    if (delta > 0 && delta <= ahead) {
        advance(delta);
        return;
    }
    // Forward past what is decoded: through what is there, which frees the decoder to go on, then
    // the rest as it arrives; unless the play ended.
    if (delta > 0 && !ended_) {
        pending_steps_ += delta - ahead;
        if (ahead > 0) advance(ahead);
        return;
    }
    seek(wrap(frames_.front().index + delta));
}

void PinAnimation::change_speed(int direction) {
    const int count = static_cast<int>(std::size(SPEEDS));
    speed_index_ = direction == 0 ? NORMAL_SPEED : std::clamp(speed_index_ + direction, 0, count - 1);
    // The frame on screen is timed again at the new speed.
    if (!paused_ && !waiting_ && !seeking_) schedule_from_now();
}
}
