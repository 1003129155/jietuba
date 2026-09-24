# -*- coding: utf-8 -*-

"""Shared size metrics for the clipboard management dialog."""

from core.ui_scale import get_dialog_scale

MANAGE_DIALOG_BASE_WIDTH = 900
MANAGE_DIALOG_BASE_HEIGHT = 600

# 打开时的默认尺寸，与上面的设计稿基准分开：基准只用来推算下面的外框换算比例。
MANAGE_DIALOG_DEFAULT_WIDTH = 1160
MANAGE_DIALOG_DEFAULT_HEIGHT = 760

MANAGE_DIALOG_MIN_WIDTH_BASE = 960
MANAGE_DIALOG_MIN_HEIGHT_BASE = 600

# 默认尺寸最多占所在屏幕可用区域的比例，小屏上底部按钮不能被挤出屏幕。
_SCREEN_FILL = 0.92

# 900x600 是设计稿基准，+20/+120 是实际窗口比设计稿多出的外框留白；
# 二者的比值是固定的"设计稿 -> 实际窗口"换算比例，和用户的窗口缩放设置无关。
_CHROME_SCALE_X = (MANAGE_DIALOG_BASE_WIDTH + 20) / MANAGE_DIALOG_BASE_WIDTH
_CHROME_SCALE_Y = (MANAGE_DIALOG_BASE_HEIGHT + 120) / MANAGE_DIALOG_BASE_HEIGHT


def _dialog_factor() -> float:
    """实时读取设置里的窗口缩放比例。取成函数而不是模块级常量，
    是因为常量只在模块第一次被 import 时算一次，之后用户改设置也不会再变。"""
    return get_dialog_scale().factor


def _scale(value: int, factor: float) -> int:
    if value == 0:
        return 0
    return max(1, round(value * factor))


def manage_dialog_width() -> int:
    return round(MANAGE_DIALOG_DEFAULT_WIDTH * _dialog_factor())


def manage_dialog_height() -> int:
    return round(MANAGE_DIALOG_DEFAULT_HEIGHT * _dialog_factor())


def manage_dialog_min_width() -> int:
    return round(MANAGE_DIALOG_MIN_WIDTH_BASE * _dialog_factor())


def manage_dialog_min_height() -> int:
    return round(MANAGE_DIALOG_MIN_HEIGHT_BASE * _dialog_factor())


def fit_manage_dialog_size(available_width: int, available_height: int) -> tuple:
    """默认尺寸按屏幕可用区域收缩，但不小于最小尺寸。"""
    width = min(manage_dialog_width(), int(available_width * _SCREEN_FILL))
    height = min(manage_dialog_height(), int(available_height * _SCREEN_FILL))
    return max(width, manage_dialog_min_width()), max(height, manage_dialog_min_height())


def scale_x(value: int) -> int:
    return _scale(value, _CHROME_SCALE_X * _dialog_factor())


def scale_y(value: int) -> int:
    return _scale(value, _CHROME_SCALE_Y * _dialog_factor())


def scale_ui(value: int) -> int:
    """字号、圆角、内边距。不乘设计稿换算比例：那是窗口外框的留白，
    乘上去字会比其他窗口大一圈。"""
    return _scale(value, _dialog_factor())
