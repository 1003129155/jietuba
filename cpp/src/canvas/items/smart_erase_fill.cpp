#include "canvas/items/smart_erase_fill.h"
#include "canvas/smart_erase.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>
#include <QTransform>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace jietuba {
namespace {
constexpr int FRAME_MS = 33;
std::atomic_int runs{0};

std::optional<SmartErasePatch> moved(std::optional<SmartErasePatch> patch, const QPointF &offset) {
    if (patch) patch->scene_rect.translate(offset);
    return patch;
}
}

std::optional<SmartErasePatch> compute_smart_erase(const SmartEraseInput &input, const std::atomic_bool *cancelled) {
    if (input.source.isNull() || input.source_rect.isEmpty() || input.shape.isEmpty()) return std::nullopt;
    const QTransform to_pixels = QTransform::fromTranslate(-input.source_rect.x(), -input.source_rect.y())
                                 * QTransform::fromScale(input.source.width() / input.source_rect.width(), input.source.height() / input.source_rect.height());
    const QPainterPath area = to_pixels.map(input.shape);
    const QRect rect = area.boundingRect().toAlignedRect().adjusted(-1, -1, 1, 1).intersected(input.source.rect());
    if (rect.isEmpty()) return std::nullopt;
    // Any coverage counts: antialiased edge pixels carry the erased content's fringe.
    QImage mask(rect.size(), QImage::Format_Grayscale8);
    if (mask.isNull()) return std::nullopt;
    mask.fill(0);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(-rect.topLeft());
        painter.fillPath(area, Qt::white);
    }
    auto result = smart_erase(input.source, rect, mask, cancelled);
    if (result.image.isNull()) return std::nullopt;
    return SmartErasePatch{std::move(result.image), to_pixels.inverted().mapRect(QRectF(rect))};
}

int smart_erase_runs() { return runs.load(); }

// One reconstruction, shared by the fills showing it: whichever thread claims it first computes, the
// others wait. Listeners are touched on the UI thread only; it is cancelled once none is left.
class SmartEraseJob : public std::enable_shared_from_this<SmartEraseJob> {
public:
    explicit SmartEraseJob(SmartEraseInput input) : input_(std::move(input)) { clock_.start(); }
    void run_in_pool() {
        if (!claimed_.exchange(true)) execute();
    }
    void run_or_wait() {
        if (!claimed_.exchange(true)) {
            execute();
            return;
        }
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return finished_; });
    }
    std::optional<SmartErasePatch> result() const {
        std::lock_guard lock(mutex_);
        return patch_;
    }
    qint64 elapsed_ms() const { return clock_.elapsed(); }
    void listen(SmartEraseFill *fill) { listeners_.push_back(fill); }
    void forget(SmartEraseFill *fill) {
        listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [fill](const QPointer<SmartEraseFill> &f) { return !f || f == fill; }),
                         listeners_.end());
        if (listeners_.empty()) cancelled_ = true;
    }

private:
    void execute() {
        ++runs;
        std::optional<SmartErasePatch> patch;
        // An exception leaving a pool thread would end the process; a failed solve leaves no patch.
        try {
            patch = compute_smart_erase(input_, &cancelled_);
        } catch (...) {
        }
        // The screenshot it read may be gone with its window otherwise.
        input_ = SmartEraseInput();
        {
            std::lock_guard lock(mutex_);
            patch_ = std::move(patch);
            finished_ = true;
        }
        done_.notify_all();
        if (auto *app = QCoreApplication::instance())
            QMetaObject::invokeMethod(app, [self = shared_from_this()] { self->deliver(); }, Qt::QueuedConnection);
    }
    void deliver() {
        const auto listeners = listeners_;
        for (const auto &fill : listeners)
            if (fill) fill->finish(shared_from_this());
    }
    SmartEraseInput input_;
    QElapsedTimer clock_;
    std::atomic_bool claimed_{false};
    std::atomic_bool cancelled_{false};
    mutable std::mutex mutex_;
    std::condition_variable done_;
    bool finished_ = false;
    std::optional<SmartErasePatch> patch_;
    std::vector<QPointer<SmartEraseFill>> listeners_;
};

SmartEraseFill::SmartEraseFill(InputProvider input, std::function<void()> changed) : input_(std::move(input)), changed_(std::move(changed)) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &SmartEraseFill::start);
    connect(&frames_, &QTimer::timeout, this, [this] {
        frames_.setInterval(FRAME_MS);
        if (changed_) changed_();
    });
}

SmartEraseFill::~SmartEraseFill() { cancel(); }

void SmartEraseFill::notify() {
    if (changed_) changed_();
    emit state_changed();
}

void SmartEraseFill::invalidate(int delay_ms) {
    cancel();
    patch_.reset();
    state_ = State::Stale;
    if (delay_ms >= 0) timer_.start(delay_ms);
    notify();
}

void SmartEraseFill::cancel() {
    timer_.stop();
    frames_.stop();
    if (job_) job_->forget(this);
    job_.reset();
    offset_ = QPointF();
    if (state_ == State::Running) state_ = State::Stale;
}

bool SmartEraseFill::start_now() {
    if (state_ == State::Stale) start();
    return state_ == State::Running;
}

void SmartEraseFill::compute_now() {
    if (state_ == State::Ready) return;
    auto job = job_;
    if (!job) {
        cancel();
        auto input = input_();
        if (!input) return;
        // Not handed to the pool: it is computed right here.
        job = std::make_shared<SmartEraseJob>(std::move(*input));
        attach(job, QPointF());
    }
    job->run_or_wait();
    finish(job);
}

void SmartEraseFill::adopt(std::optional<SmartErasePatch> patch) {
    cancel();
    patch_ = std::move(patch);
    state_ = State::Ready;
    notify();
}

void SmartEraseFill::follow(SmartEraseFill &source, const QPointF &offset) {
    cancel();
    if (source.ready()) {
        adopt(moved(source.patch_, offset));
        return;
    }
    if (!source.start_now()) {
        invalidate(0);
        return;
    }
    attach(source.job_, source.offset_ + offset);
    notify();
}

std::optional<qint64> SmartEraseFill::running_ms() const {
    if (!job_) return std::nullopt;
    return job_->elapsed_ms();
}

void SmartEraseFill::start() {
    cancel();
    auto input = input_();
    if (!input) return;
    auto job = std::make_shared<SmartEraseJob>(std::move(*input));
    attach(job, QPointF());
    QThreadPool::globalInstance()->start([job] { job->run_in_pool(); });
    emit state_changed();
}

void SmartEraseFill::attach(const std::shared_ptr<SmartEraseJob> &job, const QPointF &offset) {
    job_ = job;
    offset_ = offset;
    job->listen(this);
    state_ = State::Running;
    frames_.start(static_cast<int>(std::max<qint64>(0, SLOW_MS - job->elapsed_ms())));
}

void SmartEraseFill::finish(const std::shared_ptr<SmartEraseJob> &job) {
    if (job != job_ || state_ != State::Running) return;
    patch_ = moved(job->result(), offset_);
    cancel();
    state_ = State::Ready;
    notify();
}
}
