"""验证三级菜单不会在有空位时折回覆盖第一级。"""

from PySide6.QtCore import QPoint, QRect, QSize, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QMenu

from clipboard.ui.menus.submenu_position import avoid_submenu_overlap, submenu_position


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
        assert grandchild.geometry().right() < child.geometry().left()
        assert not grandchild.geometry().intersects(root.geometry())
    finally:
        grandchild.close()
        child.close()
        root.close()
        root.deleteLater()
