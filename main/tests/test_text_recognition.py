# -*- coding: utf-8 -*-
"""文字识别测试

文字图现场画，不依赖图片素材。覆盖四层：识别线程的四种结局与线程登记表的进出、结果
窗口的三种状态与排版、置顶、截图工具栏「文字识别」从按钮到弹窗的路径、以及排布升级后
新按钮的位置。

线程这块是重点：识别跑在后台，窗口可能先关、进程可能先退，这两条路径都单独测。
"""
import time
from types import SimpleNamespace
from unittest.mock import MagicMock

import pytest
from PySide6.QtCore import QEvent, QObject, QPoint, QRect, QRectF, Qt, Slot
from PySide6.QtGui import QColor, QFont, QFontDatabase, QImage, QPainter
from PySide6.QtWidgets import QApplication

import text_recognition
from text_recognition import TextRecognitionWindow, recognize_async, shutdown_recognition
from text_recognition import result_window
from settings import get_tool_settings_manager
from text_recognition.placement import CURSOR_OFFSET, GAP
from text_recognition.recognizer import FAILED, MODEL_BROKEN, NO_MODEL, NO_TEXT, UNAVAILABLE, _running
from tools.action import ActionTools
from ui.toolbar import Toolbar
from ui.toolbar_layout import MORE, SHOW, normalize_layout


# 测试跑在 offscreen 平台上，那里一个字体都没有，按字体名取到的只会是画成方框的豆腐块，
# 真引擎当然一个字也读不出来。所以直接把系统字体文件装进 Qt，再按它画字。
_FONT_FILE = "C:/Windows/Fonts/arial.ttf"


@pytest.fixture(scope="module")
def font_family(qapp):
    font_id = QFontDatabase.addApplicationFont(_FONT_FILE)
    families = QFontDatabase.applicationFontFamilies(font_id) if font_id != -1 else []
    if not families:
        pytest.skip(f"装不上 {_FONT_FILE}，画不出可识别的文字")
    return families[0]


def _text_image(family, text="HELLO WORLD", width=640, height=200):
    image = QImage(width, height, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    painter = QPainter(image)
    painter.setPen(QColor("black"))
    painter.setFont(QFont(family, 48))
    painter.drawText(image.rect(), Qt.AlignmentFlag.AlignCenter, text)
    painter.end()
    return image


def _blank_image(width=320, height=200):
    image = QImage(width, height, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    return image


class _Collector(QObject):
    """识别结果必须发给某个 QObject 的方法，这里就是测试里的那个对象"""

    def __init__(self):
        super().__init__()
        self.results = []

    @Slot(object, str)
    def collect(self, lines, reason):
        self.results.append((lines, reason))


def _run_to_completion(qapp, thread, timeout_ms=30000):
    """等线程跑完，再把排队的信号派送掉——跨线程的结果是排队送到主线程的"""
    assert thread.wait(timeout_ms), "识别线程超时未结束"
    qapp.processEvents()


def _fake_ocr(monkeypatch, *, available=True, result=None, delay=0.0, boom=None, ppocr_status="missing_engine"):
    """替掉 ocr 模块的三个入口。线程里是 from ocr import ...，取的是调用时的模块属性"""
    def recognize_text(image, **kwargs):
        if delay:
            time.sleep(delay)
        if boom is not None:
            raise boom
        return result

    monkeypatch.setattr("ocr.is_ocr_available", lambda: available)
    monkeypatch.setattr("ocr.recognize_text", recognize_text)
    monkeypatch.setattr("ocr.get_ppocr_status", lambda: ppocr_status)


def _result_text(lines):
    """识别线程报的是带位置的行，断言里只关心文字"""
    return "\n".join(line["text"] for line in lines)


def _ocr_dict(*lines):
    return {"code": 100, "data": [
        {"text": line, "box": [[0, index * 40], [100, index * 40], [100, index * 40 + 30], [0, index * 40 + 30]]}
        for index, line in enumerate(lines)
    ]}


class TestRecognizeThread:

    def test_recognizes_text_with_the_real_engine(self, qapp, font_family):
        """真引擎跑一遍：图 → 线程 → 主线程收到文字，中间没有任何 GUI 对象过线程"""
        import ocr

        if not ocr.is_ocr_available():
            pytest.skip("本环境不带 OCR 引擎")

        collector = _Collector()
        thread = recognize_async(_text_image(font_family), collector.collect)
        _run_to_completion(qapp, thread)

        assert len(collector.results) == 1
        lines, reason = collector.results[0]
        assert reason == ""
        assert "HELLO" in _result_text(lines).upper()

    def test_blank_image_reports_no_text(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, result={"code": 100, "data": []})
        collector = _Collector()
        thread = recognize_async(_blank_image(), collector.collect)
        _run_to_completion(qapp, thread)

        assert collector.results == [([], NO_TEXT)]

    def test_build_without_engine_reports_unavailable(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, available=False)
        collector = _Collector()
        thread = recognize_async(_blank_image(), collector.collect)
        _run_to_completion(qapp, thread)

        assert collector.results == [([], UNAVAILABLE)]

    def test_engine_exception_is_reported_not_raised(self, qapp, monkeypatch):
        """识别在后台线程里炸了，不能把异常带出线程——只报一个失败"""
        _fake_ocr(monkeypatch, boom=RuntimeError("engine died"))
        collector = _Collector()
        thread = recognize_async(_blank_image(), collector.collect)
        _run_to_completion(qapp, thread)

        assert collector.results == [([], FAILED)]

    def test_cancelled_thread_reports_nothing(self, qapp, monkeypatch):
        """取消只是丢结果：识别照样跑完，但不再报出去"""
        _fake_ocr(monkeypatch, result=_ocr_dict("late"), delay=0.2)
        collector = _Collector()
        thread = recognize_async(_blank_image(), collector.collect)
        thread.cancel()
        _run_to_completion(qapp, thread)

        assert collector.results == []

    def test_multi_line_result_keeps_reading_order(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, result=_ocr_dict("first", "second"))
        collector = _Collector()
        thread = recognize_async(_blank_image(), collector.collect)
        _run_to_completion(qapp, thread)

        lines, reason = collector.results[0]
        assert reason == ""
        assert _result_text(lines) == "first\nsecond"

    def test_thread_is_registered_while_running_and_dropped_after(self, qapp, monkeypatch):
        """登记表是退出时能把线程等回来的前提，跑完也必须自己退出登记表，否则越攒越多"""
        _fake_ocr(monkeypatch, result=_ocr_dict("x"), delay=0.15)
        collector = _Collector()
        thread = recognize_async(_blank_image(), collector.collect)
        assert thread in _running

        _run_to_completion(qapp, thread)
        assert thread not in _running


class TestShutdown:

    def test_shutdown_waits_for_a_running_thread(self, qapp, monkeypatch):
        """退出时线程还停在 FFI 里会让进程 abort，所以这里必须是真的等回来了"""
        _fake_ocr(monkeypatch, result=_ocr_dict("x"), delay=0.3)
        collector = _Collector()
        thread = recognize_async(_blank_image(), collector.collect)

        shutdown_recognition()

        assert not thread.isRunning()
        qapp.processEvents()
        assert collector.results == []   # 结果被取消丢弃

    def test_shutdown_with_nothing_running_is_a_no_op(self, qapp):
        shutdown_recognition()
        assert list(_running) == []


class TestResultWindow:

    def _window(self, qapp, image=None):
        """返回 (窗口, 识别线程)。线程要在构造后立刻抓住：识别一完成窗口就把引用放掉了"""
        window = TextRecognitionWindow(image if image is not None else _blank_image())
        thread = window._thread
        window.show()
        qapp.processEvents()
        return window, thread

    def test_window_waits_with_a_recognizing_state(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, result=_ocr_dict("x"), delay=0.3)
        window, _thread = self._window(qapp)
        try:
            assert window.status_label.text() == result_window._tr("Recognizing...")
            assert window.text_edit.isReadOnly()
            assert not window.copy_button.isEnabled()
        finally:
            window.close()

    def test_recognized_text_lands_in_an_editable_box(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, result=_ocr_dict("recognized line"))
        window, thread = self._window(qapp)
        _run_to_completion(qapp, thread)

        assert window.text_edit.toPlainText() == "recognized line"
        assert not window.text_edit.isReadOnly()
        assert window.copy_button.isEnabled()
        assert window.status_label.text() == (
            result_window._tr("Recognized %1 characters").replace("%1", "15"))
        window.close()

    @pytest.mark.parametrize("kwargs, reason", [
        ({"result": {"code": 100, "data": []}}, NO_TEXT),
        ({"available": False}, UNAVAILABLE),
        ({"available": False, "ppocr_status": "missing_models"}, NO_MODEL),
        ({"result": {"code": -1, "msg": "x", "reason": "model_broken", "data": []}}, MODEL_BROKEN),
        ({"result": {"code": -1, "msg": "x", "data": []}}, FAILED),
        ({"boom": RuntimeError("engine died")}, FAILED),
    ])
    def test_every_failure_says_why_and_leaves_copy_disabled(self, qapp, monkeypatch, kwargs, reason):
        _fake_ocr(monkeypatch, **kwargs)
        window, thread = self._window(qapp)
        _run_to_completion(qapp, thread)

        assert window.status_label.text() == result_window._reason_text(reason)
        assert not window.copy_button.isEnabled()
        assert window.text_edit.isReadOnly()
        window.close()

    def test_copy_puts_the_edited_text_on_the_clipboard(self, qapp, monkeypatch):
        """复制取的是文本框里的内容，不是识别结果——改过错字再复制才有意义"""
        _fake_ocr(monkeypatch, result=_ocr_dict("recogniced"))
        window, thread = self._window(qapp)
        _run_to_completion(qapp, thread)

        window.text_edit.setPlainText("recognized")
        window.copy_button.click()

        assert QApplication.clipboard().text() == "recognized"
        assert window.copy_button.text() == result_window._tr("Copied")
        window.close()

    def test_closing_while_recognizing_cancels_instead_of_blocking(self, qapp, monkeypatch):
        """窗口先关、识别后完成：结果被丢掉，回调不会打到已销毁的窗口上"""
        _fake_ocr(monkeypatch, result=_ocr_dict("too late"), delay=0.25)
        window, thread = self._window(qapp)
        assert thread is not None

        window.close()
        QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        _run_to_completion(qapp, thread)   # 不崩即通过

    def test_window_survives_a_shutdown_that_cancels_its_thread(self, qapp, monkeypatch):
        """退出清理会把线程取消掉，这时结果不会再回来——窗口得靠 finished 放掉引用，
        否则它攥着一个已经被回收的线程对象，关窗口时就碰上悬空的 C++ 对象。
        """
        _fake_ocr(monkeypatch, result=_ocr_dict("x"), delay=0.3)
        window, thread = self._window(qapp)

        shutdown_recognition()
        qapp.processEvents()
        QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)

        assert window._thread is None
        window.close()

    def test_window_stays_alive_until_closed(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, result=_ocr_dict("x"))
        window = text_recognition.show_text_recognition(_blank_image())
        assert window.isVisible()
        assert window in qapp._modeless_dialogs

        window.close()
        QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        assert window not in qapp._modeless_dialogs


@pytest.fixture
def layout_settings():
    """改排版设置，用完恢复默认——这些键是全局的，留着会影响后面的用例"""
    manager = get_tool_settings_manager()
    yield manager
    for key in ("ocr_layout_mode", "ocr_remove_spaces", "ocr_punctuation",
                "ocr_copy_on_close", "ocr_result_on_top"):
        manager.qsettings.remove(f"app/{key}")


def _wrapped_lines():
    """两行折开的中文：上一行排到右边界，自动排版会把它们接回一行"""
    return {"code": 100, "data": [
        {"text": "这一行排到了右边界", "box": [[0, 0], [180, 0], [180, 20], [0, 20]]},
        {"text": "接着往下排", "box": [[0, 28], [100, 28], [100, 48], [0, 48]]},
    ]}


JOINED = "这一行排到了右边界接着往下排"
KEPT = "这一行排到了右边界\n接着往下排"


class TestLayoutMenu:
    """排版菜单：勾选状态来自设置，换一项就按原始的行重排一遍"""

    def _window(self, qapp, monkeypatch, result=None):
        _fake_ocr(monkeypatch, result=result if result is not None else _wrapped_lines())
        window = TextRecognitionWindow(_blank_image())
        thread = window._thread
        window.show()
        _run_to_completion(qapp, thread)
        return window

    def test_auto_joins_a_wrapped_line_and_keep_every_line_does_not(
            self, qapp, monkeypatch, layout_settings):
        layout_settings.set_ocr_layout_mode("auto")
        window = self._window(qapp, monkeypatch)
        try:
            assert window.text_edit.toPlainText() == JOINED

            window._set_layout_mode("multiline")
            assert window.text_edit.toPlainText() == KEPT
        finally:
            window.close()

    def test_every_relayout_starts_from_the_recognized_lines(
            self, qapp, monkeypatch, layout_settings):
        """来回切不会越切越歪：重排的输入永远是识别出的行，不是上一次排出来的文本"""
        window = self._window(qapp, monkeypatch)
        try:
            window._set_layout_mode("multiline")
            window._set_remove_spaces(True)
            window._set_remove_spaces(False)
            window._set_layout_mode("auto")
            assert window.text_edit.toPlainText() == JOINED
        finally:
            window.close()

    def test_the_menu_ticks_what_the_settings_say(self, qapp, monkeypatch, layout_settings):
        layout_settings.set_ocr_layout_mode("single_line")
        layout_settings.set_ocr_remove_spaces(True)
        layout_settings.set_ocr_punctuation("halfwidth")
        window = self._window(qapp, monkeypatch)
        try:
            menu = window._build_layout_menu()
            ticked = {action.text() for action in menu.actions() if action.isChecked()}
            assert result_window._tr("Remove line breaks") in ticked
            assert result_window._tr("Remove spaces") in ticked
            assert result_window._tr("Auto") not in ticked

            submenu = [action.menu() for action in menu.actions() if action.menu()][0]
            assert {action.text() for action in submenu.actions() if action.isChecked()} == {
                result_window._tr("Half-width")}
            menu.deleteLater()
        finally:
            window.close()

    def test_an_edit_survives_a_relayout_through_undo(self, qapp, monkeypatch, layout_settings):
        """改过错字再换排版，改动不能就这么没了：一次撤销要能拿回来"""
        window = self._window(qapp, monkeypatch)
        try:
            window.text_edit.insertPlainText("（改过）")
            edited = window.text_edit.toPlainText()
            window._set_layout_mode("multiline")
            assert window.text_edit.toPlainText() == KEPT

            window.text_edit.undo()
            assert window.text_edit.toPlainText() == edited
        finally:
            window.close()

    def test_the_first_result_cannot_be_undone_away(self, qapp, monkeypatch, layout_settings):
        """识别结果不算一次编辑：否则刚出结果按一下撤销就清空了"""
        window = self._window(qapp, monkeypatch)
        try:
            window.text_edit.undo()
            assert window.text_edit.toPlainText() == JOINED
        finally:
            window.close()

    def test_the_layout_entry_waits_for_a_result(self, qapp, monkeypatch, layout_settings):
        _fake_ocr(monkeypatch, result=_wrapped_lines(), delay=0.2)
        window = TextRecognitionWindow(_blank_image())
        thread = window._thread
        window.show()
        qapp.processEvents()
        try:
            assert not window.layout_button.isEnabled()
            _run_to_completion(qapp, thread)
            assert window.layout_button.isEnabled()
        finally:
            window.close()


def test_the_direct_copy_toast_follows_the_layout_settings(qapp, monkeypatch, layout_settings):
    """「文字识别直接复制」不开窗口，排版设置照样要管用"""
    layout_settings.set_ocr_layout_mode("multiline")
    QApplication.clipboard().setText("before")
    _fake_ocr(monkeypatch, result=_wrapped_lines(), delay=0.1)
    before = set(_running)
    toast = text_recognition.copy_text_recognition(_blank_image())
    (thread,) = set(_running) - before
    _run_to_completion(qapp, thread)

    assert QApplication.clipboard().text() == KEPT
    toast.close()


class TestCopyOnClose:

    def _window(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, result=_ocr_dict("要复制的文字"))
        window = TextRecognitionWindow(_blank_image())
        thread = window._thread
        window.show()
        _run_to_completion(qapp, thread)
        return window

    def test_off_by_default_the_clipboard_is_left_alone(self, qapp, monkeypatch, layout_settings):
        QApplication.clipboard().setText("原来的剪贴板")
        self._window(qapp, monkeypatch).close()
        assert QApplication.clipboard().text() == "原来的剪贴板"

    def test_on_it_copies_what_the_box_shows_when_closing(self, qapp, monkeypatch, layout_settings):
        layout_settings.set_ocr_copy_on_close(True)
        QApplication.clipboard().setText("原来的剪贴板")
        window = self._window(qapp, monkeypatch)
        window.text_edit.setPlainText("改过之后的文字")
        window.close()
        assert QApplication.clipboard().text() == "改过之后的文字"


class TestAlwaysOnTop:
    """置顶只改窗口层级、不设 WindowStaysOnTopHint，同翻译窗口（见 set_window_topmost）"""

    def test_the_pin_state_is_applied_on_show_and_remembered(
            self, qapp, monkeypatch, layout_settings):
        applied = []
        monkeypatch.setattr(result_window, "set_window_topmost",
                            lambda _hwnd, topmost: applied.append(topmost))
        layout_settings.set_ocr_result_on_top(True)
        _fake_ocr(monkeypatch, result=_ocr_dict("x"))

        window = TextRecognitionWindow(_blank_image())
        flags = window.windowFlags()
        window.show()
        qapp.processEvents()
        try:
            assert applied == [True]
            assert not flags & Qt.WindowType.WindowStaysOnTopHint

            window.pin_button.click()
            assert applied == [True, False]
            assert window.windowFlags() == flags
            assert layout_settings.get_ocr_result_on_top() is False
        finally:
            window.close()

    def test_the_next_window_opens_with_the_remembered_state(
            self, qapp, monkeypatch, layout_settings):
        layout_settings.set_ocr_result_on_top(False)
        _fake_ocr(monkeypatch, result=_ocr_dict("x"))
        window = TextRecognitionWindow(_blank_image())
        try:
            assert not window.pin_button.isChecked()
        finally:
            window.close()


@pytest.fixture
def result_position():
    """改文字识别结果窗口的位置设置，用完恢复默认"""
    manager = get_tool_settings_manager()
    yield manager.set_ocr_result_position
    manager.set_ocr_result_position("region_screen")
    manager.qsettings.remove("app/ocr_result_last_center")


class TestResultWindowPlacement:
    """结果窗口按设置里的位置摆放。offscreen 平台只有一块屏幕，多屏选屏在 placement 的测试里"""

    def _open(self, qapp, monkeypatch, anchor):
        _fake_ocr(monkeypatch, result=_ocr_dict("x"))
        window = TextRecognitionWindow(_blank_image(), anchor=anchor)
        window.show()
        qapp.processEvents()
        return window, QApplication.primaryScreen().availableGeometry()

    def _assert_centered(self, window, available):
        center = window.geometry().center()
        assert abs(center.x() - available.center().x()) <= 1
        assert abs(center.y() - available.center().y()) <= 1

    @pytest.mark.parametrize("position", ["region_screen", "primary_screen"])
    def test_center_positions_center_the_window_whatever_the_region_is(
            self, qapp, monkeypatch, result_position, position):
        result_position(position)
        window, available = self._open(qapp, monkeypatch, QRect(20, 40, 120, 80))
        try:
            self._assert_centered(window, available)
        finally:
            window.close()

    def test_without_a_region_it_centers_like_before(self, qapp, monkeypatch, result_position):
        result_position("beside_region")
        window, available = self._open(qapp, monkeypatch, None)
        try:
            self._assert_centered(window, available)
        finally:
            window.close()

    def test_beside_region_sits_next_to_the_region_without_covering_it(
            self, qapp, monkeypatch, result_position):
        result_position("beside_region")
        region = QRect(20, 40, 120, 80)
        window, available = self._open(qapp, monkeypatch, region)
        try:
            assert window.geometry().left() == region.right() + 1 + GAP
            assert window.geometry().top() == region.top()
            assert not window.geometry().intersects(region)
            assert available.contains(window.geometry())
        finally:
            window.close()

    def test_beside_region_falls_back_to_center_when_there_is_no_room(
            self, qapp, monkeypatch, result_position):
        result_position("beside_region")
        available = QApplication.primaryScreen().availableGeometry()
        window, available = self._open(qapp, monkeypatch, available.adjusted(4, 4, -4, -4))
        try:
            self._assert_centered(window, available)
        finally:
            window.close()

    def test_near_cursor_puts_the_window_by_the_cursor(self, qapp, monkeypatch, result_position):
        result_position("near_cursor")
        monkeypatch.setattr(result_window, "QCursor", SimpleNamespace(pos=lambda: QPoint(100, 60)))
        window, _available = self._open(qapp, monkeypatch, QRect(20, 40, 120, 80))
        try:
            assert window.geometry().topLeft() == QPoint(100 + CURSOR_OFFSET, 60 + CURSOR_OFFSET)
        finally:
            window.close()

    def test_last_position_comes_back_where_the_window_was_closed(self, qapp, monkeypatch, result_position):
        result_position("last_position")
        window, available = self._open(qapp, monkeypatch, None)
        try:
            self._assert_centered(window, available)    # 还没有记录时和默认一样居中
            window.move(30, 40)
        finally:
            window.close()

        window, _available = self._open(qapp, monkeypatch, None)
        try:
            assert window.geometry().topLeft() == QPoint(30, 40)
        finally:
            window.close()

    def test_only_last_position_records_where_the_window_was_closed(self, qapp, monkeypatch, result_position):
        result_position("region_screen")
        window, _available = self._open(qapp, monkeypatch, None)
        window.move(30, 40)
        window.close()
        assert get_tool_settings_manager().get_ocr_result_last_center() is None

    def test_show_hands_the_region_to_the_window(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, result=_ocr_dict("x"))
        region = QRect(20, 40, 120, 80)
        window = text_recognition.show_text_recognition(_blank_image(), anchor=region)
        try:
            assert window._anchor == region
        finally:
            window.close()


class TestCopyDirectly:
    """快捷行为「文字识别直接复制」：不开窗口，结果进剪贴板，光标旁提示"""

    def _start(self, qapp):
        """返回 (提示, 识别线程)。线程从登记表里取：提示本身不留线程引用"""
        before = set(_running)
        toast = text_recognition.copy_text_recognition(_blank_image())
        (thread,) = set(_running) - before
        return toast, thread

    def test_recognized_text_goes_to_the_clipboard(self, qapp, monkeypatch):
        QApplication.clipboard().setText("before")
        _fake_ocr(monkeypatch, result=_ocr_dict("recognized line"), delay=0.1)
        toast, thread = self._start(qapp)
        assert toast.label.text() == result_window._tr("Recognizing...")

        _run_to_completion(qapp, thread)

        assert QApplication.clipboard().text() == "recognized line"
        assert toast.label.text() == result_window._tr("Copied %1 characters").replace("%1", "15")
        toast.close()

    @pytest.mark.parametrize("kwargs, reason", [
        ({"result": {"code": 100, "data": []}}, NO_TEXT),
        ({"available": False}, UNAVAILABLE),
    ])
    def test_failure_says_why_and_leaves_the_clipboard_alone(self, qapp, monkeypatch, kwargs, reason):
        QApplication.clipboard().setText("before")
        _fake_ocr(monkeypatch, **kwargs)
        toast, thread = self._start(qapp)
        _run_to_completion(qapp, thread)

        assert QApplication.clipboard().text() == "before"
        assert toast.label.text() == result_window._reason_text(reason)
        toast.close()

    def test_checkbox_in_the_window_writes_the_setting(self, qapp, monkeypatch):
        _fake_ocr(monkeypatch, result=_ocr_dict("x"))
        manager = get_tool_settings_manager()
        manager.set_ocr_copy_directly_enabled(False)
        window = TextRecognitionWindow(_blank_image())
        try:
            assert not window.copy_directly_check.isChecked()
            window.copy_directly_check.setChecked(True)
            assert manager.get_ocr_copy_directly_enabled() is True
        finally:
            window.close()
            manager.set_ocr_copy_directly_enabled(False)
            shutdown_recognition()


def test_toolbar_button_emits_its_signal(qapp):
    toolbar = Toolbar()
    clicks = []
    toolbar.text_recognize_clicked.connect(lambda: clicks.append(True))
    toolbar.text_recognize_btn.click()
    assert clicks == [True]


def test_upgrade_puts_the_new_button_next_to_screenshot_translate(qapp):
    """老用户存过的排布里没有这个按钮，补回来时不能堆到「确定」右边"""
    stored = [("long_screenshot", SHOW), ("save", SHOW), ("screenshot_translate", SHOW),
              ("scan_code", MORE), ("confirm", SHOW)]
    keys = [key for key, _mode in normalize_layout(stored)]

    assert keys.index("text_recognize") == keys.index("screenshot_translate") + 1
    assert dict(normalize_layout(stored))["text_recognize"] == MORE


class TestTextRecognizeAction:

    def _tools(self, image, confirmed=True):
        tools = ActionTools.__new__(ActionTools)   # 只用得到选区和导出，跳过构造里的保存服务
        tools.scene = MagicMock()
        tools.scene.selection_model.is_confirmed = confirmed
        tools.export_service = MagicMock()
        tools.export_service.export_base_image_only.return_value = image
        tools.parent_window = MagicMock()
        tools.config_manager = None
        return tools

    def test_closes_the_capture_first_then_shows_the_result_window(self, monkeypatch):
        """截图界面全屏置顶，先弹结果窗口会被它盖住"""
        steps = []
        image = _blank_image()
        tools = self._tools(image)
        tools.parent_window.cleanup_and_close.side_effect = lambda: steps.append("close capture")
        monkeypatch.setattr("text_recognition.show_text_recognition",
                            lambda shown, anchor=None: steps.append(("show", shown)))

        tools.handle_text_recognize()

        assert steps == ["close capture", ("show", image)]

    def test_result_window_is_placed_by_the_selection_taken_before_closing(self, monkeypatch):
        """结果窗口按选区的屏幕位置摆放；截图界面一关选区就没了，所以得先取"""
        closed, anchors = [], []
        monkeypatch.setattr("core.last_capture_region.set_last_region", lambda _region: None)
        tools = self._tools(_blank_image())
        tools.scene.selection_model.rect.side_effect = lambda: (
            QRectF() if closed else QRectF(10.4, 20.6, 300.2, 200.2))
        tools.parent_window.cleanup_and_close.side_effect = lambda: closed.append(True)
        monkeypatch.setattr("text_recognition.show_text_recognition",
                            lambda shown, anchor=None: anchors.append(anchor))

        tools.handle_text_recognize()

        assert anchors == [QRect(10, 20, 301, 201)]

    def test_copy_directly_setting_skips_the_window(self, monkeypatch):
        steps = []
        image = _blank_image()
        tools = self._tools(image)
        tools.config_manager = MagicMock()
        tools.config_manager.get_ocr_copy_directly_enabled.return_value = True
        monkeypatch.setattr("text_recognition.show_text_recognition",
                            lambda shown: pytest.fail("不该弹出结果窗口"))
        monkeypatch.setattr("text_recognition.copy_text_recognition",
                            lambda shown: steps.append(("copy", shown)))

        tools.handle_text_recognize()

        assert steps == [("copy", image)]

    def test_without_a_confirmed_selection_it_only_warns(self, monkeypatch):
        warnings = []
        monkeypatch.setattr("ui.dialogs.show_modeless_warning_dialog", lambda *args: warnings.append(args))
        monkeypatch.setattr("text_recognition.show_text_recognition",
                            lambda shown: pytest.fail("不该弹出结果窗口"))
        tools = self._tools(_blank_image(), confirmed=False)

        tools.handle_text_recognize()

        assert len(warnings) == 1
        tools.export_service.export_base_image_only.assert_not_called()
        tools.parent_window.cleanup_and_close.assert_not_called()
