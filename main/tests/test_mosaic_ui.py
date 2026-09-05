from pathlib import Path

from PySide6.QtCore import QPoint, QPointF, QRectF, Qt, QTranslator
from PySide6.QtGui import QImage

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


def test_pin_toolbar_hides_mosaic_without_overlap(qapp):
    toolbar = PinToolbar()

    assert toolbar.mosaic_btn.isHidden()
    assert "mosaic" not in toolbar.tool_buttons
    visible = [button.geometry() for button in toolbar.tool_buttons.values() if not button.isHidden()]
    for index, left in enumerate(visible):
        for right in visible[index + 1:]:
            assert not left.intersects(right)


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
