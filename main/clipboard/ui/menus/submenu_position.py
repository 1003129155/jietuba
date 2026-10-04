"""让级联子菜单优先使用没有被上层菜单占据的屏幕空间。"""

import weakref

from PySide6.QtCore import QEvent, QObject, QPoint, QRect, QSize
from PySide6.QtWidgets import QApplication, QMenu


def submenu_position(size: QSize, y: int, ancestors: list[QRect], screen: QRect) -> QPoint:
    """ancestors 从直接父菜单到根菜单排列；同等遮挡时延续展开方向。"""
    parent = ancestors[0]
    prefer_left = len(ancestors) > 1 and parent.center().x() < ancestors[1].center().x()
    directions = (-1, 1) if prefer_left else (1, -1)
    candidates = []
    for direction in directions:
        x = parent.left() - size.width() if direction < 0 else parent.right() + 1
        x = max(screen.left(), min(x, screen.right() + 1 - size.width()))
        top = max(screen.top(), min(y, screen.bottom() + 1 - size.height()))
        rect = QRect(QPoint(x, top), size)
        overlap = 0
        for ancestor in ancestors:
            intersection = rect.intersected(ancestor)
            overlap += intersection.width() * intersection.height()
        candidates.append((overlap, rect.topLeft()))
    return min(candidates, key=lambda candidate: candidate[0])[1]


class _SubmenuPositioner(QObject):
    def __init__(self, submenu: QMenu, parent_menu: QMenu):
        super().__init__(submenu)
        self._parent_menu = weakref.ref(parent_menu)

    def eventFilter(self, watched, event):
        if event.type() == QEvent.Type.Show:
            ancestors = []
            menu = self._parent_menu()
            while isinstance(menu, QMenu) and menu.isVisible():
                ancestors.append(menu.geometry())
                menu = menu.parentWidget()
            screen = QApplication.screenAt(ancestors[0].center()) if ancestors else None
            if screen is not None:
                watched.move(submenu_position(
                    watched.size(), watched.y(), ancestors, screen.availableGeometry(),
                ))
        return super().eventFilter(watched, event)


def avoid_submenu_overlap(menu: QMenu):
    """菜单组装完后安装一次，保留 Qt 原有的悬停、键盘和关闭行为。"""
    # 保留 QAction 的 Python 包装，避免 PySide 在遍历后回收其关联子菜单。
    menu._submenu_actions = menu.actions()
    for action in menu._submenu_actions:
        submenu = action.menu()
        if submenu is None:
            continue
        submenu._positioner = _SubmenuPositioner(submenu, menu)
        submenu.installEventFilter(submenu._positioner)
        avoid_submenu_overlap(submenu)
