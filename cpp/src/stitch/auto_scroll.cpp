#include "stitch/auto_scroll.h"
#include "stitch/incremental.h"
#include <QCursor>
#include <cstdlib>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
constexpr UINT SPI_GETMOUSEWHEELROUTING_VALUE = 0x201C;
constexpr UINT MOUSEWHEEL_ROUTING_MOUSE_HOVER_VALUE = 2;
}

bool wheel_follows_focus() {
    UINT routing = MOUSEWHEEL_ROUTING_MOUSE_HOVER_VALUE;
    const BOOL ok = SystemParametersInfoW(SPI_GETMOUSEWHEELROUTING_VALUE, 0, &routing, 0);
    return ok && routing != MOUSEWHEEL_ROUTING_MOUSE_HOVER_VALUE;
}

void activate_window_at(int x, int y) {
    HWND hwnd = WindowFromPoint(POINT{x, y});
    if (hwnd) {
        HWND root = GetAncestor(hwnd, GA_ROOT);
        SetForegroundWindow(root ? root : hwnd);
    }
}

void inject_wheel(int delta, bool horizontal) {
    mouse_event(horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(static_cast<uint32_t>(delta)), 0);
}

AutoScroller::AutoScroller(Capture capture, Inject inject, CursorAccess cursor, QObject *parent)
    : QObject(parent), capture_(std::move(capture)), inject_(std::move(inject)), cursor_(std::move(cursor)), settle_timer_(this), step_timer_(this), poll_timer_(this) {
    if (!cursor_.pos) cursor_.pos = [] { return QCursor::pos(); };
    if (!cursor_.set_pos) cursor_.set_pos = [](const QPoint &point) { QCursor::setPos(point); };
    settle_timer_.setSingleShot(true);
    connect(&settle_timer_, &QTimer::timeout, this, &AutoScroller::capture_frame);
    step_timer_.setSingleShot(true);
    connect(&step_timer_, &QTimer::timeout, this, &AutoScroller::scroll);
    poll_timer_.setInterval(POLL_MS);
    connect(&poll_timer_, &QTimer::timeout, this, &AutoScroller::check_cursor);
    injected_at_.start();
}

void AutoScroller::start(const QPoint &center, bool horizontal, std::optional<bool> forward) {
    if (running_) return;
    const bool ahead = forward.value_or(forward_);
    running_ = true;
    horizontal_ = horizontal;
    // Wheel sign: vertical positive scrolls up, horizontal positive right.
    sign_ = (ahead ? 1 : -1) * (horizontal ? 1 : -1);
    idle_ = 0;
    waiting_.reset();
    cursor_.set_pos(center);
    parked_ = cursor_.pos();
    poll_timer_.start();
    scroll();
}

void AutoScroller::stop(const QString &reason) {
    if (!running_) return;
    running_ = false;
    poll_timer_.stop();
    step_timer_.stop();
    waiting_.reset();
    emit stopped(reason);
}

void AutoScroller::flush() {
    if (settle_timer_.isActive()) {
        settle_timer_.stop();
        capture_frame();
    }
}

void AutoScroller::cancel() {
    settle_timer_.stop();
    stop();
}

void AutoScroller::reset() {
    last_top_.reset();
    forward_ = true;
}

void AutoScroller::on_frame(const FrameResult &result) {
    std::optional<int> moved;
    if (result.ok) {
        if (last_top_) {
            const int delta = result.top - result.shift - *last_top_;
            moved = std::abs(delta);
            if (delta) forward_ = delta > 0;
        }
        last_top_ = result.top;
    }
    if (!running_ || result.index != waiting_) return;
    waiting_.reset();
    if (!result.ok) {
        stop("failed");
        return;
    }
    if (moved && *moved) {
        idle_ = 0;
    } else {
        idle_ += 1;
        if (idle_ >= IDLE_STEPS_AT_END) {
            stop("end");
            return;
        }
    }
    const auto wait = STEP_INTERVAL_MS - injected_at_.nsecsElapsed() / 1e6;
    if (wait > 0) step_timer_.start(static_cast<int>(wait));
    else scroll();
}

void AutoScroller::scroll() {
    inject_(WHEEL_DELTA * sign_, horizontal_);
    injected_at_.restart();
    settle_timer_.start(SETTLE_MS);
}

void AutoScroller::capture_frame() {
    capture_([this](std::optional<int> index) { on_captured(index); });
}

void AutoScroller::on_captured(std::optional<int> index) {
    if (!running_) return;
    if (!index) {
        stop("failed");
        return;
    }
    waiting_ = index;
}

void AutoScroller::check_cursor() {
    if ((cursor_.pos() - parked_).manhattanLength() > 2) stop("mouse");
}
}
