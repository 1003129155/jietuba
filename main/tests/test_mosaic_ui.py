from pathlib import Path

from PySide6.QtCore import QPoint, QPointF, QRectF, Qt, QTranslator
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QWidget

from canvas.scene import CanvasScene
from canvas.view import CanvasView
from pin.pin_toolbar import PinToolbar
from ui.toolbar import Toolbar


def test_screenshot_toolbar_exposes_mosaic(qapp):
    toolbar = Toolbar()

    assert toolbar.tool_buttons["mosaic"] is toolbar.mosaic_btn
    assert toolbar.mosaic_btn.isCheckable()
    assert "wheel" in toolbar.mosaic_btn.toolTip().lower()


def test_toolbar_shortcut_selection_is_idempotent_and_mouse_toggle_remains(qapp):
    toolbar = Toolbar()
    emitted = []
    toolbar.tool_changed.connect(emitted.append)

    toolbar.select_tool("text", toggle=False)
    toolbar.select_tool("text", toggle=False)
    assert toolbar.current_tool == "text"
    assert toolbar.text_btn.isChecked()
    assert emitted == ["text"]

    toolbar.select_tool("text", toggle=True)
    assert toolbar.current_tool is None
    assert not toolbar.text_btn.isChecked()
    assert emitted == ["text", "cursor"]


def test_pin_toolbar_shows_mosaic_without_overlap(qapp):
    """钉图工具栏排布是手写的绝对坐标，新按钮最容易压到别人身上。"""
    toolbar = PinToolbar()

    assert not toolbar.mosaic_btn.isHidden()
    assert "mosaic" in toolbar.tool_buttons
    visible = [button.geometry() for button in toolbar.tool_buttons.values() if not button.isHidden()]
    for index, left in enumerate(visible):
        for right in visible[index + 1:]:
            assert not left.intersects(right)
    # 按钮都得落在工具栏自己算出来的宽度里，否则会被裁掉一半
    assert all(toolbar.rect().contains(geometry) for geometry in visible)


def test_pin_toolbar_resolves_its_host_window(qapp):
    """钉图工具栏是顶层窗口，parent() 为 None；找宿主必须走 _host_window。"""
    host = QWidget()
    toolbar = PinToolbar(parent_pin_window=host)

    assert toolbar.parent() is None
    assert toolbar._host_window() is host
    # 宿主还没有画布时不应该炸，只是拿不到光标管理器
    assert toolbar._host_cursor_manager() is None


def test_mosaic_cursor_uses_sized_pixmap(qapp):
    image = QImage(80, 80, QImage.Format.Format_ARGB32)
    image.fill(0xFFFFFFFF)
    scene = CanvasScene(image, QRectF(0, 0, 80, 80), enable_mosaic=True)
    view = CanvasView(scene)

    cursor = view.cursor_manager.create_tool_cursor_with_size("mosaic", 30)

    assert not cursor.pixmap().isNull()
    assert cursor.pixmap().width() >= 30


def test_mosaic_mouse_wheel_resizes_brush_and_cursor(monkeypatch, qapp):
    from PySide6.QtGui import QWheelEvent

    image = QImage(80, 80, QImage.Format.Format_ARGB32)
    image.fill(0xFFFFFFFF)
    scene = CanvasScene(image, QRectF(0, 0, 80, 80), enable_mosaic=True)
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 80, 80))
    view = CanvasView(scene)
    scene.activate_tool("mosaic")
    scene.update_style(width=30)
    resized = []
    monkeypatch.setattr(view.cursor_manager, "update_tool_cursor_size", resized.append)
    event = QWheelEvent(
        QPointF(40, 40), QPointF(40, 40), QPoint(), QPoint(0, 120),
        Qt.MouseButton.NoButton, Qt.KeyboardModifier.NoModifier,
        Qt.ScrollPhase.ScrollUpdate, False,
    )

    view.wheelEvent(event)

    assert scene.tool_controller.context.stroke_width == 31
    assert resized == [31]


def test_mosaic_translation_exists_for_supported_languages():
    translations = Path(__file__).parents[1] / "translations"
    source = "Mosaic (mouse wheel to resize)"

    for language in ("en", "zh", "ja", "ko"):
        text = (translations / f"app_{language}.xml").read_text(encoding="utf-8")
        assert f"<source>{source}</source>" in text


def test_mosaic_translation_loads_from_runtime_resources(qapp):
    source = "Mosaic (mouse wheel to resize)"
    expected = {
        "zh": "马赛克（鼠标滚轮调整大小）",
        "ja": "モザイク（マウスホイールでサイズ変更）",
        "ko": "모자이크(마우스 휠로 크기 조절)",
    }

    translations = Path(__file__).parents[1] / "translations"
    for language, translated in expected.items():
        translator = QTranslator()
        assert translator.load(str(translations / f"app_{language}.qm"))
        assert translator.translate("Toolbar", source) == translated


def test_mosaic_panel_exposes_block_size_slider(qapp):
    from tools.mosaic import MosaicTool
    from ui.mosaic_settings_panel import MosaicSettingsPanel

    panel = MosaicSettingsPanel()

    assert panel.block_size_slider.minimum() == MosaicTool.MIN_BLOCK_SIZE
    assert panel.block_size_slider.maximum() == MosaicTool.MAX_BLOCK_SIZE
    assert panel.block_size == MosaicTool.DEFAULT_BLOCK_SIZE

    emitted = []
    panel.block_size_changed.connect(emitted.append)
    panel.block_size_slider.setValue(MosaicTool.MIN_BLOCK_SIZE + 2)

    assert emitted == [MosaicTool.MIN_BLOCK_SIZE + 2]
    assert panel.block_size == MosaicTool.MIN_BLOCK_SIZE + 2

    panel.set_block_size(MosaicTool.DEFAULT_BLOCK_SIZE)
    assert emitted == [MosaicTool.MIN_BLOCK_SIZE + 2]  # set_block_size 不应重新触发信号


def test_mosaic_icon_exists():
    icon = Path(__file__).parents[2] / "svg" / "马赛克.svg"
    assert icon.is_file()
    assert "<svg" in icon.read_text(encoding="utf-8")


def test_annotation_shortcut_translations_exist_and_load(qapp):
    import xml.etree.ElementTree as ET

    translations = Path(__file__).parents[1] / "translations"
    expected_by_language = {
        "en": {
            "Annotation Tools": "Annotation Tools",
            "Select / Cursor": "Select / Cursor",
            "Pen": "Pen",
            "Highlighter": "Highlighter",
            "Mosaic": "Mosaic",
            "Arrow": "Arrow",
            "Number": "Number",
            "Rectangle": "Rectangle",
            "Ellipse": "Ellipse",
            "Text": "Text",
            "Eraser": "Eraser",
        },
        "zh": {
            "Annotation Tools": "标注工具",
            "Select / Cursor": "选择 / 光标",
            "Pen": "画笔",
            "Highlighter": "荧光笔",
            "Mosaic": "马赛克",
            "Arrow": "箭头",
            "Number": "序号",
            "Rectangle": "矩形",
            "Ellipse": "椭圆",
            "Text": "文字",
            "Eraser": "橡皮擦",
        },
        "ja": {
            "Annotation Tools": "注釈ツール",
            "Select / Cursor": "選択 / カーソル",
            "Pen": "ペン",
            "Highlighter": "蛍光ペン",
            "Mosaic": "モザイク",
            "Arrow": "矢印",
            "Number": "番号",
            "Rectangle": "矩形",
            "Ellipse": "楕円",
            "Text": "テキスト",
            "Eraser": "消しゴム",
        },
        "ko": {
            "Annotation Tools": "주석 도구",
            "Select / Cursor": "선택 / 커서",
            "Pen": "펜",
            "Highlighter": "형광펜",
            "Mosaic": "모자이크",
            "Arrow": "화살표",
            "Number": "번호",
            "Rectangle": "사각형",
            "Ellipse": "타원",
            "Text": "텍스트",
            "Eraser": "지우개",
        },
    }
    hint_source = "💡 Configured shortcuts take priority over WASD and C. Arrow keys remain available; Esc is reserved."

    for language, expected in expected_by_language.items():
        root = ET.parse(translations / f"app_{language}.xml").getroot()
        settings_context = next(
            context
            for context in root.findall("context")
            if context.findtext("name") == "SettingsDialog"
        )
        settings_sources = {
            node.text for node in settings_context.findall("message/source")
        }
        assert set(expected) | {hint_source} <= settings_sources

        translator = QTranslator()
        assert translator.load(str(translations / f"app_{language}.qm"))
        for source, translated in expected.items():
            assert translator.translate("SettingsDialog", source) == translated


def test_block_size_slider_only_reports_the_released_value(qapp):
    """拖动中的中间值是"还没想好"，不该发出去。

    每个中间值都会让下游重算整张缩小图并压一条撤销命令，所以一次拖动必须
    只对应一次上报。
    """
    from ui.mosaic_settings_panel import MosaicSettingsPanel

    panel = MosaicSettingsPanel()
    slider = panel.block_size_slider
    reported = []
    panel.block_size_changed.connect(reported.append)

    assert slider.hasTracking() is False

    # sliderMoved 是拖动中的中间值：Qt 在 tracking 关闭时不会转成 valueChanged
    slider.setSliderDown(True)
    for value in range(slider.minimum(), slider.maximum() + 1):
        slider.setSliderPosition(value)
    assert reported == []

    # 松手才是一次决定
    slider.setSliderDown(False)
    assert reported == [slider.maximum()]
    assert panel.block_size == slider.maximum()


def test_block_size_slider_still_reports_each_keyboard_step(qapp):
    """键盘每按一下就是一次独立的决定，仍然要逐次上报。"""
    from PySide6.QtWidgets import QSlider
    from ui.mosaic_settings_panel import MosaicSettingsPanel

    panel = MosaicSettingsPanel()
    slider = panel.block_size_slider
    slider.setValue(slider.minimum())
    reported = []
    panel.block_size_changed.connect(reported.append)

    slider.triggerAction(QSlider.SliderAction.SliderSingleStepAdd)
    slider.triggerAction(QSlider.SliderAction.SliderSingleStepAdd)

    assert reported == [slider.minimum() + 1, slider.minimum() + 2]
