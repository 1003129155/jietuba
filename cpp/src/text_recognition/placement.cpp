#include "text_recognition/placement.h"
#include <QString>
#include <algorithm>

namespace jietuba::text_placement {
namespace {
int clamp(int value, int low, int high) { return std::max(low, std::min(value, high)); }

std::optional<QPoint> beside(const QRect &anchor, const QSize &size, const QRect &available) {
    const int width = size.width(), height = size.height();
    // Side by side the top edges align for line-by-line comparison; stacked, the left edges do.
    const int top = clamp(anchor.top(), available.top(), available.bottom() + 1 - height);
    const int left = clamp(anchor.left(), available.left(), available.right() + 1 - width);

    int x = anchor.right() + 1 + GAP;
    if (x + width <= available.right() + 1) return QPoint(x, top);
    x = anchor.left() - GAP - width;
    if (x >= available.left()) return QPoint(x, top);
    int y = anchor.bottom() + 1 + GAP;
    if (y + height <= available.bottom() + 1) return QPoint(left, y);
    y = anchor.top() - GAP - height;
    if (y >= available.top()) return QPoint(left, y);
    return std::nullopt;
}

QPoint near_cursor(const QPoint &cursor, const QSize &size, const QRect &available) {
    const int width = size.width(), height = size.height();
    int x = cursor.x() + CURSOR_OFFSET;
    if (x + width > available.right() + 1) x = cursor.x() - CURSOR_OFFSET - width;
    int y = cursor.y() + CURSOR_OFFSET;
    if (y + height > available.bottom() + 1) y = cursor.y() - CURSOR_OFFSET - height;
    return QPoint(clamp(x, available.left(), available.right() + 1 - width), clamp(y, available.top(), available.bottom() + 1 - height));
}
}

ScreenArea pick_screen(const QString &position, const std::optional<QRect> &anchor, const std::optional<QPoint> &cursor, const std::optional<QPoint> &last_center,
                       const std::vector<ScreenArea> &screens, const ScreenArea &primary) {
    if (position == "primary_screen") return primary;
    const std::optional<QPoint> region = anchor ? std::optional<QPoint>(anchor->center()) : std::nullopt;
    std::vector<std::optional<QPoint>> points;
    if (position == "last_position") points = {last_center, region, cursor};
    else if (position == "near_cursor") points = {cursor, region};
    else points = {region, cursor};
    for (const auto &point : points) {
        if (!point) continue;
        for (const auto &screen : screens)
            if (screen.geometry.contains(*point)) return screen;
    }
    return primary;
}

QPoint window_position(const QString &position, const QSize &size, const ScreenArea &screen, const std::optional<QRect> &anchor, const std::optional<QPoint> &cursor,
                       const std::optional<QPoint> &last_center) {
    const auto &available = screen.available;
    if (position == "beside_region" && anchor) {
        if (const auto placed = beside(*anchor, size, available)) return *placed;
    } else if (position == "near_cursor" && cursor && screen.geometry.contains(*cursor)) {
        return near_cursor(*cursor, size, available);
    } else if (position == "last_position" && last_center && screen.geometry.contains(*last_center)) {
        const auto top_left = *last_center - QRect(QPoint(0, 0), size).center();
        return QPoint(clamp(top_left.x(), available.left(), available.right() + 1 - size.width()),
                      clamp(top_left.y(), available.top(), available.bottom() + 1 - size.height()));
    }
    return available.center() - QRect(QPoint(0, 0), size).center();
}
}
