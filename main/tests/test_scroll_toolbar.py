"""长截图工具栏：长图尺寸、方向按钮、裁剪菜单。"""

import pytest

from stitch.scroll_toolbar import FloatingToolbar


@pytest.fixture
def toolbar(qapp):
    toolbar = FloatingToolbar()
    yield toolbar
    toolbar.deleteLater()


def test_size_changes_do_not_resize_the_toolbar(toolbar):
    toolbar.set_result_size(800, 600)
    width = toolbar.sizeHint().width()
    toolbar.set_result_size(800, 25392)
    assert toolbar.size_label.text() == "800 × 25392"
    assert toolbar.sizeHint().width() == width


def test_direction_button_follows_the_direction(toolbar):
    assert toolbar.direction_btn.text() == toolbar.tr("Vertical")
    toolbar.update_direction("horizontal")
    assert toolbar.direction_btn.text() == toolbar.tr("Horizontal")


def test_crop_menu_names_the_sides_by_direction(toolbar):
    sides = []
    toolbar.crop_requested.connect(sides.append)
    actions = toolbar._crop_menu().actions()
    assert [a.text() for a in actions] == [
        toolbar.tr("Crop top: remove everything above the current view"),
        toolbar.tr("Crop bottom: remove everything below the current view"),
    ]
    for action in actions:
        action.trigger()
    assert sides == ["top", "bottom"]

    toolbar.update_direction("horizontal")
    actions = toolbar._crop_menu().actions()
    assert [a.text() for a in actions] == [
        toolbar.tr("Crop left: remove everything left of the current view"),
        toolbar.tr("Crop right: remove everything right of the current view"),
    ]
    actions[1].trigger()
    assert sides == ["top", "bottom", "bottom"]


def test_auto_scroll_button_shows_whether_it_is_running(toolbar):
    toolbar.set_auto_scrolling(True)
    assert toolbar.auto_scroll_btn.isChecked()
    toolbar.set_auto_scrolling(False)
    assert not toolbar.auto_scroll_btn.isChecked()
