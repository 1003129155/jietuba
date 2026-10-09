# -*- coding: utf-8 -*-
"""识别结果的排版：折行合并、去空格、标点全半角。纯文本，不开窗口。"""

from text_recognition.layout import (
    MODE_AUTO, MODE_MULTILINE, MODE_SINGLE_LINE,
    PUNCT_FULLWIDTH, PUNCT_HALFWIDTH, render,
)

HEIGHT = 20
LEADING = 8      # 行与行之间的空隙，小于 GAP_LIMIT * HEIGHT


def _line(text, row, left=0, right=400):
    top = row * (HEIGHT + LEADING)
    return {"text": text, "left": left, "right": right, "top": top, "bottom": top + HEIGHT}


def _paragraph(*texts, left=0, right=400):
    return [_line(text, row, left, right) for row, text in enumerate(texts)]


class TestModes:

    def test_multiline_keeps_every_line_break(self):
        lines = _paragraph("第一行", "第二行")
        assert render(lines, MODE_MULTILINE) == "第一行\n第二行"

    def test_single_line_joins_even_across_paragraphs(self):
        lines = _paragraph("第一段。", "第二段。")
        assert render(lines, MODE_SINGLE_LINE) == "第一段。第二段。"

    def test_latin_lines_are_joined_with_a_space(self):
        lines = _paragraph("hello", "world")
        assert render(lines, MODE_SINGLE_LINE) == "hello world"

    def test_no_lines_gives_empty_text(self):
        assert render([], MODE_AUTO) == ""

    def test_one_line_is_returned_as_is(self):
        assert render(_paragraph("只有一行"), MODE_AUTO) == "只有一行"


class TestAuto:

    def test_wrapped_lines_are_merged(self):
        lines = _paragraph("这一行排到了右边界", "接着往下排")
        assert render(lines, MODE_AUTO) == "这一行排到了右边界接着往下排"

    def test_a_line_ending_in_a_full_stop_closes_the_paragraph(self):
        lines = _paragraph("上一段到此结束。", "下一段从这里开始")
        assert render(lines, MODE_AUTO) == "上一段到此结束。\n下一段从这里开始"

    def test_a_short_line_closes_the_paragraph(self):
        lines = [_line("这一行没排到右边", 0, right=200), _line("下一行", 1)]
        assert render(lines, MODE_AUTO) == "这一行没排到右边\n下一行"

    def test_an_indented_line_starts_a_new_paragraph(self):
        lines = [_line("上一行满到右边", 0), _line("缩进开头的一行", 1, left=60)]
        assert render(lines, MODE_AUTO) == "上一行满到右边\n缩进开头的一行"

    def test_a_wide_gap_closes_the_paragraph(self):
        lines = [_line("上一行满到右边", 0), _line("隔了一大段距离", 3)]
        assert render(lines, MODE_AUTO) == "上一行满到右边\n隔了一大段距离"

    def test_a_hyphen_at_the_line_break_is_dropped(self):
        lines = _paragraph("recog-", "nition")
        assert render(lines, MODE_AUTO) == "recognition"

    def test_line_height_comes_from_the_median_not_the_outliers(self):
        # 第一行是大号标题，用均值算行高会把正文的行距误判成段落分隔
        lines = [
            {"text": "标题", "left": 0, "right": 400, "top": 0, "bottom": 80},
            _line("正文第一行", 4),
            _line("正文第二行", 5),
        ]
        assert render(lines, MODE_AUTO) == "标题\n正文第一行正文第二行"


class TestRemoveSpaces:

    def test_spaces_inside_a_line_are_removed_and_breaks_kept(self):
        lines = _paragraph("识 别 出 的 文 字。", "下 一 行")
        assert render(lines, MODE_AUTO, remove_spaces=True) == "识别出的文字。\n下一行"

    def test_full_width_spaces_are_removed_too(self):
        lines = _paragraph("全角　空格")
        assert render(lines, MODE_AUTO, remove_spaces=True) == "全角空格"

    def test_spaces_between_latin_words_are_kept(self):
        """识别在汉字间塞的空格是噪声，西文的词间空格是内容"""
        lines = _paragraph("Remove the noise, not the words.")
        assert render(lines, MODE_AUTO, remove_spaces=True) == "Remove the noise, not the words."

    def test_a_latin_word_inside_chinese_keeps_its_own_spaces(self):
        lines = _paragraph("打开 Visual Studio Code 看看")
        assert render(lines, MODE_AUTO, remove_spaces=True) == "打开Visual Studio Code看看"


class TestPunctuation:

    def test_chinese_punctuation_becomes_ascii(self):
        lines = _paragraph("你好，世界（真的）！")
        assert render(lines, MODE_AUTO, punctuation=PUNCT_HALFWIDTH) == "你好,世界(真的)!"

    def test_ascii_punctuation_between_chinese_becomes_full_width(self):
        lines = _paragraph("你好,世界!")
        assert render(lines, MODE_AUTO, punctuation=PUNCT_FULLWIDTH) == "你好，世界！"

    def test_a_decimal_point_is_left_alone(self):
        lines = _paragraph("圆周率是 3.14 左右")
        assert render(lines, MODE_AUTO, punctuation=PUNCT_FULLWIDTH) == "圆周率是 3.14 左右"

    def test_punctuation_in_latin_text_is_left_alone(self):
        lines = _paragraph("Hello, world!")
        assert render(lines, MODE_AUTO, punctuation=PUNCT_FULLWIDTH) == "Hello, world!"


def test_the_settings_accept_exactly_the_modes_the_layout_knows():
    """设置里的取值表和这里的常量是两份，对不上就会有选项永远存不进去"""
    from settings.tool_settings import OCR_LAYOUT_MODES, OCR_PUNCTUATION_MODES
    from text_recognition.layout import LAYOUT_MODES, PUNCTUATION_MODES

    assert set(OCR_LAYOUT_MODES) == set(LAYOUT_MODES)
    assert set(OCR_PUNCTUATION_MODES) == set(PUNCTUATION_MODES)
