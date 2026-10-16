#pragma once
#include <QObject>
#include <QRect>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// Change watching for long screenshots: a background thread captures the region every
// so often and tells the window to capture when it differs from the last look.
//
// It only decides whether the picture moved; the frames for stitching are still
// captured by the window. The screen DC BitBlt and the comparison stay off the UI
// thread. Idle, it looks every IDLE_MS; on change it switches to ACTIVE_MS until
// ACTIVE_HOLD_MS pass without change. Each wait starts after the look finishes, so a
// slow machine looks less often instead of piling up. The app's own toolbar and
// preview inside the region also trigger captures; stitching then only records the position.
namespace jietuba {
constexpr int CHANGE_WATCH_IDLE_MS = 300;
constexpr int CHANGE_WATCH_ACTIVE_MS = 100;
constexpr int CHANGE_WATCH_ACTIVE_HOLD_MS = 1000;

// A region to compare against its previous capture.
class ChangeRegion {
public:
    virtual ~ChangeRegion() = default;
    // Captures once; whether it differs from the previous capture. The first capture
    // and failed captures count as unchanged.
    virtual bool changed() = 0;
};

// A screen region (physical pixels) captured alternately into two buffers. GDI handles
// belong to the creating thread and are released on it by the destructor.
class ScreenRegion : public ChangeRegion {
public:
    explicit ScreenRegion(const QRect &rect);
    ~ScreenRegion() override;
    bool changed() override;
private:
    struct Buffer {
        void *dc;
        void *bitmap;
        void *old;
        void *bits;
    };
    int x_, y_, w_, h_;
    size_t size_;
    void *screen_;
    Buffer buffers_[2];
    int current_ = 0;
    bool filled_ = false;
};

using ChangeRegionFactory = std::function<std::unique_ptr<ChangeRegion>(const QRect &)>;

// Watches the region in the background and emits changed when the picture moves. The
// signal comes from the background thread; receivers must connect queued.
class ChangeWatch : public QObject {
    Q_OBJECT
public:
    explicit ChangeWatch(const QRect &rect, ChangeRegionFactory region = {}, QObject *parent = nullptr);
    ~ChangeWatch() override;
    void start();
    // Stops and waits for the background thread; no changed signals follow.
    void stop();
signals:
    void changed();
private:
    void run();
    QRect rect_;
    ChangeRegionFactory region_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};
}
