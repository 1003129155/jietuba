#pragma once
#include "gif/frame_recorder.h"
#include "gif/gifrecorder.h"
#include <QRect>
#include <QTimer>
#include <QWidget>
#include <optional>
#include <utility>
#include <vector>

class QPainter;
class QSvgRenderer;

// The cursor overlay for playback, a separate transparent window. While recording,
// FrameRecorder samples the position, buttons and wheel into FrameData::cursor; during
// playback this draws them per frame: the mouse icon; a left or right half firework
// for button presses (a three-frame spreading animation, held while pressed); three
// stacked theme-coloured arrows beside the cursor for scrolling. Export rasterises
// all sprites once (rasterize_cursor_sprites) and lets the native side blend them.
namespace jietuba {
// Firework animation frames: three, each with longer rays.
constexpr int BURST_FRAMES = 3;

class CursorOverlay : public QWidget {
    Q_OBJECT
public:
    static constexpr int CURSOR_SIZE = 24;
    static constexpr int BURST_R_START = 10;
    static constexpr int BURST_R_STEP = 9;
    static constexpr int DOT_R = 6;
    explicit CursorOverlay(QWidget *parent = nullptr);
    // Follows the recording / playback region.
    void update_rect(const QRect &rect) { setGeometry(rect); }
    // The user's cursor display switch.
    void set_cursor_visible(bool visible);
    bool is_cursor_visible() const { return visible_flag_; }
    // The current frame's mouse state; repaints.
    void set_frame_cursor(const std::optional<CursorSnapshot> &cursor);
    // Shared by every overlay; loaded once.
    static QSvgRenderer *svg_renderer();
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    // Advances one frame every 40 ms.
    void on_burst_tick();
    // Half a firework split along the cursor's top-left to bottom-right diagonal:
    // left button the upper-left half, right button the lower-right half; later
    // frames reach further and fade.
    void draw_burst(QPainter &p, int x, int y, int side, int frame) const;
    // Three stacked arrows right of the cursor, more opaque towards the motion.
    void draw_scroll_arrows(QPainter &p, int x, int y, int scroll) const;

    std::optional<CursorSnapshot> cursor_;
    bool visible_flag_ = true;
    // 0 hidden, 1-3 the current frame.
    int burst_frame_ = 0;
    // 1 left button, 2 right button.
    int burst_side_ = 0;
    QTimer burst_timer_;
};

// Rasterises every cursor sprite once for GIF export; scale is export size over
// recording size (for example 640/1280 = 0.5).
gifrecorder::CursorSprites rasterize_cursor_sprites(double scale = 1.0);

// Each frame's animation state (burst_frame 0 none or 1-3, burst_side 1 left or 2 right),
// walking the frame sequence.
std::vector<std::pair<int, int>> compute_burst_states(const std::vector<std::optional<CursorSnapshot>> &cursors);
}
