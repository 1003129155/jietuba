#pragma once
#include <QByteArray>
#include <QImage>
#include <QMetaType>
#include "core/serial_executor.h"
#include <QObject>
#include <optional>
#include <utility>

struct JbStitchSession;
struct JbPushResult;

// Incremental long-screenshot stitching.
//
// The UI thread only captures and submits frames; stitching runs on one background
// thread in submission order and conclusions come back through signals. The stitched
// image stays in the native session; each frame reports only the stitch parameters
// and its position, and the preview thumbnail is maintained here from them. A frame
// inside the stitched content leaves the result unchanged; one extending past either
// end grows that end, so a capture started mid-page grows both ways.
//
// Vertical mode stitches in the page's own direction; horizontal mode rotates frames
// 90 degrees clockwise, stitches vertically and rotates back on export. The first
// frame is pushed only when the second arrives, since the mode may change in between.
namespace jietuba {
// One capture: width x height BGRA pixels. heading is the scroll direction before
// this frame in stitch coordinates ("down" is down or right, "up" is up or left);
// unknown when the window captures on content change without knowing the scroll.
struct StitchFrame {
    QByteArray bgra;
    int width = 0;
    int height = 0;
    int index = 0;
    QString scroll_direction;
    std::optional<QString> heading;
};

// A frame's stitch conclusion. width/height/top are in stitch coordinates (orientation
// not restored); shift is how far this push moved existing content down (positive when
// extending the start), to add to the previous frame's top before comparing with top;
// box is the frame's [start, end) pixels along the preview's long side and size the
// long image's (width, height), both with orientation restored.
struct FrameResult {
    bool ok = false;
    bool first = false;
    int index = 0;
    int width = 0;
    int height = 0;
    QImage preview;
    std::optional<QString> error;
    int top = 0;
    std::optional<std::pair<int, int>> box;
    int shift = 0;
    std::optional<std::pair<int, int>> size;
    // The direction the preview was made for: the stitch's, or the first frame's until
    // the second frame fixes it.
    QString scroll_direction;
};

// Stitches long-screenshot frames in order in the background. frame_done and crop_done
// are emitted on the worker thread; connect them queued.
class IncrementalStitcher : public QObject {
    Q_OBJECT
public:
    IncrementalStitcher(int thumb_side, int ignore_right_pixels, int ignore_top_pixels, QObject *parent = nullptr);
    ~IncrementalStitcher() override;
    void submit(const StitchFrame &frame);
    // Crops the long image to the newest frame: side "top" removes the content above
    // it, "bottom" the content below. Runs after the frames already submitted and
    // reports through crop_done; with only the first frame there is nothing to crop.
    void crop(const QString &side);
    // Drops and frees the long image so far, after the frames already submitted; the
    // next frame starts a new one in its own direction.
    void restart();
    // Waits for submitted frames, then returns the whole long image with orientation
    // restored; null before any frame.
    QImage export_image();
    // Drops unprocessed frames and releases the stitched result.
    void close();
signals:
    void frame_done(const jietuba::FrameResult &result);
    // The cropped result, index 0.
    void crop_done(const jietuba::FrameResult &result);

private:
    // Everything below runs on the worker thread (close touches state only after it stops).
    void process(const StitchFrame &frame);
    void crop_task(const QString &side);
    FrameResult apply_crop(const QString &side);
    FrameResult push(const StitchFrame &frame);
    FrameResult start(const StitchFrame &frame);
    FrameResult append(const StitchFrame &frame);
    QImage export_task() const;
    QByteArray thumbnail(const QByteArray &bgra, int width, int height) const;
    // The thumbnail follows the result: extending the start drops its top and adds the
    // new frame's upper part, extending the end cuts the tail and adds the frame's lower
    // part. Rows come from the total height after the push, not per frame: rounding
    // each frame separately would accumulate errors.
    void apply_step(const ::JbPushResult &step, const QByteArray &frame_thumb);
    // The first frame's orientation is not settled yet, so the original is scaled
    // directly; the horizontal panel aligns by height and scales by height.
    QImage first_preview(const StitchFrame &frame) const;
    QImage render(const QByteArray &thumb) const;
    // The frame's range along the preview's long side; the horizontal preview turns
    // back counter-clockwise from stitch coordinates, so rows become x.
    std::pair<int, int> box(int top) const;
    FrameResult success(const StitchFrame &frame, const ::JbPushResult &step);
    std::pair<int, int> size() const;
    static FrameResult failure(const StitchFrame &frame, const std::optional<QString> &error = std::nullopt);
    void release_session();

    int thumb_side_;
    int ignore_right_pixels_;
    int ignore_top_pixels_;
    SerialExecutor executor_;
    std::optional<StitchFrame> first_;
    JbStitchSession *session_ = nullptr;
    QString direction_ = "vertical";
    double scale_ = 1.0;
    int frame_rows_ = 0;
    int top_ = 0;
    QByteArray thumb_;
};
}

Q_DECLARE_METATYPE(jietuba::FrameResult)
