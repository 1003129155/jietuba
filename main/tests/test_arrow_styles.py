# -*- coding: utf-8 -*-
"""箭头的九种样式。

形状不是各写一份，而是由 ArrowItem.STYLE_SPECS 声明"箭杆 + 两端端头"拼出来，
直线和曲线共用同一条代码路径。这种拼法出问题时通常不抛异常，只是悄悄画歪——
头跑到起点外面去、杆穿出箭头尖、空心的肚子点不中。所以断言一律盯着"画出来的
东西落在哪、点不点得中"，而不是内部字段。
"""

import math
from pathlib import Path

import pytest
from PySide6.QtCore import QPointF, QRect, QSize, Qt, QTranslator
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPen, QPixmap
from PySide6.QtWidgets import QStyleOptionViewItem

from canvas.items import ArrowItem
from settings.tool_settings import ToolSettingsManager
from ui.arrow_settings_panel import (
    ARROW_STYLE_NAMES,
    PREVIEW_INK,
    PREVIEW_INK_SELECTED,
    preview_row_size,
    preview_size,
    ArrowSettingsPanel,
    render_arrow_style_preview,
)

# 样式名写死一份在测试里：它会进用户配置、也会进钉图克隆，改名等于让老存档里
# 的箭头全部退回默认，不该顺手改掉
ARROW_STYLES = (
    "single", "double", "hollow",
    "line", "line_double",
    "triangle", "triangle_double",
    "bar", "bar_arrow",
)

START = QPointF(0.0, 0.0)
END = QPointF(200.0, 0.0)


def _arrow(style, width=9.0, start=START, end=END):
    return ArrowItem(QPointF(start), QPointF(end), QPen(QColor("#FF0000"), width), style)


def _image_bytes(image):
    """拷一份像素内容。

    参数绑定让 QImage 在拷贝期间引用计数不为零——constBits() 返回的是不持有
    图像的裸指针视图，写成 bytes(x.toImage().constBits()) 读的就是已释放的内存。
    """
    return bytes(image.constBits())


def _preview_bytes(style, ink=PREVIEW_INK):
    image = render_arrow_style_preview(style, ink, preview_size()).toImage()
    return _image_bytes(image.convertToFormat(QImage.Format.Format_ARGB32))


def test_the_style_list_matches_the_item(qapp):
    """面板按 STYLES 列、几何按 STYLE_SPECS 查，两边漏一个都是少一种样式。"""
    assert tuple(ArrowItem.STYLES) == ARROW_STYLES
    assert set(ArrowItem.STYLES) == set(ArrowItem.STYLE_SPECS)


def test_the_saved_default_is_a_real_style(qapp):
    """默认值不在样式表里的话，每次新建箭头都会被悄悄退回 single。"""
    saved = ToolSettingsManager.DEFAULT_SETTINGS["arrow"]["arrow_style"]
    assert saved in ArrowItem.STYLE_SPECS


def test_unknown_styles_fall_back_instead_of_drawing_nothing(qapp):
    """老存档里的未知值必须落到默认样式上，不能留着走没有分支的状态。"""
    for style in ARROW_STYLES:
        assert ArrowItem.normalize_style(style) == style
    assert ArrowItem.normalize_style("nonsense") == ArrowItem.STYLE_SINGLE
    assert ArrowItem.normalize_style(None) == ArrowItem.STYLE_SINGLE


@pytest.mark.parametrize("style", ARROW_STYLES)
def test_every_style_draws_ink_on_both_endpoints(qapp, style):
    """用户是照着自己拖的两个点来对位的，两端都得真的有东西画在那儿。"""
    arrow = _arrow(style)
    shape = arrow.shape()

    assert not arrow.path().isEmpty()
    assert shape.contains(QPointF(START.x() + 2, START.y()))
    assert shape.contains(QPointF(END.x() - 2, END.y()))


@pytest.mark.parametrize("style", ARROW_STYLES)
def test_every_style_paints_something_distinct(qapp, style):
    """九种样式必须长得两两不同，否则选了等于没选。"""
    shots = {s: _preview_bytes(s) for s in ARROW_STYLES}
    others = [shot for s, shot in shots.items() if s != style]
    assert all(shots[style] != other for other in others)


@pytest.mark.parametrize("style", ARROW_STYLES)
def test_a_short_arrow_is_not_swallowed_by_its_own_head(qapp, style):
    """线宽 20 画一支 30px 的短箭头：头得跟着缩，不能反过来把箭头顶出去。

    头长是按线宽算的，不收着的话颈部会落到起点后面，画出来是一个巨大的头拖着
    一截倒刺，包围盒也会比用户拖出来的那段还长。
    """
    arrow = _arrow(style, width=20.0, end=QPointF(30.0, 0.0))
    rect = arrow.path().boundingRect()

    # 容差给到半个线宽：标注线的横杠本来就骑在端点上，空心箭头的描边也会在尖角
    # 处鼓出一点。超过这个量就不是"笔有厚度"，是头真的顶到端点外面去了。
    slack = 20.0 * 0.9 / 2 + 1
    assert rect.left() >= -slack, "头顶到起点外面去了"
    assert rect.right() <= 30.0 + slack, "杆穿出箭头尖了"
    assert rect.height() > 0


@pytest.mark.parametrize("style", ARROW_STYLES)
def test_dragging_the_control_point_bends_every_style(qapp, style):
    """弯曲不是某几种样式的特权：九种都得跟着控制点走。"""
    arrow = _arrow(style)
    straight = arrow.path().boundingRect()

    arrow.set_control_point(QPointF(100.0, -80.0))
    curved = arrow.path().boundingRect()

    assert arrow.is_curved() is True
    assert curved.top() < straight.top() - 40, "拖了控制点却没弯"
    assert curved.contains(QPointF(100.0, -78.0)), "曲线没经过用户拖的那个点"


@pytest.mark.parametrize("style", ARROW_STYLES)
def test_the_head_lands_on_the_endpoint_whatever_the_direction(qapp, style):
    """端头必须压在端点上：方向算反的话头会长到杆的另一头去。"""
    for angle in (0, 90, 180, 270, 37):
        radians = math.radians(angle)
        end = QPointF(200 * math.cos(radians), 200 * math.sin(radians))
        arrow = _arrow(style, end=end)
        assert arrow.path().boundingRect().adjusted(-1, -1, 1, 1).contains(end)


def test_the_outlined_arrow_is_hollow_but_still_clickable(qapp):
    """空心箭头画出来只有一圈描边，可点的却该是整块剪影。

    照着 path() 去点的话，用户点自己画的箭头肚子会点空——那一圈描边才几个像素宽。
    """
    arrow = _arrow(ArrowItem.STYLE_HOLLOW)
    belly = QPointF(150.0, 0.0)

    assert not arrow.path().contains(belly), "这样就不是空心的了"
    assert arrow.shape().contains(belly)


def test_the_solid_arrow_tapers_from_tail_to_head(qapp):
    """默认那支箭头的样子：尾巴收尖、往颈部渐宽、头明显比杆宽。"""
    arrow = _arrow(ArrowItem.STYLE_SINGLE)
    path = arrow.path()

    def ink_height(x):
        hits = [y for y in range(-60, 61) if path.contains(QPointF(float(x), float(y)))]
        return (max(hits) - min(hits)) if hits else 0

    tail, middle, neck, head = ink_height(4), ink_height(100), ink_height(160), ink_height(175)
    assert tail < middle < neck, f"箭杆没有从尾到颈渐宽: {tail} {middle} {neck}"
    assert head > neck * 1.5, f"头不够显眼: 头 {head} vs 颈 {neck}"


def _ink_height(path, x):
    """x 这一列上墨迹的高度（像素）"""
    hits = [y for y in range(-120, 121) if path.contains(QPointF(float(x), float(y)))]
    return (max(hits) - min(hits) + 1) if hits else 0


@pytest.mark.parametrize("style", (ArrowItem.STYLE_SINGLE, ArrowItem.STYLE_HOLLOW))
def test_the_tapered_tail_comes_to_a_point(qapp, style):
    """流线型箭头的尾巴要收成尖。

    尾巴留着宽度的话，粗线宽下那一截就是一个平口，整支看着像被剪掉了一段——
    尤其空心箭头，平口连描边一起有小二十像素宽。
    """
    path = _arrow(style, width=20.0).path()

    assert _ink_height(path, 2) <= 2, "尾巴是个平口，不是尖"
    assert _ink_height(path, 2) < _ink_height(path, 60) < _ink_height(path, 150),         "尾巴到颈部没有渐宽"


@pytest.mark.parametrize("style", (ArrowItem.STYLE_LINE, ArrowItem.STYLE_LINE_DOUBLE))
def test_the_open_head_keeps_its_point(qapp, style):
    """开口箭头的头尖必须是尖的。

    杆是根长方形，一路怼到端点的话，平口会从尖的两侧支出来——线越粗越明显，
    最后头和杆一样是个长方形。
    """
    path = _arrow(style, width=20.0).path()
    line_w = 20.0 * 0.9

    tip, mid, back = (_ink_height(path, END.x() - d) for d in (3, 15, 40))
    assert tip <= line_w / 2, f"头尖被杆顶成了平口: {tip} 像素宽"
    assert tip < mid < back, f"头没有越往回越宽: {tip} {mid} {back}"


def test_the_outlined_arrow_takes_up_the_same_room_as_the_solid_one(qapp):
    """空心和实心是同一个剪影，占的地方就得分毫不差。

    描边骑在轮廓线上画的话，收成尖的尾巴会被斜接拉出一根长刺，空心箭头比用户
    拖出来的那段长出一大截。
    """
    for width in (1.0, 3.0, 9.0, 20.0):
        solid = _arrow(ArrowItem.STYLE_SINGLE, width=width).path().boundingRect()
        outlined = _arrow(ArrowItem.STYLE_HOLLOW, width=width).path().boundingRect()
        assert (outlined.left(), outlined.top(), outlined.right(), outlined.bottom()) ==             pytest.approx((solid.left(), solid.top(), solid.right(), solid.bottom()), abs=0.5),             f"线宽 {width} 时空心箭头比实心的多占了地方"


def test_the_panel_offers_every_style(qapp):
    panel = ArrowSettingsPanel()
    try:
        combo = panel.arrow_style_combo
        assert combo.count() == len(ArrowItem.STYLES)
        assert [combo.itemData(i) for i in range(combo.count())] == list(ArrowItem.STYLES)
        # 全部一次列完：样式靠形状认，得滚动着比对等于认不出
        assert combo.maxVisibleItems() >= combo.count()
    finally:
        panel.deleteLater()


def test_every_row_in_the_panel_has_its_own_picture(qapp):
    """下拉里没有文字，一行的图是空的或者和别人重了，那一行就没法选。"""
    panel = ArrowSettingsPanel()
    try:
        combo = panel.arrow_style_combo
        shots = []
        for index in range(combo.count()):
            image = combo.itemIcon(index).pixmap(preview_size()).toImage()
            assert not image.isNull()
            inked = any(image.pixelColor(x, y).alpha()
                        for x in range(image.width())
                        for y in range(image.height()))
            assert inked, f"第 {index} 行是张空图"
            shots.append(_image_bytes(image.convertToFormat(QImage.Format.Format_ARGB32)))
        assert len(set(shots)) == combo.count()
    finally:
        panel.deleteLater()


def test_the_selected_row_gets_its_own_legible_icon(qapp):
    """下拉选中行是蓝底：深墨色图标糊在上面，那一行等于没有图。"""
    for style in ArrowItem.STYLES:
        assert _preview_bytes(style) != _preview_bytes(style, PREVIEW_INK_SELECTED)

    panel = ArrowSettingsPanel()
    try:
        icon = panel.arrow_style_combo.itemIcon(0)
        normal = icon.pixmap(preview_size(), QIcon.Mode.Normal).toImage()
        selected = icon.pixmap(preview_size(), QIcon.Mode.Selected).toImage()
        assert not normal.isNull() and not selected.isNull()
        assert _image_bytes(normal) != _image_bytes(selected)
    finally:
        panel.deleteLater()


def test_every_style_says_its_name_in_every_language(qapp):
    """图标只画形状，"这是哪一种"全靠 tooltip——少一句就是一行没名字的图。"""
    assert set(ARROW_STYLE_NAMES) == set(ArrowItem.STYLES)

    translations = Path(__file__).parents[1] / "translations"
    for language in ("en", "zh", "ja", "ko"):
        source_file = (translations / f"app_{language}.xml").read_text(encoding="utf-8")
        translator = QTranslator()
        assert translator.load(str(translations / f"app_{language}.qm"))

        rendered = []
        for name in ARROW_STYLE_NAMES.values():
            assert f"<source>{name}</source>" in source_file, f"{language} 缺翻译源: {name}"
            text = translator.translate("ArrowSettingsPanel", name)
            assert text, f"{language} 的 .qm 里没编进去: {name}（改完 xml 要跑 compile_translations.py）"
            rendered.append(text)
        assert len(set(rendered)) == len(rendered), f"{language} 有两种样式叫同一个名字"


def test_the_panel_keeps_junk_out_of_the_style(qapp):
    panel = ArrowSettingsPanel()
    try:
        panel.arrow_style = ArrowItem.STYLE_BAR_ARROW
        assert panel.arrow_style == ArrowItem.STYLE_BAR_ARROW

        panel.arrow_style = "nonsense"
        assert panel.arrow_style == ArrowItem.STYLE_BAR_ARROW
    finally:
        panel.deleteLater()


def test_the_dropdown_puts_the_preview_in_the_middle(qapp):
    """下拉一行里只有图标没有文字。

    Qt 会把图标顶到行首、剩下的宽度全留给那串空文字，于是九张预览齐刷刷贴着
    左边、右边空出一大块；行宽不够时还要先缩一道，箭头尖就顶在行的边线上。
    量的是一块实心色：摆在哪儿不该跟着某种样式自己的形状变。
    """
    panel = ArrowSettingsPanel()
    try:
        combo = panel.arrow_style_combo
        block = QPixmap(preview_size())
        block.fill(QColor("#000000"))
        combo.setItemIcon(0, QIcon(block))

        # 故意画进一行比预览宽出一截的行里：行到底多宽不由我们说了算（列表撑
        # 开、缩放变了都会变），宽出多少都得把预览摆回正中
        row = QRect(0, 0, preview_row_size().width() + 40, preview_row_size().height())
        image = QImage(row.size(), QImage.Format.Format_ARGB32)
        image.fill(Qt.GlobalColor.transparent)
        option = QStyleOptionViewItem()
        option.rect = row
        painter = QPainter(image)
        try:
            combo.itemDelegate().paint(painter, option, combo.model().index(0, 0))
        finally:
            painter.end()

        inked = [x for x in range(image.width())
                 if any(image.pixelColor(x, y) == QColor("#000000")
                        for y in range(image.height()))]
        assert inked, "这一行没画出图标"

        left, right = inked[0], image.width() - 1 - inked[-1]
        assert abs(left - right) <= 1, f"预览没摆在行的正中：左 {left}px 右 {right}px"
        assert left >= 4, "预览贴在行的边线上"
        assert combo.view().minimumWidth() >= preview_row_size().width(),             "弹出列表没按行宽撑开，预览会被挤回去"
    finally:
        panel.deleteLater()


def test_the_preview_fits_the_row(qapp):
    """预览图顶到边的话，下拉里每种样式都会被切掉上下两刀。"""
    for style in ArrowItem.STYLES:
        image = render_arrow_style_preview(style, PREVIEW_INK, preview_size()).toImage()
        assert image.size() == QSize(preview_size().width(), preview_size().height())

        for x in range(image.width()):
            assert image.pixelColor(x, 0).alpha() == 0, f"{style} 顶到上边了"
            assert image.pixelColor(x, image.height() - 1).alpha() == 0, f"{style} 顶到下边了"
