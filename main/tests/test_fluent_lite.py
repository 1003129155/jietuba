"""Regression tests for the project-owned UI compatibility layer."""

import ast
from pathlib import Path

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QStyle, QStyleOptionButton

from ui.fluent_lite import (
    FluentIcon,
    NavigationInterface,
    NavigationItemPosition,
    LineEdit,
    RadioButton,
    SegmentedWidget,
    SettingCard,
    SettingCardGroup,
    SpinBox,
    SwitchSettingCard,
    TextEdit,
)


def test_all_declared_icons_exist():
    missing = [icon.path() for icon in FluentIcon if not Path(icon.path()).is_file()]
    assert missing == []


def test_setting_cards_keep_the_expected_api(qapp):
    card = SettingCard(FluentIcon.INFO, "Title", "Description")
    assert card.titleLabel.text() == "Title"
    assert card.contentLabel.text() == "Description"
    assert card.hBoxLayout is not None

    switch = SwitchSettingCard(FluentIcon.SETTING, "Enabled")
    states = []
    switch.checkedChanged.connect(states.append)
    switch.setChecked(True)
    assert switch.isChecked() is True
    assert states == [True]


def test_setting_card_group_height_tracks_available_text_width(qapp):
    group = SettingCardGroup("Group")
    card = SettingCard(
        FluentIcon.INFO,
        "Title",
        "A translated description that is deliberately long enough to wrap " * 4,
    )
    group.addSettingCard(card)

    narrow_height = group.cardLayout.heightForWidth(260)
    wide_height = group.cardLayout.heightForWidth(600)

    assert wide_height < narrow_height
    assert wide_height >= card.minimumHeight()


def test_navigation_selects_routes_without_firing_actions(qapp):
    nav = NavigationInterface()
    calls = []
    nav.addItem("top", FluentIcon.CAMERA, "Top", lambda: calls.append("top"))
    nav.addItem(
        "bottom", FluentIcon.INFO, "Bottom", lambda: calls.append("bottom"),
        position=NavigationItemPosition.BOTTOM,
    )
    nav.setCurrentItem("bottom")
    assert nav.widget("bottom").isChecked()
    assert nav.widget("top").toolTip() == ""
    assert calls == []
    nav.clearCurrentItem()
    assert not nav.widget("bottom").isChecked()


def test_segmented_widget_tracks_current_route(qapp):
    segmented = SegmentedWidget()
    segmented.addItem("one", "One")
    segmented.addItem("two", "Two")
    segmented.setCurrentItem("two")
    assert segmented.currentItem() == "two"


def test_radio_button_label_does_not_overlap_indicator(qapp):
    radio = RadioButton("常规组")
    radio.resize(radio.sizeHint())
    option = QStyleOptionButton()
    radio.initStyleOption(option)
    indicator = radio.style().subElementRect(
        QStyle.SubElement.SE_RadioButtonIndicator, option, radio
    )

    label_rect = radio._label_rect(indicator)
    assert label_rect.left() > indicator.right()

    _center, outer = radio._indicator_ellipse(indicator)
    # Half of the one-pixel pen extends beyond the ellipse path.
    assert outer.left() - 0.5 >= indicator.left()
    assert outer.top() - 0.5 >= indicator.top()
    assert outer.right() + 0.5 <= indicator.right() + 1
    assert outer.bottom() + 0.5 <= indicator.bottom() + 1


def test_shared_text_inputs_install_themed_context_menus(qapp):
    line_edit = LineEdit()
    text_edit = TextEdit()
    spin_box = SpinBox()

    for editor in (line_edit, text_edit):
        menu = editor.createThemedContextMenu()
        assert menu.autoFillBackground() is True
        assert "QMenu::item:disabled" in menu.styleSheet()
        menu.deleteLater()

    assert (
        spin_box.lineEdit().contextMenuPolicy()
        == Qt.ContextMenuPolicy.CustomContextMenu
    )


def test_line_edit_can_keep_a_caller_owned_body_style(qapp):
    line_edit = LineEdit(use_default_style=False)
    custom_style = "QLineEdit { background: #123456; }"
    line_edit.setStyleSheet(custom_style)

    assert line_edit.styleSheet() == custom_style
    menu = line_edit.createThemedContextMenu()
    assert "background-color:" in menu.styleSheet()
    menu.deleteLater()


def test_frontend_uses_shared_text_editor_controls():
    """Prevent new raw Qt editors from bypassing the shared context menu."""
    source_root = Path(__file__).parents[1]
    raw_editor_names = {"QLineEdit", "QTextEdit", "QPlainTextEdit"}
    offenders = []

    for path in source_root.rglob("*.py"):
        if path.parts[-2] == "tests":
            continue
        tree = ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
        for node in ast.walk(tree):
            if (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Name)
                and node.func.id in raw_editor_names
            ):
                offenders.append(f"{path.relative_to(source_root)}:{node.lineno}")

    assert offenders == []
