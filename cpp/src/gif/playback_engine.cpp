#include "gif/playback_engine.h"
#include "core/logger.h"
#include "gif/gifrecorder.h"

namespace jietuba {
PlaybackEngine::PlaybackEngine(QObject *parent) : QObject(parent), timer_(this) { connect(&timer_, &QTimer::timeout, this, &PlaybackEngine::advance); }

PlaybackEngine::~PlaybackEngine() { stop_decoder(); }

void PlaybackEngine::load(std::vector<FrameData> *frames, int fps, std::shared_ptr<gifrecorder::FrameStore> store) {
    stop_decoder();
    timer_.stop();
    frames_ = frames;
    fps_ = fps;
    store_ = std::move(store);
    current_ = 0;
    trim_start_ = 0;
    trim_end_ = std::max(0, frame_count() - 1);
    state_ = PlayState::IDLE;
}

void PlaybackEngine::set_display_size(int width, int height) {
    const bool changed = display_w_ != width || display_h_ != height;
    display_w_ = width;
    display_h_ = height;
    if (changed && state_ == PlayState::PLAYING) restart_decoder_from(current_);
}

void PlaybackEngine::play() {
    if (frame_count() == 0 || !store_) return;
    if (state_ == PlayState::PLAYING) return;
    state_ = PlayState::PLAYING;
    if (!decoder_) restart_decoder_from(current_);
    timer_.start(interval_ms());
}

void PlaybackEngine::pause() {
    timer_.stop();
    state_ = PlayState::PAUSED;
}

void PlaybackEngine::stop() {
    timer_.stop();
    stop_decoder();
    state_ = PlayState::IDLE;
}

void PlaybackEngine::cleanup(bool) {
    stop_decoder();
    frames_ = nullptr;
    store_.reset();
    state_ = PlayState::IDLE;
    log_debug(T("PlaybackEngine cleanup 完成"), "PlaybackEngine");
}

void PlaybackEngine::seek(int index) {
    index = std::max(trim_start_, std::min(index, trim_end_));
    current_ = index;
    if (state_ == PlayState::PLAYING) restart_decoder_from(index);
    else stop_decoder();
    emit frame_changed(index);
}

void PlaybackEngine::set_speed(double speed) {
    speed = std::max(0.1, speed);
    if (speed_ == speed) return;
    speed_ = speed;
    timer_.setInterval(interval_ms());
}

void PlaybackEngine::set_trim(int start, int end) {
    trim_start_ = std::max(0, start);
    trim_end_ = std::min(frame_count() - 1, end);
    if (current_ < trim_start_) seek(trim_start_);
    else if (current_ > trim_end_) seek(trim_end_);
}

QImage PlaybackEngine::get_frame_image(int index, int display_width, int display_height) const {
    if (frame_count() == 0 || index >= frame_count()) return {};
    if (!store_) return {};
    QByteArray rgb;
    try {
        rgb = store_->get_frame_rgb(index, display_width, display_height);
    } catch (const std::exception &e) {
        log_error(T("get_frame_image 失败 idx={index}: {e}", {{"index", index}, {"e", QString::fromUtf8(e.what())}}), "PlaybackEngine");
        return {};
    }
    // copy() takes the pixels, so the buffer may go.
    return QImage(reinterpret_cast<const uchar *>(rgb.constData()), display_width, display_height, display_width * 3, QImage::Format_RGB888).copy();
}

int PlaybackEngine::interval_ms() const { return std::max(10, static_cast<int>(1000.0 / fps_ / speed_)); }

void PlaybackEngine::stop_decoder() {
    if (decoder_) {
        decoder_->stop();
        decoder_.reset();
    }
}

void PlaybackEngine::restart_decoder_from(int index) {
    stop_decoder();
    if (!store_ || frame_count() == 0) return;
    const int w = display_w_ ? display_w_ : frames_->front().width;
    const int h = display_h_ ? display_h_ : frames_->front().height;
    if (w <= 0 || h <= 0) return;
    // Each frame is its own JPEG, so decoding starts right at the target.
    decoder_ = store_->start_decoder(w, h, 6, index);
    current_ = index;
}

void PlaybackEngine::advance() {
    if (!decoder_) return;
    const auto result = decoder_->try_next_frame();
    if (!result) {
        // The decoder may be momentarily empty, or done.
        if (decoder_->is_finished()) {
            stop_decoder();
            timer_.stop();
            state_ = PlayState::IDLE;
            emit playback_finished();
        }
        return;
    }
    const auto &rgb_data = result->first;
    const int w = display_w_, h = display_h_;
    const auto image = QImage(reinterpret_cast<const uchar *>(rgb_data.constData()), w, h, w * 3, QImage::Format_RGB888).copy();
    emit frame_ready(image, current_);
    emit frame_changed(current_);
    const int next = current_ + 1;
    if (next > trim_end_) {
        stop_decoder();
        timer_.stop();
        state_ = PlayState::IDLE;
        emit playback_finished();
        return;
    }
    current_ = next;
}
}
