from pathlib import Path
from types import SimpleNamespace

import pytest

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


# ---------------------------------------------------------------------------
# 工具栏 → 面板 → 设置 的转发接线
# ---------------------------------------------------------------------------

@pytest.fixture
def mosaic_settings():
    """给马赛克设置做快照，用完还原。"""
    from settings import get_tool_settings_manager

    manager = get_tool_settings_manager()
    keys = ("draw_mode", "style", "block_size")
    before = {k: manager.get_setting("mosaic", k) for k in keys}
    yield manager
    manager.update_settings("mosaic", **before)


class _FakeCursorManager:
    def __init__(self):
        self.calls = []

    def set_tool_cursor(self, tool_id, force=False):
        self.calls.append((tool_id, force))


def _toolbar_with_host():
    """造一个能被 _host_window() 找到画布的工具栏。"""
    toolbar = Toolbar()
    cursor_manager = _FakeCursorManager()
    host = SimpleNamespace(view=SimpleNamespace(cursor_manager=cursor_manager))
    toolbar._host_window = lambda: host
    return toolbar, cursor_manager


def test_switching_draw_mode_persists_it_and_refreshes_the_cursor(qapp, mosaic_settings):
    """框选/自由涂抹的光标不同，改了模式光标必须跟着换。"""
    from tools.mosaic import MosaicTool

    toolbar, cursor_manager = _toolbar_with_host()
    try:
        toolbar._on_mosaic_mode_changed(MosaicTool.MODE_RECT)
        assert mosaic_settings.get_setting("mosaic", "draw_mode") == MosaicTool.MODE_RECT
        assert ("mosaic", True) in cursor_manager.calls
    finally:
        toolbar.deleteLater()


def test_style_and_granularity_are_persisted_and_forwarded(qapp, mosaic_settings):
    from tools.mosaic import MosaicTool

    toolbar, _ = _toolbar_with_host()
    styles, sizes = [], []
    toolbar.mosaic_style_changed.connect(styles.append)
    toolbar.mosaic_block_size_changed.connect(sizes.append)
    try:
        toolbar._on_mosaic_style_changed(MosaicTool.STYLE_BLUR)
        toolbar._on_mosaic_block_size_changed(16)

        assert mosaic_settings.get_setting("mosaic", "style") == MosaicTool.STYLE_BLUR
        assert mosaic_settings.get_setting("mosaic", "block_size") == 16
        assert styles == [MosaicTool.STYLE_BLUR]
        assert sizes == [16]
    finally:
        toolbar.deleteLater()


def test_a_temporary_cross_tool_edit_never_writes_the_tool_default(qapp, mosaic_settings):
    """Ctrl 临时编辑只该改选中的那一块，不该改工具的默认值。

    但信号还是要发——否则选中的那块图元不会跟着变。
    """
    from tools.mosaic import MosaicTool

    mosaic_settings.update_settings(
        "mosaic",
        draw_mode=MosaicTool.MODE_FREEHAND,
        style=MosaicTool.STYLE_PIXELATE,
        block_size=8,
    )
    toolbar, _ = _toolbar_with_host()
    styles, sizes = [], []
    toolbar.mosaic_style_changed.connect(styles.append)
    toolbar.mosaic_block_size_changed.connect(sizes.append)
    try:
        toolbar.set_temporary_edit_active(True)
        toolbar._on_mosaic_mode_changed(MosaicTool.MODE_RECT)
        toolbar._on_mosaic_style_changed(MosaicTool.STYLE_BLUR)
        toolbar._on_mosaic_block_size_changed(24)

        assert mosaic_settings.get_setting("mosaic", "draw_mode") == MosaicTool.MODE_FREEHAND
        assert mosaic_settings.get_setting("mosaic", "style") == MosaicTool.STYLE_PIXELATE
        assert mosaic_settings.get_setting("mosaic", "block_size") == 8
        assert styles == [MosaicTool.STYLE_BLUR]
        assert sizes == [24]
    finally:
        toolbar.deleteLater()


def test_the_panel_is_filled_from_the_saved_settings(qapp, mosaic_settings):
    """三个入口（启动加载 / 切回工具 / 单独弹面板）都走同一份回填。"""
    from tools.mosaic import MosaicTool

    mosaic_settings.update_settings(
        "mosaic", draw_mode=MosaicTool.MODE_RECT,
        style=MosaicTool.STYLE_BLUR, block_size=20,
    )
    toolbar = Toolbar()
    try:
        toolbar._sync_mosaic_panel()
        assert toolbar.mosaic_panel.draw_mode == MosaicTool.MODE_RECT
        assert toolbar.mosaic_panel.style == MosaicTool.STYLE_BLUR
        assert toolbar.mosaic_panel.block_size == 20

        toolbar.mosaic_panel.set_draw_mode(MosaicTool.MODE_FREEHAND)
        toolbar.restore_active_tool_state("mosaic")
        assert toolbar.mosaic_panel.draw_mode == MosaicTool.MODE_RECT
    finally:
        toolbar.deleteLater()


def test_switching_language_refreshes_button_tips_and_the_mosaic_panel(qapp):
    toolbar = Toolbar()
    try:
        toolbar.mosaic_btn.setToolTip("stale")
        toolbar.mosaic_panel.style_combo.setItemText(0, "stale")

        toolbar._retranslate("zh")

        assert toolbar.mosaic_btn.toolTip() != "stale"
        assert toolbar.mosaic_panel.style_combo.itemText(0) != "stale"
    finally:
        toolbar.deleteLater()


def test_a_toolbar_without_a_host_canvas_just_has_no_cursor_manager(qapp):
    """工具栏可以先于画布存在，找不到宿主时静默降级而不是炸。"""
    toolbar = Toolbar()
    try:
        assert toolbar._host_cursor_manager() is None
        toolbar._on_mosaic_mode_changed("rect")  # 不应抛异常
    finally:
        toolbar.deleteLater()


def test_the_panel_emits_what_the_user_picked(qapp):
    """面板自己的控件 → 信号；set_* 回填则不该触发信号（否则会绕回来打架）。"""
    from tools.mosaic import MosaicTool
    from ui.mosaic_settings_panel import MosaicSettingsPanel

    panel = MosaicSettingsPanel()
    modes, styles, sizes = [], [], []
    panel.draw_mode_changed.connect(modes.append)
    panel.style_changed.connect(styles.append)
    panel.size_changed.connect(sizes.append)
    try:
        panel.rect_btn.setChecked(True)
        panel._on_mode_clicked()
        assert modes == [MosaicTool.MODE_RECT]

        panel.style_combo.setCurrentIndex(panel.style_combo.findData(MosaicTool.STYLE_BLUR))
        assert styles == [MosaicTool.STYLE_BLUR]

        panel._on_size_changed(42)
        assert sizes == [42]

        # 回填不发信号
        panel.set_draw_mode(MosaicTool.MODE_FREEHAND)
        panel.set_style(MosaicTool.STYLE_PIXELATE)
        panel.set_size(20)
        panel.set_block_size(12)
        assert modes == [MosaicTool.MODE_RECT]
        assert styles == [MosaicTool.STYLE_BLUR]
        assert sizes == [42]

        assert panel.draw_mode == MosaicTool.MODE_FREEHAND
        assert panel.style == MosaicTool.STYLE_PIXELATE
        assert panel.block_size == 12
    finally:
        panel.deleteLater()


def test_the_panel_normalises_junk_instead_of_showing_it(qapp):
    from tools.mosaic import MosaicTool
    from ui.mosaic_settings_panel import MosaicSettingsPanel

    panel = MosaicSettingsPanel()
    try:
        panel.set_draw_mode("nonsense")
        panel.set_style("nonsense")
        panel.set_block_size(9999)
        assert panel.draw_mode == MosaicTool.MODE_FREEHAND
        assert panel.style == MosaicTool.STYLE_PIXELATE
        assert panel.block_size == MosaicTool.MAX_BLOCK_SIZE
    finally:
        panel.deleteLater()


def test_the_centred_combo_paints_its_text_itself(qapp):
    """闭合态文本居中是自绘的（QSS 的 text-align 对 QComboBox 无效）。"""
    from PySide6.QtGui import QImage
    from ui.mosaic_settings_panel import MosaicSettingsPanel

    panel = MosaicSettingsPanel()
    try:
        combo = panel.style_combo
        combo.resize(72, 24)
        image = QImage(72, 24, QImage.Format.Format_ARGB32)
        image.fill(0)
        combo.render(image)
        assert any(
            image.pixelColor(x, y).alpha() > 0
            for y in range(image.height())
            for x in range(image.width())
        )
    finally:
        panel.deleteLater()
