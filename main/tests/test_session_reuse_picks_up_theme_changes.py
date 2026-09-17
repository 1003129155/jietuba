# -*- coding: utf-8 -*-
"""截图窗口跨会话复用时，遮罩/放大镜要能跟上运行期改过的主题色。

MaskOverlayWidget/MagnifierOverlay 都只在 __init__ 里读一次主题色，缓存成
QColor/QPen/QBrush 供每帧直接用，不必每次重新构造。但截图窗口本身是跨会话
复用的（同一个 widget 实例会经历"用户在设置里改了主题色"这件事，而不是随着
改动重新创建），如果窗口复用、新会话开始时（rebind_model / rebind）不重新读
一遍，颜色就会停在窗口第一次创建时的旧值，直到重启应用才会更新。
"""
import pytest
from PySide6.QtGui import QColor
from PySide6.QtWidgets import QApplication, QWidget


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture(autouse=True)
def reset_theme_singleton(qapp):
    """ThemeManager 是真单例（__new__ 返回同一个实例），测试之间必须隔离，
    否则这个文件改过的主题色会泄漏给其他测试文件。"""
    from core.theme import ThemeManager
    ThemeManager._instance = None
    ThemeManager._config_manager = None
    ThemeManager._theme_color = None
    ThemeManager._mask_color = None
    yield
    ThemeManager._instance = None
    ThemeManager._config_manager = None
    ThemeManager._theme_color = None
    ThemeManager._mask_color = None


def test_mask_overlay_picks_up_a_changed_mask_color_on_session_reuse(qapp):
    from core.theme import get_theme
    from canvas.selection_model import SelectionModel
    from ui.mask_overlay import MaskOverlayWidget

    theme = get_theme()
    theme.set_mask_color(QColor(0, 0, 0))  # 存进去时 alpha 固定为 120

    parent = QWidget()
    model_a = SelectionModel()
    overlay = MaskOverlayWidget(parent, model_a)
    assert overlay._mask_color.getRgb() == (0, 0, 0, 120)

    # 会话之间：用户在设置里换了遮罩色
    theme.set_mask_color(QColor(0, 0, 255))

    # 窗口复用、新会话开始：rebind_model 是这个时刻唯一固定会被调用的入口
    model_b = SelectionModel()
    overlay.rebind_model(model_b)

    assert overlay._mask_color.getRgb() == (0, 0, 255, 120), \
        "rebind_model 之后应该反映新的遮罩色，而不是窗口第一次创建时的旧值"


def test_magnifier_picks_up_a_changed_theme_color_on_session_reuse(qapp):
    from core.theme import get_theme
    from ui.magnifier import MagnifierOverlay

    theme = get_theme()
    theme.set_theme_color(QColor("#40E0D0"))

    parent = QWidget()
    overlay = MagnifierOverlay(parent, scene=None, view=None)
    assert overlay._pen_teal_2.color().name() == "#40e0d0"
    assert overlay._brush_crosshair.color().red() == QColor("#40E0D0").red()

    # 会话之间：用户在设置里换了主题色
    theme.set_theme_color(QColor("#FF0000"))

    # 窗口复用、新会话开始：rebind 是这个时刻唯一固定会被调用的入口
    overlay.rebind(scene=None, view=None)

    assert overlay._pen_teal_2.color().name() == "#ff0000", \
        "rebind 之后画笔颜色应该跟上新主题色，而不是构造时缓存的旧值"
    assert overlay._pen_teal_1.color().name() == "#ff0000"
    assert overlay._brush_crosshair.color().red() == 255
    assert overlay._brush_crosshair.color().alpha() == 120, "半透明色带的 alpha 不该被主题色覆盖"
    # 非主题色的缓存不该被这次刷新误伤
    assert overlay._pen_white_2.color() == QColor(255, 255, 255)
