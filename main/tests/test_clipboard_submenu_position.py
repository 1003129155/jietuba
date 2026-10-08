"""验证三级菜单不会在有空位时折回覆盖第一级。"""

import pytest
from PySide6.QtCore import QPoint, QRect, QSize, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QMenu, QProxyStyle, QStyle, QWidget

from clipboard.ui.menus.submenu_position import avoid_submenu_overlap, submenu_position


class _ZeroMenuDelayStyle(QProxyStyle):
    """系统菜单延迟设成 0 时，Qt 子菜单的弹出和收起计时都是 0。"""

    def styleHint(self, hint, option=None, widget=None, return_data=None):
        if hint in (QStyle.StyleHint.SH_Menu_SubMenuPopupDelay,
                    QStyle.StyleHint.SH_Menu_SubMenuSloppyCloseTimeout):
            return 0
        return super().styleHint(hint, option, widget, return_data)


def test_third_level_continues_left_instead_of_covering_root():
    root = QRect(600, 100, 200, 400)
    parent = QRect(400, 200, 200, 120)
    pos = submenu_position(QSize(180, 160), 200, [parent, root], QRect(0, 0, 1000, 800))
    assert pos == QPoint(220, 200)


def test_third_level_continues_right_when_space_is_free():
    root = QRect(100, 100, 200, 400)
    parent = QRect(300, 200, 200, 120)
    pos = submenu_position(QSize(180, 160), 200, [parent, root], QRect(0, 0, 1000, 800))
    assert pos == QPoint(500, 200)


def test_screen_edge_chooses_side_with_less_overlap():
    root = QRect(200, 100, 200, 400)
    parent = QRect(400, 200, 200, 120)
    # 右侧仅剩 150 像素：靠边展开只遮住父菜单 30 像素，优于回折覆盖根菜单。
    pos = submenu_position(QSize(180, 160), 200, [parent, root], QRect(0, 0, 750, 800))
    assert pos == QPoint(570, 200)


def test_negative_screen_coordinates_and_bottom_edge():
    parent = QRect(-250, 600, 200, 120)
    pos = submenu_position(QSize(180, 200), 650, [parent], QRect(-1000, 0, 1000, 800))
    assert pos == QPoint(-430, 600)


def test_submenu_attaches_to_action_edge():
    screen = QRect(0, 0, 1000, 800)
    parent = QRect(300, 200, 200, 120)
    action = QRect(305, 230, 190, 30)
    assert submenu_position(QSize(180, 160), 230, [parent], screen, action) == QPoint(495, 230)

    root = QRect(500, 100, 200, 400)
    assert submenu_position(QSize(180, 160), 230, [parent, root], screen, action) == QPoint(125, 230)


@pytest.mark.parametrize("opens_left", [False, True])
def test_submenu_stays_open_over_parent_padding_with_zero_menu_delay(qapp, opens_left):
    style = _ZeroMenuDelayStyle()
    host = QWidget()
    host.setStyle(style)
    host.setStyleSheet("QMenu { border: 1px solid #888888; padding: 4px; }")
    root = QMenu(host)
    sub = root.addMenu('Theme')
    sub.addAction('Light')
    avoid_submenu_overlap(root)
    screen = qapp.primaryScreen().availableGeometry()
    x = screen.right() - root.sizeHint().width() if opens_left else screen.left() + 40
    try:
        root.popup(QPoint(x, screen.top() + 40))
        rect = root.actionGeometry(sub.menuAction())
        window = root.windowHandle()
        for dx in range(8):
            QTest.mouseMove(window, QPoint(rect.center().x() + dx, rect.center().y()))
        assert sub.isVisible()
        assert (sub.geometry().center().x() < root.geometry().center().x()) == opens_left

        # 菜单项和父菜单外沿之间的边框与内边距
        padding_x = rect.left() - 2 if opens_left else rect.right() + 2
        QTest.mouseMove(window, QPoint(padding_x, rect.center().y()))
        QTest.qWait(20)
        assert sub.isVisible()
    finally:
        sub.close()
        root.close()
        host.deleteLater()


def test_real_submenu_show_uses_free_side(qapp):
    screen = qapp.primaryScreen().availableGeometry()
    root = QMenu()
    child = root.addMenu('Multi-Select Paste')
    grandchild = child.addMenu('Separator')
    grandchild.addAction('New Line')
    avoid_submenu_overlap(root)
    try:
        root.popup(QPoint(screen.right() - root.sizeHint().width(), screen.top() + 40))
        root.setActiveAction(child.menuAction())
        QTest.keyClick(root, Qt.Key.Key_Right)
        child.setActiveAction(grandchild.menuAction())
        QTest.keyClick(child, Qt.Key.Key_Right)
        qapp.processEvents()
        assert child.isVisible() and grandchild.isVisible()
        assert grandchild.geometry().left() < child.geometry().left()
        assert not grandchild.geometry().intersects(root.geometry())
    finally:
        grandchild.close()
        child.close()
        root.close()
        root.deleteLater()
