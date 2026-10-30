#pragma once
#include <QImage>
#include <QObject>
#include <QPainterPath>
#include <QPointF>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace jietuba {
// Background pixels rebuilt under one shape.
struct SmartErasePatch {
    QImage image;
    QRectF scene_rect;
};

// What a reconstruction reads, taken from the item when it starts.
struct SmartEraseInput {
    // The background at full resolution and where it lies in the scene.
    QImage source;
    QRectF source_rect;
    // The area to erase, in scene coordinates.
    QPainterPath shape;
};

// The pixels under the shape rebuilt from their surroundings; nullopt when the shape misses the
// background, when cancelled, or when nothing around it can be sampled. Safe on any thread.
std::optional<SmartErasePatch> compute_smart_erase(const SmartEraseInput &input, const std::atomic_bool *cancelled = nullptr);
// Reconstructions started in this process, so tests can tell a wait from a restart.
int smart_erase_runs();

class SmartEraseJob;

// Keeps one shape's patch current. A geometry change drops the patch and starts a worker after a
// delay; painting for an export waits for the worker, or computes in place when none has started.
class SmartEraseFill : public QObject {
    Q_OBJECT
public:
    // A computation running this long is shown as running.
    static constexpr int SLOW_MS = 200;
    // Returns nullopt while the item has nothing to erase from (no scene or background yet).
    using InputProvider = std::function<std::optional<SmartEraseInput>()>;
    // changed is called when what the item shows changes, and every frame of a slow computation.
    SmartEraseFill(InputProvider input, std::function<void()> changed);
    ~SmartEraseFill() override;
    // Drops the patch; a worker starts after delay_ms, or not at all when delay_ms is negative.
    void invalidate(int delay_ms);
    // Stops a pending or running computation; a finished patch stays.
    void cancel();
    // Starts a computation still waiting out its delay. False when none is running afterwards: the patch
    // is ready, or there is nothing to compute from.
    bool start_now();
    // Blocks until the patch for the current geometry exists. A running computation is waited for, not
    // started again.
    void compute_now();
    // Takes a finished patch for the current geometry instead of computing one.
    void adopt(std::optional<SmartErasePatch> patch);
    // Shows source's patch moved by offset: at once when it is finished, otherwise when source's
    // computation ends, which runs once for both. Without anything source can compute from, it rebuilds itself.
    void follow(SmartEraseFill &source, const QPointF &offset);
    // Finished for the current geometry; patch() is empty when there was nothing to rebuild.
    bool ready() const { return state_ == State::Ready; }
    bool running() const { return state_ == State::Running; }
    // How long the computation in flight has run, for a follower the one it follows.
    std::optional<qint64> running_ms() const;
    const std::optional<SmartErasePatch> &patch() const { return patch_; }
signals:
    // ready() or running() changed.
    void state_changed();
private:
    friend class SmartEraseJob;
    enum class State { Stale, Running, Ready };
    void start();
    void attach(const std::shared_ptr<SmartEraseJob> &job, const QPointF &offset);
    void finish(const std::shared_ptr<SmartEraseJob> &job);
    void notify();
    InputProvider input_;
    std::function<void()> changed_;
    QTimer timer_;
    // Repaints while a computation is slow.
    QTimer frames_;
    std::shared_ptr<SmartEraseJob> job_;
    // Where this fill shows the job's patch relative to where it was computed.
    QPointF offset_;
    std::optional<SmartErasePatch> patch_;
    State state_ = State::Stale;
};
}
