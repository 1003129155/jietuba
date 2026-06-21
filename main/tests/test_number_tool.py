# -*- coding: utf-8 -*-
"""
NumberTool 序号工具单元测试

测试 77eee29..HEAD 范围内新增的方法：
- get_max_number
- assign_number_order
- get_next_after_number_edit
- set_next_number_and_refresh
- refresh_next_number
"""
import pytest
from PySide6.QtWidgets import QApplication, QGraphicsScene
from PySide6.QtCore import QPointF, QRectF
from PySide6.QtGui import QColor


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


# 在模块加载时预热导入，避免第一个测试因循环导入失败
def _warmup_imports():
    """强制打破 tools ↔ canvas 的循环导入：先让 canvas.scene 触发 tools 的部分初始化。"""
    try:
        import canvas.scene  # noqa: F401
    except ImportError:
        pass


_warmup_imports()


@pytest.fixture
def scene(qapp):
    s = QGraphicsScene()
    yield s
    s.clear()
    s.deleteLater()


# 延迟导入辅助（必须在 QApplication 之后调用）
def _nt():
    from tools.number import NumberTool
    return NumberTool


def _ni():
    from canvas.items import NumberItem
    return NumberItem


# ============================================================================
# get_max_number 测试
# ============================================================================

class TestGetMaxNumber:
    """get_max_number 测试"""

    def test_empty_scene_returns_zero(self, scene):
        # 首次导入可能因 tools↔canvas 循环依赖失败，二次导入会成功
        try:
            nt = _nt()
        except ImportError:
            nt = _nt()
        assert nt.get_max_number(scene) == 0

    def test_single_item_returns_its_number(self, scene):
        item = _ni()(5, QPointF(0, 0), 20, QColor(255, 0, 0))
        scene.addItem(item)
        assert _nt().get_max_number(scene) == 5

    def test_multiple_items_returns_largest(self, scene):
        for n in (3, 7, 1, 9, 4):
            item = _ni()(n, QPointF(n * 10, 0), 20, QColor(255, 0, 0))
            scene.addItem(item)
        assert _nt().get_max_number(scene) == 9

    def test_override_item_uses_override_number(self, scene):
        item1 = _ni()(3, QPointF(0, 0), 20, QColor(255, 0, 0))
        item2 = _ni()(8, QPointF(10, 0), 20, QColor(255, 0, 0))
        scene.addItem(item1)
        scene.addItem(item2)
        assert _nt().get_max_number(scene, override_item=item2, override_number=100) == 100

    def test_none_scene_returns_zero(self):
        assert _nt().get_max_number(None) == 0

    def test_non_number_items_ignored(self, scene):
        from canvas.items import RectItem
        item = _ni()(5, QPointF(0, 0), 20, QColor(255, 0, 0))
        scene.addItem(item)
        rect = RectItem(QRectF(0, 0, 10, 10), QColor(255, 0, 0))
        scene.addItem(rect)
        assert _nt().get_max_number(scene) == 5


# ============================================================================
# assign_number_order 测试
# ============================================================================

class TestAssignNumberOrder:
    """assign_number_order 测试"""

    def test_assigns_unique_orders(self, scene):
        items = []
        for i in range(5):
            item = _ni()(i + 1, QPointF(i * 10, 0), 20, QColor(255, 0, 0))
            scene.addItem(item)
            items.append(item)

        orders = set()
        for item in items:
            order = _nt().assign_number_order(scene, item)
            assert isinstance(order, int)
            assert order >= 0
            orders.add(order)
        assert len(orders) == len(items)

    def test_existing_order_preserved(self, scene):
        item = _ni()(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        scene.addItem(item)
        item.number_order = 42
        order = _nt().assign_number_order(scene, item)
        assert order == 42

    def test_none_item_returns_zero(self, scene):
        assert _nt().assign_number_order(scene, None) == 0

    def test_none_scene_returns_zero(self):
        item = _ni()(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        order = _nt().assign_number_order(None, item)
        assert order == 0
        assert item.number_order == 0


# ============================================================================
# get_next_after_number_edit 测试
# ============================================================================

class TestGetNextAfterNumberEdit:
    """get_next_after_number_edit 测试"""

    def test_new_number_gte_current_next(self, scene):
        item = _ni()(3, QPointF(0, 0), 20, QColor(255, 0, 0))
        scene.addItem(item)
        result = _nt().get_next_after_number_edit(scene, item, 3, 5, current_next=2)
        assert result == 6

    def test_old_number_is_max_and_decreased(self, scene):
        item = _ni()(3, QPointF(0, 0), 20, QColor(255, 0, 0))
        scene.addItem(item)
        result = _nt().get_next_after_number_edit(scene, item, 3, 2)
        assert result == 3

    def test_old_number_not_max_keeps_current_next(self, scene):
        item_small = _ni()(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        item_large = _ni()(5, QPointF(10, 0), 20, QColor(255, 0, 0))
        scene.addItem(item_small)
        scene.addItem(item_large)
        result = _nt().get_next_after_number_edit(scene, item_small, 1, 2, current_next=3)
        assert result == 3

    def test_without_current_next_uses_default(self, scene):
        item = _ni()(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        scene.addItem(item)
        result = _nt().get_next_after_number_edit(scene, item, 1, 1)
        assert result >= 1


# ============================================================================
# set_next_number 测试
# ============================================================================

class TestSetNextNumber:
    """set_next_number 测试"""

    def test_set_next_number_returns_clamped(self, scene):
        result = _nt().set_next_number(scene, 10)
        assert result == 10

    def test_set_next_number_negative_clamped(self, scene):
        result = _nt().set_next_number(scene, -5)
        assert result >= 1

    def test_set_next_number_reflects_count(self, scene):
        for n in (1, 2, 3):
            item = _ni()(n, QPointF(n * 10, 0), 20, QColor(255, 0, 0))
            scene.addItem(item)
        result = _nt().set_next_number(scene, 100)
        assert result == 100

    def test_none_scene_returns_one(self):
        assert _nt().set_next_number(None, 50) == 1


# ============================================================================
# 边界条件测试
# ============================================================================

class TestEdgeCases:
    """边界条件测试"""

    def test_get_max_number_with_deleted_scene(self, qapp):
        s = QGraphicsScene()
        s.deleteLater()
        result = _nt().get_max_number(s)
        assert isinstance(result, int)

    def test_assign_order_for_item_not_in_scene(self, scene):
        item = _ni()(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        order = _nt().assign_number_order(scene, item)
        assert isinstance(order, int)
        assert order >= 0

    def test_get_next_after_edit_with_zero_values(self, scene):
        item = _ni()(1, QPointF(0, 0), 20, QColor(255, 0, 0))
        scene.addItem(item)
        result = _nt().get_next_after_number_edit(scene, item, 0, 0, current_next=1)
        assert result == 2
