#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QTimer>
#include <functional>
#include <optional>

// Automatic scrolling for long screenshots: parks the cursor mid-region, injects one
// wheel notch at a time and captures, stopping at the end or when the mouse moves.
//
// Injections are at least STEP_INTERVAL_MS apart (about four notches a second); each
// step waits for the previous frame to be stitched, and the window waits for the
// picture to settle before capturing. The stitch result says how far each frame moved;
// three notches without movement mean the end.
namespace jietuba {
struct FrameResult;

constexpr int WHEEL_DELTA = 120;
constexpr int STEP_INTERVAL_MS = 240;
// Wait after injecting so the page starts scrolling before waiting for it to settle.
constexpr int SETTLE_MS = 80;
// Interval for checking whether the cursor was moved.
constexpr int POLL_MS = 30;
// Pages sometimes start scrolling a step late; one idle step alone would misread the end.
constexpr int IDLE_STEPS_AT_END = 3;

// True when the user turned off "scroll inactive windows when hovering", so the wheel
// goes to the focused window.
bool wheel_follows_focus();
// Brings the top-level window under this point to the foreground. The capture frame
// is click-through, so this is the captured window.
void activate_window_at(int x, int y);
// Injects one wheel event: vertical positive scrolls up, horizontal positive right,
// as with WM_MOUSEWHEEL / WM_MOUSEHWHEEL.
void inject_wheel(int delta, bool horizontal);

// Where the cursor is read and placed; QCursor by default.
struct CursorAccess {
    std::function<QPoint()> pos;
    std::function<void(const QPoint &)> set_pos;
};

// The rhythm of automatic scrolling: when to scroll and when to stop. Capturing and
// stitching stay in the window.
//
// capture(done) captures a frame, submits it and calls done with the frame index, or
// nullopt when nothing was captured; every stitch result the window receives must be
// passed to on_frame.
class AutoScroller : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(std::optional<int>)>;
    using Capture = std::function<void(Done)>;
    using Inject = std::function<void(int, bool)>;
    explicit AutoScroller(Capture capture, Inject inject = inject_wheel, CursorAccess cursor = {}, QObject *parent = nullptr);
    bool running() const { return running_; }
    // Parks the cursor at center and scrolls: forward is down or right, otherwise up or
    // left; unspecified follows the latest movement, down or right before any.
    void start(const QPoint &center, bool horizontal, std::optional<bool> forward = std::nullopt);
    // Stops scrolling. A step already scrolled but not yet captured is still captured,
    // so the long image matches where the screen stopped.
    void stop(const QString &reason = "user");
    // Captures the pending step now, before exporting the long image.
    void flush();
    // Stops scrolling without capturing the pending step.
    void cancel();
    // A frame's stitch result. Frames not captured by auto scrolling go in too, to track
    // the newest frame's position and movement direction.
    void on_frame(const FrameResult &result);
    // Forgets the frames seen so far, for a new long image.
    void reset();
signals:
    // "end" reached the end, "mouse" the mouse moved, "failed" capture or stitch failed, "user" stopped.
    void stopped(const QString &reason);
private:
    void scroll();
    void capture_frame();
    void on_captured(std::optional<int> index);
    void check_cursor();

    Capture capture_;
    Inject inject_;
    CursorAccess cursor_;
    bool running_ = false;
    bool horizontal_ = false;
    int sign_ = -1;
    int idle_ = 0;
    QElapsedTimer injected_at_;
    std::optional<int> waiting_;
    std::optional<int> last_top_;
    // The latest movement went down or right.
    bool forward_ = true;
    QPoint parked_;
    QTimer settle_timer_;
    QTimer step_timer_;
    QTimer poll_timer_;
};
}
