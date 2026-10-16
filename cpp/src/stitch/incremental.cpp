#include "stitch/incremental.h"
#include "core/native.h"
#include "core/ui_scale.h"
#include <QTransform>
#include <future>
#include <tuple>

namespace jietuba {
namespace {
// Vertically, matching avoids the fixed title bar at the long image's top: scrolling
// down it is only in the very first frame, so 15% of the frame height is skipped;
// scrolling up the top is the growing end with short overlaps, so only 5%.
// Horizontally the title bar has turned to the side and is not skipped.
constexpr float TOP_RATIO_DOWN = 0.15f;
constexpr float TOP_RATIO_UP = 0.05f;

int heading_code(const std::optional<QString> &heading) {
    if (!heading) return -1;
    if (*heading == "down") return 0;
    if (*heading == "up") return 1;
    return -1;
}

std::optional<QString> last_move(const JbStitchSession *session) {
    switch (jb_stitch_session_last_move(session)) {
    case 0: return QString("down");
    case 1: return QString("up");
    default: return std::nullopt;
    }
}

// Pushes one frame; without a known heading the session's last movement is used,
// and the skipped ratio follows it. -1 failure, 0 no overlap, 1 pushed.
int push_frame(JbStitchSession *session, const QByteArray &data, const QString &direction, std::optional<QString> heading, JbPushResult *step,
               QString *error) {
    if (!heading) heading = last_move(session);
    const float top_ratio = direction == "horizontal" ? 0.0f : (heading && *heading == "up" ? TOP_RATIO_UP : TOP_RATIO_DOWN);
    char message[512] = {};
    const int outcome = jb_stitch_session_push(session, reinterpret_cast<const uint8_t *>(data.constData()), static_cast<size_t>(data.size()), heading_code(heading),
                                               top_ratio, 0.0f, false, step, message, sizeof message);
    if (outcome < 0 && error) *error = QString::fromUtf8(message);
    return outcome;
}

QImage bgra_image(const QByteArray &bgra, int width, int height, QImage::Format format) {
    return QImage(reinterpret_cast<const uchar *>(bgra.constData()), width, height, width * 4, format).copy();
}

// The frame in stitch coordinates: (bgra, width, height).
std::tuple<QByteArray, int, int> oriented(const StitchFrame &frame, const QString &direction) {
    if (direction != "horizontal") return {frame.bgra, frame.width, frame.height};
    const auto image = bgra_image(frame.bgra, frame.width, frame.height, QImage::Format_ARGB32).transformed(QTransform().rotate(90));
    QByteArray data;
    data.reserve(static_cast<qsizetype>(image.width()) * image.height() * 4);
    for (int y = 0; y < image.height(); ++y) data.append(reinterpret_cast<const char *>(image.constScanLine(y)), image.width() * 4);
    return {data, image.width(), image.height()};
}

QString thrown_message(const std::exception &e) {
    const auto message = QString::fromUtf8(e.what());
    return message.isEmpty() ? QString("Exception") : message;
}
}

IncrementalStitcher::IncrementalStitcher(int thumb_side, int ignore_right_pixels, int ignore_top_pixels, QObject *parent)
    : QObject(parent), thumb_side_(thumb_side), ignore_right_pixels_(ignore_right_pixels), ignore_top_pixels_(ignore_top_pixels) {
    qRegisterMetaType<FrameResult>();
}

IncrementalStitcher::~IncrementalStitcher() { close(); }

void IncrementalStitcher::submit(const StitchFrame &frame) {
    executor_.submit([this, frame] { process(frame); });
}

void IncrementalStitcher::crop(const QString &side) {
    executor_.submit([this, side] { crop_task(side); });
}

QImage IncrementalStitcher::export_image() {
    auto future = executor_.submit([this] { return export_task(); });
    if (!future.valid()) return {};
    return future.get();
}

void IncrementalStitcher::close() {
    executor_.shutdown(true);
    release_session();
    first_.reset();
    thumb_.clear();
}

void IncrementalStitcher::release_session() {
    if (session_) {
        jb_stitch_session_close(session_);
        jb_stitch_session_free(session_);
        session_ = nullptr;
    }
}

void IncrementalStitcher::process(const StitchFrame &frame) {
    FrameResult result;
    try {
        result = push(frame);
    } catch (const std::exception &e) {
        result = failure(frame, thrown_message(e));
    }
    emit frame_done(result);
}

void IncrementalStitcher::crop_task(const QString &side) {
    if (!session_) return;
    FrameResult result;
    try {
        result = apply_crop(side);
    } catch (const std::exception &e) {
        result = FrameResult{false, false, 0, 0, 0, QImage(), thrown_message(e)};
    }
    emit crop_done(result);
}

FrameResult IncrementalStitcher::apply_crop(const QString &side) {
    // The thumbnail is cropped too, its rows again derived from the total height after
    // the crop, as in apply_step.
    const int row_bytes = thumb_side_ * 4;
    const int rows = static_cast<int>(thumb_.size() / row_bytes);
    int shift;
    if (side == "top") {
        shift = -static_cast<int>(jb_stitch_session_crop_top(session_));
        top_ = 0;
    } else {
        jb_stitch_session_crop_bottom(session_);
        shift = 0;
    }
    const int height = static_cast<int>(jb_stitch_session_height(session_));
    const int target = static_cast<int>(python_round(height * scale_));
    if (side == "top") thumb_.remove(0, static_cast<qsizetype>(std::max(0, rows - target)) * row_bytes);
    else thumb_.truncate(static_cast<qsizetype>(target) * row_bytes);
    FrameResult result{true, false, 0, static_cast<int>(jb_stitch_session_width(session_)), height, render(thumb_)};
    result.top = top_;
    result.box = box(top_);
    result.shift = shift;
    result.size = size();
    return result;
}

FrameResult IncrementalStitcher::push(const StitchFrame &frame) {
    if (!first_) {
        first_ = frame;
        const auto preview = first_preview(frame);
        const int long_side = frame.scroll_direction == "horizontal" ? preview.width() : preview.height();
        FrameResult result{true, true, frame.index, frame.width, frame.height, preview};
        result.box = std::make_pair(0, long_side);
        result.size = std::make_pair(frame.width, frame.height);
        return result;
    }
    if (!session_) return start(frame);
    return append(frame);
}

FrameResult IncrementalStitcher::start(const StitchFrame &frame) {
    const auto direction = frame.scroll_direction;
    const auto [first, width, height] = oriented(*first_, direction);
    const auto [second, width2, height2] = oriented(frame, direction);
    if (width2 != width || height2 != height) return failure(frame, QString("frame size %1x%2 differs from %3x%4").arg(width2).arg(height2).arg(width).arg(height));

    char message[512] = {};
    auto *session = jb_stitch_session_new(static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(ignore_right_pixels_),
                                          static_cast<uint32_t>(ignore_top_pixels_), 0.01f, message, sizeof message);
    if (!session) return failure(frame, QString::fromUtf8(message));
    JbPushResult ignored{};
    QString error;
    if (jb_stitch_session_push(session, reinterpret_cast<const uint8_t *>(first.constData()), static_cast<size_t>(first.size()), -1, 0.0f, 0.0f, false, &ignored,
                               message, sizeof message) < 0) {
        jb_stitch_session_free(session);
        return failure(frame, QString::fromUtf8(message));
    }
    JbPushResult step{};
    const int outcome = push_frame(session, second, direction, frame.heading, &step, &error);
    if (outcome <= 0) {
        jb_stitch_session_close(session);
        jb_stitch_session_free(session);
        return failure(frame, outcome < 0 ? std::optional<QString>(error) : std::nullopt);
    }

    session_ = session;
    direction_ = direction;
    scale_ = static_cast<double>(thumb_side_) / width;
    frame_rows_ = height;
    thumb_ = thumbnail(first, width, height);
    apply_step(step, thumbnail(second, width, height));
    return success(frame, step);
}

FrameResult IncrementalStitcher::append(const StitchFrame &frame) {
    const auto [data, width, height] = oriented(frame, direction_);
    JbPushResult step{};
    QString error;
    const int outcome = push_frame(session_, data, direction_, frame.heading, &step, &error);
    if (outcome <= 0) return failure(frame, outcome < 0 ? std::optional<QString>(error) : std::nullopt);
    apply_step(step, thumbnail(data, width, height));
    return success(frame, step);
}

QImage IncrementalStitcher::export_task() const {
    if (!session_) {
        if (!first_) return {};
        return bgra_image(first_->bgra, first_->width, first_->height, QImage::Format_RGB32);
    }
    const int width = static_cast<int>(jb_stitch_session_width(session_));
    const int height = static_cast<int>(jb_stitch_session_height(session_));
    // 32-bit rows carry no padding, so the session writes straight into the image.
    QImage image(width, height, QImage::Format_ARGB32);
    if (image.isNull() || !jb_stitch_session_export(session_, image.bits(), static_cast<size_t>(image.sizeInBytes()))) return {};
    if (direction_ == "horizontal") image = image.transformed(QTransform().rotate(-90));
    return image;
}

QByteArray IncrementalStitcher::thumbnail(const QByteArray &bgra, int width, int height) const {
    const int side = thumb_side_;
    const int thumb_height = std::max<int>(1, static_cast<int>(python_round(static_cast<double>(height) * side / width)));
    const QImage source(reinterpret_cast<const uchar *>(bgra.constData()), width, height, width * 4, QImage::Format_RGB32);
    const auto thumb = source.scaled(side, thumb_height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(side) * thumb.height() * 4);
    for (int y = 0; y < thumb.height(); ++y) bytes.append(reinterpret_cast<const char *>(thumb.constScanLine(y)), side * 4);
    return bytes;
}

void IncrementalStitcher::apply_step(const JbPushResult &step, const QByteArray &frame_thumb) {
    const int row_bytes = thumb_side_ * 4;
    const int rows = static_cast<int>(thumb_.size() / row_bytes);
    const int target = static_cast<int>(python_round(static_cast<double>(step.height) * scale_));
    if (step.head_cut || step.head_rows) {
        const int cut = std::min(static_cast<int>(python_round(static_cast<double>(step.head_cut) * scale_)), rows);
        const int added = target - (rows - cut);
        thumb_ = frame_thumb.left(static_cast<qsizetype>(std::max(0, added)) * row_bytes) + thumb_.mid(static_cast<qsizetype>(cut) * row_bytes);
        return;
    }
    const int kept = std::min(static_cast<int>(python_round(static_cast<double>(step.keep) * scale_)), rows);
    const int added = target - kept;
    thumb_.truncate(static_cast<qsizetype>(kept) * row_bytes);
    // frame_thumb[-added * row_bytes:] clamps to the whole buffer when it is shorter.
    if (added > 0) thumb_ += frame_thumb.right(static_cast<qsizetype>(added) * row_bytes);
}

QImage IncrementalStitcher::first_preview(const StitchFrame &frame) const {
    const QImage source(reinterpret_cast<const uchar *>(frame.bgra.constData()), frame.width, frame.height, frame.width * 4, QImage::Format_RGB32);
    const auto thumb = frame.scroll_direction == "horizontal" ? source.scaledToHeight(thumb_side_, Qt::SmoothTransformation)
                                                              : source.scaledToWidth(thumb_side_, Qt::SmoothTransformation);
    return thumb.convertToFormat(QImage::Format_RGB32);
}

QImage IncrementalStitcher::render(const QByteArray &thumb) const {
    const int side = thumb_side_;
    const int rows = static_cast<int>(thumb.size() / (side * 4));
    auto image = QImage(reinterpret_cast<const uchar *>(thumb.constData()), side, rows, side * 4, QImage::Format_RGB32).copy();
    if (direction_ == "horizontal") image = image.transformed(QTransform().rotate(-90));
    return image;
}

std::pair<int, int> IncrementalStitcher::box(int top) const {
    const int rows = static_cast<int>(thumb_.size() / (thumb_side_ * 4));
    const int start = std::min<int>(rows, static_cast<int>(python_round(top * scale_)));
    const int end = std::min<int>(rows, static_cast<int>(python_round((top + frame_rows_) * scale_)));
    return {start, end};
}

FrameResult IncrementalStitcher::success(const StitchFrame &frame, const JbPushResult &step) {
    top_ = static_cast<int>(step.top);
    FrameResult result{true, false, frame.index, static_cast<int>(jb_stitch_session_width(session_)), static_cast<int>(jb_stitch_session_height(session_)),
                       render(thumb_)};
    result.top = top_;
    result.box = box(top_);
    result.shift = static_cast<int>(step.head_rows) - static_cast<int>(step.head_cut);
    result.size = size();
    return result;
}

std::pair<int, int> IncrementalStitcher::size() const {
    const int width = static_cast<int>(jb_stitch_session_width(session_));
    const int height = static_cast<int>(jb_stitch_session_height(session_));
    return direction_ == "horizontal" ? std::make_pair(height, width) : std::make_pair(width, height);
}

FrameResult IncrementalStitcher::failure(const StitchFrame &frame, const std::optional<QString> &error) {
    FrameResult result{false, false, frame.index, 0, 0, QImage()};
    result.error = error;
    return result;
}
}
