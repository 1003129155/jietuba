#pragma once
#include <QMenu>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <optional>
#include <vector>

namespace jietuba {
// Where a cascading submenu opens: preferably in screen space its ancestor menus do
// not cover. ancestors run from the direct parent to the root; with equal overlap
// the current opening direction continues.
//
// anchor is the parent's item that opens it (global coordinates). The submenu sits
// against that item, covering the parent's border and padding: with the cursor on
// that strip Qt closes the submenu after the system menu delay, which may be zero.
QPoint submenu_position(const QSize &size, int y, const std::vector<QRect> &ancestors, const QRect &screen, const std::optional<QRect> &anchor = std::nullopt);

// Installed once after the menu is assembled; keeps Qt's own hover, keyboard and close behaviour.
void avoid_submenu_overlap(QMenu *menu);
}
