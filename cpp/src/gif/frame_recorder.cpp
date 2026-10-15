#include "gif/frame_recorder.h"
#include "capture/capture_service.h"
#include "core/input_hub.h"
#include "core/logger.h"
#include "gif/gifrecorder.h"
#include <QThread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
const QString WHEEL_WATCHER = "gif";

// The native recording thread has exited: logs which capture path (DXGI / GDI) it used.
void session_stopped(const std::shared_ptr<gifrecorder::RecordSession> &session) {
    const auto backend = session->backend();
    log_info(T("录制截取路径: {backend}", {{"backend", backend.isEmpty() ? QString("None") : backend}}), "GIF");
}

template <class T>
std::shared_future<T> completed(T value) {
    std::promise<T> done;
    done.set_value(std::move(value));
    return done.get_future().share();
}

std::shared_future<void> completed_void() {
    std::promise<void> done;
    done.set_value();
    return done.get_future().share();
}
}

QString record_state_name(RecordState state) {
    switch (state) {
    case RecordState::IDLE: return "IDLE";
    case RecordState::RECORDING: return "RECORDING";
    case RecordState::PAUSED: return "PAUSED";
    case RecordState::STOPPED: return "STOPPED";
    }
    return {};
}

FrameRecorder::FrameRecorder(QObject *parent) : QObject(parent), timer_(this) {
    // The timer only reports frame counts and checks the time limit.
    connect(&timer_, &QTimer::timeout, this, &FrameRecorder::tick);
}

FrameRecorder::~FrameRecorder() { stop_scroll_listener(); }

int FrameRecorder::total_duration_ms() const {
    if (store_) return store_->total_duration_ms();
    if (frames_.empty()) return 0;
    return frames_.back().elapsed_ms;
}

void FrameRecorder::set_fps(int fps) {
    fps_ = fps;
    if (state_ == RecordState::RECORDING) timer_.setInterval(1000 / fps);
}

void FrameRecorder::prepare() {
    if (prepared_) return;
    const bool prefer_dxgi = uses_hdr_engine();
    auto make = [prefer_dxgi] { return gifrecorder::RecordSession::prepare(prefer_dxgi); };
    if (prefer_dxgi && !hdr_lent_) {
        hdr_lent_ = true;
        auto promise = std::make_shared<std::promise<std::shared_ptr<gifrecorder::RecordSession>>>();
        prepared_ = promise->get_future().share();
        const auto lent = lend_hdr_session([promise, make] {
            try {
                promise->set_value(make());
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        });
        // The session thread has stopped (exit): prepare here instead of waiting forever.
        if (!lent.valid()) promise->set_value(make());
    } else {
        prepared_ = completed(make());
    }
}

std::shared_future<void> FrameRecorder::release() {
    timer_.stop();
    stop_scroll_listener();
    auto prepared = std::move(prepared_);
    prepared_.reset();
    auto session = std::move(session_);
    session_.reset();
    auto stop_threads = [prepared, session] {
        for (const auto &pending : {prepared, std::optional<std::shared_future<std::shared_ptr<gifrecorder::RecordSession>>>(completed(session))}) {
            try {
                const auto running = pending ? pending->get() : nullptr;
                if (running) running->stop();
            } catch (const std::exception &e) {
                log_exception(e, T("停止 Rust 截屏会话"));
            }
        }
    };
    if (hdr_lent_) {
        hdr_lent_ = false;
        const auto returned = return_hdr_session(stop_threads);
        if (returned.valid()) return returned;
    }
    stop_threads();
    return completed_void();
}

void FrameRecorder::start() {
    if (state_ != RecordState::IDLE && state_ != RecordState::STOPPED) return;
    frames_.clear();
    store_.reset();
    session_.reset();

    // Insets 1 px on every side so the selection border is not recorded.
    const int inset = 1;
    int w = rect_.width() - inset * 2;
    int h = rect_.height() - inset * 2;
    const int left = rect_.x() + inset;
    const int top = rect_.y() + inset;
    w = std::max(w, 2);
    h = std::max(h, 2);
    rec_width_ = w;
    rec_height_ = h;
    rec_left_ = left;
    rec_top_ = top;
    cursor_track_.clear();

    try {
        store_ = std::make_shared<gifrecorder::FrameStore>(w, h, fps_, 90);
        store_->set_state(gifrecorder::STATE_RECORDING);
    } catch (const std::exception &e) {
        log_error(T("FrameStore 创建失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "GIF");
        store_.reset();
        return;
    }

    // Takes the prepared capture thread; without one it is prepared now and the first frame comes later.
    prepare();
    auto prepared = std::move(*prepared_);
    prepared_.reset();
    try {
        session_ = prepared.get();
        session_->begin(*store_, left, top, w, h, fps_);
    } catch (const std::exception &e) {
        log_error(T("RecordSession 启动失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "GIF");
        if (session_) session_->stop();
        store_.reset();
        session_.reset();
        return;
    }

    clock_.start();
    pause_offset_ = 0;
    pause_start_ = 0;
    state_ = RecordState::RECORDING;
    timer_.start(1000 / fps_);
    start_scroll_listener();
    emit state_changed(record_state_name(state_));
    log_info(T("录制开始: {w}x{h} @ {fps}fps", {{"w", w}, {"h", h}, {"fps", fps_}}), "GIF");
}

void FrameRecorder::pause() {
    if (state_ != RecordState::RECORDING) return;
    timer_.stop();
    pause_start_ = clock_.nsecsElapsed() / 1e9;
    if (session_) session_->pause();
    if (store_) store_->set_state(gifrecorder::STATE_PAUSED);
    state_ = RecordState::PAUSED;
    emit state_changed(record_state_name(state_));
}

void FrameRecorder::resume() {
    if (state_ != RecordState::PAUSED) return;
    pause_offset_ += clock_.nsecsElapsed() / 1e9 - pause_start_;
    pause_start_ = 0;
    if (session_) session_->resume();
    if (store_) store_->set_state(gifrecorder::STATE_RECORDING);
    state_ = RecordState::RECORDING;
    timer_.start(1000 / fps_);
    emit state_changed(record_state_name(state_));
}

std::vector<FrameData> &FrameRecorder::stop() {
    timer_.stop();
    stop_session();
    if (store_) store_->set_state(gifrecorder::STATE_STOPPED);
    sync_frames_from_store();
    state_ = RecordState::STOPPED;
    emit state_changed(record_state_name(state_));
    return frames_;
}

void FrameRecorder::stop_async() {
    if (state_ != RecordState::RECORDING && state_ != RecordState::PAUSED) {
        emit stop_finished();
        return;
    }
    timer_.stop();
    auto session = std::move(session_);
    session_.reset();
    // The worker holds its own references and never this recorder: the window may close and
    // delete it while the capture thread is still stopping. Frames are synced on the GUI thread.
    auto *thread = QThread::create([session, store = store_] {
        try {
            if (session) {
                try {
                    session->stop();
                } catch (const std::exception &e) {
                    log_error(T("RecordSession.stop 异常: {e}", {{"e", QString::fromUtf8(e.what())}}), "GIF");
                }
                session_stopped(session);
            }
            if (store) store->set_state(gifrecorder::STATE_STOPPED);
        } catch (const std::exception &e) {
            log_error(T("StopWorker 异常: {e}", {{"e", QString::fromUtf8(e.what())}}), "GIF");
        }
    });
    connect(thread, &QThread::finished, this, &FrameRecorder::on_stop_worker_done);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void FrameRecorder::on_stop_worker_done() {
    // Background stop finished, back on the GUI thread.
    try {
        sync_frames_from_store();
    } catch (const std::exception &e) {
        log_error(T("StopWorker 异常: {e}", {{"e", QString::fromUtf8(e.what())}}), "GIF");
    }
    state_ = RecordState::STOPPED;
    emit state_changed(record_state_name(state_));
    emit stop_finished();
}

void FrameRecorder::reset() {
    timer_.stop();
    stop_session();
    if (store_) store_->clear();
    store_.reset();
    frames_.clear();
    state_ = RecordState::IDLE;
    emit state_changed(record_state_name(state_));
}

void FrameRecorder::reset_data() {
    stop_session();
    if (store_) store_->clear();
    store_.reset();
    frames_.clear();
    state_ = RecordState::IDLE;
}

void FrameRecorder::stop_session() {
    stop_scroll_listener();
    if (session_) {
        try {
            session_->stop();
        } catch (const std::exception &e) {
            log_exception(e, T("停止 Rust 截屏会话"));
        }
        session_stopped(session_);
        session_.reset();
    }
}

void FrameRecorder::sync_frames_from_store() {
    if (!store_) return;
    frames_.clear();
    const auto timestamps = store_->frame_timestamps();
    const auto n_frames = timestamps.size();
    const auto n_cursor = cursor_track_.size();
    for (size_t i = 0; i < n_frames; ++i) {
        // Cursor samples (GUI timer) and frames (native thread) need not match in count:
        // aligned by index, or mapped proportionally when there are fewer samples.
        std::optional<CursorSnapshot> cursor;
        if (n_cursor > 0) {
            const size_t ci = n_frames <= n_cursor ? std::min(i, n_cursor - 1) : static_cast<size_t>(static_cast<double>(i) * n_cursor / n_frames);
            cursor = cursor_track_[ci];
        }
        frames_.push_back(FrameData{timestamps[i], rec_width_, rec_height_, {}, cursor});
    }
}

void FrameRecorder::tick() {
    const int count = store_ ? store_->frame_count() : static_cast<int>(frames_.size());
    sample_cursor();
    emit frame_captured(count);
    // Past the maximum duration: stop automatically.
    const double elapsed = clock_.nsecsElapsed() / 1e9 - pause_offset_;
    if (max_duration_s > 0 && elapsed >= max_duration_s) {
        timer_.stop();
        emit limit_reached();
    }
}

void FrameRecorder::start_scroll_listener() {
    if (input_hub_) return;
    try {
        auto *hub = input_hub();
        wheel_connection_ = connect(hub, &InputHub::wheel, this, &FrameRecorder::on_wheel, Qt::QueuedConnection);
        hub->native->watch_wheel(WHEEL_WATCHER);
        input_hub_ = hub;
    } catch (const std::exception &e) {
        log_error(T("滚轮监听启动失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "GIF");
    }
}

void FrameRecorder::stop_scroll_listener() {
    auto hub = input_hub_;
    input_hub_ = nullptr;
    if (hub) {
        try {
            hub->native->unwatch_wheel(WHEEL_WATCHER);
            disconnect(wheel_connection_);
        } catch (const std::exception &e) {
            log_exception(e, T("停止滚轮监听"));
        }
    }
    scroll_value_ = 0;
}

void FrameRecorder::on_wheel(const QString &watcher, int, int, int delta, bool horizontal) {
    if (watcher == WHEEL_WATCHER && !horizontal && delta) scroll_value_ = delta > 0 ? 1 : -1;
}

void FrameRecorder::sample_cursor() {
    POINT point{};
    GetCursorPos(&point);
    const int rx = point.x - rec_left_;
    const int ry = point.y - rec_top_;
    const bool visible = 0 <= rx && rx < rec_width_ && 0 <= ry && ry < rec_height_;
    // Left button first, then right.
    int press = 0;
    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) press = 1;
    else if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) press = 2;
    // The wheel state is read and cleared: once per frame.
    const int scroll = scroll_value_;
    scroll_value_ = 0;
    cursor_track_.push_back(CursorSnapshot{rx, ry, press, visible, scroll});
}
}
