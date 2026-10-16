#include "capture/uia_element_finder.h"
#include "core/logger.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#include <UIAutomation.h>
#endif

namespace jietuba {
bool is_uia_available() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

bool is_hung_window(quintptr hwnd) {
#ifdef Q_OS_WIN
    return IsHungAppWindow(reinterpret_cast<HWND>(hwnd)) != FALSE;
#else
    Q_UNUSED(hwnd);
    return false;
#endif
}

namespace {
int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}

ElementIndex::ElementIndex(std::vector<UIAElementSnapshot> elements) {
    // On equal area the later tree entry wins: when parent and child report the
    // same rectangle, the child is what the pointer really points at.
    std::stable_sort(elements.begin(), elements.end(), [](const auto &a, const auto &b) {
        return a.area() != b.area() ? a.area() < b.area() : a.order > b.order;
    });
    count_ = elements.size();
    for (const auto &element : elements) {
        const int top = element.rect[1], bottom = element.rect[3];
        for (int bucket = floor_div(top, BUCKET_PX); bucket <= floor_div(bottom - 1, BUCKET_PX); ++bucket)
            buckets_[bucket].push_back(element);
    }
}

std::vector<UIAElementSnapshot> ElementIndex::at(int x, int y, QSize min_size) const {
    std::vector<UIAElementSnapshot> result;
    const auto found = buckets_.find(floor_div(y, BUCKET_PX));
    if (found == buckets_.end()) return result;
    for (const auto &row : found->second) {
        if (row.rect[0] <= x && x < row.rect[2] && row.rect[1] <= y && y < row.rect[3]
            && row.rect[2] - row.rect[0] >= min_size.width() && row.rect[3] - row.rect[1] >= min_size.height())
            result.push_back(row);
    }
    return result;
}

struct UIARequest {
    quintptr hwnd;
    std::shared_ptr<UIASession> session;
    // Only the log reads it; on slow machines it explains late control rectangles.
    double elapsed_ms = 0;
};

struct UIAReply {
    UIARequest request;
    ElementIndex index;
    std::string error;
};

// One capture's inbox. Cancelling sets one flag that workers check when they take
// a request, which holds however many stale requests are still queued.
struct UIASession {
    std::mutex lock;
    std::deque<UIAReply> replies;
    // Wakes the finder after a reply is queued; cleared under lock when it closes.
    std::function<void()> notify;
    std::atomic<bool> cancelled{false};
};

namespace {
#ifdef Q_OS_WIN
// Two workers cold-starting together make the new CUIAutomation8's first call
// fail with E_FAIL; only the first initialisation per thread is serialised.
std::mutex uia_init_lock;

// Created, used and released on one worker's multithreaded apartment.
class UIABackend {
public:
    static constexpr int MAX_ELEMENTS = 4096;
    UIABackend() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) throw std::runtime_error("CoInitializeEx failed");
        com_initialized_ = true;
        std::lock_guard guard(uia_init_lock);
        IUIAutomation2 *automation2 = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation2), reinterpret_cast<void **>(&automation2)))) {
            automation2->put_ConnectionTimeout(300);
            automation2->put_TransactionTimeout(300);
            automation_ = automation2;
        } else if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), reinterpret_cast<void **>(&automation_)))) {
            close();
            throw std::runtime_error("CoCreateInstance(CUIAutomation) failed");
        }
        if (FAILED(automation_->CreateCacheRequest(&cache_request_))) {
            close();
            throw std::runtime_error("CreateCacheRequest failed");
        }
        cache_request_->put_TreeScope(TreeScope_Element);
        cache_request_->put_AutomationElementMode(AutomationElementMode_None);
        cache_request_->AddProperty(UIA_BoundingRectanglePropertyId);
        // Offscreen elements dominate deep trees; letting the provider filter them
        // avoids marshalling each one (about 290 ms -> 90 ms on browser windows).
        IUIAutomationCondition *control_view = nullptr, *on_screen = nullptr;
        VARIANT offscreen_false;
        VariantInit(&offscreen_false);
        offscreen_false.vt = VT_BOOL;
        offscreen_false.boolVal = VARIANT_FALSE;
        automation_->get_ControlViewCondition(&control_view);
        automation_->CreatePropertyCondition(UIA_IsOffscreenPropertyId, offscreen_false, &on_screen);
        if (control_view && on_screen) automation_->CreateAndCondition(control_view, on_screen, &condition_);
        if (control_view) control_view->Release();
        if (on_screen) on_screen->Release();
        if (!condition_) {
            close();
            throw std::runtime_error("CreateAndCondition failed");
        }
    }
    ~UIABackend() { close(); }
    std::vector<UIAElementSnapshot> scan(quintptr hwnd) {
        std::vector<UIAElementSnapshot> snapshots;
        IUIAutomationElement *root = nullptr;
        if (FAILED(automation_->ElementFromHandle(reinterpret_cast<UIA_HWND>(hwnd), &root)) || !root) return snapshots;
        IUIAutomationElementArray *elements = nullptr;
        const HRESULT hr = root->FindAllBuildCache(TreeScope_Descendants, condition_, cache_request_, &elements);
        root->Release();
        if (FAILED(hr)) throw std::runtime_error(QString("FindAllBuildCache 0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0')).toStdString());
        if (!elements) return snapshots;
        int length = 0;
        elements->get_Length(&length);
        for (int index = 0; index < std::min(length, MAX_ELEMENTS); ++index) {
            IUIAutomationElement *element = nullptr;
            // A child that disappears must not discard the other controls.
            if (FAILED(elements->GetElement(index, &element)) || !element) continue;
            RECT rect{};
            if (SUCCEEDED(element->get_CachedBoundingRectangle(&rect)) && rect.right > rect.left && rect.bottom > rect.top)
                snapshots.push_back({{static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right), static_cast<int>(rect.bottom)}, index});
            element->Release();
        }
        elements->Release();
        return snapshots;
    }
    void close() {
        if (condition_) { condition_->Release(); condition_ = nullptr; }
        if (cache_request_) { cache_request_->Release(); cache_request_ = nullptr; }
        if (automation_) { automation_->Release(); automation_ = nullptr; }
        if (com_initialized_) { CoUninitialize(); com_initialized_ = false; }
    }
private:
    IUIAutomation *automation_ = nullptr;
    IUIAutomationCacheRequest *cache_request_ = nullptr;
    IUIAutomationCondition *condition_ = nullptr;
    bool com_initialized_ = false;
};
#endif
}

// Fixed process-wide workers; no Qt object crosses this boundary. Two workers
// overlap the waits on cross-process providers (about 1.74x); more do not help
// because the UIA client library serialises internally.
class UIAScanService {
public:
    static constexpr int WORKERS = 2;
    static UIAScanService &instance() {
        // Deliberately leaked: a provider stuck in a call must never block process exit.
        static auto *service = new UIAScanService;
        return *service;
    }
    // urgent is the window under the pointer (front of the queue); prewarm requests keep their order.
    void submit(UIARequest request, bool urgent) {
        {
            std::lock_guard guard(lock_);
            if (closed_ || request.session->cancelled) return;
            // Drop requests of cancelled captures so repeated capture/cancel does not grow the queue.
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const auto &pending) { return pending.session->cancelled.load(); }), queue_.end());
            if (urgent) queue_.push_front(std::move(request));
            else queue_.push_back(std::move(request));
        }
        condition_.notify_one();
    }
private:
    UIAScanService() {
        for (int index = 0; index < WORKERS; ++index) std::thread([this, index] { run(index); }).detach();
    }
    void run(int index) {
#ifdef Q_OS_WIN
        SetThreadDescription(GetCurrentThread(), reinterpret_cast<PCWSTR>(QString("UIA detection %1").arg(index).utf16()));
        std::unique_ptr<UIABackend> backend;
#else
        Q_UNUSED(index);
#endif
        while (true) {
            UIARequest request;
            {
                std::unique_lock guard(lock_);
                condition_.wait(guard, [this] { return closed_ || !queue_.empty(); });
                if (closed_) return;
                request = std::move(queue_.front());
                queue_.pop_front();
            }
            if (request.session->cancelled) continue;
            ElementIndex index_result;
            std::string error;
            const auto started = std::chrono::steady_clock::now();
#ifdef Q_OS_WIN
            try {
                if (!backend) backend = std::make_unique<UIABackend>();
                // Built here: sorting and bucketing thousands of rects would otherwise
                // stall the UI thread when several results arrive together.
                index_result = ElementIndex(backend->scan(request.hwnd));
            } catch (const std::exception &e) {
                error = e.what();
            }
#endif
            request.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (!request.session->cancelled) {
                auto session = request.session;
                std::lock_guard guard(session->lock);
                session->replies.push_back({std::move(request), std::move(index_result), std::move(error)});
                if (session->notify) session->notify();
            }
        }
    }
    std::mutex lock_;
    std::condition_variable condition_;
    std::deque<UIARequest> queue_;
    bool closed_ = false;
};

UIAElementFinder::UIAElementFinder(QObject *parent) : QObject(parent), session_(std::make_shared<UIASession>()) {
    // A reply wakes the UI thread at once. The reference polls every 33 ms because its
    // workers keep away from Qt objects; posting to this object is thread-safe, and close()
    // clears the hook under the session lock before this object can go away.
    session_->notify = [this] { QMetaObject::invokeMethod(this, [this] { drain_results(); }, Qt::QueuedConnection); };
}

UIAElementFinder::~UIAElementFinder() { close(); }

std::vector<UIAElementSnapshot> UIAElementFinder::elements_at(quintptr hwnd, int x, int y, QSize min_size) const {
    const auto found = snapshots_.find(hwnd);
    return found == snapshots_.end() ? std::vector<UIAElementSnapshot>{} : found->second.at(x, y, min_size);
}

void UIAElementFinder::request_refresh_if_needed(quintptr hwnd) {
    if (hwnd > 0) enqueue(hwnd, true);
}

void UIAElementFinder::prewarm(const std::vector<quintptr> &hwnds) {
    int queued = 0;
    for (const auto hwnd : hwnds) {
        if (queued >= PREWARM_LIMIT) break;
        if (hwnd == 0 || is_hung_window(hwnd)) continue;
        enqueue(hwnd, false);
        ++queued;
    }
}

void UIAElementFinder::enqueue(quintptr hwnd, bool urgent) {
    if (closed_ || snapshots_.count(hwnd) || inflight_.count(hwnd)) return;
    inflight_.insert(hwnd);
    UIAScanService::instance().submit({hwnd, session_, 0}, urgent);
}

void UIAElementFinder::drain_results() {
    while (!closed_) {
        std::optional<UIAReply> reply;
        {
            std::lock_guard guard(session_->lock);
            if (session_->replies.empty()) break;
            reply = std::move(session_->replies.front());
            session_->replies.pop_front();
        }
        inflight_.erase(reply->request.hwnd);
        const auto count = static_cast<int>(reply->index.size());
        snapshots_[reply->request.hwnd] = std::move(reply->index);
        last_error_ = reply->error;
        log_debug(T("控件扫描耗时 {elapsed} ms，{count} 个元素{error}",
                    {{"elapsed", static_cast<int>(std::nearbyint(reply->request.elapsed_ms))}, {"count", count},
                     {"error", reply->error.empty() ? QString() : QString(": %1").arg(QString::fromStdString(reply->error))}}),
                  "SmartSelection");
        // The receiver may close the capture on the spot; close() then discards the rest.
        emit cache_updated(reply->request.hwnd);
    }
}

void UIAElementFinder::close() {
    if (closed_) return;
    closed_ = true;
    {
        std::lock_guard guard(session_->lock);
        session_->notify = nullptr;
        session_->replies.clear();
    }
    session_->cancelled = true;
    inflight_.clear();
    snapshots_.clear();
}
}
