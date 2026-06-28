# -*- coding: utf-8 -*-

"""Shared size metrics for the clipboard management dialog."""

MANAGE_DIALOG_BASE_WIDTH = 900
MANAGE_DIALOG_BASE_HEIGHT = 600

MANAGE_DIALOG_WIDTH = MANAGE_DIALOG_BASE_WIDTH + 20
MANAGE_DIALOG_HEIGHT = MANAGE_DIALOG_BASE_HEIGHT + 120

MANAGE_DIALOG_MIN_WIDTH = 820
MANAGE_DIALOG_MIN_HEIGHT = 580

MANAGE_SCALE_X = MANAGE_DIALOG_WIDTH / MANAGE_DIALOG_BASE_WIDTH
MANAGE_SCALE_Y = MANAGE_DIALOG_HEIGHT / MANAGE_DIALOG_BASE_HEIGHT
MANAGE_SCALE_UI = (MANAGE_SCALE_X + MANAGE_SCALE_Y) / 2


def _scale(value: int, factor: float) -> int:
    if value == 0:
        return 0
    return max(1, round(value * factor))


def scale_x(value: int) -> int:
    return _scale(value, MANAGE_SCALE_X)


def scale_y(value: int) -> int:
    return _scale(value, MANAGE_SCALE_Y)


def scale_ui(value: int) -> int:
    return _scale(value, MANAGE_SCALE_UI)
