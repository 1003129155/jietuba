# -*- coding: utf-8 -*-
"""钉图识别文字上的双击：只选中不复制，钉图的鼠标快捷键优先。

真实双击时，文字层在第二下会先收到一个普通按下、再收到双击。以前文字层自己比较两次
按下的间隔判定双击并立即复制，于是「左键双击关闭钉图」会顺带把文字写进剪贴板。
"""
import pytest
from PySide6.QtCore import QEvent, QPoint, QPointF, QSettings, Qt
from PySide6.QtGui import QColor, QImage, QMouseEvent
from PySide6.QtWidgets import QApplication

from settings.tool_settings import ToolSettingsManager

# 在真实钉图窗口上用真实输入记录到的顺序
REAL_DOUBLE_CLICK = (
    QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonRelease,
    QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonDblClick, QEvent.Type.MouseButtonRelease,
)


def _send(widget, kind, pos):
    held = Qt.MouseButton.NoButton if kind == QEvent.Type.MouseButtonRelease else Qt.MouseButton.LeftButton
    QApplication.sendEvent(widget, QMouseEvent(
        kind, QPointF(pos), QPointF(widget.mapToGlobal(pos)),
        Qt.MouseButton.LeftButton, held, Qt.KeyboardModifier.NoModifier))


@pytest.fixture
def pin_with_text(qapp, tmp_path):
    from pin.ocr_text_layer import OCRTextLayer
    from pin.pin_window import PinWindow

    def _make(close_binding):
        config = ToolSettingsManager(qsettings=QSettings(str(tmp_path / "pin.ini"), QSettings.Format.IniFormat))
        config.set_app_setting("mouse_pin_close", close_binding)
        image = QImage(300, 200, QImage.Format.Format_RGB32)
        image.fill(QColor(240, 240, 240))
        pin = PinWindow(image, QPoint(100, 100), config)
        layer = OCRTextLayer(pin)
        layer.setGeometry(pin.content_rect().toRect())
        pin._ocr_mgr.ocr_text_layer = layer
        pin._ocr_mgr._text_selection_enabled = True
        pin._ocr_mgr._apply_text_layer_enabled()
        layer.load_ocr_result({"code": 100, "data": [
            {"text": "Hello world", "box": [[20, 40], [200, 40], [200, 80], [20, 80]], "score": 0.99},
        ]}, 300, 200)
        layer.show()
        layer.raise_()
        created.append(pin)
        return pin, layer

    created = []
    yield _make
    for pin in created:
        if not pin._is_closed:
            pin.close_window()


def _double_click(pin, layer):
    for kind in REAL_DOUBLE_CLICK:
        if pin._is_closed:
            break
        _send(layer, kind, QPoint(60, 60))


def test_double_click_bound_to_close_closes_without_copying(qapp, pin_with_text):
    qapp.clipboard().setText("untouched")
    pin, layer = pin_with_text("doubleleft")
    _double_click(pin, layer)
    assert pin._is_closed
    assert qapp.clipboard().text() == "untouched"


def test_unbound_double_click_selects_the_block_without_copying(qapp, pin_with_text):
    qapp.clipboard().setText("untouched")
    pin, layer = pin_with_text("")
    _double_click(pin, layer)
    assert not pin._is_closed
    assert layer.get_selected_text() == "Hello world"
    assert qapp.clipboard().text() == "untouched"
