#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

struct JbFrameStore;
struct JbFrameDecoder;
struct JbRecordSession;

// C++ handles over the native gifrecorder: what the reference imports as the
// gifrecorder extension module. Methods throw std::runtime_error where the
// reference's raise.
namespace jietuba::gifrecorder {
constexpr quint8 STATE_IDLE = 0;
constexpr quint8 STATE_RECORDING = 1;
constexpr quint8 STATE_PAUSED = 2;
constexpr quint8 STATE_STOPPED = 3;

class FrameDecoder;

// One RGBA32 cursor sprite.
struct Sprite {
    QByteArray data;
    int w = 0;
    int h = 0;
};

struct CursorSprites {
    Sprite cursor;
    Sprite burst_left[3];
    Sprite burst_right[3];
    Sprite scroll_up;
    Sprite scroll_down;
};

// One frame's cursor in GIF coordinates: (x, y, press, scroll, burst_frame, burst_side).
struct CursorInfo {
    int x = 0, y = 0;
    int press = 0;
    int scroll = 0;
    int burst_frame = 0;
    int burst_side = 0;
};

// Frames stored as JPEG while recording, decoded on demand for playback and export.
class FrameStore {
public:
    FrameStore(int width, int height, int fps, int jpeg_quality = 95, size_t max_frames = 0, size_t max_memory_bytes = 0);
    ~FrameStore();
    FrameStore(const FrameStore &) = delete;
    FrameStore &operator=(const FrameStore &) = delete;
    int width() const;
    int height() const;
    int fps() const;
    int frame_count() const;
    quint64 memory_usage_bytes() const;
    quint64 dropped_frames() const;
    int total_duration_ms() const;
    std::vector<int> frame_timestamps() const;
    bool is_recording() const;
    bool is_paused() const;
    void set_state(quint8 state);
    quint8 state() const;
    // RGB24, display size 0 for the original.
    QByteArray get_frame_rgb(int index, int display_w = 0, int display_h = 0) const;
    std::unique_ptr<FrameDecoder> start_decoder(int display_w = 0, int display_h = 0, int prefetch = 4, int start_frame = 0) const;
    // progress(current, total) returning false cancels; failures and cancellation throw.
    void export_gif(const QString &path, int width = 0, int height = 0, int repeat = 0, int frame_start = 0, int frame_end = 0,
                    std::function<bool(size_t, size_t)> progress = {}, const CursorSprites *cursor_sprites = nullptr,
                    const std::vector<std::optional<CursorInfo>> *cursor_infos = nullptr, std::optional<double> speed = std::nullopt) const;
    void cancel_export() const;
    void clear() const;
    JbFrameStore *handle() const { return handle_; }
private:
    JbFrameStore *handle_;
};

// Decodes JPEG to RGB24 on its own thread ahead of playback.
class FrameDecoder {
public:
    explicit FrameDecoder(JbFrameDecoder *handle) : handle_(handle) {}
    ~FrameDecoder();
    FrameDecoder(const FrameDecoder &) = delete;
    FrameDecoder &operator=(const FrameDecoder &) = delete;
    // Blocks for the next frame: (RGB24, elapsed_ms), nullopt when all are taken.
    std::optional<std::pair<QByteArray, int>> next_frame();
    std::optional<std::pair<QByteArray, int>> try_next_frame();
    int buffered_count() const;
    bool is_finished() const;
    int total_frames() const;
    int fetched_count() const;
    // Drops up to n frames without handing them out; returns how many.
    int skip(int n);
    void stop();
private:
    JbFrameDecoder *handle_;
    bool stopped_ = false;
};

// Screen capture on its own native thread (DXGI first, GDI fallback).
class RecordSession {
public:
    // Builds the DXGI session on the capture thread now; begin() starts capturing.
    static std::shared_ptr<RecordSession> prepare(bool prefer_dxgi = true);
    ~RecordSession();
    RecordSession(const RecordSession &) = delete;
    RecordSession &operator=(const RecordSession &) = delete;
    void begin(const FrameStore &store, int left, int top, int width, int height, int fps);
    void pause();
    void resume();
    // Waits for the capture thread to exit.
    void stop();
    // "dxgi" or "gdi"; empty before any frame. Kept after stopping.
    QString backend() const;
    quint8 state() const;
    bool is_stopped() const;
private:
    explicit RecordSession(JbRecordSession *handle) : handle_(handle) {}
    JbRecordSession *handle_;
};
}
