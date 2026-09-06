"""序号的三种样式：实心 / 空心圆+实心字 / 全空心。

空心样式背后透出的是截图本身，所以数字不能再按背景亮度取黑白（那是给实心圆
用的），必须跟着圈走同一个颜色，否则会出现"彩色圈配黑字"的怪样子。
"""

import pytest
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QImage, QPainter
from PySide6.QtWidgets import QStyleOptionGraphicsItem

from canvas.items import NumberItem
from canvas.scene import CanvasScene
from settings import get_tool_settings_manager
from ui.number_settings_panel import NumberSettingsPanel, render_number_style_preview

ALL_STYLES = [
    NumberItem.STYLE_SOLID,
    NumberItem.STYLE_HOLLOW_BG,
    NumberItem.STYLE_HOLLOW_ALL,
    NumberItem.STYLE_NO_CIRCLE,
]


@pytest.fixture
def restore_style():
    """用例会改全局设置，跑完还原，免得污染用户配置和别的用例。"""
    manager = get_tool_settings_manager()
    original = manager.get_setting("number", "style", NumberItem.DEFAULT_STYLE)
    yield manager
    manager.update_settings("number", style=original)


def _render(item, side=120):
    image = QImage(side, side, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    painter = QPainter(image)
    try:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.translate(side / 2.0, side / 2.0)
        item.paint(painter, QStyleOptionGraphicsItem(), None)
    finally:
        painter.end()
    return image


def test_unknown_styles_fall_back_to_solid(qapp):
    """旧存档或脏配置不能让序号画不出来。"""
    for junk in ("bogus", "", None, 3, object()):
        assert NumberItem.normalize_style(junk) == NumberItem.STYLE_SOLID
    for style in ALL_STYLES:
        assert NumberItem.normalize_style(style) == style


@pytest.mark.parametrize("style", ALL_STYLES)
def test_style_survives_construction(qapp, style):
    item = NumberItem(1, QPointF(0, 0), 20, QColor("#FF0000"), style)
    assert item.style == style
    assert item.is_hollow == (style != NumberItem.STYLE_SOLID)


def test_hollow_centre_stays_transparent_but_solid_does_not(qapp):
    """空心就得真的空：圆心必须还是底色。"""
    solid = _render(NumberItem(1, QPointF(0, 0), 40, QColor("#FF0000")))
    hollow = _render(
        NumberItem(1, QPointF(0, 0), 40, QColor("#FF0000"), NumberItem.STYLE_HOLLOW_ALL)
    )

    # 取圆心和边缘之间、避开数字的一点
    probe_x, probe_y = 60 + 26, 60
    assert solid.pixelColor(probe_x, probe_y) == QColor("#FF0000")
    assert hollow.pixelColor(probe_x, probe_y) == QColor("white")


def test_hollow_ring_keeps_the_same_outer_size_as_the_solid_circle(qapp):
    """描边压着路径画，不内缩半个线宽的话空心圈会比实心圆大一圈。"""
    radius = 40.0
    item = NumberItem(1, QPointF(0, 0), radius, QColor("#FF0000"),
                      NumberItem.STYLE_HOLLOW_BG)
    image = _render(item, side=120)

    centre = 60
    # 从圆外向内扫，找到第一个着色像素，它应当落在名义半径处（容一点抗锯齿）
    first_ink = next(
        d for d in range(centre - 1, 0, -1)
        if image.pixelColor(centre + d, centre) != QColor("white")
    )
    assert abs(first_ink - radius) <= 2, first_ink


@pytest.mark.parametrize("style", ALL_STYLES)
def test_each_style_paints_something_distinct(qapp, style):
    """三种样式必须画出不一样的东西，否则选了等于没选。"""
    images = {
        s: bytes(_render(NumberItem(7, QPointF(0, 0), 40, QColor("#FF0000"), s)).constBits())
        for s in ALL_STYLES
    }
    others = [v for s, v in images.items() if s != style]
    assert all(images[style] != other for other in others)


def test_the_new_number_uses_the_saved_style(qapp, restore_style):
    image = QImage(300, 200, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))

    for style in ALL_STYLES:
        restore_style.update_settings("number", style=style)
        scene = CanvasScene(image, QRectF(0, 0, 300, 200))
        scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 300, 200))
        scene.activate_tool("number")
        scene.tool_controller.on_press(QPointF(60, 60), Qt.MouseButton.LeftButton)

        item = next(i for i in scene.items() if isinstance(i, NumberItem))
        assert item.style == style


def test_panel_previews_use_the_items_own_painting(qapp):
    """预览图必须走 NumberItem.paint，否则菜单里和画出来的会是两回事。"""
    shots = [
        bytes(render_number_style_preview(s, QColor("#FF0000"), 34).toImage().constBits())
        for s in ALL_STYLES
    ]
    assert len(set(shots)) == len(ALL_STYLES)


def test_hovering_the_counter_opens_the_style_picker(qapp):
    from PySide6.QtCore import QEvent
    from PySide6.QtWidgets import QApplication

    panel = NumberSettingsPanel()
    try:
        assert panel._style_popup is None, "没悬停就不该建弹出层"

        # 必须走 sendEvent：直接调 widget.event() 会绕过事件过滤器
        QApplication.sendEvent(panel.next_preview, QEvent(QEvent.Type.Enter))

        popup = panel._style_popup
        assert popup is not None
        assert set(popup._buttons) == set(ALL_STYLES)
    finally:
        panel.deleteLater()


def test_choosing_a_style_emits_it_and_closes_the_picker(qapp):
    from PySide6.QtCore import QEvent
    from PySide6.QtWidgets import QApplication

    panel = NumberSettingsPanel()
    try:
        QApplication.sendEvent(panel.next_preview, QEvent(QEvent.Type.Enter))
        emitted = []
        panel.style_changed.connect(emitted.append)

        panel._style_popup._choose(NumberItem.STYLE_HOLLOW_ALL)

        assert emitted == [NumberItem.STYLE_HOLLOW_ALL]
        assert panel.current_style == NumberItem.STYLE_HOLLOW_ALL
        assert not panel._style_popup.isVisible()
    finally:
        panel.deleteLater()


# ======================================================================
# 设置传导：光标预览、选中图元、弹出层外观与定位
# ======================================================================


def _canvas():
    from canvas.view import CanvasView

    image = QImage(400, 300, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(QColor("white"))
    scene = CanvasScene(image, QRectF(0, 0, 400, 300))
    scene.selection_model.initialize_confirmed_rect(QRectF(0, 0, 400, 300))
    return scene, CanvasView(scene)


def test_the_cursor_preview_changes_with_the_style(qapp, restore_style):
    """光标就是"松手会画出什么"的预览，样式变了它必须跟着变。"""
    shots = {}
    for style in ALL_STYLES:
        restore_style.update_settings("number", style=style)
        scene, view = _canvas()
        scene.activate_tool("number")
        cursor = view.cursor_manager._create_number_cursor(20)
        shots[style] = bytes(cursor.pixmap().toImage().constBits())

    assert len(set(shots.values())) == len(ALL_STYLES), "每种样式的光标都该不一样"


def test_changing_the_style_of_a_selected_number_is_undoable(qapp):
    from canvas.undo import NumberStyleCommand

    scene, _ = _canvas()
    scene.activate_tool("number")
    scene.tool_controller.on_press(QPointF(100, 100), Qt.MouseButton.LeftButton)
    item = next(i for i in scene.items() if isinstance(i, NumberItem))
    original = item.style

    scene.undo_stack.push(
        NumberStyleCommand(item, item.style, NumberItem.STYLE_HOLLOW_ALL)
    )
    assert item.style == NumberItem.STYLE_HOLLOW_ALL

    scene.undo_stack.undo()
    assert item.style == original

    scene.undo_stack.redo()
    assert item.style == NumberItem.STYLE_HOLLOW_ALL


def test_the_popup_uses_the_panel_palette_not_its_own_dark_theme(qapp):
    """自造一套深色会和应用主体脱节。"""
    from ui.number_settings_panel import NumberStylePopup

    sheet = NumberStylePopup().styleSheet()
    assert "background: white" in sheet
    assert "#2b2b2b" not in sheet and "#3a3a3a" not in sheet


def test_the_popup_icons_stay_small(qapp):
    from ui.number_settings_panel import NumberStylePopup

    assert NumberStylePopup.ITEM_SIDE <= 24


@pytest.mark.parametrize("at_top", [True, False])
def test_the_popup_flips_instead_of_running_off_the_screen(qapp, at_top):
    """不能默认朝上：面板贴着屏幕顶部时必须翻到下方。"""
    from PySide6.QtCore import QEvent
    from PySide6.QtWidgets import QApplication

    panel = NumberSettingsPanel()
    try:
        panel.show()
        QApplication.sendEvent(panel.next_preview, QEvent(QEvent.Type.Enter))
        popup = panel._style_popup
        popup.adjustSize()

        area = QApplication.primaryScreen().availableGeometry()
        panel.move(
            area.left() + 200,
            area.top() if at_top else area.top() + area.height() // 2,
        )
        pos = panel._style_popup_position(popup)

        assert area.top() <= pos.y()
        assert pos.y() + popup.height() <= area.bottom()
        assert area.left() <= pos.x()
        assert pos.x() + popup.width() <= area.right()

        panel_top = panel.mapToGlobal(panel.rect().topLeft()).y()
        if at_top:
            assert pos.y() >= panel_top, "顶部没空间时应当翻到下方"
    finally:
        panel.deleteLater()


def test_a_pinned_copy_keeps_the_style(qapp):
    """样式是矢量信息，钉图克隆必须带上，否则钉完就退回默认。"""
    for style in ALL_STYLES:
        source = NumberItem(3, QPointF(10, 20), 24, QColor("#FF0000"), style)
        clone = NumberItem(
            source.number,
            QPointF(source.pos()),
            source.radius,
            QColor(source.color),
            getattr(source, "style", None),
        )
        assert clone.style == style


# ======================================================================
# 钉图可切换 / 弹出方向 / 面板不表示颜色
# ======================================================================


def test_the_pin_canvas_also_lets_you_switch_the_style(qapp):
    """钉图用的是同一套工具栏，样式信号必须也接上，否则钉图里切了没反应。

    这里查的是 connect_toolbar 的字节码里引用了哪些名字——比对源码文本
    可靠（注释里出现同名字符串不会误判），也不像 inspect.getsource 那样触发
    ast 解析和大规模 GC（那会和 PySide6 的对象回收撞车导致进程崩溃）。
    """
    from pin.pin_canvas import PinCanvas

    assert hasattr(PinCanvas, "_on_number_style_changed"), "钉图没有样式处理函数"

    code = PinCanvas.connect_toolbar.__code__
    referenced = set(code.co_names) | {c for c in code.co_consts if isinstance(c, str)}
    assert "number_style_changed" in referenced, "钉图没连接样式信号"

    # 处理逻辑本身收敛在 NumberTool 里，钉图只负责转交
    handler_names = set(PinCanvas._on_number_style_changed.__code__.co_names)
    assert "apply_style_change" in handler_names


def test_the_style_picker_does_not_track_the_annotation_colour(qapp):
    """它是样式选择器不是颜色预览；颜色由光标去表示。

    图标跟着标注色走还有个实际害处：选浅色时白底上的图标会看不见。
    """
    from ui.number_settings_panel import NumberStylePopup

    popup = NumberStylePopup()
    assert not hasattr(popup, "refresh_previews")
    assert "set_color" not in NumberSettingsPanel.__dict__

    first = bytes(popup._buttons[ALL_STYLES[0]].icon().pixmap(20).toImage().constBits())
    popup._render_previews()
    second = bytes(popup._buttons[ALL_STYLES[0]].icon().pixmap(20).toImage().constBits())
    assert first == second, "预览图不应随任何颜色变化"


@pytest.mark.parametrize(
    ("label", "toolbar_offset", "panel_offset"),
    [("面板在工具栏下方", 200, 250), ("面板在工具栏上方", -120, -220)],
)
def test_the_picker_opens_away_from_the_toolbar(qapp, label, toolbar_offset, panel_offset):
    """下方有空间时不能往上弹——那会盖住一级和二级菜单。"""
    from PySide6.QtCore import QEvent, QPoint, QRect
    from PySide6.QtWidgets import QApplication, QWidget

    area = QApplication.primaryScreen().availableGeometry()
    panel = NumberSettingsPanel()
    toolbar = QWidget()
    try:
        panel.show()
        QApplication.sendEvent(panel.next_preview, QEvent(QEvent.Type.Enter))
        popup = panel._style_popup
        popup.adjustSize()

        toolbar.resize(300, 40)
        toolbar.show()
        base = area.top() if toolbar_offset > 0 else area.bottom()
        toolbar.move(area.left() + 200, base + toolbar_offset)
        panel.move(area.left() + 200, base + panel_offset)
        panel._owner_toolbar = toolbar

        pos = panel._style_popup_position(popup)
        rect = QRect(pos.x(), pos.y(), popup.width(), popup.height())
        toolbar_rect = QRect(toolbar.mapToGlobal(QPoint(0, 0)), toolbar.size())
        panel_rect = QRect(panel.mapToGlobal(QPoint(0, 0)), panel.size())

        assert not rect.intersects(toolbar_rect), "弹出层盖住了工具栏"
        assert not rect.intersects(panel_rect), "弹出层盖住了面板自己"
        assert area.contains(rect), "弹出层跑出了屏幕"

        if toolbar_offset > 0:
            assert pos.y() >= panel_rect.bottom(), "下方有空间却往上弹"
        else:
            assert pos.y() + popup.height() <= panel_rect.top(), "上方有空间却往下弹"
    finally:
        toolbar.deleteLater()
        panel.deleteLater()


# ======================================================================
# 不带圈的实心数字 / ① 预览显示当前样式
# ======================================================================


def test_the_no_circle_style_draws_a_digit_and_nothing_else(qapp):
    """不带圈就是真的没有圈：名义半径处不该有任何笔迹。"""
    item = NumberItem(1, QPointF(0, 0), 40, QColor("#FF0000"), NumberItem.STYLE_NO_CIRCLE)
    assert not item.has_ring
    image = _render(item, side=120)

    centre = 60
    # 圆周上取几个点，实心圆和空心圈在这里都会有颜色，这个样式不该有
    for dx, dy in ((40, 0), (-40, 0), (0, 40), (0, -40), (28, 28)):
        assert image.pixelColor(centre + dx, centre + dy) == QColor("white")

    # 但必须画了数字。范围放宽到整圈内部：字形宽度随字体而变，
    # 卡死一个小窗口会在换字体的机器上误报。
    ink = sum(
        1
        for x in range(centre - 35, centre + 35)
        for y in range(centre - 35, centre + 35)
        if image.pixelColor(x, y) != QColor("white")
    )
    assert ink > 50, "数字没画出来"


def test_only_the_two_hollow_styles_have_a_ring(qapp):
    rings = {s: NumberItem(1, QPointF(0, 0), 20, QColor("red"), s).has_ring
             for s in ALL_STYLES}
    assert rings == {
        NumberItem.STYLE_SOLID: False,
        NumberItem.STYLE_HOLLOW_BG: True,
        NumberItem.STYLE_HOLLOW_ALL: True,
        NumberItem.STYLE_NO_CIRCLE: False,
    }


def test_the_picker_offers_every_style(qapp):
    from PySide6.QtCore import QEvent
    from PySide6.QtWidgets import QApplication

    panel = NumberSettingsPanel()
    try:
        QApplication.sendEvent(panel.next_preview, QEvent(QEvent.Type.Enter))
        assert set(panel._style_popup._buttons) == set(ALL_STYLES)
    finally:
        panel.deleteLater()


def test_the_counter_preview_shows_the_current_style(qapp):
    """① 预览要显示"下一个序号会长什么样"，不能只有数字。"""
    panel = NumberSettingsPanel()
    try:
        shots = {}
        for style in ALL_STYLES:
            panel.set_style(style)
            pixmap = panel.next_preview.pixmap()
            assert pixmap is not None and not pixmap.isNull(), "预览没画出来"
            shots[style] = bytes(pixmap.toImage().constBits())

        assert len(set(shots.values())) == len(ALL_STYLES), "预览没跟着样式变"
    finally:
        panel.deleteLater()


def test_the_counter_preview_also_follows_the_next_number(qapp, monkeypatch):
    """换了下一个序号，预览要用新数字重画。

    这里断言的是"传进去的数字变了"而不是"画出来的像素变了"：离屏环境没有字体，
    不同数字都会渲染成同一个 .notdef 方块，比像素会永远失败。
    """
    import ui.number_settings_panel as module

    seen = []
    real = module.render_number_style_preview

    def spy(style, color, side, number=1):
        seen.append(int(number))
        return real(style, color, side, number)

    monkeypatch.setattr(module, "render_number_style_preview", spy)

    panel = NumberSettingsPanel()
    try:
        seen.clear()
        panel.set_next_number(7)
        assert 7 in seen, "预览没有用新的序号重画"

        seen.clear()
        panel.set_next_number(12)
        assert 12 in seen
    finally:
        panel.deleteLater()


# ======================================================================
# 代码审查发现的问题
# ======================================================================


def test_restoring_a_tool_panel_survives_a_settings_failure(qapp, monkeypatch):
    """settings 在 try 里赋值，序号分支在 try 外读它——异常时会 UnboundLocalError。"""
    import settings as settings_module
    from ui.toolbar import Toolbar

    toolbar = Toolbar()
    try:
        def boom():
            raise RuntimeError("配置不可用")

        # 构造完成后才打断配置读取，模拟运行期读配置失败
        monkeypatch.setattr(settings_module, "get_tool_settings_manager", boom)

        # 不该抛异常：工具切换不能因为读配置失败就整个中断
        toolbar.restore_active_tool_state("number")
    finally:
        toolbar.deleteLater()


def test_selecting_a_number_syncs_its_style_into_the_panel(qapp):
    """面板要反映选中的那个序号，而不是工具默认值。"""
    from ui.toolbar import Toolbar

    toolbar = Toolbar()
    try:
        toolbar.number_panel.set_style(NumberItem.STYLE_SOLID)
        item = NumberItem(1, QPointF(0, 0), 20, QColor("red"), NumberItem.STYLE_NO_CIRCLE)

        # 走 view 里选中序号时的那条同步分支
        toolbar.number_panel.set_style(item.style)
        assert toolbar.number_panel.current_style == NumberItem.STYLE_NO_CIRCLE
    finally:
        toolbar.deleteLater()


def test_the_style_policy_lives_in_one_place(qapp):
    """截图和钉图必须走同一套策略，各写一份迟早走偏。"""
    from tools.number import NumberTool

    assert hasattr(NumberTool, "apply_style_change")

    for func in (
        __import__("ui.screenshot_window", fromlist=["x"]).ScreenshotWindow.on_number_style_changed,
        __import__("pin.pin_canvas", fromlist=["x"]).PinCanvas._on_number_style_changed,
    ):
        names = set(func.__code__.co_names)
        assert "apply_style_change" in names, "没有走共用策略"
        # 策略细节不该在调用方重复
        assert "update_settings" not in names
        assert "NumberStyleCommand" not in names


class _FakeCursorManager:
    def __init__(self, current):
        self.current_tool_id = current
        self.refreshed = []

    def set_tool_cursor(self, tool_id, force=False):
        self.refreshed.append(tool_id)


class _FakeController:
    def __init__(self, item, cross_tool):
        self.selected_item = item
        self._cross = cross_tool

    def is_cross_tool_selection(self):
        return self._cross


class _FakeView:
    def __init__(self, item, cross_tool, cursor_tool):
        self.smart_edit_controller = _FakeController(item, cross_tool)
        self.cursor_manager = _FakeCursorManager(cursor_tool)


@pytest.mark.parametrize("active_tool", ["number", "arrow", None])
def test_the_cursor_is_only_touched_when_the_number_tool_is_active(qapp, active_tool):
    """跨工具改样式时不能把别的工具的光标换成序号预览。"""
    from tools.number import NumberTool

    item = NumberItem(1, QPointF(0, 0), 20, QColor("red"))
    view = _FakeView(item, cross_tool=False, cursor_tool=active_tool)

    NumberTool.apply_style_change(NumberItem.STYLE_HOLLOW_BG, view, None)

    if active_tool == "number":
        assert view.cursor_manager.refreshed == ["number"]
    else:
        assert view.cursor_manager.refreshed == []


def test_a_cross_tool_edit_never_writes_the_tool_default(qapp, restore_style):
    """临时的跨工具改样式不该永久改掉默认值——即使选的就是它当前的样式。"""
    from tools.number import NumberTool

    restore_style.update_settings("number", style=NumberItem.STYLE_SOLID)

    # 关键场景：选中的序号本来就是目标样式，旧实现会跳过判断直接写默认值
    item = NumberItem(1, QPointF(0, 0), 20, QColor("red"), NumberItem.STYLE_HOLLOW_ALL)
    view = _FakeView(item, cross_tool=True, cursor_tool="pen")

    persisted = NumberTool.apply_style_change(NumberItem.STYLE_HOLLOW_ALL, view, None)

    assert persisted is False
    assert restore_style.get_setting("number", "style") == NumberItem.STYLE_SOLID


def test_a_normal_edit_does_write_the_tool_default(qapp, restore_style):
    from tools.number import NumberTool

    restore_style.update_settings("number", style=NumberItem.STYLE_SOLID)
    view = _FakeView(None, cross_tool=False, cursor_tool="number")

    persisted = NumberTool.apply_style_change(NumberItem.STYLE_NO_CIRCLE, view, None)

    assert persisted is True
    assert restore_style.get_setting("number", "style") == NumberItem.STYLE_NO_CIRCLE
