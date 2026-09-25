# -*- coding: utf-8 -*-
"""放大镜的显示开关与取色复制格式

总开关、像素网格线、快捷键提示行都存在 app/ 配置里，而截图窗口是跨会话复用的：
同一个 MagnifierOverlay 实例会横跨用户改设置的前后，所以每个新会话（rebind）
都要重读一次，不能只在 __init__ 里读。这里把「读得到」和「读得新」都锁住。
"""
import json
from dataclasses import replace
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QPointF, QRect, QRectF
from PySide6.QtGui import QColor, QImage
from PySide6.QtWidgets import QWidget

from settings import color_formats
from ui.magnifier import MagnifierOverlay

_BACKDROP = QColor("#3366AA")
_PICKED = QColor("#FF4081")


class _Config:
    """只提供放大镜要用的两个读取入口。"""

    def __init__(self, formats=None, **values):
        self.values = {
            "magnifier_enabled": True,
            "magnifier_grid": False,
            "magnifier_hint": True,
        }
        self.values.update(values)
        if formats is not None:
            self.values[color_formats.SETTING_KEY] = json.dumps([
                {"name": f.name, "enabled": f.enabled}
                for f in formats
            ])
        self.qsettings = SimpleNamespace(value=lambda key, default, type=None: default)

    def get_app_setting(self, key, default=None):
        return self.values.get(key, default)


@pytest.fixture
def parent(qapp):
    widget = QWidget()
    yield widget
    widget.deleteLater()


def _magnifier(parent, formats=None, **values):
    return MagnifierOverlay(parent, scene=None, view=None,
                            config_manager=_Config(formats, **values))


def _only(*names):
    """只启用这几个内置格式，顺序照给的来。"""
    by_name = {f.name: f for f in color_formats.default_formats()}
    chosen = [replace(by_name[name], enabled=True) for name in names]
    rest = [replace(f, enabled=False) for f in by_name.values() if f.name not in names]
    return chosen + rest


def test_disabled_magnifier_never_renders(parent):
    """总开关关掉后连渲染判断都不进，省掉整条采样链路。"""
    magnifier = _magnifier(parent, magnifier_enabled=False)
    magnifier.cursor_scene_pos = QPointF(10, 10)
    magnifier.scene = object()
    magnifier.view = object()
    assert magnifier._should_render() is False


def test_hint_line_shortens_the_widget(parent):
    """提示行关掉后信息区少一行，浮层整体变矮。"""
    with_hint = _magnifier(parent, formats=_only("RGB"), magnifier_hint=True)
    without_hint = _magnifier(parent, formats=_only("RGB"), magnifier_hint=False)
    assert without_hint.combined_height == (
        with_hint.combined_height - MagnifierOverlay.INFO_LINE_HEIGHT
    )
    assert without_hint.height() == without_hint.combined_height


def test_new_session_picks_up_changed_options(parent):
    """设置是在上一次截图结束后改的，rebind 时要重读，而不是沿用建实例时的值。"""
    magnifier = _magnifier(parent, formats=_only("RGB"), magnifier_hint=True)
    tall = magnifier.combined_height

    magnifier.config_manager.values["magnifier_hint"] = False
    magnifier.config_manager.values["magnifier_grid"] = False
    magnifier.rebind(scene=None, view=None)

    assert magnifier._show_grid is False
    assert magnifier.combined_height < tall
    assert magnifier.height() == magnifier.combined_height


class _Painter:
    """只记录画了哪些线，不真的绘制。"""

    def __init__(self):
        self.lines = []

    def setPen(self, _pen):
        pass

    def drawLine(self, x1, y1, x2, y2):
        self.lines.append((x1, y1, x2, y2))


def test_grid_follows_the_source_rect_actually_drawn(parent):
    """网格按 drawImage 用的源矩形分格，而不是按理论倍率——取整后两者会差半格。"""
    magnifier = _magnifier(parent)
    painter = _Painter()
    magnifier._draw_pixel_grid(painter, QRect(0, 0, 150, 120), QRect(0, 0, 10, 8))

    verticals = {line[0] for line in painter.lines if line[0] == line[2]}
    horizontals = {line[1] for line in painter.lines if line[1] == line[3]}
    # 十格之间九条竖线、八行之间七条横线，四周边框由外框描边负责，这里不重复画
    assert len(verticals) == 9
    assert len(horizontals) == 7
    assert min(verticals) == 15    # 150 / 10
    assert min(horizontals) == 15  # 120 / 8


def test_grid_is_skipped_when_cells_get_too_small(parent):
    """格子小到几个像素时，网格线自己就把像素盖住了，不如不画。"""
    magnifier = _magnifier(parent)
    painter = _Painter()
    magnifier._draw_pixel_grid(painter, QRect(0, 0, 150, 120), QRect(0, 0, 100, 80))
    assert painter.lines == []


def _bind_to_a_drawable_session(magnifier):
    """给放大镜配一套刚好能走完 paintEvent 的 scene/view。

    _should_render 会逐项检查这些状态，缺一项就直接不画——那样下面的断言会
    在「没画网格」上假通过，所以这里一次性配齐，再用开/关两种设置做对照。
    """
    image = QImage(64, 64, QImage.Format.Format_ARGB32)
    image.fill(_BACKDROP)
    # 只有光标底下那一格是另一种颜色：取色块画的就是它，放大图右上角显示的
    # 则是光标右上方的背景色，两处一比就知道色块画在了哪。
    image.setPixelColor(32, 32, _PICKED)
    magnifier.scene = SimpleNamespace(
        background=SimpleNamespace(image=lambda: image),
        scene_rect=QRectF(0, 0, 64, 64),
        selection_model=SimpleNamespace(is_confirmed=False),
        tool_controller=SimpleNamespace(current_tool_id="cursor"),
    )
    magnifier.view = SimpleNamespace(
        drawing=SimpleNamespace(active=False),
        text_drag=SimpleNamespace(active=False),
        smart_edit_controller=SimpleNamespace(
            layer_editor=SimpleNamespace(dragging_handle=False)
        ),
    )
    magnifier.cursor_scene_pos = QPointF(32, 32)
    assert magnifier._should_render(), "这套 scene/view 配不齐，下面的断言会假通过"


@pytest.mark.parametrize("grid_on", [True, False])
def test_grid_switch_decides_whether_it_is_drawn(parent, monkeypatch, grid_on):
    magnifier = _magnifier(parent, magnifier_grid=grid_on)
    _bind_to_a_drawable_session(magnifier)

    calls = []
    monkeypatch.setattr(magnifier, "_draw_pixel_grid",
                        lambda *args: calls.append(args))
    magnifier.grab()   # 触发一次真实的 paintEvent
    assert bool(calls) is grid_on


def test_swatch_sits_in_the_value_row_not_on_the_magnified_view(parent):
    """取色色块和颜色值同一行、一直显示；放大图右上角不再画它。"""
    magnifier = _magnifier(parent)
    _bind_to_a_drawable_session(magnifier)

    rendered = magnifier.grab().toImage()
    assert rendered.pixelColor(magnifier.MAG_WIDTH - 14, 14) != _PICKED
    info_pixels = (
        rendered.pixelColor(x, y)
        for y in range(magnifier.MAG_HEIGHT, rendered.height())
        for x in range(rendered.width())
    )
    assert any(pixel == _PICKED for pixel in info_pixels)


def test_shift_hint_only_shows_when_there_is_something_to_switch_to(parent):
    """只勾了一个格式时 Shift 什么也不做，提示里不该出现它。"""
    one = _magnifier(parent, formats=_only("RGB"))
    two = _magnifier(parent, formats=_only("RGB", "HEX"))
    assert one._hint_texts() == ["C: Copy color value"]
    assert two._hint_texts() == ["Shift: Switch color format", "C: Copy color value"]
    assert two.combined_height - one.combined_height == MagnifierOverlay.INFO_LINE_HEIGHT


class TestColorFormats:
    """放大镜同一时间只显示一个格式，默认是列表里第一个启用的。"""

    SAMPLE = QColor(230, 153, 60)

    def _pick_sample(self, magnifier):
        _bind_to_a_drawable_session(magnifier)
        magnifier.cursor_scene_pos = QPointF(32, 32)   # 只有这一格是探针色
        magnifier.scene.background.image().setPixelColor(32, 32, self.SAMPLE)

    def test_copy_uses_the_first_enabled_format(self, parent):
        magnifier = _magnifier(parent, formats=_only("CSS rgb()", "HEX"))
        self._pick_sample(magnifier)
        assert magnifier.get_color_info_text() == "rgb(230, 153, 60)"

    def test_reordering_changes_the_default_shown_format(self, parent):
        """把哪个格式拖到最前，循环从哪个开始——这正是列表顺序的意义。"""
        magnifier = _magnifier(parent, formats=_only("HEX", "CSS rgb()"))
        self._pick_sample(magnifier)
        assert magnifier.get_color_info_text() == "#E6993C"

    def test_enabling_more_formats_does_not_change_the_height(self, parent):
        """同一时间只显示一个格式（Shift 循环切换），高度不再跟着勾选条数走。"""
        one = _magnifier(parent, formats=_only("RGB"), magnifier_hint=False)
        three = _magnifier(parent, formats=_only("RGB", "HEX", "CSS hsl()"),
                           magnifier_hint=False)
        assert three.combined_height == one.combined_height

    def test_shift_cycles_to_the_next_enabled_format(self, parent):
        magnifier = _magnifier(parent, formats=_only("RGB", "HEX", "CSS hsl()"))
        self._pick_sample(magnifier)
        assert magnifier.get_color_info_text() == "230, 153, 60"

        magnifier.cycle_color_format()
        assert magnifier.get_color_info_text() == "#E6993C"

        magnifier.cycle_color_format()
        assert magnifier.get_color_info_text() == "hsl(32, 77%, 57%)"

        magnifier.cycle_color_format()   # 循环回到第一个
        assert magnifier.get_color_info_text() == "230, 153, 60"

    def test_shift_is_a_no_op_with_only_one_format_enabled(self, parent):
        magnifier = _magnifier(parent, formats=_only("HEX"))
        self._pick_sample(magnifier)
        magnifier.cycle_color_format()
        assert magnifier.get_color_info_text() == "#E6993C"

    def test_new_session_picks_up_edited_formats(self, parent):
        """格式是在设置窗口里改的，下一次截图（rebind）就该按新的显示。"""
        magnifier = _magnifier(parent, formats=_only("RGB"))
        self._pick_sample(magnifier)
        assert magnifier.get_color_info_text() == "230, 153, 60"

        magnifier.config_manager.values[color_formats.SETTING_KEY] = json.dumps([
            {"name": f.name, "enabled": f.enabled}
            for f in _only("CSS hsl()")
        ])
        magnifier.rebind(scene=None, view=None)
        self._pick_sample(magnifier)
        assert magnifier.get_color_info_text() == "hsl(32, 77%, 57%)"
