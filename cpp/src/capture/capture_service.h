#pragma once
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QString>
#include <functional>
#include <future>
#include <memory>
#include <utility>

namespace jietuba {
class SystemCursor;

// Grabs a region of the virtual desktop (physical pixels) with the HDR session.
// Throws std::runtime_error when no session is available.
QImage grab_region_hdr(const QRect &rect, bool adaptive = false);
// Whether any display has HDR (Windows advanced colour) on; cheap, callable before every capture.
bool hdr_display_active();
// auto uses HDR only while a display has HDR on: on SDR screens both engines give
// identical pixels, and a resident session loads the GPU driver into the process.
bool uses_hdr_engine(const QString &engine = {});
// Builds the HDR session in the background so the first capture need not wait.
void warm_up_hdr_session(const QString &engine = "auto");
// After display configuration changes: rebuild per the new configuration, or release.
void refresh_hdr_session(const QString &engine);
// When the GIF recording window opens: closes the capture session in the background to
// free the duplication for the recording thread; then runs after it is closed. While
// lent, captures get no HDR session and fall back or fail per the engine setting.
std::shared_future<void> lend_hdr_session(std::function<void()> then = {});
// When the recording window closes: after stops the recording threads, then the capture
// session is rebuilt in the background.
std::shared_future<void> return_hdr_session(std::function<void()> after = {});
// Before the application exits.
void shutdown_hdr_session();
// After the capture engine setting is saved; never blocks.
void apply_capture_engine(const QString &engine);

// Multi-monitor virtual desktop capture. On HDR displays GDI BitBlt clips content
// above desktop white to pure white; the HDR engine tone-maps on the GPU.
// auto: HDR when a display has HDR on, else GDI, falling back to GDI when HDR fails;
// mss / hdr: only that engine, throwing on failure.
class CaptureService {
public:
    explicit CaptureService(const QString &engine = {});
    // A region of the virtual desktop in physical pixels; the origin may be negative.
    QImage capture_region(const QRect &rect, const std::shared_ptr<SystemCursor> &cursor = nullptr);
    // The whole virtual desktop and its rect; draws the pointer when given.
    std::pair<QImage, QRectF> capture_all_screens(const std::shared_ptr<SystemCursor> &cursor = nullptr);
    QString engine;
private:
    template <class Result>
    Result with_engine(const std::function<Result()> &capture_hdr, const std::function<Result()> &capture_mss) const;
    static std::pair<QImage, QRectF> capture_with_hdr();
    static QImage capture_region_with_mss(const QRect &rect);
    static std::pair<QImage, QRectF> capture_with_mss();
};
}
