"""
文字描边 / 阴影

这两个效果长期只有字段和空的绘制分支：TextItem 里默认开着，画的时候什么都没画，
工具栏上声明了信号却没有人发。补完之后，这里锁住几件容易悄悄退化的事：
画出来的描边贴着字、包围盒包得住、半透明阴影不叠色、改动落到选中的图元上但不进撤销栈、
设置能存能读、钉图克隆带得走、三个宿主窗口都接上了信号。
"""
import xml.etree.ElementTree as ET
from math import ceil

import pytest
from PySide6.QtCore import QEvent, QPointF, QRectF, Qt, QTranslator
from PySide6.QtGui import QColor, QFont, QImage, QPainter
from PySide6.QtWidgets import QApplication, QSlider, QStyleOptionGraphicsItem

from canvas.items import TextItem
from canvas.scene import CanvasScene
from canvas.view import CanvasView
from settings import get_tool_settings_manager
from ui.text_settings_panel import OUTLINE_WIDTH_TIPS, TextSettingsPanel

THIN, MEDIUM, THICK, EXTRA_THICK = TextItem.OUTLINE_WIDTH_LEVELS


def _text_item(text="Ag", size=40):
    item = TextItem(text, QPointF(0, 0), QFont("Arial", size), QColor("black"))
    item.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)
    return item


def _render(item, margin=0) -> QImage:
    """按包围盒出图；margin 把画布再往外放一圈，用来检查有没有画出包围盒。"""
    rect = item.boundingRect()
    image = QImage(
        ceil(rect.width()) + 2 * margin,
        ceil(rect.height()) + 2 * margin,
        QImage.Format.Format_ARGB32_Premultiplied,
    )
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    try:
        painter.translate(margin - rect.left(), margin - rect.top())
        item.paint(painter, QStyleOptionGraphicsItem(), None)
    finally:
        painter.end()
    return image


def _alphas(image: QImage):
    return [image.pixelColor(x, y).alpha() for y in range(image.height()) for x in range(image.width())]


def _painted_count(image: QImage) -> int:
    return sum(1 for alpha in _alphas(image) if alpha > 10)


@pytest.fixture
def restore_text_settings():
    """用例会改全局设置，跑完还原，免得污染别的用例。"""
    manager = get_tool_settings_manager()
    original = manager.get_tool_settings("text").to_dict()
    yield manager
    manager.update_settings("text", **original)


# ======================================================================
# 图元：画法与几何
# ======================================================================


def test_effects_are_off_by_default_and_the_settings_agree(qapp):
    """默认值有两份（设置模块不能 import canvas），这里保证两份说的是同一件事。

    默认关闭是有意的：此前描边、阴影虽然"默认开启"，却从来没真正画出来过，
    用户看到的一直是没有效果的字。补完画法不该让所有人的文字突然变样。
    """
    item = _text_item()
    assert not item.has_outline and not item.has_shadow

    defaults = get_tool_settings_manager().DEFAULT_SETTINGS["text"]
    assert defaults["outline_enabled"] is False
    assert defaults["shadow_enabled"] is False
    assert QColor(defaults["outline_color"]) == QColor(TextItem.DEFAULT_OUTLINE_COLOR)
    assert QColor(defaults["shadow_color"]) == QColor(TextItem.DEFAULT_SHADOW_COLOR)
    assert defaults["outline_width"] == TextItem.DEFAULT_OUTLINE_WIDTH
    assert TextItem.DEFAULT_OUTLINE_WIDTH in TextItem.OUTLINE_WIDTH_LEVELS


def test_outline_actually_paints_pixels(qapp):
    """描边曾经是纯摆设：8 次 save/translate/restore 中间没有任何绘制调用。"""
    item = _text_item()
    base = _painted_count(_render(item))

    item.set_outline(True, QColor("red"), THICK)

    assert _painted_count(_render(item)) > base


def test_shadow_actually_paints_pixels(qapp):
    """阴影分支曾经是一个孤零零的 pass，打开开关画面没有任何变化。"""
    item = _text_item()
    base = _painted_count(_render(item))

    item.set_shadow(True, QColor(0, 0, 0, 255))

    assert _painted_count(_render(item)) > base


def test_glyph_outline_lines_up_with_the_painted_text(qapp):
    """描边、阴影都沿 _glyph_path 画，它必须和 QGraphicsTextItem 自己画出来的字重合。

    自己用 addText 重排一遍就得猜边距，历史上那版描边就是卡死在对齐上。
    """
    item = TextItem("Ag\n标注", QPointF(0, 0), QFont("Arial", 40), QColor("black"))
    text_mask = _render(item)

    glyph_mask = QImage(text_mask.size(), QImage.Format.Format_ARGB32_Premultiplied)
    glyph_mask.fill(Qt.GlobalColor.transparent)
    painter = QPainter(glyph_mask)
    try:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        # 和 _render 用同一个平移：包围盒左上角未必就是文字起点（框要让开光标、
        # 命中区还有旷量），不对齐的话量到的是这段固定偏移，而不是两者的差异。
        painter.translate(-item.boundingRect().topLeft())
        painter.fillPath(item._glyph_path(), QColor("black"))
    finally:
        painter.end()

    # 比的是墨迹的范围而不是逐像素重合：测试跑在 offscreen 平台上，那里没有字体，
    # 每个字都画成 1px 的空心方框，逐像素比较只是在比抗锯齿。而且这个替身字体画
    # 出来的方框和它报告的轮廓整体差 2px（实测三行文字四条边都是 +2）；真实字体下
    # 两者完全重合。所以要求"整体平移、且不超过 2px"：猜错 3px 文档边距、第二行
    # 错位这类真正会出的问题都过不去。
    def ink_extent(image):
        points = [
            (x, y)
            for y in range(image.height())
            for x in range(image.width())
            if image.pixelColor(x, y).alpha() > 64
        ]
        assert points, "什么都没画出来，这个用例就失去了意义"
        xs, ys = zip(*points)
        return min(xs), min(ys), max(xs), max(ys)

    shifts = {glyph_edge - text_edge for text_edge, glyph_edge in zip(ink_extent(text_mask), ink_extent(glyph_mask))}
    assert len(shifts) == 1, f"字形轮廓和画出来的字不是整体平移关系：{shifts}"
    assert abs(shifts.pop()) <= 2


def test_underline_is_part_of_the_outline(qapp):
    """下划线不在字形轮廓里，不补上的话开了描边的下划线是光秃秃的一条。"""
    font = QFont("Arial", 40)
    plain = TextItem("AB", QPointF(0, 0), font, QColor("black"))
    font.setUnderline(True)
    underlined = TextItem("AB", QPointF(0, 0), font, QColor("black"))

    plain_path = plain._glyph_path()
    underlined_path = underlined._glyph_path()
    # 字形完全一样，多出来的只能是下划线那一条矩形
    assert underlined_path.elementCount() == plain_path.elementCount() + 5
    assert underlined_path.boundingRect().contains(plain_path.boundingRect())


def test_effects_scale_with_the_font_size(qapp):
    """描边粗细、阴影距离按字号比例算：拖手柄把字放大，同一档的观感不变。"""
    item = _text_item(size=20)
    item.set_outline(True, QColor("white"), MEDIUM)
    item.set_shadow(True)
    outline, shadow = item.outline_extent(), item.shadow_distance()

    item.set_font_point_size(40)

    assert item.outline_extent() == pytest.approx(outline * 2)
    assert item.shadow_distance() == pytest.approx(shadow * 2)


def test_bounding_rect_contains_everything_painted(qapp):
    """包围盒漏掉描边和阴影，拖动时会留残影、导出时会被裁掉。"""
    item = _text_item()
    item.set_outline(True, QColor("red"), EXTRA_THICK)
    item.set_shadow(True, QColor(0, 0, 0, 255))
    rect = item.boundingRect()
    margin = 30

    image = _render(item, margin)
    inside = QRectF(margin - 1, margin - 1, rect.width() + 2, rect.height() + 2)
    outside = [
        (x, y)
        for y in range(image.height())
        for x in range(image.width())
        if image.pixelColor(x, y).alpha() > 0 and not inside.contains(x + 0.5, y + 0.5)
    ]
    assert not outside, f"有 {len(outside)} 个像素画到了包围盒外面"


def test_turning_effects_on_grows_the_bounding_rect_and_off_restores_it(qapp):
    item = _text_item()
    plain = item.boundingRect()

    item.set_outline(True, QColor("white"), THICK)
    item.set_shadow(True)
    assert item.boundingRect().contains(plain)
    assert item.boundingRect() != plain

    item.set_outline(False)
    item.set_shadow(False)
    assert item.boundingRect() == plain


def test_translucent_shadow_is_not_painted_twice_under_the_outline(qapp):
    """开了描边的阴影由"字形 + 描边带"拼成，两部分各画一次会在字形边缘内侧叠两层。

    这里把字和描边都设成全透明，画面上只剩阴影：任何一个像素都不该比阴影色本身更深。
    """
    shadow_alpha = 102
    item = TextItem("Ag", QPointF(0, 0), QFont("Arial", 40), QColor(0, 0, 0, 0))
    item.set_outline(True, QColor(0, 0, 0, 0), MEDIUM)
    item.set_shadow(True, QColor(0, 0, 0, shadow_alpha))

    alphas = _alphas(_render(item))

    assert any(alphas), "阴影没画出来"
    assert max(alphas) <= shadow_alpha + 5


def test_outline_width_snaps_to_a_level(qapp):
    assert TextItem.normalize_outline_width(THICK) == THICK
    assert TextItem.normalize_outline_width(0) == THIN
    assert TextItem.normalize_outline_width(3) == EXTRA_THICK
    # 正中间取更粗的一档
    assert TextItem.normalize_outline_width((MEDIUM + THICK) / 2) == THICK
    assert TextItem.normalize_outline_width("garbage") == TextItem.DEFAULT_OUTLINE_WIDTH
    assert TextItem.normalize_outline_width(None) == TextItem.DEFAULT_OUTLINE_WIDTH

    item = _text_item()
    item.set_outline(True, width=0.1)
    assert item.outline_width in TextItem.OUTLINE_WIDTH_LEVELS


# ======================================================================
# 选中图元上的改动：连带改，但不进撤销栈
# ======================================================================


def _selected_text():
    image = QImage(200, 120, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    scene = CanvasScene(image, QRectF(0, 0, 200, 120))
    view = CanvasView(scene)
    item = _text_item(size=20)
    scene.addItem(item)
    view.smart_edit_controller.select_item(item)
    return scene, view, item


def test_effects_follow_the_selected_text(qapp):
    """面板上改效果，画布上选中的那一个要跟着变，而不只影响之后新建的。"""
    scene, view, item = _selected_text()
    try:
        controller = view.smart_edit_controller
        controller.on_text_outline_changed(True, QColor("#00FF00"), THICK)
        controller.on_text_shadow_changed(True, QColor(255, 0, 0, 80))

        assert item.outline_state() == (True, QColor("#00FF00"), THICK)
        assert item.shadow_state() == (True, QColor(255, 0, 0, 80))
    finally:
        view.close()
        scene.deleteLater()


def test_changing_effects_never_touches_the_undo_stack(qapp):
    """撤销栈留给画布内容（新建、删除、移动、缩放），面板上调样式不该占掉一次 Ctrl+Z。

    阴影不透明度还是个滑块，拖一下就连发几十次改动——真要入栈，撤销得按几十下才
    退得回去。字体、颜色、背景一直就是直接改不入栈，描边和阴影跟它们保持一致。
    """
    scene, view, item = _selected_text()
    try:
        controller = view.smart_edit_controller
        controller.on_text_outline_changed(True, QColor("white"), THICK)
        for alpha in range(100, 200, 10):
            controller.on_text_shadow_changed(True, QColor(0, 0, 0, alpha))

        assert item.has_outline and item.shadow_color.alpha() == 190
        assert scene.undo_stack.count() == 0
    finally:
        view.close()
        scene.deleteLater()


# ======================================================================
# 面板
# ======================================================================


def test_panel_shows_the_selected_items_effects(qapp):
    item = _text_item()
    item.set_outline(True, QColor("#00FF00"), EXTRA_THICK)
    item.set_shadow(True, QColor(255, 0, 0, 51))
    panel = TextSettingsPanel()
    try:
        panel.set_state_from_item(item)

        assert panel.outline_btn.isChecked()
        assert panel.outline_color_btn.color == QColor("#00FF00")
        assert panel.outline_width_slider.value() == TextItem.OUTLINE_WIDTH_LEVELS.index(EXTRA_THICK)
        assert panel.shadow_btn.isChecked()
        assert panel.shadow_opacity_slider.value() == 51
    finally:
        panel.deleteLater()


def test_panel_emits_outline_changes_and_ignores_reselecting_the_current_level(qapp):
    panel = TextSettingsPanel()
    emitted = []
    panel.outline_changed.connect(lambda *args: emitted.append(args))
    try:
        panel.outline_btn.setChecked(True)
        assert emitted[-1][0] is True

        panel.outline_width_slider.setValue(TextItem.OUTLINE_WIDTH_LEVELS.index(THICK))
        assert emitted[-1][2] == THICK

        count = len(emitted)
        panel.outline_width_slider.setValue(TextItem.OUTLINE_WIDTH_LEVELS.index(THICK))
        assert len(emitted) == count, "重设当前档不该重复上报"
    finally:
        panel.deleteLater()


def test_picking_a_shadow_color_keeps_its_opacity(qapp):
    """颜色和不透明度分开调：点预设色不能把滑块调好的不透明度冲掉。"""
    panel = TextSettingsPanel()
    emitted = []
    panel.shadow_changed.connect(lambda *args: emitted.append(args))
    try:
        panel.set_shadow_settings(True, QColor(0, 0, 0, 51))
        panel._on_shadow_color_picked(QColor("#FFFF00"))
        assert emitted[-1][1] == QColor(255, 255, 0, 51)
    finally:
        panel.deleteLater()


def test_effect_popup_opens_on_hover_only_while_the_switch_is_on(qapp):
    panel = TextSettingsPanel()
    popup = panel._effect_popups[panel.outline_btn]
    try:
        panel.show()
        # 必须走 sendEvent：直接调 widget.event() 会绕过事件过滤器
        QApplication.sendEvent(panel.outline_btn, QEvent(QEvent.Type.Enter))
        assert not popup.isVisible(), "开关关着的时候不该弹设置层"

        panel.set_outline_settings(True, QColor("white"), MEDIUM)
        QApplication.sendEvent(panel.outline_btn, QEvent(QEvent.Type.Enter))
        assert popup.isVisible()

        panel.outline_btn.setChecked(False)
        assert not popup.isVisible(), "关掉开关要把设置层一起收起"
    finally:
        popup.hide()
        panel.deleteLater()


def test_effect_popup_disappears_with_the_panel(qapp):
    """弹出层是独立的顶层窗口，面板隐藏时 Qt 不会连它一起藏。

    切工具、结束截图都会隐藏面板，漏掉这一步屏幕上就留着一块盖在截图上的白框。
    """
    panel = TextSettingsPanel()
    popup = panel._effect_popups[panel.shadow_btn]
    try:
        panel.show()
        popup.show_beside(panel, panel.shadow_btn)
        assert popup.isVisible()

        panel.hide()
        assert not popup.isVisible()
    finally:
        popup.hide()
        panel.deleteLater()


def test_effect_popups_fit_their_contents_instead_of_expanding_to_200px(qapp):
    """顶层 QWidget 遇到可扩展滑条会默认撑到 200px，弹层应收口到内容宽度。"""
    panel = TextSettingsPanel()
    try:
        for popup in panel._effect_popups.values():
            assert popup.maximumWidth() == popup.sizeHint().width()
            assert popup.maximumWidth() < 200
    finally:
        panel.deleteLater()


def test_outline_width_slider_is_discrete_and_maps_every_level(qapp):
    """描边只有四档；滑条的每一个位置都必须一一对应一个合法档位。"""
    panel = TextSettingsPanel()
    emitted = []
    panel.outline_changed.connect(lambda *args: emitted.append(args))
    try:
        assert len(OUTLINE_WIDTH_TIPS) == len(TextItem.OUTLINE_WIDTH_LEVELS)
        slider = panel.outline_width_slider
        assert slider.minimum() == 0
        assert slider.maximum() == len(TextItem.OUTLINE_WIDTH_LEVELS) - 1
        assert slider.singleStep() == 1
        assert slider.pageStep() == 1
        assert slider.tickInterval() == 1
        assert slider.tickPosition() == QSlider.TickPosition.TicksBelow
        assert not slider.hasTracking()

        panel.outline_btn.setChecked(True)
        for index, level in enumerate(TextItem.OUTLINE_WIDTH_LEVELS):
            slider.setValue(index)
            assert panel.outline_width == level
            assert emitted[-1][2] == level
    finally:
        panel.deleteLater()


# ======================================================================
# 设置、工具、克隆、宿主接线
# ======================================================================


def test_effect_settings_round_trip_through_config(qapp, restore_text_settings):
    TextSettingsPanel.save_outline_to_config(True, QColor("#123456"), 0.1)
    TextSettingsPanel.save_shadow_to_config(True, QColor(255, 0, 0, 51))

    panel = TextSettingsPanel()
    try:
        panel.load_from_config()
        assert panel.outline_enabled and panel.outline_color == QColor("#123456")
        assert panel.outline_width == THICK
        assert panel.shadow_enabled and panel.shadow_color == QColor(255, 0, 0, 51)
    finally:
        panel.deleteLater()


def test_new_text_uses_the_saved_effects(qapp, restore_text_settings):
    restore_text_settings.update_settings(
        "text",
        outline_enabled=True, outline_color="#00FF00", outline_width=EXTRA_THICK,
        shadow_enabled=True, shadow_color="#33FF0000",
    )
    image = QImage(200, 120, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    scene = CanvasScene(image, QRectF(0, 0, 200, 120))
    view = CanvasView(scene)
    try:
        scene.activate_tool("text")
        scene.tool_controller.on_press(QPointF(20, 20), Qt.MouseButton.LeftButton)
        item = next(i for i in scene.items() if isinstance(i, TextItem))

        assert item.outline_state() == (True, QColor("#00FF00"), EXTRA_THICK)
        assert item.shadow_state() == (True, QColor(255, 0, 0, 0x33))
    finally:
        view.close()
        scene.deleteLater()


def test_toolbar_saves_effects_unless_editing_temporarily(qapp, monkeypatch):
    from ui.toolbar import Toolbar

    saved = []
    monkeypatch.setattr(TextSettingsPanel, "save_outline_to_config",
                        staticmethod(lambda *args: saved.append(("outline", args))))
    monkeypatch.setattr(TextSettingsPanel, "save_shadow_to_config",
                        staticmethod(lambda *args: saved.append(("shadow", args))))
    toolbar = Toolbar()
    emitted = []
    toolbar.text_outline_changed.connect(lambda *args: emitted.append("outline"))
    toolbar.text_shadow_changed.connect(lambda *args: emitted.append("shadow"))
    try:
        toolbar._on_text_outline_changed(True, QColor("white"), MEDIUM)
        toolbar._on_text_shadow_changed(True, QColor(0, 0, 0, 102))
        assert [kind for kind, _ in saved] == ["outline", "shadow"]

        saved.clear()
        toolbar.set_temporary_edit_active(True)
        toolbar._on_text_outline_changed(False, QColor("white"), MEDIUM)
        toolbar._on_text_shadow_changed(False, QColor(0, 0, 0, 102))
        assert saved == [], "跨工具临时编辑不能改写文字工具的默认值"
        assert emitted == ["outline", "shadow", "outline", "shadow"]
    finally:
        toolbar.deleteLater()


def test_a_pinned_copy_keeps_outline_and_shadow(qapp):
    """描边、阴影是矢量信息，钉图克隆必须带上，否则钉完就退回默认。"""
    from pin.pin_canvas import PinCanvas

    item = _text_item()
    item.set_outline(True, QColor("#00FF00"), THICK)
    item.set_shadow(True, QColor(255, 0, 0, 51))

    clone = PinCanvas._clone_text_item(None, item)

    assert clone.outline_state() == item.outline_state()
    assert clone.shadow_state() == item.shadow_state()


def _referenced_names(function):
    """函数字节码里引用到的名字，比对源码文本可靠（注释里的同名字符串不会误判）。"""
    code = function.__code__
    return set(code.co_names) | {c for c in code.co_consts if isinstance(c, str)}


def test_every_host_forwards_outline_and_shadow_to_the_selection(qapp):
    """截图、钉图、GIF 录制三个窗口共用文字面板，少接一个，那个窗口里点了就没反应。"""
    from gif.record_window import GifRecordWindow
    from pin.pin_canvas import PinCanvas
    from ui.screenshot_window import ScreenshotWindow

    for function in (ScreenshotWindow._connect_session_signals, ScreenshotWindow._disconnect_session_signals):
        names = _referenced_names(function)
        assert {"text_outline_changed", "text_shadow_changed"} <= names, function.__name__

    assert {"text_outline_changed", "text_shadow_changed"} <= _referenced_names(PinCanvas.connect_toolbar)
    assert {"outline_changed", "shadow_changed"} <= _referenced_names(GifRecordWindow._connect_record_toolbar)


@pytest.mark.parametrize("language", ["en", "ja", "ko", "zh"])
def test_effect_texts_are_compiled_in_their_contexts(language):
    from core.i18n import I18nManager

    translations_dir = I18nManager.get_translations_dir()
    root = ET.parse(translations_dir / f"app_{language}.xml").getroot()
    translator = QTranslator()
    assert translator.load(str(translations_dir / f"app_{language}.qm"))

    sources = [
        ("TextSettingsPanel", "Text Outline"),
        ("TextSettingsPanel", "Text Shadow"),
        ("TextSettingsPanel", "Custom Outline Color"),
        ("TextSettingsPanel", "Custom Shadow Color"),
        ("TextSettingsPanel", "Shadow Opacity"),
        *(("TextSettingsPanel", tip) for tip in OUTLINE_WIDTH_TIPS),
    ]
    for context_name, source in sources:
        expected = {
            message.findtext("source"): message.findtext("translation")
            for context in root.findall("context")
            if context.findtext("name") == context_name
            for message in context.findall("message")
        }
        assert expected.get(source), (context_name, source)
        assert translator.translate(context_name, source) == expected[source]
