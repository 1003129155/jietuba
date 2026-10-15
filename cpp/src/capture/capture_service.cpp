#include "capture/capture_service.h"
#include "capture/system_cursor.h"
#include "core/logger.h"
#include "core/native.h"
#include "core/serial_executor.h"
#include "settings/tool_settings.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

namespace jietuba {
namespace {
// The BitBlt path of the reference's capture library: top-down 32-bit DIB of the
// screen DC, CAPTUREBLT so layered windows are included.
QImage grab_gdi(int left, int top, int width, int height) {
    HDC srcdc = GetWindowDC(nullptr);
    HDC memdc = CreateCompatibleDC(srcdc);
    HBITMAP bmp = CreateCompatibleBitmap(srcdc, width, height);
    HGDIOBJ previous = SelectObject(memdc, bmp);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    QImage image(width, height, QImage::Format_RGB32);
    BitBlt(memdc, 0, 0, width, height, srcdc, left, top, SRCCOPY | CAPTUREBLT);
    // RGB32 rows are width * 4 bytes, the same as the DIB, so it reads straight into the image.
    const int lines = GetDIBits(memdc, bmp, 0, height, image.bits(), &bmi, DIB_RGB_COLORS);
    SelectObject(memdc, previous);
    DeleteObject(bmp);
    DeleteDC(memdc);
    ReleaseDC(nullptr, srcdc);
    if (lines != height) throw std::runtime_error("gdi32.GetDIBits() failed.");
    return image;
}

// DXGI frames and BitBlt both lack the pointer, so both paths add it here; a
// failure keeps the capture without it.
void draw_cursor(QImage &image, int origin_x, int origin_y, const std::shared_ptr<SystemCursor> &cursor) {
    if (!cursor) return;
    try {
        cursor->draw_onto(image, origin_x, origin_y);
    } catch (const std::exception &e) {
        log_exception(e, T("绘制鼠标指针"));
    }
}
}

namespace {
// Per-output DXGI wait budget. 0 takes only queued frames: no new present means the
// picture did not change and the cached frame is current; anything else would make
// captures of a still desktop wait for the next present.
constexpr uint32_t HDR_TIMEOUT_MS = 0;
// A new session gets its first frame only on a desktop present. Built in the background,
// it waits this long once; the first capture falls back to GDI when nothing came.
constexpr int64_t PRIME_TIMEOUT_MS = 250;

// One grabbed HDR frame.
struct HdrFrame {
    QImage image;
    std::vector<JbHdrFrameInfo> infos;
};

// The process-wide DXGI capture session; built, used and closed on one dedicated thread.
// - Resident: a new session gets its first frame only on a desktop present, which never
//   comes while displays sleep.
// - The capture belongs to the thread that created it; a duplication leaked elsewhere
//   leaves that display unable to build a session again.
// - A failed build is remembered and not retried on every capture; forget_failure() and
//   reset() clear it.
// - Each display allows one duplication at a time: while GIF recording holds it (lend),
//   acquire() fails until give_back(), without counting as a failure.
class HdrSession {
public:
    static HdrSession &instance() {
        static HdrSession session;
        return session;
    }

    // Runs on the session thread and waits; runs directly when already there, which
    // would otherwise wait for itself.
    template <class F>
    auto run(F task) -> std::invoke_result_t<F> {
        if (executor_.on_worker_thread()) return task();
        auto future = executor_.submit(std::move(task));
        if (!future.valid()) throw std::runtime_error("HDR capture session unavailable");
        return future.get();
    }

    bool ensure(bool remember_failure = true) {
        if (failed_ || lent_) return false;
        if (!capture_) {
            char message[512] = {};
            int32_t kind = 0;
            capture_ = jb_hdr_capture_new(HDR_TIMEOUT_MS, message, sizeof message, &kind);
            if (!capture_) {
                if (remember_failure) failed_ = true;
                log_exception("CaptureError", QString::fromUtf8(message), T("建立 HDR 捕获会话失败"));
                return false;
            }
            log_debug(T("HDR 捕获会话已建立"), "CaptureService");
        }
        return true;
    }

    // For background builds: a new session also waits for its first frame; an existing one is left alone.
    void ensure_primed() {
        const bool created = capture_ == nullptr;
        if (ensure(false) && created) {
            std::vector<JbHdrMonitor> monitors(jb_hdr_capture_monitors(capture_, nullptr, 0));
            jb_hdr_capture_monitors(capture_, monitors.data(), monitors.size());
            for (size_t i = 1; i < monitors.size(); ++i) {
                JbHdrFrame frame{};
                char message[512] = {};
                if (jb_hdr_capture_grab(capture_, monitors[i].index, PRIME_TIMEOUT_MS, false, &frame, message, sizeof message) == JB_HDR_OK)
                    jb_hdr_frame_free(&frame);
                else
                    log_debug(T("HDR 会话预取首帧未完成: {error}", {{"error", QString::fromUtf8(message)}}), "CaptureService");
            }
        }
    }

    void close() {
        if (capture_) {
            jb_hdr_capture_free(capture_);
            capture_ = nullptr;
        }
    }

    // Whether a session is ready; false when unavailable or lent. Usually already built
    // in the background, so this need not wait.
    bool acquire() {
        if (failed_ || lent_) return false;
        return run([this] { return ensure(); });
    }

    std::vector<JbHdrMonitor> monitors() {
        return run([this] {
            if (!capture_) throw std::runtime_error("HDR capture session unavailable");
            std::vector<JbHdrMonitor> result(jb_hdr_capture_monitors(capture_, nullptr, 0));
            jb_hdr_capture_monitors(capture_, result.data(), result.size());
            return result;
        });
    }

    HdrFrame grab(size_t monitor_index, bool adaptive) {
        return run([this, monitor_index, adaptive] {
            if (!capture_) throw std::runtime_error("HDR capture session unavailable");
            JbHdrFrame frame{};
            char message[512] = {};
            if (jb_hdr_capture_grab(capture_, monitor_index, HDR_TIMEOUT_MS, adaptive, &frame, message, sizeof message) != JB_HDR_OK)
                throw std::runtime_error(message);
            return take(frame);
        });
    }

    HdrFrame grab_region(const QRect &rect, bool adaptive) {
        return run([this, rect, adaptive] {
            if (!capture_) throw std::runtime_error("HDR capture session unavailable");
            JbHdrFrame frame{};
            char message[512] = {};
            if (jb_hdr_capture_grab_region(capture_, rect.x(), rect.y(), static_cast<uint32_t>(rect.width()), static_cast<uint32_t>(rect.height()),
                                           HDR_TIMEOUT_MS, adaptive, &frame, message, sizeof message) != JB_HDR_OK)
                throw std::runtime_error(message);
            return take(frame);
        });
    }

    // Builds the session in the background without waiting; only_if runs on the session
    // thread and closes the session when false. A failed warm-up is not remembered: at
    // autostart the desktop may not be ready, so the first capture retries.
    void warm_up(std::function<bool()> only_if) {
        executor_.submit([this, only_if = std::move(only_if)] {
            if (!only_if || only_if()) ensure_primed();
            else close();
        });
    }

    // After display configuration changes: rebuilds in the background, or only releases
    // when only_if is false. Nothing happens while lent: the recording thread's session
    // handles changes itself. A session that failed before may build now, so the
    // failure is cleared.
    void refresh(std::function<bool()> only_if) {
        executor_.submit([this, only_if = std::move(only_if)] {
            if (lent_) return;
            close();
            failed_ = false;
            if (!only_if || only_if()) ensure_primed();
        });
    }

    // Closes the session in the background without counting as a failure; acquire rebuilds it.
    void release() {
        if (capture_) executor_.submit([this] { close(); });
    }

    // Marks it lent and closes it in the background; then runs afterwards on the same thread.
    std::shared_future<void> lend(std::function<void()> then) {
        lent_ = true;
        auto future = executor_.submit([this, then = std::move(then)] {
            close();
            if (then) then();
        });
        return future.valid() ? future.share() : std::shared_future<void>();
    }

    // after runs first on the session thread (waiting for the borrower to release its
    // duplication); then the session is taken back and rebuilt in the background.
    std::shared_future<void> give_back(std::function<void()> after) {
        auto future = executor_.submit([this, after = std::move(after)] {
            try {
                if (after) after();
            } catch (...) {
                lent_ = false;
                throw;
            }
            lent_ = false;
            ensure_primed();
        });
        return future.valid() ? future.share() : std::shared_future<void>();
    }

    void forget_failure() { failed_ = false; }

    // Releases the session in the background and clears the failure record.
    void reset() {
        failed_ = false;
        executor_.submit([this] { close(); });
    }

    // Before exit: closes the session on its thread and waits, then stops the thread.
    // Queued work is not cancelled: a cancelled close would leave the session to be
    // released on another thread at teardown, where it leaks.
    void shutdown() {
        auto closed = executor_.submit([this] { close(); });
        if (closed.valid()) closed.wait();
        executor_.shutdown(false);
    }

private:
    static HdrFrame take(JbHdrFrame &frame) {
        HdrFrame result;
        result.image = QImage(static_cast<int>(frame.width), static_cast<int>(frame.height), QImage::Format_RGB32);
        if (!result.image.isNull())
            std::memcpy(result.image.bits(), frame.bgra, std::min<size_t>(frame.len, static_cast<size_t>(result.image.sizeInBytes())));
        result.infos.assign(frame.infos, frame.infos + frame.info_count);
        jb_hdr_frame_free(&frame);
        return result;
    }

    SerialExecutor executor_;
    // Created and freed only on the session thread; atomic because release() checks it from the capturing thread.
    std::atomic<JbHdrCapture *> capture_ = nullptr;
    std::atomic_bool failed_ = false;
    std::atomic_bool lent_ = false;
};

void log_tone_map_peaks(const HdrFrame &frame) {
    for (const auto &info : frame.infos)
        if (info.has_peak)
            log_debug(T("HDR 自适应色调映射: 显示器 {index} 峰值 {peak} 倍 SDR 白", {{"index", static_cast<qint64>(info.index)}, {"peak", python_fixed(info.tone_map_peak, 2)}}),
                      "CaptureService");
}

// Waits for DWM to compose pending changes first: DXGI returns composed frames, so a
// just-closed window or a just-set capture exclusion only applies at the next
// composition. adaptive darkens the whole screen for HDR content to keep highlights,
// varying with the picture; long screenshots need identical overlaps and use the
// static mapping.
HdrFrame grab_hdr_frame(size_t monitor_index, bool adaptive) {
    DwmFlush();
    auto frame = HdrSession::instance().grab(monitor_index, adaptive);
    log_tone_map_peaks(frame);
    return frame;
}

// Reads back only this region: cropped on the GPU within one display, composed from
// the whole virtual desktop across displays.
QImage grab_region(const QRect &rect, bool adaptive) {
    DwmFlush();
    auto frame = HdrSession::instance().grab_region(rect, adaptive);
    log_tone_map_peaks(frame);
    return frame.image;
}
}

QImage grab_region_hdr(const QRect &rect, bool adaptive) {
    if (!HdrSession::instance().acquire()) throw std::runtime_error("HDR capture session unavailable");
    return grab_region(rect, adaptive);
}

bool hdr_display_active() {
    std::vector<JbHdrMonitor> displays(16);
    size_t count = 0;
    char message[512] = {};
    if (!jb_hdr_displays(displays.data(), displays.size(), &count, message, sizeof message)) {
        log_warning(T("查询显示器 HDR 状态失败，按未开启处理: {error}", {{"error", QString::fromUtf8(message)}}), "CaptureService");
        return false;
    }
    if (count > displays.size()) {
        displays.resize(count);
        jb_hdr_displays(displays.data(), displays.size(), &count, message, sizeof message);
    }
    displays.resize(std::min(count, displays.size()));
    return std::any_of(displays.begin(), displays.end(), [](const JbHdrMonitor &display) { return display.hdr_enabled; });
}

bool uses_hdr_engine(const QString &requested) {
    const auto engine = requested.isEmpty() ? get_tool_settings_manager()->get_capture_engine() : requested;
    return engine == "hdr" || (engine == "auto" && hdr_display_active());
}

void warm_up_hdr_session(const QString &engine) {
    HdrSession::instance().warm_up(engine == "hdr" ? std::function<bool()>() : std::function<bool()>(hdr_display_active));
}

void refresh_hdr_session(const QString &engine) {
    if (engine == "mss") return;
    HdrSession::instance().refresh(engine == "hdr" ? std::function<bool()>() : std::function<bool()>(hdr_display_active));
}

std::shared_future<void> lend_hdr_session(std::function<void()> then) { return HdrSession::instance().lend(std::move(then)); }

std::shared_future<void> return_hdr_session(std::function<void()> after) { return HdrSession::instance().give_back(std::move(after)); }

void shutdown_hdr_session() { HdrSession::instance().shutdown(); }

void apply_capture_engine(const QString &engine) {
    if (engine == "mss") {
        HdrSession::instance().reset();
    } else {
        HdrSession::instance().forget_failure();
        warm_up_hdr_session(engine);
    }
}

CaptureService::CaptureService(const QString &requested) : engine(requested.isEmpty() ? get_tool_settings_manager()->get_capture_engine() : requested) {}

QImage CaptureService::capture_region(const QRect &rect, const std::shared_ptr<SystemCursor> &cursor) {
    if (rect.width() <= 0 || rect.height() <= 0) throw std::invalid_argument("Capture region must have positive dimensions");
    // The adaptive mapping as in the main capture, so both ways give the same picture.
    auto image = with_engine<QImage>([&rect] { return grab_region(rect, true); }, [&rect] { return capture_region_with_mss(rect); });
    draw_cursor(image, rect.x(), rect.y(), cursor);
    return image;
}

std::pair<QImage, QRectF> CaptureService::capture_all_screens(const std::shared_ptr<SystemCursor> &cursor) {
    auto [image, rect] = with_engine<std::pair<QImage, QRectF>>(capture_with_hdr, capture_with_mss);
    draw_cursor(image, static_cast<int>(rect.x()), static_cast<int>(rect.y()), cursor);
    return {image, rect};
}

template <class Result>
Result CaptureService::with_engine(const std::function<Result()> &capture_hdr, const std::function<Result()> &capture_mss) const {
    if (!uses_hdr_engine(engine)) {
        // Under auto with HDR just turned off, a kept session only holds memory.
        HdrSession::instance().release();
        return capture_mss();
    }
    const bool session = HdrSession::instance().acquire();
    if (engine == "hdr") {
        if (!session) throw std::runtime_error("HDR capture session unavailable");
        return capture_hdr();
    }
    if (session) {
        try {
            return capture_hdr();
        } catch (const std::exception &e) {
            log_warning(T("HDR 截图失败，回落 mss: {error}", {{"error", QString::fromUtf8(e.what())}}), "CaptureService");
        }
    }
    return capture_mss();
}

std::pair<QImage, QRectF> CaptureService::capture_with_hdr() {
    // Index 0 is the whole virtual desktop. Sessions are rebuilt after hot plugging or
    // display changes, so the monitors are read every time, never cached.
    const auto monitors = HdrSession::instance().monitors();
    if (monitors.empty()) throw std::runtime_error("HDR capture session unavailable");
    const auto &monitor = monitors.front();
    auto frame = grab_hdr_frame(monitor.index, true);
    return {frame.image, QRectF(monitor.x, monitor.y, monitor.width, monitor.height)};
}

QImage CaptureService::capture_region_with_mss(const QRect &rect) { return grab_gdi(rect.x(), rect.y(), rect.width(), rect.height()); }

std::pair<QImage, QRectF> CaptureService::capture_with_mss() {
    // The virtual desktop: the union of all monitors.
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return {grab_gdi(left, top, width, height), QRectF(left, top, width, height)};
}
}
