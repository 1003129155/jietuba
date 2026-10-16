#pragma once
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <optional>
#include <vector>

// Placement of the text recognition window: positions only, no window access.
namespace jietuba::text_placement {
// Gap between the window and the recognized region, in pixels.
constexpr int GAP = 12;
// Distance between the window and the cursor when placed beside it, in pixels.
constexpr int CURSOR_OFFSET = 16;

// A screen's full and available geometry.
struct ScreenArea {
    QRect geometry;
    QRect available;
};

// The screen for the window. anchor is the recognized region (global QRect),
// last_center the window's centre when last closed. A point on no screen falls
// through to the next, finally to the primary screen.
ScreenArea pick_screen(const QString &position, const std::optional<QRect> &anchor, const std::optional<QPoint> &cursor, const std::optional<QPoint> &last_center,
                       const std::vector<ScreenArea> &screens, const ScreenArea &primary);

// The window's top-left on screen (from pick_screen). beside_region tries right, left,
// below and above the region; near_cursor goes below right of the cursor, flipping
// where it does not fit; last_position puts the centre back where it was. When the
// condition fails (no region, cursor or record on screen, or no side fits) the
// window is centred in the screen's available area.
QPoint window_position(const QString &position, const QSize &size, const ScreenArea &screen, const std::optional<QRect> &anchor = std::nullopt,
                       const std::optional<QPoint> &cursor = std::nullopt, const std::optional<QPoint> &last_center = std::nullopt);
}
