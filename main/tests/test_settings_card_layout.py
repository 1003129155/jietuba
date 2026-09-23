# -*- coding: utf-8 -*-
"""设置卡片的控件列布局

每张设置卡片右侧都有一列控件。以前各个页面自己给控件定宽、各自往卡片的
横向布局里右对齐，于是同一组里开关、下拉框、按钮的左边缘互相错开，开关
还因为少一段右边距比别的控件更靠外。这里锁住统一之后的规则：列宽按组取
组内最宽的一套控件、右边距一致、撑满的控件从列左端排起、开关贴列右端。
"""
import pytest

from core.ui_scale import dialog_scaled
from ui.fluent_lite import SettingCard, SettingCardGroup
from ui.settings_ui.dialog import SettingsDialog


@pytest.fixture
def dialog(qapp):
    instance = SettingsDialog()
    instance.show()
    qapp.processEvents()
    yield instance
    instance.hide()
    instance.deleteLater()


def _pages(dialog, qapp):
    """逐页切换并交出页面。"""
    for index in range(dialog.content_stack.count()):
        dialog.content_stack.setCurrentIndex(index)
        qapp.processEvents()
        yield dialog.content_stack.widget(index)


def _visible_cards(root):
    """交出还显示着的卡片。

    翻译页会按当前引擎隐藏另外几家的凭据行，隐藏的卡片没有有效几何。
    """
    return [card for card in root.findChildren(SettingCard) if card.isVisibleTo(root)]


def _cards_of_every_page(dialog, qapp):
    for page in _pages(dialog, qapp):
        yield from _visible_cards(page)


def test_one_group_shares_one_control_column(dialog, qapp):
    """列宽按组算：组内对齐就够，不必跟别的组一样宽。"""
    ceiling = dialog_scaled(SettingCard.CONTROL_COLUMN_MAX)
    for page in _pages(dialog, qapp):
        for group in page.findChildren(SettingCardGroup):
            cards = _visible_cards(group)
            if not cards:
                continue
            expected = min(max(card.controlColumnHint() for card in cards), ceiling)
            assert {card.controlContainer.width() for card in cards} == {expected}


def test_one_page_ends_every_control_column_at_the_same_line(dialog, qapp):
    """开关以前少一段右边距，比同一页的下拉框和按钮更靠外十几个像素。

    只看装得下的卡片：窗口窄到卡片放不下自己的内容时，布局只能把右边的
    控件挤出边界，那时谈不上对齐。卡片宽度受布局取整影响可能差 1 像素，
    容差只留这么多。
    """
    for page in _pages(dialog, qapp):
        right_gaps = [
            card.width() - card.controlContainer.geometry().right()
            for card in _visible_cards(page)
            if card.hBoxLayout.minimumSize().width() <= card.width()
        ]
        if not right_gaps:
            continue
        assert max(right_gaps) - min(right_gaps) <= 1


def test_stretched_controls_start_at_the_left_edge_of_the_column(dialog, qapp):
    """下拉框、数字框、按钮铺满整列，左右边缘都落在列的两端。"""
    for card in _cards_of_every_page(dialog, qapp):
        first = card.controlLayout.itemAt(0)
        if first is None or first.widget() is None:
            continue
        if card.controlLayout.stretch(0) != 1:
            continue
        assert first.widget().x() == 0


def test_switches_line_up_on_the_right_of_the_column(dialog, qapp):
    """开关个个一样宽，贴着控件列的右端就彼此对齐，不必铺满整列。"""
    for card in _cards_of_every_page(dialog, qapp):
        switch = getattr(card, "switchButton", None)
        if switch is None:
            continue
        assert switch.geometry().right() == card.controlContainer.rect().right()
