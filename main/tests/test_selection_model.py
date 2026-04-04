# -*- coding: utf-8 -*-
"""
SelectionModel 选区模型单元测试

测试选区矩形的创建、拖拽、确认、归一化等逻辑。
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QRectF, QSizeF


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture
def model(qapp):
    from canvas.selection_model import SelectionModel
    return SelectionModel()


class TestSelectionModel:
    """SelectionModel 测试"""

    def test_initial_empty(self, model):
        """初始状态应为空"""
        assert model.is_empty()
        assert model.rect().isNull()
        assert not model.is_confirmed

    def test_set_rect(self, model):
        """设置选区矩形"""
        model.set_rect(QRectF(10, 20, 100, 200))
        rect = model.rect()
        assert rect.x() == 10
        assert rect.y() == 20
        assert rect.width() == 100
        assert rect.height() == 200

    def test_set_rect_min_size(self, model):
        """选区应不小于最小尺寸"""
        model.set_rect(QRectF(0, 0, 1, 1))
        rect = model.rect()
        assert rect.width() >= model.min_size.width()
        assert rect.height() >= model.min_size.height()

    def test_set_rect_emits_signal(self, model):
        """设置选区应触发 rectChanged 信号"""
        signals = []
        model.rectChanged.connect(lambda r: signals.append(r))
        model.set_rect(QRectF(0, 0, 50, 50))
        assert len(signals) == 1

    def test_set_same_rect_no_signal(self, model):
        """设置相同选区不应重复触发信号"""
        model.set_rect(QRectF(10, 10, 100, 100))
        signals = []
        model.rectChanged.connect(lambda r: signals.append(r))
        model.set_rect(QRectF(10, 10, 100, 100))
        assert len(signals) == 0

    def test_dragging(self, model):
        """拖拽状态管理"""
        assert not model.is_dragging
        model.start_dragging()
        assert model.is_dragging
        model.stop_dragging()
        assert not model.is_dragging

    def test_dragging_signal(self, model):
        """拖拽应触发 draggingChanged 信号"""
        signals = []
        model.draggingChanged.connect(lambda v: signals.append(v))
        model.start_dragging()
        model.stop_dragging()
        assert signals == [True, False]

    def test_double_start_dragging(self, model):
        """重复开始拖拽不应重复触发信号"""
        signals = []
        model.draggingChanged.connect(lambda v: signals.append(v))
        model.start_dragging()
        model.start_dragging()  # 第二次不应触发
        assert len(signals) == 1

    def test_activate_deactivate(self, model):
        """激活/取消"""
        model.activate()
        model.set_rect(QRectF(0, 0, 50, 50))
        assert model.is_active()
        model.deactivate()
        assert not model.is_active()
        assert model.is_empty()

    def test_confirm(self, model):
        """确认选区"""
        model.activate()
        model.set_rect(QRectF(0, 0, 100, 100))
        signals = []
        model.confirmed.connect(lambda r: signals.append(r))
        model.confirm()
        assert model.is_confirmed
        assert len(signals) == 1

    def test_confirm_inactive_noop(self, model):
        """未激活时确认不应触发信号"""
        signals = []
        model.confirmed.connect(lambda r: signals.append(r))
        model.confirm()
        assert not model.is_confirmed
        assert len(signals) == 0

    def test_normalize(self, model):
        """规范化负宽高的矩形"""
        # 创建一个负宽的矩形
        model.set_rect(QRectF(100, 100, -50, -50))
        model.normalize()
        rect = model.rect()
        assert rect.width() > 0
        assert rect.height() > 0

    def test_rect_returns_copy(self, model):
        """rect() 应返回副本"""
        model.set_rect(QRectF(10, 10, 100, 100))
        r1 = model.rect()
        r1.setWidth(999)
        r2 = model.rect()
        assert r2.width() == 100

    def test_initialize_confirmed_rect(self, model):
        """初始化已确认选区（钉图场景）"""
        model.initialize_confirmed_rect(QRectF(0, 0, 200, 200))
        assert model.is_confirmed
        assert not model.is_empty()
 