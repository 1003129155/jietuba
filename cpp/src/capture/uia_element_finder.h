#pragma once
#include <QObject>
#include <QSize>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace jietuba {
using UIARect = std::array<int, 4>;  // left, top, right, bottom

struct UIAElementSnapshot {
    UIARect rect;
    int order = 0;
    long long area() const { return static_cast<long long>(rect[2] - rect[0]) * (rect[3] - rect[1]); }
    bool contains(int x, int y) const { return rect[0] <= x && x < rect[2] && rect[1] <= y && y < rect[3]; }
};

bool is_uia_available();
// A window that no longer processes messages; its UIA provider is unlikely to answer either.
bool is_hung_window(quintptr hwnd);

// One window's control rectangles, sorted by area once and bucketed by y.
// Hit tests run on every mouse move and a browser window reports thousands of
// rectangles; scanning only the pointer's band keeps a query near 0.02 ms.
class ElementIndex {
public:
    // Taller buckets degrade to a full scan; shorter ones copy large rects into too many buckets.
    static constexpr int BUCKET_PX = 64;
    ElementIndex() = default;
    explicit ElementIndex(std::vector<UIAElementSnapshot> elements);
    size_t size() const { return count_; }
    // Elements under the point, finest first: the mouse wheel walks this chain outward.
    std::vector<UIAElementSnapshot> at(int x, int y, QSize min_size = QSize(1, 1)) const;
private:
    size_t count_ = 0;
    std::unordered_map<int, std::vector<UIAElementSnapshot>> buckets_;
};

struct UIASession;
class UIAScanService;

// Per-capture cache of control rectangles rooted at windows found before the
// overlay appeared, so the overlay itself is never hit. A failed scan leaves
// the window level in place.
class UIAElementFinder : public QObject {
    Q_OBJECT
public:
    // Windows prewarmed at capture start: scanning makes the target process build
    // its accessibility tree, and the window to capture is almost always frontmost.
    static constexpr int PREWARM_LIMIT = 3;
    explicit UIAElementFinder(QObject *parent = nullptr);
    ~UIAElementFinder() override;
    std::vector<UIAElementSnapshot> elements_at(quintptr hwnd, int x, int y, QSize min_size = QSize(1, 1)) const;
    // The window under the pointer jumps the queue. hwnd 0 is a gap between windows.
    // The capture is frozen, so a window's first result (even a failure) serves the whole capture.
    void request_refresh_if_needed(quintptr hwnd);
    // Queues windows in Z order at capture start, skipping hung ones; the scan
    // cost cannot be avoided, only paid while the user moves the pointer.
    void prewarm(const std::vector<quintptr> &hwnds);
    // Invalidates the whole session at once: queued, running and unread results.
    void close();
signals:
    void cache_updated(quintptr hwnd);
private:
    void enqueue(quintptr hwnd, bool urgent);
    void drain_results();
    std::shared_ptr<UIASession> session_;
    std::map<quintptr, ElementIndex> snapshots_;
    std::set<quintptr> inflight_;
    bool closed_ = false;
    std::string last_error_;
};
}
