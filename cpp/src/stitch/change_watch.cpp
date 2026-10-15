#include "stitch/change_watch.h"
#include "core/logger.h"
#include <chrono>
#include <cstring>
#include <optional>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
// Layered windows are captured under DWM without CAPTUREBLT, which makes the cursor flicker.
constexpr DWORD COPY_FLAGS = SRCCOPY;
}

ScreenRegion::ScreenRegion(const QRect &rect) : x_(rect.x()), y_(rect.y()), w_(rect.width()), h_(rect.height()) {
    size_ = static_cast<size_t>(w_) * h_ * 4;
    screen_ = GetDC(nullptr);
    BITMAPINFOHEADER header{};
    header.biSize = sizeof header;
    header.biWidth = w_;
    header.biHeight = -h_;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    for (auto &buffer : buffers_) {
        void *bits = nullptr;
        auto *bitmap = CreateDIBSection(static_cast<HDC>(screen_), reinterpret_cast<BITMAPINFO *>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
        auto *dc = CreateCompatibleDC(static_cast<HDC>(screen_));
        auto *old = SelectObject(dc, bitmap);
        buffer = Buffer{dc, bitmap, old, bits};
    }
}

ScreenRegion::~ScreenRegion() {
    for (auto &buffer : buffers_) {
        SelectObject(static_cast<HDC>(buffer.dc), static_cast<HGDIOBJ>(buffer.old));
        DeleteObject(static_cast<HGDIOBJ>(buffer.bitmap));
        DeleteDC(static_cast<HDC>(buffer.dc));
    }
    ReleaseDC(nullptr, static_cast<HDC>(screen_));
}

bool ScreenRegion::changed() {
    const int target = 1 - current_;
    const auto &buffer = buffers_[target];
    if (!BitBlt(static_cast<HDC>(buffer.dc), 0, 0, w_, h_, static_cast<HDC>(screen_), x_, y_, COPY_FLAGS)) return false;
    const void *previous = buffers_[current_].bits;
    current_ = target;
    if (!filled_) {
        filled_ = true;
        return false;
    }
    if (!buffer.bits || !previous) return false;
    return std::memcmp(buffer.bits, previous, size_) != 0;
}

ChangeWatch::ChangeWatch(const QRect &rect, ChangeRegionFactory region, QObject *parent) : QObject(parent), rect_(rect), region_(std::move(region)) {
    if (!region_) region_ = [](const QRect &area) { return std::make_unique<ScreenRegion>(area); };
}

ChangeWatch::~ChangeWatch() { stop(); }

void ChangeWatch::start() {
    if (thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        stop_ = false;
    }
    thread_ = std::thread([this] { run(); });
}

void ChangeWatch::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void ChangeWatch::run() {
    std::unique_ptr<ChangeRegion> region;
    try {
        region = region_(rect_);
    } catch (const std::exception &e) {
        log_exception(e, "ChangeWatch");
        return;
    }
    using clock = std::chrono::steady_clock;
    try {
        std::optional<clock::time_point> last_change;
        while (true) {
            const bool active = last_change && clock::now() - *last_change < std::chrono::milliseconds(CHANGE_WATCH_ACTIVE_HOLD_MS);
            {
                std::unique_lock lock(mutex_);
                if (wake_.wait_for(lock, std::chrono::milliseconds(active ? CHANGE_WATCH_ACTIVE_MS : CHANGE_WATCH_IDLE_MS), [this] { return stop_; })) break;
            }
            if (region->changed()) {
                last_change = clock::now();
                emit changed();
            }
        }
    } catch (const std::exception &e) {
        log_exception(e, "ChangeWatch");
    }
}
}
