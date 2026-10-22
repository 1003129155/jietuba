#pragma once
#include "stitch/incremental.h"
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>

// Scrolling long-screenshot window: a click-through frame marking the capture region.
// It captures whenever the region's picture changes (wheel, dragging the scroll bar or
// content, keyboard paging), waiting for the picture to settle first so frames from
// mid-animation are avoided; auto scrolling is optional.
//
//     auto *window = new ScrollCaptureWindow(capture_rect, parent);
//     connect(window, &ScrollCaptureWindow::finished, ...);
//     window->show();
namespace jietuba {
class AutoScroller;
class ChangeWatch;
class FloatingToolbar;
class InputHub;
class ToolSettingsManager;

// One grabbed frame: width x height BGRA pixels.
struct GrabbedFrame {
    QByteArray bgra;
    int width = 0;
    int height = 0;
};

// Live preview: the short side is fixed, the stitched thumbnail keeps its proportions
// and the newest frame is outlined. When the thumbnail does not fit only a section
// shows, moving only when the newest frame's box nears the viewport's edge; clipped
// sides fade out with an arrow.
//
// The window is a strip one screen long in the growth direction and the panel is drawn
// inside it, so growing never resizes the window: a translucent window shows one
// composited frame between a resize and its repaint, with the old picture in the new
// rectangle, and the content jumps.
class PreviewPanel : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPanel(QWidget *parent = nullptr);
    // image is the stitched thumbnail (orientation restored); box the newest frame's
    // [start, end) pixels along its long side.
    void update_preview(const QImage &image, const QString &scroll_direction, const std::optional<std::pair<int, int>> &box = std::nullopt);
    QSize panel_size() const { return panel_rect_.size(); }
    // The panel in screen coordinates.
    QRect panel_geometry() const { return panel_rect_.translated(geometry().topLeft()); }
    // Puts the panel's top-left corner at top_left (screen coordinates). The window
    // only moves when the strip through that point on screen changes.
    void place(const QPoint &top_left, const QRect &screen);
    // Excludes the panel from capture when it overlaps the capture region.
    void set_capture_excluded(bool exclude);
    void show_warning(const QString &message);
    void clear_warning();
    QLabel *warning_label = nullptr;
protected:
    void closeEvent(QCloseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    // Click-through, so the panel never takes wheel events.
    void setup_mouse_transparent();
    void build_ui();
    void place_warning();
    int max_length() const;
    // Viewport start: from the beginning when it fits; otherwise moved only when the
    // newest frame's box nears the edge, just far enough to show it with some margin.
    int follow(int total, int length) const;
    void paint_content(QPainter &painter);
    // A clipped side: fades out, with an arrow pointing outwards.
    void paint_clipped_edge(QPainter &painter, bool at_start);

    int fixed_side_;
    // In window coordinates.
    QRect panel_rect_;
    QImage image_;
    std::optional<std::pair<int, int>> box_;
    bool vertical_ = true;
    int offset_ = 0;
    bool capture_excluded_ = false;
};

class ScrollCaptureWindow : public QWidget {
    Q_OBJECT
public:
    // capture_rect is the capture region in screen coordinates; config_manager is needed for pinning.
    ScrollCaptureWindow(const QRect &capture_rect, QWidget *parent = nullptr, ToolSettingsManager *config_manager = nullptr);

    QRect capture_rect;
    ToolSettingsManager *config_manager;
    // Only counts frames; the images live in the stitcher.
    int screenshots = 0;
    // "vertical" or "horizontal".
    QString scroll_direction = "vertical";
    // In horizontal mode Shift scrolls right; auto-repeat while held counts once.
    bool horizontal_scroll_key_pressed = false;
    // The long image exported on finish or pin (orientation restored).
    QImage stitched_result;
    bool preview_warning_active = false;
    QWidget *transparent_area = nullptr;
    FloatingToolbar *toolbar = nullptr;
    QPointer<PreviewPanel> preview_panel;

signals:
    void finished();
    void cancelled();

protected:
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    // capture_rect is already in real screen coordinates; the window only grows by the
    // border. No single-screen clamping: a region across screens shows across screens.
    QPoint correct_window_position(int border_width) const;
    void setup_window();
    void setup_ui();
    void setup_floating_toolbar();
    // Above the region and centred, falling back below, left and right at screen edges.
    void position_floating_toolbar();
    void setup_preview_panel();
    // Near the region without entering it or the toolbar.
    void position_preview_panel();
    void refresh_preview_panel();
    // By value: show_pending_warning passes *pending_warning_, which this clears first.
    void show_preview_warning(QString message);
    void show_pending_warning();
    void clear_preview_warning();
    // Drops the frame that did not join; the hint waits WARNING_DELAY_MS and does not
    // appear if a frame joins meanwhile.
    void handle_stitch_failure(const QString &hint, bool immediate = false);
    // Click-through window plus the global keys (Shift in horizontal mode).
    void setup_input();
    // The region changed: capture once it settles; auto scrolling decides by itself.
    void on_screen_changed();
    // Starts or stops auto scrolling in the latest movement's direction, down or right before any.
    void toggle_auto_scroll();
    // Stops auto scrolling and captures what is on screen now: the step injected but
    // not captured, the frame waiting to settle, or the last scroll the watch has not
    // noticed yet. Called before export and crop so they match the settled screen.
    void capture_pending_now();
    void on_auto_scroll_stopped(const QString &reason);
    void toggle_direction();
    // Sends one notch of horizontal scrolling to the right.
    void send_horizontal_scroll();
    // Horizontal mode: Shift scrolls one notch right; the watch does the capturing.
    void start_keyboard_listener();
    void stop_keyboard_listener();
    // Shift in horizontal mode, already on the GUI thread.
    void on_key(const QString &watcher, int vk, bool pressed);
    // Hash matching stitches vertically only; horizontal frames are rotated 90 degrees
    // before stitching and back afterwards.
    void reconfigure_stitch_engine();
    // The system rounds window corners into the region's corners (a grey dot per frame,
    // a column of them in the long image), and the preview panel's system shadow
    // reaches into the region; both only change once the native window is shown.
    void drop_system_corners();
    // Raises the window and its floating windows to the top of the TOPMOST order.
    void raise_all_topmost();
    void verify_window_position();
    void force_fix_window_position();
    // The first frame; afterwards any change triggers a capture.
    void capture_initial_screenshot();
    // Excludes or restores capture for UI windows overlapping the region.
    void exclude_overlapping_ui(bool exclude);
    // HDR first; after it fails the rest of this long screenshot uses grabWindow, never
    // switching back: stitching needs identical pixels in overlaps, and the two paths
    // do not give exactly the same values.
    QImage grab_capture_rect();
    std::optional<GrabbedFrame> grab_frame();
    // Hands a frame to the stitcher; returns its index (results go to on_frame_stitched).
    std::optional<int> submit_frame(const std::optional<GrabbedFrame> &frame);
    std::optional<int> do_capture();
    // Captures once the picture settles: two grabs STILL_CHECK_MS apart must match,
    // at most STILL_CHECK_TRIES grabs while it keeps changing. Smooth-scrolling programs
    // often have an area not yet repainted mid-animation, which would show as a row of
    // misaligned text. done gets the frame index (nullopt when nothing was captured);
    // requests while waiting only add callbacks.
    void capture_when_still(std::function<void(std::optional<int>)> done = {});
    void check_still();
    void finish_still(std::optional<int> index);
    // Captures and submits the frame still waiting to settle at once.
    void flush_still();
    void on_frame_stitched(const FrameResult &result);
    // Crops the long image to the current view: "top" removes earlier content, "bottom" later.
    void crop(const QString &side);
    void on_cropped(const FrameResult &result);
    QImage export_result();
    void on_finish();
    // Saved file names follow the UI language, such as "Long Screenshot_Vertical".
    QString file_prefix() const;
    QImage result_qimage();
    // Pins the current result to the desktop, then ends the long screenshot.
    void on_pin();
    void on_cancel();
    void cleanup();

    QImage latest_preview_;
    std::optional<std::pair<int, int>> latest_box_;
    // The panel follows the preview's own direction, not scroll_direction: results already
    // in flight when the direction switches were stitched the old way.
    QString preview_direction_ = "vertical";
    std::optional<QString> pending_warning_;
    QTimer warning_timer_;
    // One log line per run of failures.
    bool stitch_failing_ = false;
    IncrementalStitcher *stitcher_ = nullptr;
    AutoScroller *auto_scroller_ = nullptr;
    ChangeWatch *change_watch_ = nullptr;
    // Fixed by the capture engine setting at the start; see grab_capture_rect.
    bool use_hdr_ = false;
    QTimer still_timer_;
    std::optional<GrabbedFrame> still_frame_;
    int still_tries_ = 0;
    std::vector<std::function<void(std::optional<int>)>> still_callbacks_;
    QTimer position_fix_timer_;
    QPointer<InputHub> input_hub_;
    QMetaObject::Connection key_connection_;
};
}
