#pragma once
#include <QObject>
#include <QPoint>
#include <QSize>
#include <QTimer>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace jietuba {
struct MSAAElementSnapshot {
    std::array<int, 4> rect;
    int order = 0;
    bool contains(int x, int y) const { return rect[0] <= x && x < rect[2] && rect[1] <= y && y < rect[3]; }
};
using MSAAQuery = std::function<std::vector<MSAAElementSnapshot>(quintptr, int, int, bool)>;
struct MSAASession;
void prepare_point_detection();

// One query is in flight at a time; points arriving meanwhile only replace the next
// one. A reply is kept even if the pointer has moved since, so the answer keeps up
// with a moving pointer instead of being discarded until it stops.
class MSAAElementFinder : public QObject {
    Q_OBJECT
public:
    // How long a single query may keep the caller's previous selection on screen.
    static constexpr int HOLD_MS = 150;
    explicit MSAAElementFinder(QObject *parent = nullptr, MSAAQuery query = {});
    ~MSAAElementFinder() override;
    std::vector<MSAAElementSnapshot> elements_at(quintptr hwnd, int x, int y, QSize min_size = QSize(1, 1));
    void request_refresh_if_needed(quintptr hwnd, int x, int y);
    bool has_parent_chain(quintptr hwnd, int x, int y) const;
    void request_parent_chain(quintptr hwnd, int x, int y);
    bool prepare_at(quintptr hwnd, int x, int y);
    // A query for the point is under way and no answered control contains it yet.
    bool resolving(quintptr hwnd, int x, int y) const;
    void close();
    double last_elapsed_ms() const { return last_elapsed_ms_; }
    QString last_error() const { return last_error_; }
signals:
    void cache_updated(quintptr hwnd);
private:
    struct Point {
        quintptr hwnd;
        int x, y;
        bool operator==(const Point &other) const { return hwnd == other.hwnd && x == other.x && y == other.y; }
        bool operator!=(const Point &other) const { return !(*this == other); }
    };
    bool leaf_covers(const Point &point) const;
    void enqueue(bool settle = false, bool parents = false);
    void drain_results();
    std::shared_ptr<MSAASession> session_;
    std::optional<Point> desired_, answer_point_;
    std::vector<MSAAElementSnapshot> answer_;
    quint64 sequence_ = 0;
    bool closed_ = false, inflight_ = false, parents_wanted_ = false, parents_ready_ = false;
    double last_elapsed_ms_ = 0;
    QString last_error_;
    QTimer settle_timer_, hold_timer_;
};
}
