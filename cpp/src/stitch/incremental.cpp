#include "stitch/incremental.h"
#include "core/native.h"
#include "core/ui_scale.h"
#include <QTransform>
#include <QPainter>
#include <future>
#include <cstring>
#include <stdexcept>
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
    if (frame.width <= 0 || frame.height <= 0 || frame.bgra.size() != qsizetype(frame.width) * frame.height * 4)
        throw std::invalid_argument("invalid capture pixels");
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

void IncrementalStitcher::restart() {
    executor_.submit([this] {
        release_session();
        first_.reset();
        thumb_.clear();
        prefix_ = suffix_ = QImage();
        last_view_.clear();
        extension_.reset();
        ++revision_;
    });
}

void IncrementalStitcher::prepare_extension(const StitchFrame &frame, quint64 request) {
    executor_.submit([this, frame, request] {
        extension_.reset();
        bool ok = false;
        try { ok = prepare_extension_task(frame, request); } catch (const std::exception &) {}
        emit extension_ready(request, ok);
    });
}

bool IncrementalStitcher::prepare_extension_task(const StitchFrame &frame, quint64 request) {
    if (!first_ || frame.scroll_direction != direction_ || frame.leading < 0 || frame.trailing < 0) return false;
    const auto [view, width, rows] = oriented(frame, direction_);
    if (width != core_width_ || rows != frame_rows_ + frame.leading + frame.trailing || last_view_.isEmpty()) return false;
    const int head = frame.leading - leading_, tail = frame.trailing - trailing_;
    // Only one outward edge, at the actual end of the current view in the result.
    if (!((head > 0 && tail == 0 && top_ - leading_ == -prefix_.height()) ||
          (tail > 0 && head == 0 && top_ + frame_rows_ + trailing_ == core_height() + suffix_.height()))) return false;
    const qsizetype offset = qsizetype(head) * width * 4;
    if (offset < 0 || offset + last_view_.size() > view.size() ||
        std::memcmp(view.constData() + offset, last_view_.constData(), size_t(last_view_.size())) != 0) return false;
    const QImage image(reinterpret_cast<const uchar *>(view.constData()), width, rows, width * 4, QImage::Format_RGB32);
    auto prefix = prefix_, suffix = suffix_;
    if (head) prefix = image.copy(0, 0, width, prefix.height() + head);
    if (tail) suffix = image.copy(0, rows - suffix.height() - tail, width, suffix.height() + tail);
    if ((head && prefix.isNull()) || (tail && suffix.isNull())) return false;
    extension_ = Extension{request, revision_, view, frame.leading, frame.trailing, prefix, suffix};
    return true;
}

FrameResult IncrementalStitcher::commit_extension(quint64 request, quint64 generation) {
    auto future = executor_.submit([this, request, generation] {
        if (!extension_ || extension_->request != request || extension_->revision != revision_) return FrameResult{};
        prefix_ = std::move(extension_->prefix);
        suffix_ = std::move(extension_->suffix);
        last_view_ = std::move(extension_->view);
        leading_ = extension_->leading;
        trailing_ = extension_->trailing;
        extension_.reset();
        generation_ = generation;
        ++revision_;
        return decorate(FrameResult{true, false, last_index_, core_width_, core_height(), {}});
    });
    return future.valid() ? future.get() : FrameResult{};
}

void IncrementalStitcher::discard_extension(quint64 request) {
    executor_.submit([this, request] { if (extension_ && extension_->request == request) extension_.reset(); });
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
    prefix_ = suffix_ = QImage();
    last_view_.clear();
    extension_.reset();
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
        result.generation = generation_;
    }
    emit crop_done(result);
}

FrameResult IncrementalStitcher::apply_crop(const QString &side) {
    // The thumbnail is cropped too, its rows again derived from the total height after
    // the crop, as in apply_step.
    const int row_bytes = thumb_side_ * 4;
    const int rows = static_cast<int>(thumb_.size() / row_bytes);
    int shift;
    const int view_rows = frame_rows_ + leading_ + trailing_;
    const QImage view(reinterpret_cast<const uchar *>(last_view_.constData()), core_width_, view_rows, core_width_ * 4, QImage::Format_RGB32);
    if (side == "top") {
        shift = -static_cast<int>(jb_stitch_session_crop_top(session_));
        top_ = 0;
        prefix_ = leading_ ? view.copy(0, 0, core_width_, leading_) : QImage();
    } else {
        jb_stitch_session_crop_bottom(session_);
        shift = 0;
        suffix_ = trailing_ ? view.copy(0, view_rows - trailing_, core_width_, trailing_) : QImage();
    }
    const int height = static_cast<int>(jb_stitch_session_height(session_));
    const int target = static_cast<int>(python_round(height * scale_));
    if (side == "top") thumb_.remove(0, static_cast<qsizetype>(std::max(0, rows - target)) * row_bytes);
    else thumb_.truncate(static_cast<qsizetype>(target) * row_bytes);
    ++revision_;
    extension_.reset();
    auto result = decorate(FrameResult{true, false, 0, core_width_, height, {}});
    result.shift = shift;
    return result;
}

FrameResult IncrementalStitcher::push(const StitchFrame &frame) {
    const auto direction = session_ ? direction_ : frame.scroll_direction;
    const auto [view, width, rows] = oriented(frame, direction);
    const int height = rows - frame.leading - frame.trailing;
    if (frame.leading < 0 || frame.trailing < 0 || height <= 0) return failure(frame, QString("invalid capture margins"));
    const auto data = view.mid(qsizetype(frame.leading) * width * 4, qsizetype(height) * width * 4);
    const int old_height = first_ ? core_height() : 0;
    FrameResult result;
    if (!first_) {
        first_ = frame;
        direction_ = direction;
        core_width_ = width;
        frame_rows_ = height;
        top_ = 0;
        scale_ = double(thumb_side_) / width;
        thumb_ = thumbnail(data, width, height);
        result = FrameResult{true, true, frame.index, frame.width, frame.height, {}};
    } else {
        result = session_ ? append(frame, data, width, height) : start(frame, data, width, height);
    }
    if (!result.ok) return result;
    update_edges(frame, view, old_height, result.shift);
    return decorate(result);
}

FrameResult IncrementalStitcher::start(const StitchFrame &frame, const QByteArray &second, int width2, int height2) {
    const auto direction = frame.scroll_direction;
    const auto [first_view, width, rows] = oriented(*first_, direction);
    const int height = rows - first_->leading - first_->trailing;
    const auto first = first_view.mid(qsizetype(first_->leading) * width * 4, qsizetype(height) * width * 4);
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
    core_width_ = width;
    frame_rows_ = height;
    thumb_ = thumbnail(first, width, height);
    apply_step(step, thumbnail(second, width, height));
    return success(frame, step);
}

FrameResult IncrementalStitcher::append(const StitchFrame &frame, const QByteArray &data, int width, int height) {
    if (width != core_width_ || height != frame_rows_) return failure(frame, QString("fixed capture size changed"));
    JbPushResult step{};
    QString error;
    const int outcome = push_frame(session_, data, direction_, frame.heading, &step, &error);
    if (outcome <= 0) return failure(frame, outcome < 0 ? std::optional<QString>(error) : std::nullopt);
    apply_step(step, thumbnail(data, width, height));
    return success(frame, step);
}

QImage IncrementalStitcher::export_task() const {
    if (!first_) return {};
    const int height = core_height(), prefix = prefix_.height(), suffix = suffix_.height();
    QImage image(core_width_, prefix + height + suffix, QImage::Format_ARGB32);
    if (image.isNull()) return {};
    const qsizetype stride = qsizetype(core_width_) * 4;
    if (prefix) std::memcpy(image.bits(), prefix_.constBits(), size_t(stride * prefix));
    // Write directly into the final buffer; no second full-size long image is needed.
    if (session_) {
        if (!jb_stitch_session_export(session_, image.scanLine(prefix), size_t(stride * height))) return {};
    } else {
        const auto [view, width, rows] = oriented(*first_, direction_);
        std::memcpy(image.scanLine(prefix), view.constData() + first_->leading * stride, size_t(stride * height));
    }
    if (suffix) std::memcpy(image.scanLine(prefix + height), suffix_.constBits(), size_t(stride * suffix));
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

QImage IncrementalStitcher::render(const QByteArray &thumb) const {
    const int side = thumb_side_;
    const int rows = static_cast<int>(thumb.size() / (side * 4));
    const QImage core(reinterpret_cast<const uchar *>(thumb.constData()), side, rows, side * 4, QImage::Format_RGB32);
    QImage image;
    if (prefix_.isNull() && suffix_.isNull()) image = core.copy();
    else {
        const int head = int(python_round(prefix_.height() * scale_));
        const int end = int(python_round((prefix_.height() + core_height()) * scale_));
        const int total = int(python_round((prefix_.height() + core_height() + suffix_.height()) * scale_));
        image = QImage(side, total, QImage::Format_RGB32);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (head) painter.drawImage(QRect(0, 0, side, head), prefix_);
        painter.drawImage(QRect(0, head, side, end - head), core);
        if (total > end) painter.drawImage(QRect(0, end, side, total - end), suffix_);
    }
    if (direction_ == "horizontal") image = image.transformed(QTransform().rotate(-90));
    return image;
}

std::pair<int, int> IncrementalStitcher::box(int top) const {
    const int rows = int(python_round((prefix_.height() + core_height() + suffix_.height()) * scale_));
    const int start = std::clamp(int(python_round((prefix_.height() + top - leading_) * scale_)), 0, rows);
    const int end = std::clamp(int(python_round((prefix_.height() + top + frame_rows_ + trailing_) * scale_)), start, rows);
    return {start, end};
}

FrameResult IncrementalStitcher::success(const StitchFrame &frame, const JbPushResult &step) {
    top_ = static_cast<int>(step.top);
    FrameResult result{true, false, frame.index, static_cast<int>(jb_stitch_session_width(session_)), static_cast<int>(jb_stitch_session_height(session_)), {}};
    result.top = top_;
    result.shift = static_cast<int>(step.head_rows) - static_cast<int>(step.head_cut);
    return result;
}

int IncrementalStitcher::core_height() const { return session_ ? int(jb_stitch_session_height(session_)) : frame_rows_; }

FrameResult IncrementalStitcher::decorate(FrameResult result) const {
    result.preview = render(thumb_);
    result.top = top_;
    result.box = box(top_);
    result.size = size();
    result.scroll_direction = direction_;
    result.can_extend_start = top_ - leading_ == -prefix_.height();
    result.can_extend_end = top_ + frame_rows_ + trailing_ == core_height() + suffix_.height();
    result.generation = generation_;
    return result;
}

void IncrementalStitcher::update_edges(const StitchFrame &frame, const QByteArray &view, int old_height, int shift) {
    // Native growth consumes previously supplemented pixels exactly once.
    const int head = std::max(0, prefix_.height() - shift);
    if (head != prefix_.height()) prefix_ = head ? prefix_.copy(0, 0, core_width_, head) : QImage();
    const int tail = std::max(0, suffix_.height() - (core_height() - old_height - shift));
    if (tail != suffix_.height()) suffix_ = tail ? suffix_.copy(0, suffix_.height() - tail, core_width_, tail) : QImage();
    const int rows = frame_rows_ + frame.leading + frame.trailing;
    const QImage image(reinterpret_cast<const uchar *>(view.constData()), core_width_, rows, core_width_ * 4, QImage::Format_RGB32);
    const int exposed_head = std::max(0, frame.leading - top_);
    const int exposed_tail = std::max(0, top_ + frame_rows_ + frame.trailing - core_height());
    const auto merge_strip = [&](QImage &strip, int count, bool at_start) {
        if (!count) return;
        const int source = at_start ? 0 : rows - count;
        if (count > strip.height()) strip = image.copy(0, source, core_width_, count);
        else {
            // A view near an end can cover only part of the old strip. Refresh that
            // overlap without discarding the pixels further outside the current view.
            const int target = at_start ? strip.height() - count : 0;
            const size_t bytes = size_t(core_width_) * count * 4;
            if (std::memcmp(strip.constScanLine(target), image.constScanLine(source), bytes) != 0)
                std::memcpy(strip.scanLine(target), image.constScanLine(source), bytes);
        }
    };
    merge_strip(prefix_, exposed_head, true);
    merge_strip(suffix_, exposed_tail, false);
    last_view_ = view;
    leading_ = frame.leading;
    trailing_ = frame.trailing;
    last_index_ = frame.index;
    generation_ = frame.generation;
    ++revision_;
}

std::pair<int, int> IncrementalStitcher::size() const {
    const int width = core_width_;
    const int height = core_height() + prefix_.height() + suffix_.height();
    return direction_ == "horizontal" ? std::make_pair(height, width) : std::make_pair(width, height);
}

FrameResult IncrementalStitcher::failure(const StitchFrame &frame, const std::optional<QString> &error) {
    FrameResult result{false, false, frame.index, 0, 0, QImage()};
    result.error = error;
    result.generation = frame.generation;
    return result;
}
}
