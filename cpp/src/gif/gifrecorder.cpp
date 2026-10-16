#include "gif/gifrecorder.h"
#include "core/native.h"
#include <algorithm>
#include <stdexcept>

namespace jietuba::gifrecorder {
namespace {
QByteArray take_bytes(uint8_t *data, size_t len) {
    QByteArray bytes(reinterpret_cast<const char *>(data), static_cast<qsizetype>(len));
    jb_gif_bytes_free(data, len);
    return bytes;
}

JbSprite sprite_view(const Sprite &sprite) {
    return JbSprite{reinterpret_cast<const uint8_t *>(sprite.data.constData()), static_cast<size_t>(sprite.data.size()), static_cast<uint32_t>(sprite.w),
                    static_cast<uint32_t>(sprite.h)};
}

bool progress_trampoline(size_t current, size_t total, void *user) {
    auto *callback = static_cast<std::function<bool(size_t, size_t)> *>(user);
    try {
        return (*callback)(current, total);
    } catch (...) {
        // A failing callback cancels, as an exception in the reference's callback does.
        return false;
    }
}
}

FrameStore::FrameStore(int width, int height, int fps, int jpeg_quality, size_t max_frames, size_t max_memory_bytes)
    : handle_(jb_frame_store_new(static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(fps), jpeg_quality, max_frames,
                                 max_memory_bytes)) {}

FrameStore::~FrameStore() { jb_frame_store_free(handle_); }

int FrameStore::width() const { return static_cast<int>(jb_frame_store_width(handle_)); }
int FrameStore::height() const { return static_cast<int>(jb_frame_store_height(handle_)); }
int FrameStore::fps() const { return static_cast<int>(jb_frame_store_fps(handle_)); }
int FrameStore::frame_count() const { return static_cast<int>(jb_frame_store_frame_count(handle_)); }
quint64 FrameStore::memory_usage_bytes() const { return jb_frame_store_memory_usage_bytes(handle_); }
quint64 FrameStore::dropped_frames() const { return jb_frame_store_dropped_frames(handle_); }
int FrameStore::total_duration_ms() const { return static_cast<int>(jb_frame_store_total_duration_ms(handle_)); }

std::vector<int> FrameStore::frame_timestamps() const {
    // Frames may arrive between the two calls; only what the buffer received counts.
    std::vector<uint32_t> raw(jb_frame_store_frame_timestamps(handle_, nullptr, 0));
    const size_t total = jb_frame_store_frame_timestamps(handle_, raw.data(), raw.size());
    raw.resize(std::min(total, raw.size()));
    return std::vector<int>(raw.begin(), raw.end());
}

bool FrameStore::is_recording() const { return jb_frame_store_is_recording(handle_); }
bool FrameStore::is_paused() const { return jb_frame_store_is_paused(handle_); }

void FrameStore::set_state(quint8 state) {
    if (!jb_frame_store_set_state(handle_, state)) throw std::invalid_argument("state must be 0-3 (Idle/Recording/Paused/Stopped)");
}

quint8 FrameStore::state() const { return jb_frame_store_state(handle_); }

QByteArray FrameStore::get_frame_rgb(int index, int display_w, int display_h) const {
    uint8_t *data = nullptr;
    size_t len = 0;
    char error[512] = {};
    if (!jb_frame_store_get_frame_rgb(handle_, static_cast<size_t>(index), static_cast<uint32_t>(display_w), static_cast<uint32_t>(display_h), &data, &len, error,
                                      sizeof error))
        throw std::runtime_error(error);
    return take_bytes(data, len);
}

std::unique_ptr<FrameDecoder> FrameStore::start_decoder(int display_w, int display_h, int prefetch, int start_frame) const {
    return std::make_unique<FrameDecoder>(jb_frame_store_start_decoder(handle_, static_cast<uint32_t>(display_w), static_cast<uint32_t>(display_h),
                                                                      static_cast<size_t>(prefetch), static_cast<size_t>(start_frame)));
}

void FrameStore::export_gif(const QString &path, int width, int height, int repeat, int frame_start, int frame_end, std::function<bool(size_t, size_t)> progress,
                            const CursorSprites *cursor_sprites, const std::vector<std::optional<CursorInfo>> *cursor_infos, std::optional<double> speed) const {
    JbCursorSprites sprites{};
    if (cursor_sprites) {
        sprites.cursor = sprite_view(cursor_sprites->cursor);
        for (int i = 0; i < 3; ++i) {
            sprites.burst_left[i] = sprite_view(cursor_sprites->burst_left[i]);
            sprites.burst_right[i] = sprite_view(cursor_sprites->burst_right[i]);
        }
        sprites.scroll_up = sprite_view(cursor_sprites->scroll_up);
        sprites.scroll_down = sprite_view(cursor_sprites->scroll_down);
    }
    std::vector<JbCursorInfo> infos;
    if (cursor_infos) {
        infos.reserve(cursor_infos->size());
        for (const auto &info : *cursor_infos) {
            if (!info) {
                infos.push_back(JbCursorInfo{false, 0, 0, 0, 0, 0, 0});
                continue;
            }
            infos.push_back(JbCursorInfo{true, info->x, info->y, static_cast<uint8_t>(info->press), static_cast<int8_t>(info->scroll),
                                         static_cast<uint8_t>(info->burst_frame), static_cast<uint8_t>(info->burst_side)});
        }
    }
    char error[512] = {};
    const auto encoded = path.toUtf8();
    const bool ok = jb_frame_store_export_gif(handle_, encoded.constData(), static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint16_t>(repeat),
                                              static_cast<size_t>(frame_start), static_cast<size_t>(frame_end), progress ? progress_trampoline : nullptr,
                                              progress ? &progress : nullptr, cursor_sprites ? &sprites : nullptr, cursor_infos ? infos.data() : nullptr,
                                              infos.size(), speed ? static_cast<float>(*speed) : 1.0f, error, sizeof error);
    if (!ok) throw std::runtime_error(error);
}

void FrameStore::cancel_export() const { jb_frame_store_cancel_export(handle_); }
void FrameStore::clear() const { jb_frame_store_clear(handle_); }

FrameDecoder::~FrameDecoder() { jb_frame_decoder_free(handle_); }

namespace {
std::optional<std::pair<QByteArray, int>> deliver(int32_t outcome, uint8_t *data, size_t len, uint32_t elapsed) {
    if (outcome < 0) throw std::runtime_error("decoder already stopped");
    if (outcome == 0) return std::nullopt;
    return std::make_pair(take_bytes(data, len), static_cast<int>(elapsed));
}
}

std::optional<std::pair<QByteArray, int>> FrameDecoder::next_frame() {
    uint8_t *data = nullptr;
    size_t len = 0;
    uint32_t elapsed = 0;
    return deliver(jb_frame_decoder_next_frame(handle_, &data, &len, &elapsed), data, len, elapsed);
}

std::optional<std::pair<QByteArray, int>> FrameDecoder::try_next_frame() {
    uint8_t *data = nullptr;
    size_t len = 0;
    uint32_t elapsed = 0;
    return deliver(jb_frame_decoder_try_next_frame(handle_, &data, &len, &elapsed), data, len, elapsed);
}

int FrameDecoder::buffered_count() const { return static_cast<int>(jb_frame_decoder_buffered_count(handle_)); }
bool FrameDecoder::is_finished() const { return jb_frame_decoder_is_finished(handle_); }
int FrameDecoder::total_frames() const { return static_cast<int>(jb_frame_decoder_total_frames(handle_)); }
int FrameDecoder::fetched_count() const { return static_cast<int>(jb_frame_decoder_fetched_count(handle_)); }
int FrameDecoder::skip(int n) { return static_cast<int>(jb_frame_decoder_skip(handle_, static_cast<size_t>(std::max(0, n)))); }

void FrameDecoder::stop() {
    jb_frame_decoder_stop(handle_);
    stopped_ = true;
}

std::shared_ptr<RecordSession> RecordSession::prepare(bool prefer_dxgi) { return std::shared_ptr<RecordSession>(new RecordSession(jb_record_session_prepare(prefer_dxgi))); }

RecordSession::~RecordSession() { jb_record_session_free(handle_); }

void RecordSession::begin(const FrameStore &store, int left, int top, int width, int height, int fps) {
    char error[512] = {};
    if (!jb_record_session_begin(handle_, store.handle(), left, top, width, height, static_cast<uint32_t>(fps), error, sizeof error)) throw std::runtime_error(error);
}

void RecordSession::pause() {
    if (!jb_record_session_pause(handle_)) throw std::runtime_error("session already stopped");
}

void RecordSession::resume() {
    if (!jb_record_session_resume(handle_)) throw std::runtime_error("session already stopped");
}

void RecordSession::stop() { jb_record_session_stop(handle_); }

QString RecordSession::backend() const {
    switch (jb_record_session_backend(handle_)) {
    case 1: return "dxgi";
    case 2: return "gdi";
    default: return {};
    }
}

quint8 RecordSession::state() const { return jb_record_session_state(handle_); }
bool RecordSession::is_stopped() const { return jb_record_session_is_stopped(handle_); }
}
