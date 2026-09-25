# -*- coding: utf-8 -*-
"""
扫码测试

码图用 zxing-cpp 现场生成，不依赖图片素材。覆盖三层：解码结果与编号顺序、结果窗口里
图和卡片的联动与按钮、截图工具栏「扫码」从按钮到弹窗的路径。
"""
from unittest.mock import MagicMock

import pytest
import zxingcpp
from PySide6.QtCore import QEvent, QPointF, Qt
from PySide6.QtGui import QColor, QEnterEvent, QImage, QMouseEvent, QPainter, QPolygonF
from PySide6.QtWidgets import QApplication

from barcode import BarcodeResultWindow, DecodedCode, read_codes, result_window, show_barcode_result
from barcode.reader import _reading_order
from settings import get_tool_settings_manager
from tools.action import ActionTools
from ui.toast import Toast
from ui.toolbar import Toolbar

Format = zxingcpp.BarcodeFormat


def _code_image(text, barcode_format, scale):
    bitmap = zxingcpp.write_barcode_to_image(zxingcpp.create_barcode(text, barcode_format), scale=scale)
    height, width = bitmap.shape
    return QImage(bytes(memoryview(bitmap)), width, height, width, QImage.Format.Format_Grayscale8).copy()


def _canvas(placements, width=1200, height=700):
    """白底图上按 (内容, 格式, 放大倍数, x, y) 摆几个码"""
    canvas = QImage(width, height, QImage.Format.Format_RGB32)
    canvas.fill(QColor("white"))
    painter = QPainter(canvas)
    for text, barcode_format, scale, x, y in placements:
        painter.drawImage(x, y, _code_image(text, barcode_format, scale))
    painter.end()
    return canvas


def _box(text, x, y, width, height):
    corners = [QPointF(x, y), QPointF(x + width, y), QPointF(x + width, y + height), QPointF(x, y + height)]
    return DecodedCode(text, "QR Code", QPolygonF(corners))


@pytest.fixture
def two_codes():
    """左上一个链接二维码，右下一个条形码"""
    return _canvas([
        ("https://example.com/?q=1", Format.QRCode, 4, 80, 60),
        ("HELLO-128", Format.Code128, 3, 500, 300),
    ])


class TestReadCodes:

    def test_text_format_and_outline_of_each_code(self, two_codes):
        codes = read_codes(two_codes)
        assert [(code.text, code.format_name) for code in codes] == [
            ("https://example.com/?q=1", "QR Code"),
            ("HELLO-128", "Code 128"),
        ]
        assert codes[0].outline.boundingRect().contains(QPointF(146, 126))
        assert codes[1].outline.boundingRect().left() >= 500

    @pytest.mark.parametrize("image_format", [
        QImage.Format.Format_ARGB32_Premultiplied,   # 截图导出的底图就是这个格式
        QImage.Format.Format_RGBA8888_Premultiplied,
        QImage.Format.Format_RGBX8888,
        QImage.Format.Format_Grayscale16,
        QImage.Format.Format_RGB16,
        QImage.Format.Format_Indexed8,
    ])
    def test_formats_zxing_cannot_read_directly_still_decode(self, two_codes, image_format):
        """这些格式交给 zxing-cpp 会走它的 convertToFormat(int)，被 PySide6 拒绝而抛 TypeError"""
        codes = read_codes(two_codes.convertToFormat(image_format))
        assert [code.text for code in codes] == ["https://example.com/?q=1", "HELLO-128"]

    def test_decodes_the_real_selection_export(self, qapp):
        """底图走真实的 CanvasScene + ExportService 导出，格式和截图里点「扫码」时拿到的一致。

        动作层测试里导出服务是 mock 的，喂进去的是 RGB32，真实导出却是 ARGB32_Premultiplied，
        扫码崩溃就是从这个缝里漏过去的。
        """
        from PySide6.QtCore import QRectF

        from canvas import CanvasScene
        from core.export import ExportService

        screen = _canvas([("https://example.com/?q=1", Format.QRCode, 4, 1000, 600)], width=2560, height=1440)
        scene = CanvasScene(screen, QRectF(0, 0, 2560, 1440), enable_mosaic=True)
        base = ExportService(scene).export_base_image_only(QRectF(801, 409, 927, 655))
        assert [code.text for code in read_codes(base)] == ["https://example.com/?q=1"]

    def test_blank_image_has_no_codes(self):
        blank = QImage(400, 300, QImage.Format.Format_RGB32)
        blank.fill(QColor("white"))
        assert read_codes(blank) == []

    def test_codes_are_numbered_in_reading_order_not_zxing_order(self):
        """zxing 返回的先后与位置无关（这一排实测按 4、2、1、3、5 返回），编号前得排好"""
        image = _canvas([
            ("A-128", Format.Code128, 3, 40, 120),
            ("B-QR", Format.QRCode, 4, 500, 100),
            ("C-DM", Format.DataMatrix, 6, 800, 115),
            ("D-AZ", Format.Aztec, 5, 1100, 95),
            ("E-PDF", Format.PDF417, 3, 1350, 130),
            ("590123412345", Format.EAN13, 3, 60, 600),
            ("G-QR", Format.QRCode, 4, 700, 560),
        ], width=1700, height=1000)
        assert [code.text for code in read_codes(image)] == [
            "A-128", "B-QR", "C-DM", "D-AZ", "E-PDF", "5901234123457", "G-QR",
        ]

    def test_codes_whose_vertical_ranges_overlap_share_a_row(self):
        """一排码的顶边对不齐——右边的码反而最高——也还是同一行，行内从左到右"""
        codes = [
            _box("row1-right", 300, 20, 50, 50),
            _box("row1-left", 0, 40, 50, 50),
            _box("row2", 0, 200, 50, 50),
            _box("row1-middle", 150, 30, 50, 50),
        ]
        assert [code.text for code in _reading_order(codes)] == [
            "row1-left", "row1-middle", "row1-right", "row2",
        ]


class TestResultWindow:

    @pytest.fixture
    def window(self, qapp, two_codes):
        window = BarcodeResultWindow(two_codes, read_codes(two_codes))
        window.show()
        qapp.processEvents()
        yield window
        window.close()

    def test_one_card_per_code_and_only_web_links_can_be_opened(self, window):
        assert window.summary_label.text() == result_window._tr("Found %1 code(s)").replace("%1", "2")
        assert len(window.cards) == 2
        assert window.cards[0].open_button is not None
        assert window.cards[1].open_button is None

    def test_copy_puts_the_code_text_on_the_clipboard(self, window):
        window.cards[1].copy_button.click()
        assert QApplication.clipboard().text() == "HELLO-128"
        assert window.cards[1].copy_button.text() == result_window._tr("Copied")

    def test_pointing_at_a_code_on_the_image_highlights_its_card(self, window):
        view = window.image_view
        point = view._image_to_widget().map(view._codes[1].outline.boundingRect().center())
        QApplication.sendEvent(view, QMouseEvent(
            QEvent.Type.MouseMove, point, view.mapToGlobal(point),
            Qt.MouseButton.NoButton, Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
        ))
        assert view._active == 1
        assert [card.property("active") for card in window.cards] == [False, True]

    def test_pointing_at_a_card_highlights_its_code_on_the_image(self, window):
        card = window.cards[0]
        QApplication.sendEvent(card, QEnterEvent(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5)))
        assert window.image_view._active == 0
        QApplication.sendEvent(card, QEvent(QEvent.Type.Leave))
        assert window.image_view._active == -1

    def test_nothing_found_says_so(self, qapp):
        blank = QImage(400, 300, QImage.Format.Format_RGB32)
        blank.fill(QColor("white"))
        window = BarcodeResultWindow(blank, [])
        assert window.cards == []
        assert window.summary_label.text() == result_window._tr("No QR code or barcode found")
        window.close()


class TestCopySingleCode:
    """快捷行为「扫码直接复制」：恰好一个码才跳过窗口"""

    @pytest.fixture(autouse=True)
    def _close_toasts(self, qapp):
        yield
        for widget in list(getattr(qapp, "_modeless_dialogs", [])):
            if isinstance(widget, Toast):
                widget.close()
        QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)

    def test_one_code_is_copied_without_a_window(self, qapp):
        QApplication.clipboard().setText("before")
        image = _canvas([("HELLO-128", Format.Code128, 3, 80, 60)], width=600, height=300)

        assert show_barcode_result(image, copy_single=True) is None

        assert QApplication.clipboard().text() == "HELLO-128"
        toasts = [w for w in qapp._modeless_dialogs if isinstance(w, Toast)]
        assert len(toasts) == 1
        assert "HELLO-128" in toasts[0].label.text()

    def test_several_codes_still_open_the_window(self, qapp, two_codes):
        QApplication.clipboard().setText("before")
        window = show_barcode_result(two_codes, copy_single=True)
        assert isinstance(window, BarcodeResultWindow)
        assert QApplication.clipboard().text() == "before"
        window.close()

    def test_no_code_still_opens_the_window(self, qapp):
        blank = QImage(400, 300, QImage.Format.Format_RGB32)
        blank.fill(QColor("white"))
        window = show_barcode_result(blank, copy_single=True)
        assert isinstance(window, BarcodeResultWindow)
        window.close()

    def test_checkbox_in_the_window_writes_the_setting(self, qapp, two_codes):
        manager = get_tool_settings_manager()
        manager.set_barcode_copy_single_enabled(False)
        window = BarcodeResultWindow(two_codes, read_codes(two_codes))
        try:
            assert not window.copy_single_check.isChecked()
            window.copy_single_check.setChecked(True)
            assert manager.get_barcode_copy_single_enabled() is True
        finally:
            window.close()
            manager.set_barcode_copy_single_enabled(False)


@pytest.mark.parametrize("text, openable", [
    ("https://example.com/a?b=1", True),
    ("  http://example.com  ", True),
    ("file:///C:/Windows/System32/calc.exe", False),
    ("ms-settings:privacy", False),
    ("WIFI:S:home;T:WPA;P:secret;;", False),
    ("just some text", False),
    ("https://", False),
])
def test_only_http_links_are_offered_to_open(text, openable):
    """码里的内容不可信：file:、系统协议之类不给打开按钮"""
    assert (result_window._web_link(text) is not None) == openable


def test_result_window_stays_alive_until_closed(qapp, two_codes):
    window = show_barcode_result(two_codes)
    assert window.isVisible()
    assert window in qapp._modeless_dialogs
    window.close()
    QApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
    assert window not in qapp._modeless_dialogs


def test_toolbar_scan_button_emits_its_signal(qapp):
    toolbar = Toolbar()
    clicks = []
    toolbar.scan_code_clicked.connect(lambda: clicks.append(True))
    toolbar.scan_code_btn.click()
    assert clicks == [True]


class TestScanAction:

    def _tools(self, image, confirmed=True):
        tools = ActionTools.__new__(ActionTools)   # 只用得到选区和导出，跳过构造里的保存服务
        tools.scene = MagicMock()
        tools.scene.selection_model.is_confirmed = confirmed
        tools.export_service = MagicMock()
        tools.export_service.export_base_image_only.return_value = image
        tools.parent_window = MagicMock()
        tools.config_manager = None
        return tools

    def test_closes_the_capture_first_then_shows_results_for_the_base_image(self, monkeypatch, two_codes):
        """截图界面全屏置顶，先弹结果窗口会被它盖住"""
        steps = []
        tools = self._tools(two_codes)
        tools.parent_window.cleanup_and_close.side_effect = lambda: steps.append("close capture")
        monkeypatch.setattr("barcode.show_barcode_result",
                            lambda image, copy_single: steps.append(("show", image, copy_single)))

        tools.handle_scan_code()

        assert steps == ["close capture", ("show", two_codes, False)]

    def test_copy_single_setting_is_passed_along(self, monkeypatch, two_codes):
        calls = []
        tools = self._tools(two_codes)
        tools.config_manager = MagicMock()
        tools.config_manager.get_barcode_copy_single_enabled.return_value = True
        monkeypatch.setattr("barcode.show_barcode_result",
                            lambda image, copy_single: calls.append(copy_single))

        tools.handle_scan_code()

        assert calls == [True]

    def test_without_a_confirmed_selection_it_only_warns(self, monkeypatch, two_codes):
        warnings = []
        monkeypatch.setattr("ui.dialogs.show_modeless_warning_dialog", lambda *args: warnings.append(args))
        monkeypatch.setattr("barcode.show_barcode_result", lambda *_a, **_k: pytest.fail("不该弹出结果窗口"))
        tools = self._tools(two_codes, confirmed=False)

        tools.handle_scan_code()

        assert len(warnings) == 1
        tools.export_service.export_base_image_only.assert_not_called()
        tools.parent_window.cleanup_and_close.assert_not_called()

    def test_screenshot_translate_still_gets_the_selection_image(self, qapp, monkeypatch, two_codes):
        """截图翻译和扫码共用同一段取底图的逻辑"""
        manager = MagicMock()
        monkeypatch.setattr("translation.TranslationManager.instance", lambda: manager)
        tools = self._tools(two_codes)

        tools.handle_screenshot_translate()

        tools.parent_window.cleanup_and_close.assert_called_once()
        assert manager.translate_from_image.call_args.kwargs["pixmap"].size() == two_codes.size()
