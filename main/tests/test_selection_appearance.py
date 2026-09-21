# -*- coding: utf-8 -*-
"""选区外观设置：边框笔宽、手柄显示档位与手柄大小

三项设置都只影响画出来的样子，不影响交互：手柄关掉、调大调小之后，八个方向
照样能拖动调整选区。这条承诺写在设置页的说明里，所以这里钉住它。
"""
import pytest
from PySide6.QtCore import QPointF, QRectF
from PySide6.QtGui import QColor, QImage, QPainter

from canvas.items.selection_item import SelectionItem
from canvas.selection_model import SelectionModel
from core.theme import ThemeManager, get_theme

W, H = 400, 300
SEL = QRectF(120, 90, 160, 120)


@pytest.fixture
def item(qapp):
    """确认状态的选区，外观设置在每个用例后还原。"""
    theme = get_theme()
    saved = (theme.selection_border_width, theme.selection_handle_style,
             theme.selection_handle_size)

    model = SelectionModel()
    model.activate()
    model.set_rect(SEL)
    model.confirm()
    yield SelectionItem(model)

    theme.set_selection_border_width(saved[0])
    theme.set_selection_handle_style(saved[1])
    theme.set_selection_handle_size(saved[2])


def _render(item) -> QImage:
    img = QImage(W, H, QImage.Format.Format_ARGB32)
    img.fill(0xFF000000)
    painter = QPainter(img)
    item.render(painter)
    painter.end()
    return img


def _is_theme_color(img: QImage, x: float, y: float) -> bool:
    return QColor(img.pixel(int(x), int(y))).name() == get_theme().theme_color.name()


def _white_pixels_around(img: QImage, pos: QPointF, box: int = 10) -> int:
    """pos 周围方框内的白色像素数，用于按面积比较手柄外圈的粗细。"""
    cx, cy = int(pos.x()), int(pos.y())
    count = 0
    for dx in range(-box, box + 1):
        for dy in range(-box, box + 1):
            x, y = cx + dx, cy + dy
            if 0 <= x < img.width() and 0 <= y < img.height():
                if QColor(img.pixel(x, y)) == QColor(255, 255, 255):
                    count += 1
    return count


# ================================================================
# 边框笔宽
# ================================================================

@pytest.mark.parametrize("width", [1, 4, 10])
def test_border_width_follows_setting(item, width):
    """左边框的粗细等于设置值，描边居中跨在选区边界上。"""
    get_theme().set_selection_border_width(width)
    img = _render(item)

    y = SEL.top() + 20  # 避开上边与四角手柄
    left = int(SEL.left())
    run = [x for x in range(left - width, left + width)
           if _is_theme_color(img, x, y)]
    assert len(run) == width
    assert run[0] == left - width // 2


def test_border_width_is_clamped_to_supported_range(item):
    """配置文件可以手改，超出档位的值读进来要被收回区间内。"""
    theme = get_theme()
    theme.set_selection_border_width(999)
    assert theme.selection_border_width == ThemeManager.BORDER_WIDTH_OPTIONS[-1]

    theme.set_selection_border_width(0)
    assert theme.selection_border_width == ThemeManager.BORDER_WIDTH_OPTIONS[0]

    theme.set_selection_border_width("不是数字")
    assert theme.selection_border_width == ThemeManager.DEFAULT_BORDER_WIDTH


# ================================================================
# 手柄大小
# ================================================================

def test_handle_size_follows_setting(item):
    """大档位比小档位画出更多的白色像素（圆点直径和外圈描边同时变大），
    手柄是抗锯齿的圆，改用面积单调性验证，不断言精确像素游程。"""
    theme = get_theme()
    theme.set_selection_border_width(1)
    theme.set_selection_handle_style(ThemeManager.HANDLES_ALL)

    theme.set_selection_handle_size(ThemeManager.HANDLE_SIZE_SMALL)
    pos = item._get_handle_positions(SEL)[SelectionItem.HANDLE_TOP_LEFT]
    small = _white_pixels_around(_render(item), pos)

    theme.set_selection_handle_size(ThemeManager.HANDLE_SIZE_LARGE)
    pos = item._get_handle_positions(SEL)[SelectionItem.HANDLE_TOP_LEFT]
    large = _white_pixels_around(_render(item), pos)

    assert large > small


def test_handle_size_falls_back_to_default_for_unknown_value(item):
    """配置文件可以手改，不认识的档位退回默认档。"""
    theme = get_theme()
    theme.set_selection_handle_size("超大")
    assert theme.selection_handle_size == ThemeManager.DEFAULT_HANDLE_SIZE


@pytest.mark.parametrize("size", ThemeManager.HANDLE_SIZE_OPTIONS)
def test_handle_size_determines_both_diameter_and_ring_width(item, size):
    """圆点直径和外圈描边是同一档位派生出来的，不是两个独立值。"""
    theme = get_theme()
    theme.set_selection_handle_size(size)
    diameter, ring_width = theme._HANDLE_SIZE_TABLE[size]
    assert theme.selection_handle_diameter == diameter
    assert theme.selection_handle_ring_width == ring_width


# ================================================================
# 手柄档位
# ================================================================

# 手柄圆心正好骑在选区边界上，边框本身也是主题色，探针必须往外挪开：
# 距圆心 4px 已经出了最细的边框，又还在半径 7px（中档手柄的内圈半径）以内。
# 这组用例测的是手柄档位（全部/四角/不显示），和手柄大小无关，所以固定用
# 中档，不随 selection_handle_size 的默认值变化而变化。
_PROBE = 4
_OUTWARD = {
    SelectionItem.HANDLE_TOP_LEFT: (-1, -1),
    SelectionItem.HANDLE_TOP: (0, -1),
    SelectionItem.HANDLE_TOP_RIGHT: (1, -1),
    SelectionItem.HANDLE_LEFT: (-1, 0),
    SelectionItem.HANDLE_RIGHT: (1, 0),
    SelectionItem.HANDLE_BOTTOM_LEFT: (-1, 1),
    SelectionItem.HANDLE_BOTTOM: (0, 1),
    SelectionItem.HANDLE_BOTTOM_RIGHT: (1, 1),
}


def _drawn_handles(item) -> set:
    """画面上真正出现了手柄的那些位置。"""
    get_theme().set_selection_border_width(1)
    get_theme().set_selection_handle_size(ThemeManager.HANDLE_SIZE_MEDIUM)
    img = _render(item)
    return {
        handle_id
        for handle_id, pos in item._get_handle_positions(SEL).items()
        if _is_theme_color(img,
                           pos.x() + _OUTWARD[handle_id][0] * _PROBE,
                           pos.y() + _OUTWARD[handle_id][1] * _PROBE)
    }


def test_all_style_draws_eight_handles(item):
    get_theme().set_selection_handle_style(ThemeManager.HANDLES_ALL)
    assert len(_drawn_handles(item)) == 8


def test_corners_style_draws_only_the_four_corners(item):
    get_theme().set_selection_handle_style(ThemeManager.HANDLES_CORNERS)
    assert _drawn_handles(item) == set(SelectionItem.CORNER_HANDLES)


def test_none_style_draws_no_handles(item):
    get_theme().set_selection_handle_style(ThemeManager.HANDLES_NONE)
    assert _drawn_handles(item) == set()


def test_unknown_style_falls_back_to_all(item):
    get_theme().set_selection_handle_style("圆的")
    assert get_theme().selection_handle_style == ThemeManager.HANDLES_ALL


def test_rounded_preview_and_corners_style_compose(item):
    """圆角预览跳过四角，再叠上「只画四角」的档位，结果是一个都不画。"""
    get_theme().set_selection_handle_style(ThemeManager.HANDLES_CORNERS)
    img = QImage(W, H, QImage.Format.Format_ARGB32)
    img.fill(0xFF000000)
    painter = QPainter(img)
    item.draw_handles(painter, SEL, skip_corners=True)
    painter.end()

    assert all(
        not _is_theme_color(img,
                            pos.x() + _OUTWARD[handle_id][0] * _PROBE,
                            pos.y() + _OUTWARD[handle_id][1] * _PROBE)
        for handle_id, pos in item._get_handle_positions(SEL).items()
    )


# ================================================================
# 选区太小时不画手柄
# ================================================================

def _handle_positions_are_clean(item, rect: QRectF) -> bool:
    """rect 的八个手柄位置上都没有白色外圈像素。"""
    img = _render(item)
    return all(_white_pixels_around(img, pos) == 0
               for pos in item._get_handle_positions(rect).values())


def test_flat_selection_draws_border_without_handles(item):
    """选区扁到挤不下手柄时只剩边框，手柄整组不画。"""
    get_theme().set_selection_handle_style(ThemeManager.HANDLES_ALL)
    get_theme().set_selection_handle_size(ThemeManager.HANDLE_SIZE_SMALL)
    get_theme().set_selection_border_width(1)
    flat = QRectF(SEL.left(), SEL.top(), 160, 24)
    item._model.set_rect(flat)

    assert not item.handles_visible()
    assert _handle_positions_are_clean(item, flat)
    assert _is_theme_color(_render(item), flat.center().x(), flat.top())


def test_hiding_threshold_follows_handle_size(item):
    """阈值跟着手柄大小档位走：同一个选区，小档画得下、大档挤不下。"""
    get_theme().set_selection_handle_style(ThemeManager.HANDLES_ALL)
    item._model.set_rect(QRectF(SEL.left(), SEL.top(), 160, 36))

    get_theme().set_selection_handle_size(ThemeManager.HANDLE_SIZE_SMALL)
    assert item.handles_visible()
    get_theme().set_selection_handle_size(ThemeManager.HANDLE_SIZE_LARGE)
    assert not item.handles_visible()


def test_corners_style_fits_into_a_smaller_selection(item):
    """只画四角时一条边上少一个手柄，同一个选区就还画得下。"""
    get_theme().set_selection_handle_size(ThemeManager.HANDLE_SIZE_SMALL)
    item._model.set_rect(QRectF(SEL.left(), SEL.top(), 160, 20))

    get_theme().set_selection_handle_style(ThemeManager.HANDLES_ALL)
    assert not item.handles_visible()
    get_theme().set_selection_handle_style(ThemeManager.HANDLES_CORNERS)
    assert item.handles_visible()


def test_roomy_selection_keeps_its_handles(item):
    """够大的选区不受影响，八个手柄照画。"""
    get_theme().set_selection_handle_style(ThemeManager.HANDLES_ALL)
    get_theme().set_selection_handle_size(ThemeManager.HANDLE_SIZE_LARGE)
    assert item.handles_visible()
    assert len(_drawn_handles(item)) == 8


# ================================================================
# 交互不受影响
# ================================================================

def test_flat_selection_stays_draggable(item):
    """挤不下手柄也只是不画：八个方向的命中判定照旧。"""
    get_theme().set_selection_handle_size(ThemeManager.HANDLE_SIZE_SMALL)
    flat = QRectF(SEL.left(), SEL.top(), 160, 24)
    item._model.set_rect(flat)
    assert not item.handles_visible()

    for handle_id, pos in item._get_handle_positions(flat).items():
        hit = item._hit_test(QPointF(pos.x(), pos.y()))
        assert hit == handle_id, (handle_id, hit)


@pytest.mark.parametrize("style", ThemeManager.HANDLE_STYLE_OPTIONS)
def test_handles_stay_draggable_whatever_is_drawn(item, style):
    """手柄档位只管画不画：八个方向的命中判定始终在。"""
    get_theme().set_selection_handle_style(style)

    for handle_id, pos in item._get_handle_positions(SEL).items():
        hit = item._hit_test(QPointF(pos.x(), pos.y()))
        assert hit == handle_id, (style, handle_id, hit)


# ================================================================
# 设置页
# ================================================================

def test_appearance_page_offers_both_selection_settings(qapp, item):
    """外观页真的建得出来，三个下拉框的档位与主题管理器一致。"""
    from PySide6.QtWidgets import QWidget
    from ui.settings_ui.page_appearance import create_appearance_page

    theme = get_theme()
    theme.set_selection_border_width(6)
    theme.set_selection_handle_style(ThemeManager.HANDLES_CORNERS)
    theme.set_selection_handle_size(ThemeManager.HANDLE_SIZE_LARGE)

    host = QWidget()
    # 页面顶层是自己持有的 QScrollArea，没接到 host 上；接住它才不会当场被回收。
    page = None
    try:
        page = create_appearance_page(host)

        widths = [host._selection_border_combo.itemData(i)
                  for i in range(host._selection_border_combo.count())]
        assert tuple(widths) == ThemeManager.BORDER_WIDTH_OPTIONS
        assert host._selection_border_combo.currentData() == 6

        styles = [host._selection_handle_combo.itemData(i)
                  for i in range(host._selection_handle_combo.count())]
        assert tuple(styles) == ThemeManager.HANDLE_STYLE_OPTIONS
        assert host._selection_handle_combo.currentData() == ThemeManager.HANDLES_CORNERS

        sizes = [host._selection_handle_size_combo.itemData(i)
                 for i in range(host._selection_handle_size_combo.count())]
        assert tuple(sizes) == ThemeManager.HANDLE_SIZE_OPTIONS
        assert host._selection_handle_size_combo.currentData() == ThemeManager.HANDLE_SIZE_LARGE
    finally:
        if page is not None:
            page.deleteLater()
        host.close()
        host.deleteLater()
