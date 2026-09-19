# -*- coding: utf-8 -*-
"""「翻译中…」后面循环的点。

抽成共用之前，这个动画只长在快捷键小窗口上，大对话框是一行静止的文字——
同一个应用两种表现。所以这里除了测动画本身，更要紧的是钉住「两个入口行为一致」。
"""
import time

import pytest
from PySide6.QtCore import QCoreApplication

from translation.ui.widgets import EllipsisAnimator


def _spin(ms):
    deadline = time.monotonic() + ms / 1000
    while time.monotonic() < deadline:
        QCoreApplication.processEvents()
        time.sleep(0.004)


# ============================================================================
# 动画本身
# ============================================================================

def test_dots_cycle_from_zero_to_three(qapp):
    seen = []
    anim = EllipsisAnimator(on_tick=seen.append)
    anim.start()
    _spin(EllipsisAnimator.INTERVAL_MS * 5 + 120)
    anim.stop()

    # 至少转满一圈
    assert len(seen) >= 4
    dot_counts = [t.count(".") for t in seen[:4]]
    assert dot_counts == [1, 2, 3, 0]


def test_base_text_keeps_no_trailing_dots(qapp):
    """各语言省略号不同（中文…、英文...），直接往后加会拼出「翻译中….」。"""
    for step in range(EllipsisAnimator.STEPS):
        text = EllipsisAnimator.text_for(step)
        stripped = text.rstrip(".")
        assert not stripped.endswith("…")
        assert not stripped.endswith("。")
        assert text.count(".") == step


def test_stop_halts_the_ticks(qapp):
    seen = []
    anim = EllipsisAnimator(on_tick=seen.append)
    anim.start()
    _spin(EllipsisAnimator.INTERVAL_MS + 80)
    anim.stop()
    count = len(seen)
    _spin(EllipsisAnimator.INTERVAL_MS * 2)

    assert not anim.is_running()
    assert len(seen) == count


def test_restart_begins_from_zero(qapp):
    """连着翻译两次，第二次不该接着上次的相位。"""
    seen = []
    anim = EllipsisAnimator(on_tick=seen.append)
    anim.start()
    _spin(EllipsisAnimator.INTERVAL_MS * 2 + 80)
    anim.stop()
    seen.clear()

    anim.start()
    _spin(EllipsisAnimator.INTERVAL_MS + 80)
    anim.stop()

    assert seen[0].count(".") == 1


def test_missing_callback_is_not_an_error(qapp):
    anim = EllipsisAnimator()
    anim.start()
    _spin(EllipsisAnimator.INTERVAL_MS + 80)   # 不该抛
    anim.stop()


# ============================================================================
# 两个入口必须一致
# ============================================================================

@pytest.fixture
def popup(qapp):
    from translation.translation_popup import TranslationPopup

    widget = TranslationPopup()
    widget.set_theme("dark")
    yield widget
    widget._loading_dots.stop()
    widget.deleteLater()


@pytest.fixture
def dialog(qapp):
    from translation.translation_dialog import TranslationDialog

    widget = TranslationDialog()
    yield widget
    widget._loading_dots.stop()
    widget.deleteLater()


def test_both_entry_points_animate_the_same_way(popup, dialog):
    """大窗口以前根本没有这个动画，用户会直接看出两边不一样。"""
    popup._enter_loading()
    dialog.set_loading()

    popup_frames, dialog_frames = [], []
    for _ in range(4):
        _spin(EllipsisAnimator.INTERVAL_MS + 40)
        popup_frames.append(popup.result_edit.toPlainText())
        dialog_frames.append(dialog.target_edit.toPlainText())

    popup._loading_dots.stop()
    dialog._loading_dots.stop()

    assert popup_frames == dialog_frames


def test_dialog_stops_animating_once_the_result_lands(dialog):
    dialog.set_loading()
    _spin(EllipsisAnimator.INTERVAL_MS + 80)
    assert dialog._loading_dots.is_running()

    dialog.set_translation_result("你好", "en")
    _spin(EllipsisAnimator.INTERVAL_MS * 2)

    assert not dialog._loading_dots.is_running()
    # 动画残帧不能盖掉译文
    assert dialog.target_edit.toPlainText() == "你好"


def test_dialog_stops_animating_on_error(dialog):
    dialog.set_loading()
    _spin(EllipsisAnimator.INTERVAL_MS + 80)

    dialog.set_translation_error("boom")
    _spin(EllipsisAnimator.INTERVAL_MS * 2)

    assert not dialog._loading_dots.is_running()
    assert "boom" in dialog.target_edit.toPlainText()


def test_neither_entry_point_keeps_its_own_copy():
    """再抄一份的话，下次有第三个入口就是第三份。"""
    import inspect

    from translation import translation_dialog, translation_popup

    for module in (translation_popup, translation_dialog):
        source = inspect.getsource(module)
        assert "EllipsisAnimator" in source, module.__name__
        # 本地的点计数实现必须已经删干净
        assert "_loading_step" not in source, module.__name__
        assert "_advance_loading" not in source, module.__name__
