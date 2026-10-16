#include "capture/window_finder.h"
#include "core/logger.h"
#include <QCursor>
#include <QGuiApplication>
#include <QRegion>
#include <QScreen>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace jietuba {
WindowRect get_window_rect_no_shadow(quintptr hwnd) {
#ifdef Q_OS_WIN
    RECT rect{};
    // DWMWA_EXTENDED_FRAME_BOUNDS; a failed call leaves an empty rect, which the size filter drops.
    DwmGetWindowAttribute(reinterpret_cast<HWND>(hwnd), DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof rect);
    return {static_cast<int>(rect.left), static_cast<int>(rect.top), static_cast<int>(rect.right), static_cast<int>(rect.bottom)};
#else
    Q_UNUSED(hwnd);
    return {0, 0, 0, 0};
#endif
}

bool is_window_cloaked(quintptr hwnd) {
#ifdef Q_OS_WIN
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(reinterpret_cast<HWND>(hwnd), DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0;
#else
    Q_UNUSED(hwnd);
    return false;
#endif
}

WindowFinder::WindowFinder(int x, int y) : screen_offset_x(x), screen_offset_y(y) {}

void WindowFinder::set_screen_offset(int offset_x, int offset_y) {
    screen_offset_x = offset_x;
    screen_offset_y = offset_y;
    if (debug) log_debug(T("使用偏移: ({offset_x}, {offset_y})", {{"offset_x", offset_x}, {"offset_y", offset_y}}), "SmartSelection");
}

#ifdef Q_OS_WIN
namespace {
BOOL CALLBACK enum_windows_callback(HWND hwnd, LPARAM param) {
    auto *finder = reinterpret_cast<WindowFinder *>(param);
    if (!IsWindowVisible(hwnd) || is_window_cloaked(reinterpret_cast<quintptr>(hwnd))) return TRUE;
    const LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t title_buffer[512] = {};
    const int title_length = GetWindowTextW(hwnd, title_buffer, 512);
    const auto title = QString::fromWCharArray(title_buffer, title_length);
    if (title.trimmed().isEmpty()) return TRUE;
    // Transparent input overlays are not real windows.
    if (ex_style & WS_EX_TRANSPARENT) return TRUE;
    auto rect = get_window_rect_no_shadow(reinterpret_cast<quintptr>(hwnd));
    const int width = rect[2] - rect[0], height = rect[3] - rect[1];
    if (width < 30 || height < 30) return TRUE;
    if (rect[2] < -1000 || rect[3] < -1000 || rect[0] > 10000 || rect[1] > 10000) return TRUE;
    wchar_t class_buffer[256] = {};
    const int class_length = GetClassNameW(hwnd, class_buffer, 256);
    const auto class_name = QString::fromWCharArray(class_buffer, class_length);
    if (class_name == "Windows.UI.Core.CoreWindow" || class_name == "WorkerW" || class_name == "Progman") return TRUE;
    rect[0] -= finder->screen_offset_x;
    rect[1] -= finder->screen_offset_y;
    rect[2] -= finder->screen_offset_x;
    rect[3] -= finder->screen_offset_y;
    finder->windows.push_back({reinterpret_cast<quintptr>(hwnd), rect, title});
    return TRUE;
}
}
#endif

void WindowFinder::find_windows() {
    windows.clear();
#ifdef Q_OS_WIN
    if (!EnumWindows(&enum_windows_callback, reinterpret_cast<LPARAM>(this))) {
        log_error(T("枚举窗口失败: {e}", {{"e", QString("[WinError %1]").arg(GetLastError())}}), "SmartSelection");
        windows.clear();
        return;
    }
    if (debug) {
        log_debug(T("找到 {count} 个有效窗口", {{"count", static_cast<int>(windows.size())}}), "SmartSelection");
        if (!windows.empty()) {
            log_debug(T("检测到的窗口列表（前5个）:"), "SmartSelection");
            for (size_t i = 0; i < std::min<size_t>(5, windows.size()); ++i) {
                const auto &w = windows[i];
                log_debug(T("{index}. 标题: {title}, 大小: {width}x{height}, 位置: ({x}, {y})",
                            {{"index", static_cast<int>(i + 1)}, {"title", w.title.left(30)}, {"width", w.rect[2] - w.rect[0]},
                             {"height", w.rect[3] - w.rect[1]}, {"x", w.rect[0]}, {"y", w.rect[1]}}), "SmartSelection");
            }
        }
    }
#endif
}

std::vector<quintptr> WindowFinder::windows_by_exposed_area() const {
    QRegion covered;
    std::vector<std::pair<long long, quintptr>> scored;
    for (const auto &window : windows) {
        const QRegion shape(QRect(window.rect[0], window.rect[1], window.rect[2] - window.rect[0], window.rect[3] - window.rect[1]));
        const auto exposed = shape.subtracted(covered);
        long long area = 0;
        for (const auto &part : exposed) area += static_cast<long long>(part.width()) * part.height();
        if (area > 0) scored.emplace_back(area, window.hwnd);
        covered = covered.united(shape);
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<quintptr> result;
    for (const auto &entry : scored) result.push_back(entry.second);
    return result;
}

std::optional<WindowInfo> WindowFinder::find_window_at_point_info(int x, int y) const {
    for (const auto &window : windows)
        if (window.rect[0] <= x && x <= window.rect[2] && window.rect[1] <= y && y <= window.rect[3]) return window;
    return std::nullopt;
}

WindowRect WindowFinder::find_window_at_point(int x, int y, const std::optional<WindowRect> &fallback_rect) const {
    // EnumWindows lists top to bottom, so the first hit is the topmost window.
    for (size_t idx = 0; idx < windows.size(); ++idx) {
        const auto &w = windows[idx];
        if (w.rect[0] <= x && x <= w.rect[2] && w.rect[1] <= y && y <= w.rect[3]) {
            if (debug)
                log_debug(T("鼠标({x}, {y})处找到窗口: '{title}', 大小: {width}x{height}, Z-order: {idx}",
                            {{"x", x}, {"y", y}, {"title", w.title.left(30)}, {"width", w.rect[2] - w.rect[0]},
                             {"height", w.rect[3] - w.rect[1]}, {"idx", static_cast<int>(idx)}}), "SmartSelection");
            return w.rect;
        }
    }
    if (debug) log_debug(T("在鼠标位置({x}, {y})未找到有效窗口，返回备选矩形", {{"x", x}, {"y", y}}), "SmartSelection");
    return fallback_rect ? *fallback_rect : get_virtual_desktop_rect();
}

WindowRect WindowFinder::find_monitor_rect_at_point(int x, int y) const {
#ifdef Q_OS_WIN
    MONITORINFO info{};
    info.cbSize = sizeof info;
    // MONITOR_DEFAULTTONEAREST also answers for points in the gap between screens.
    if (HMONITOR monitor = MonitorFromPoint(POINT{x, y}, MONITOR_DEFAULTTONEAREST); monitor && GetMonitorInfoW(monitor, &info))
        return {static_cast<int>(info.rcMonitor.left), static_cast<int>(info.rcMonitor.top), static_cast<int>(info.rcMonitor.right), static_cast<int>(info.rcMonitor.bottom)};
#else
    Q_UNUSED(x); Q_UNUSED(y);
#endif
    return get_virtual_desktop_rect();
}

WindowRect WindowFinder::get_virtual_desktop_rect() {
#ifdef Q_OS_WIN
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width > 0 && height > 0) return {x, y, x + width, y + height};
#endif
    if (auto *screen = QGuiApplication::primaryScreen()) {
        const auto geometry = screen->geometry();
        return {geometry.x(), geometry.y(), geometry.x() + geometry.width(), geometry.y() + geometry.height()};
    }
    return {0, 0, 1920, 1080};
}

bool is_smart_selection_available() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

std::optional<WindowRect> find_window_at_cursor(int screen_offset_x, int screen_offset_y) {
    if (!is_smart_selection_available()) return std::nullopt;
    WindowFinder finder(screen_offset_x, screen_offset_y);
    finder.find_windows();
    const auto cursor_pos = QCursor::pos();
    return finder.find_window_at_point(cursor_pos.x() - screen_offset_x, cursor_pos.y() - screen_offset_y);
}
}
