#include "capture/msaa_element_finder.h"
#include <QPointer>
#include <QScopeGuard>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#include <oleacc.h>
#include <wrl/client.h>
#endif

namespace jietuba {
namespace {
using Rect = std::array<int, 4>;
using Clock = std::chrono::steady_clock;
using Elements = std::vector<MSAAElementSnapshot>;
bool contains(const Rect &r, int x, int y) { return r[0] <= x && x < r[2] && r[1] <= y && y < r[3]; }
long long area(const Rect &r) { return static_cast<long long>(r[2] - r[0]) * (r[3] - r[1]); }
}

struct MSAARequest {
    quintptr hwnd;
    int x, y;
    std::shared_ptr<MSAASession> session;
    quint64 sequence;
    bool settle = false, parents = false;
    double elapsed_ms = 0;
};
struct MSAAReply {
    MSAARequest request;
    Elements answer;
    QString error;
};
struct MSAASession {
    std::mutex lock;
    std::deque<MSAAReply> replies;
    std::set<quintptr> checked_windows, slow_windows;
    std::function<void()> notify;
    MSAAQuery query;
    std::atomic<bool> cancelled{false};
    std::atomic<quint64> latest{0};
};

namespace {
#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;
struct Variant {
    VARIANT value;
    Variant() { VariantInit(&value); }
    ~Variant() { VariantClear(&value); }
};
VARIANT child_variant(long child) {
    VARIANT result;
    VariantInit(&result);
    result.vt = VT_I4;
    result.lVal = child;
    return result;
}
std::optional<Rect> location(IAccessible *node, long child = 0) {
    long left = 0, top = 0, width = 0, height = 0;
    if (!node || FAILED(node->accLocation(&left, &top, &width, &height, child_variant(child))) || width <= 0 || height <= 0)
        return std::nullopt;
    return Rect{static_cast<int>(left), static_cast<int>(top), static_cast<int>(left + width), static_cast<int>(top + height)};
}
struct Hit {
    ComPtr<IAccessible> node;
    long child = 0;
};
class MSAABackend {
public:
    MSAABackend() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) throw std::runtime_error("CoInitializeEx failed");
    }
    ~MSAABackend() { CoUninitialize(); }
    Elements query(const MSAARequest &request) {
        auto session = request.session;
        const auto hwnd = reinterpret_cast<HWND>(request.hwnd);
        bool slow = false, first = false;
        {
            std::lock_guard guard(session->lock);
            slow = session->slow_windows.count(request.hwnd) != 0;
            first = session->checked_windows.insert(request.hwnd).second;
        }
        if (slow || IsHungAppWindow(hwnd)) return native_chain(hwnd, request.x, request.y);
        if (first) {
            DWORD_PTR reply = 0;
            if (!SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, 50, &reply)) {
                std::lock_guard guard(session->lock);
                session->slow_windows.insert(request.hwnd);
                return native_chain(hwnd, request.x, request.y);
            }
        }
        const auto begin = Clock::now();
        const auto mark_slow = qScopeGuard([&] {
            if (!request.parents && Clock::now() - begin > std::chrono::milliseconds(300)) {
                std::lock_guard guard(session->lock);
                session->slow_windows.insert(request.hwnd);
            }
        });
        Elements result;
        try {
            auto root_node = root(hwnd);
            if (!root_node) return native_chain(hwnd, request.x, request.y);
            auto hit_node = hit(root_node.Get(), request.x, request.y);
            wchar_t class_name[256]{};
            GetClassNameW(hwnd, class_name, 256);
            const QString name = QString::fromWCharArray(class_name);
            const bool browser = name.startsWith("Chrome_WidgetWin") || name.startsWith("MozillaWindowClass");
            if (browser && child_count(hit_node.node.Get()) > 0) {
                try {
                    auto second = hit(root_node.Get(), request.x, request.y);
                    const auto first_rect = location(hit_node.node.Get(), hit_node.child);
                    const auto second_rect = location(second.node.Get(), second.child);
                    if (second_rect && contains(*second_rect, request.x, request.y)
                        && (!first_rect || child_count(second.node.Get()) == 0 || area(*second_rect) < area(*first_rect)))
                        hit_node = std::move(second);
                } catch (const std::exception &) {}
            }
            auto rect = location(hit_node.node.Get(), hit_node.child);
            if (browser && rect) {
                if (auto rendered = renderer_hit(hwnd, *rect, hit_node.child, request.x, request.y)) {
                    root_node = rendered->first;
                    hit_node = std::move(rendered->second);
                    rect = location(hit_node.node.Get(), hit_node.child);
                }
            }
            if (request.parents) result = parents(root_node.Get(), hit_node, request.x, request.y);
            else if (rect && contains(*rect, request.x, request.y)) result.push_back({*rect, 0});
        } catch (const std::exception &) {
            result = native_chain(hwnd, request.x, request.y);
        }
        return result;
    }
private:
    static constexpr int MAX_DEPTH = 31;
    static ComPtr<IAccessible> root(HWND hwnd) {
        ComPtr<IAccessible> result;
        AccessibleObjectFromWindow(hwnd, OBJID_WINDOW, __uuidof(IAccessible), reinterpret_cast<void **>(result.GetAddressOf()));
        return result;
    }
    static long child_count(IAccessible *node) {
        long count = -1;
        node->get_accChildCount(&count);
        return count;
    }
    static Hit hit(IAccessible *root_node, int x, int y) {
        Hit result{root_node, 0};
        std::set<IAccessible *> seen;
        // Holding every node visited keeps a released node's address from being taken by a later one.
        std::vector<ComPtr<IAccessible>> visited;
        for (int depth = 0; depth < MAX_DEPTH; ++depth) {
            if (!seen.insert(result.node.Get()).second) break;
            visited.push_back(result.node);
            Variant answer;
            if (FAILED(result.node->accHitTest(x, y, &answer.value))) throw std::runtime_error("accHitTest failed");
            if (answer.value.vt == VT_EMPTY || answer.value.vt == VT_I4) {
                result.child = answer.value.vt == VT_I4 ? answer.value.lVal : 0;
                return result;
            }
            ComPtr<IAccessible> next;
            if (answer.value.vt != VT_DISPATCH || !answer.value.pdispVal
                || FAILED(answer.value.pdispVal->QueryInterface(IID_PPV_ARGS(&next))))
                throw std::runtime_error("accHitTest returned no accessible object");
            result.node = std::move(next);
        }
        return result;
    }
    static Elements parents(IAccessible *root_node, Hit node, int x, int y) {
        const auto root_rect = location(root_node);
        const auto deadline = Clock::now() + std::chrono::milliseconds(40);
        Elements result;
        std::set<std::pair<IAccessible *, long>> seen;
        // Holding every node visited keeps a released node's address from being taken by a later one.
        std::vector<ComPtr<IAccessible>> visited;
        for (int depth = 0; depth < MAX_DEPTH; ++depth) {
            if (!result.empty() && Clock::now() > deadline) break;
            if (!seen.emplace(node.node.Get(), node.child).second) break;
            visited.push_back(node.node);
            const auto rect = location(node.node.Get(), node.child);
            if (rect && contains(*rect, x, y) && std::none_of(result.begin(), result.end(), [&](const auto &r) { return r.rect == *rect; }))
                result.push_back({*rect, static_cast<int>(result.size())});
            if ((rect && root_rect && *rect == *root_rect) || (node.node.Get() == root_node && node.child == 0)) break;
            if (node.child) node.child = 0;
            else {
                ComPtr<IDispatch> parent;
                ComPtr<IAccessible> accessible;
                if (FAILED(node.node->get_accParent(&parent)) || !parent || FAILED(parent.As(&accessible))) break;
                node.node = std::move(accessible);
            }
        }
        return result;
    }
    static Elements native_chain(HWND hwnd, int x, int y) {
        Elements result;
        std::set<HWND> seen;
        for (int depth = 0; depth < MAX_DEPTH; ++depth) {
            if (!seen.insert(hwnd).second) break;
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect)) break;
            Rect bounds{rect.left, rect.top, rect.right, rect.bottom};
            if (contains(bounds, x, y)) result.push_back({bounds, 0});
            POINT point{x, y};
            if (!ScreenToClient(hwnd, &point)) break;
            const auto child = ChildWindowFromPointEx(hwnd, point, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT);
            if (!child || child == hwnd) break;
            hwnd = child;
        }
        std::reverse(result.begin(), result.end());
        for (int i = 0; i < static_cast<int>(result.size()); ++i) result[i].order = i;
        return result;
    }
    static std::optional<std::pair<ComPtr<IAccessible>, Hit>> renderer_hit(HWND hwnd, const Rect &coarse, long child_id, int x, int y) {
        if (child_id) return std::nullopt;
        RECT client{};
        POINT origin{};
        if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)
            || coarse != Rect{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom}) return std::nullopt;
        std::set<HWND> seen;
        for (int depth = 0; depth < MAX_DEPTH; ++depth) {
            if (!seen.insert(hwnd).second) break;
            POINT point{x, y};
            if (!ScreenToClient(hwnd, &point)) break;
            // A transparent renderer still owns the accessibility tree.
            const auto child = ChildWindowFromPointEx(hwnd, point, CWP_SKIPINVISIBLE);
            if (!child || child == hwnd) break;
            hwnd = child;
            wchar_t name[256]{};
            GetClassNameW(hwnd, name, 256);
            if (QString::fromWCharArray(name) != "Chrome_RenderWidgetHostHWND") continue;
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect) || coarse != Rect{rect.left, rect.top, rect.right, rect.bottom}) break;
            auto renderer = root(hwnd);
            if (!renderer) break;
            try {
                auto candidate = hit(renderer.Get(), x, y);
                const auto bounds = location(candidate.node.Get(), candidate.child);
                if (bounds && contains(*bounds, x, y) && coarse[0] <= (*bounds)[0] && coarse[1] <= (*bounds)[1]
                    && (*bounds)[2] <= coarse[2] && (*bounds)[3] <= coarse[3])
                    return std::make_pair(std::move(renderer), std::move(candidate));
            } catch (const std::exception &) {}
            break;
        }
        return std::nullopt;
    }
};
#endif

class PointService {
public:
    static PointService &instance() {
        // A stuck provider must not prevent process exit.
        static auto *service = new PointService;
        return *service;
    }
    void submit(MSAARequest request) {
        {
            std::lock_guard guard(lock_);
            if (request.session->cancelled) return;
            request.session->latest = request.sequence;
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const auto &old) {
                return old.session == request.session || old.session->cancelled;
            }), queue_.end());
            queue_.push_back(std::move(request));
        }
        condition_.notify_one();
    }
private:
    PointService() {
        for (int i = 0; i < 2; ++i) std::thread([this] { run(); }).detach();
    }
    void run() {
#ifdef Q_OS_WIN
        std::unique_ptr<MSAABackend> backend;
        try { backend = std::make_unique<MSAABackend>(); } catch (const std::exception &) {}
#endif
        while (true) {
            MSAARequest request;
            {
                std::unique_lock guard(lock_);
                condition_.wait(guard, [this] { return !queue_.empty(); });
                request = std::move(queue_.front());
                queue_.pop_front();
            }
            auto session = request.session;
            if (session->cancelled || request.sequence != session->latest) continue;
            Elements answer;
            QString error;
            const auto begin = Clock::now();
            try {
                if (session->query) answer = session->query(request.hwnd, request.x, request.y, request.parents);
#ifdef Q_OS_WIN
                else {
                    if (!backend) backend = std::make_unique<MSAABackend>();
                    answer = backend->query(request);
                }
#endif
            } catch (const std::exception &e) { error = QString::fromUtf8(e.what()); }
            request.elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            std::lock_guard guard(session->lock);
            if (!session->cancelled && request.sequence == session->latest) {
                session->replies.push_back({std::move(request), std::move(answer), std::move(error)});
                if (session->notify) session->notify();
            }
        }
    }
    std::mutex lock_;
    std::condition_variable condition_;
    std::deque<MSAARequest> queue_;
};
}

void prepare_point_detection() { PointService::instance(); }

MSAAElementFinder::MSAAElementFinder(QObject *parent, MSAAQuery query) : QObject(parent), session_(std::make_shared<MSAASession>()) {
    session_->query = std::move(query);
    session_->notify = [this] { QMetaObject::invokeMethod(this, [this] { drain_results(); }, Qt::QueuedConnection); };
    settle_timer_.setSingleShot(true);
    settle_timer_.setInterval(40);
    connect(&settle_timer_, &QTimer::timeout, this, [this] {
        if (!closed_ && desired_ && !pending_) enqueue(true, true);
    });
    prepare_point_detection();
}
MSAAElementFinder::~MSAAElementFinder() { close(); }
void MSAAElementFinder::request_refresh_if_needed(quintptr hwnd, int x, int y) {
    const Point point{hwnd, x, y};
    if (closed_ || !hwnd || (desired_ && *desired_ == point)) return;
    desired_ = point;
    settle_timer_.stop();
    enqueue();
}
void MSAAElementFinder::enqueue(bool settle, bool parents) {
    ++sequence_;
    pending_ = true;
    PointService::instance().submit({desired_->hwnd, desired_->x, desired_->y, session_, sequence_, settle, parents});
}
bool MSAAElementFinder::has_parent_chain(quintptr hwnd, int x, int y) const {
    return parents_ready_ && answer_point_ && answer_point_->hwnd == hwnd && !answer_.empty() && answer_.front().contains(x, y);
}
void MSAAElementFinder::request_parent_chain(quintptr hwnd, int x, int y) {
    if (closed_ || has_parent_chain(hwnd, x, y)) return;
    desired_ = Point{hwnd, x, y};
    settle_timer_.stop();
    enqueue(true, true);
}
std::vector<MSAAElementSnapshot> MSAAElementFinder::elements_at(quintptr hwnd, int x, int y, QSize min_size) {
    request_refresh_if_needed(hwnd, x, y);
    Elements result;
    if (closed_ || !answer_point_ || answer_point_->hwnd != hwnd) return result;
    for (const auto &element : answer_)
        if (element.contains(x, y) && element.rect[2] - element.rect[0] >= min_size.width() && element.rect[3] - element.rect[1] >= min_size.height())
            result.push_back(element);
    return result;
}
bool MSAAElementFinder::prepare_at(quintptr hwnd, int x, int y) {
    request_refresh_if_needed(hwnd, x, y);
    drain_results();
    return !closed_ && !pending_ && answer_point_ && *answer_point_ == Point{hwnd, x, y};
}
void MSAAElementFinder::drain_results() {
    while (!closed_) {
        std::optional<MSAAReply> reply;
        {
            std::lock_guard guard(session_->lock);
            if (session_->replies.empty()) break;
            reply = std::move(session_->replies.front());
            session_->replies.pop_front();
        }
        const auto &request = reply->request;
        if (request.sequence != sequence_) continue;
        const bool same_leaf = !reply->answer.empty() && !answer_.empty() && answer_point_ && answer_point_->hwnd == request.hwnd
            && reply->answer.front().rect == answer_.front().rect;
        if (request.parents || !(same_leaf && parents_ready_)) {
            answer_ = std::move(reply->answer);
            parents_ready_ = request.parents;
        }
        answer_point_ = Point{request.hwnd, request.x, request.y};
        pending_ = false;
        last_error_ = reply->error;
        last_elapsed_ms_ = request.elapsed_ms;
        if (!request.settle) settle_timer_.start();
        const QPointer<MSAAElementFinder> self(this);
        emit cache_updated(request.hwnd);
        if (!self) return;
    }
}
void MSAAElementFinder::close() {
    if (closed_) return;
    closed_ = true;
    session_->cancelled = true;
    settle_timer_.stop();
    {
        std::lock_guard guard(session_->lock);
        session_->notify = nullptr;
        session_->replies.clear();
    }
    answer_.clear();
    answer_point_.reset();
    desired_.reset();
}
}
