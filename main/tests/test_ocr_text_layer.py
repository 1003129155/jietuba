# -*- coding: utf-8 -*-
"""
OCR 文字层测试

ocr_text_layer.py 有 528 条语句、此前覆盖率不到 10%，但它并不是难测的窗口代码：
把点击坐标换算成"第几个文字块的第几个字"、把跨块选区拼成文本，
这些都是纯粹的坐标与索引运算。

而这一块恰恰出过真实缺陷（钉图旋转+翻转后 OCR 选区整体错位到相反的角），
说明它值得被钉死。这里覆盖：

- 字符位置分配：中日文按全角算 2 份宽度、ASCII 算 1 份，否则中英混排时
  点击位置会和实际字符对不上
- 坐标 → 字符索引：取最近的字符边界，越界时钳到首尾
- 缩放与点击容错：文字块按显示比例换算，并留出容错边距
- 选区文本提取：反向拖拽要归一化；同一行的多个文字块用空格拼，
  换行的用换行符拼
- OCR 结果预处理：错误码、空数据、脏条目的处理
"""
import pytest
from PySide6.QtCore import QRect, QPoint, QRectF
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


def _box(x, y, w, h):
    """OCR 给出的四角坐标"""
    return [[x, y], [x + w, y], [x + w, y + h], [x, y + h]]


def _item(text, x=0, y=0, w=100, h=20, score=0.99):
    from pin.ocr_text_layer import OCRTextItem
    return OCRTextItem(text, _box(x, y, w, h), score)


# ============================================================================
# 文字块的几何
# ============================================================================

class TestOCRTextItemGeometry:
    """OCRTextItem 的边界矩形与缩放"""

    def test_bounding_rect_comes_from_the_four_corners(self, qapp):
        item = _item("abc", x=10, y=20, w=100, h=30)
        assert item.norm_rect.x() == 10
        assert item.norm_rect.y() == 20
        assert item.norm_rect.width() == 100
        assert item.norm_rect.height() == 30

    def test_corner_order_does_not_matter(self, qapp):
        """OCR 引擎给出的角点顺序不保证，取包围盒必须与顺序无关"""
        from pin.ocr_text_layer import OCRTextItem
        forward = OCRTextItem("x", [[0, 0], [10, 0], [10, 5], [0, 5]], 1.0)
        shuffled = OCRTextItem("x", [[10, 5], [0, 0], [0, 5], [10, 0]], 1.0)
        assert forward.norm_rect == shuffled.norm_rect

    def test_scaled_rect_follows_the_scale_factors(self, qapp):
        item = _item("abc", x=10, y=20, w=100, h=30)
        rect = item.get_scaled_rect(2.0, 3.0, 200, 100)
        assert (rect.x(), rect.y(), rect.width(), rect.height()) == (20, 60, 200, 90)

    def test_contains_hits_inside_the_block(self, qapp):
        item = _item("abc", x=0, y=0, w=100, h=20)
        assert item.contains(QPoint(50, 10), 1.0, 1.0, 100, 20) is True

    def test_contains_allows_a_small_click_margin(self, qapp):
        """紧挨着文字块外沿的点击也应命中，否则贴边的字很难选"""
        item = _item("abc", x=10, y=10, w=100, h=20)
        assert item.contains(QPoint(7, 8), 1.0, 1.0, 200, 200) is True

    def test_contains_rejects_a_clearly_outside_point(self, qapp):
        item = _item("abc", x=0, y=0, w=100, h=20)
        assert item.contains(QPoint(500, 500), 1.0, 1.0, 600, 600) is False


# ============================================================================
# 字符位置
# ============================================================================

class TestCharPositions:
    """calculate_char_positions"""

    def test_one_boundary_per_character_plus_the_trailing_edge(self, qapp):
        item = _item("abcd")
        item.calculate_char_positions(QRect(0, 0, 80, 20))
        assert len(item.char_positions) == len("abcd") + 1

    def test_ascii_characters_are_spaced_evenly(self, qapp):
        item = _item("abcd")
        item.calculate_char_positions(QRect(0, 0, 80, 20))
        assert item.char_positions == [0, 20, 40, 60, 80]

    def test_cjk_characters_take_twice_the_width_of_ascii(self, qapp):
        """
        "中a" 的宽度配比应是 2:1。若按等宽平分，中英混排时点击位置
        会和实际字符错位——越往后错得越多。
        """
        item = _item("中a")
        item.calculate_char_positions(QRect(0, 0, 90, 20))
        # 权重 2 + 1 = 3，"中" 占前 60px，"a" 占后 30px
        assert item.char_positions == [0, 60, 90]

    def test_full_width_punctuation_counts_as_wide(self, qapp):
        item = _item("，a")
        item.calculate_char_positions(QRect(0, 0, 90, 20))
        assert item.char_positions == [0, 60, 90]

    def test_positions_start_at_the_rect_offset(self, qapp):
        item = _item("ab")
        item.calculate_char_positions(QRect(100, 0, 40, 20))
        assert item.char_positions[0] == 100
        assert item.char_positions[-1] == 140

    def test_tall_narrow_block_is_laid_out_vertically(self, qapp):
        """旋转后文字块会变成竖长条，字符要沿高度分布"""
        item = _item("abcd")
        item.calculate_char_positions(QRect(0, 0, 20, 80))
        assert item._is_vertical is True
        assert item.char_positions == [0, 20, 40, 60, 80]

    def test_empty_text_produces_no_positions(self, qapp):
        item = _item("")
        item.calculate_char_positions(QRect(0, 0, 80, 20))
        assert item.char_positions == []


class TestCharIndexAtPosition:
    """get_char_index_at_pos —— 点击落在第几个字上"""

    @pytest.fixture
    def item(self, qapp):
        it = _item("abcd")
        it.calculate_char_positions(QRect(0, 0, 80, 20))   # 每字 20px
        return it

    def test_click_left_of_the_block_selects_the_first_character(self, item):
        assert item.get_char_index_at_pos(-50, QRect(0, 0, 80, 20)) == 0

    def test_click_right_of_the_block_selects_past_the_last_character(self, item):
        assert item.get_char_index_at_pos(500, QRect(0, 0, 80, 20)) == len("abcd")

    def test_click_snaps_to_the_nearest_character_boundary(self, item):
        """靠近某个字符左半边归前一个，右半边归后一个"""
        assert item.get_char_index_at_pos(22, QRect(0, 0, 80, 20)) == 1
        assert item.get_char_index_at_pos(38, QRect(0, 0, 80, 20)) == 2

    def test_click_at_the_very_start_is_index_zero(self, item):
        assert item.get_char_index_at_pos(0, QRect(0, 0, 80, 20)) == 0

    def test_vertical_block_uses_the_y_coordinate(self, qapp):
        item = _item("abcd")
        rect = QRect(0, 0, 20, 80)
        item.calculate_char_positions(rect)
        # x 给一个无意义的值，结果应只取决于 y
        assert item.get_char_index_at_pos(999, rect, y=42) == 2

    def test_item_without_computed_positions_returns_zero(self, qapp):
        item = _item("abcd")
        assert item.get_char_index_at_pos(50, QRect(0, 0, 80, 20)) == 0


# ============================================================================
# OCR 结果预处理
# ============================================================================

class TestPrepareOcrItems:
    """prepare_ocr_items：把引擎返回的字典整理成文字块（可在子线程跑）"""

    @staticmethod
    def _prepare(result):
        from pin.ocr_text_layer import OCRTextLayer
        return OCRTextLayer.prepare_ocr_items(result)

    def test_successful_result_becomes_items(self, qapp):
        items, union = self._prepare({
            "code": 100,
            "data": [
                {"text": "hello", "box": _box(0, 0, 50, 10), "score": 0.9},
                {"text": "world", "box": _box(0, 20, 60, 10), "score": 0.8},
            ],
        })
        assert [i.text for i in items] == ["hello", "world"]
        assert union == QRectF(0, 0, 60, 30)

    def test_union_rect_covers_every_block(self, qapp):
        items, union = self._prepare({
            "code": 100,
            "data": [
                {"text": "a", "box": _box(10, 10, 20, 5), "score": 1.0},
                {"text": "b", "box": _box(100, 50, 30, 5), "score": 1.0},
            ],
        })
        assert len(items) == 2
        assert union.left() == 10
        assert union.top() == 10
        assert union.right() == 130
        assert union.bottom() == 55

    @pytest.mark.parametrize("result", [
        None,
        {},
        {"code": 101, "data": [{"text": "x", "box": _box(0, 0, 1, 1), "score": 1.0}]},
        {"code": 100, "data": []},
    ])
    def test_failed_or_empty_results_yield_nothing(self, qapp, result):
        """识别失败或没有结果时必须安静地返回空，而不是抛异常"""
        items, union = self._prepare(result)
        assert items == []
        assert union is None

    def test_entries_without_text_or_box_are_skipped(self, qapp):
        items, _union = self._prepare({
            "code": 100,
            "data": [
                {"text": "", "box": _box(0, 0, 10, 10), "score": 0.9},
                {"text": "ok", "box": [], "score": 0.9},
                {"text": "good", "box": _box(0, 0, 10, 10), "score": 0.9},
            ],
        })
        assert [i.text for i in items] == ["good"]

    def test_all_entries_invalid_yields_no_union_rect(self, qapp):
        items, union = self._prepare({
            "code": 100,
            "data": [{"text": "", "box": [], "score": 0.0}],
        })
        assert items == []
        assert union is None


# ============================================================================
# 选区文本
# ============================================================================

class TestSelectedText:
    """get_selected_text：把跨块选区还原成可读文本"""

    @pytest.fixture
    def layer(self, qapp):
        from pin.ocr_text_layer import OCRTextLayer
        lay = OCRTextLayer(original_width=200, original_height=100)
        yield lay
        lay.cleanup()

    @staticmethod
    def _fill(layer, specs):
        """specs: [(text, y), ...]，同一 y 视为同一行"""
        layer.text_items = [
            _item(text, x=x, y=y, w=len(text) * 10, h=20)
            for text, x, y in specs
        ]

    def test_no_selection_returns_empty_string(self, layer):
        self._fill(layer, [("hello", 0, 0)])
        assert layer.get_selected_text() == ""

    def test_selection_within_one_block(self, layer):
        self._fill(layer, [("hello world", 0, 0)])
        layer.selection_start = (0, 0)
        layer.selection_end = (0, 5)
        assert layer.get_selected_text() == "hello"

    def test_backwards_selection_is_normalised(self, layer):
        """从右往左拖出来的选区，取文本时要和正向拖一致"""
        self._fill(layer, [("hello world", 0, 0)])
        layer.selection_start = (0, 5)
        layer.selection_end = (0, 0)
        assert layer.get_selected_text() == "hello"

    def test_blocks_on_the_same_line_are_joined_with_a_space(self, layer):
        self._fill(layer, [("hello", 0, 0), ("world", 60, 0)])
        layer.selection_start = (0, 0)
        layer.selection_end = (1, 5)
        assert layer.get_selected_text() == "hello world"

    def test_blocks_on_different_lines_are_joined_with_a_newline(self, layer):
        self._fill(layer, [("hello", 0, 0), ("world", 0, 60)])
        layer.selection_start = (0, 0)
        layer.selection_end = (1, 5)
        assert layer.get_selected_text() == "hello\nworld"

    def test_partial_blocks_at_both_ends(self, layer):
        """跨块选择时，首块从起点取到末尾，尾块从开头取到终点"""
        self._fill(layer, [("abcdef", 0, 0), ("ghijkl", 0, 60)])
        layer.selection_start = (0, 3)
        layer.selection_end = (1, 2)
        assert layer.get_selected_text() == "def\ngh"

    def test_middle_blocks_are_taken_whole(self, layer):
        self._fill(layer, [("aa", 0, 0), ("bb", 0, 60), ("cc", 0, 120)])
        layer.selection_start = (0, 1)
        layer.selection_end = (2, 1)
        assert layer.get_selected_text() == "a\nbb\nc"

    def test_backwards_selection_across_blocks(self, layer):
        self._fill(layer, [("abc", 0, 0), ("def", 0, 60)])
        layer.selection_start = (1, 2)
        layer.selection_end = (0, 1)
        assert layer.get_selected_text() == "bc\nde"

    def test_empty_range_yields_empty_string(self, layer):
        self._fill(layer, [("hello", 0, 0)])
        layer.selection_start = (0, 2)
        layer.selection_end = (0, 2)
        assert layer.get_selected_text() == ""

    def test_selection_beyond_the_last_block_stops_cleanly(self, layer):
        """索引越界不能抛异常——选区状态可能滞后于新的识别结果"""
        self._fill(layer, [("hello", 0, 0)])
        layer.selection_start = (0, 0)
        layer.selection_end = (5, 3)
        assert layer.get_selected_text() == "hello"

    def test_clear_selection_resets_the_state(self, layer):
        self._fill(layer, [("hello", 0, 0)])
        layer.selection_start = (0, 0)
        layer.selection_end = (0, 5)

        layer.clear_selection()

        assert layer.selection_start is None
        assert layer.selection_end is None
        assert layer.is_selecting is False
        assert layer.get_selected_text() == ""


class TestLayerBasics:
    """文字层的加载与缩放"""

    @pytest.fixture
    def layer(self, qapp):
        from pin.ocr_text_layer import OCRTextLayer
        lay = OCRTextLayer(original_width=200, original_height=100)
        yield lay
        lay.cleanup()

    def test_a_fresh_layer_has_no_text(self, layer):
        assert layer.has_text() is False

    def test_loading_prepared_items_makes_text_available(self, layer):
        from pin.ocr_text_layer import OCRTextLayer
        items, union = OCRTextLayer.prepare_ocr_items({
            "code": 100,
            "data": [{"text": "hi", "box": _box(0, 0, 20, 10), "score": 1.0}],
        })
        layer.load_prepared_ocr_items(items, union, 200, 100)

        assert layer.has_text() is True
        assert layer.text_items[0].text == "hi"

    def test_scale_factors_relate_widget_size_to_the_original_image(self, layer):
        layer.resize(400, 200)
        assert layer.get_scale_factors() == (2.0, 2.0)

    def test_zero_sized_original_falls_back_to_identity_scale(self, layer):
        """原图尺寸未知时不能出现除零"""
        layer.original_width = 0
        layer.original_height = 0
        assert layer.get_scale_factors() == (1.0, 1.0)
