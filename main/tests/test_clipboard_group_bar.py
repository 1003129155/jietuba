# -*- coding: utf-8 -*-

from types import SimpleNamespace

from PySide6.QtWidgets import QHBoxLayout, QVBoxLayout, QWidget

from clipboard.ui.theme.themes import THEME_LIGHT
from clipboard.ui.widgets.group_bar import GroupBar


def test_group_picked_from_overflow_menu_gets_its_own_button(qapp):
    groups = [SimpleNamespace(id=i, name=f"G{i}", icon=None) for i in range(1, 6)]
    controller = SimpleNamespace(current_group_id=None)

    def overflow(_size, is_top=False):
        visible, hidden = groups[:3], groups[3:]
        current = next((g for g in hidden if g.id == controller.current_group_id), None)
        if current:
            hidden = [visible[-1]] + [g for g in hidden if g is not current]
            visible = visible[:-1] + [current]
        return visible, hidden

    controller.get_sidebar_overflow = overflow
    controller.switch_to_group = lambda group_id: setattr(controller, "current_group_id", group_id)

    host = QWidget()
    content = QHBoxLayout(host)
    left = QWidget()
    left_layout = QVBoxLayout(left)
    content.addWidget(left)
    bar = GroupBar(controller, None, THEME_LIGHT, "right", host)
    bar.build(content, left, left_layout)
    bar.group_switched.connect(bar.switch_to_group)
    bar.refresh_buttons()
    assert [btn.property("group_id") for btn in bar.group_buttons] == [1, 2, 3]

    bar._on_overflow_group_picked(5)

    assert [btn.property("group_id") for btn in bar.group_buttons] == [1, 2, 5]
    assert [btn.property("group_id") for btn in bar.group_buttons if btn.isChecked()] == [5]
    host.deleteLater()
