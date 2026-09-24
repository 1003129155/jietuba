# -*- coding: utf-8 -*-

"""分组表单构造器。

负责新建和编辑分组时的基础控件布局，包括名称输入、分组类型切换
以及图标选择区入口，供 ManageDialog 复用。
"""

from PySide6.QtWidgets import QButtonGroup, QHBoxLayout

from core.ui_scale import dialog_scaled
from ui.fluent_lite import LineEdit
from .form_widgets import OptionCard, field_label

try:
    from ...services.group_service import get_group_display_icon
except ImportError:
    from services.group_service import get_group_display_icon


def build_new_group_form(dialog):
    """构建新建分组表单。"""
    _build_name_section(dialog, "")
    _build_group_type_section(dialog, is_file_group=False, is_hidden=False)
    dialog.detail_layout.addWidget(field_label(dialog.tr("Icon")))
    dialog._create_emoji_picker()


def build_edit_group_form(dialog, group):
    """构建编辑分组表单。"""
    _build_name_section(dialog, group.name)
    is_file_group = group.group_type == 1
    is_hidden = group.group_type == 2
    _build_group_type_section(dialog, is_file_group=is_file_group, is_hidden=is_hidden)
    dialog.detail_layout.addWidget(field_label(dialog.tr("Icon")))
    current_icon = get_group_display_icon(group.icon, is_file_group, is_hidden=is_hidden)
    dialog._create_emoji_picker(current_icon)


def _build_name_section(dialog, name: str):
    dialog.detail_layout.addWidget(field_label(dialog.tr("Group Name"), top_gap=False))
    dialog.group_name_input = LineEdit()
    dialog.group_name_input.setPlaceholderText(dialog.tr("Enter group name..."))
    dialog.group_name_input.setText(name)
    dialog.detail_layout.addWidget(dialog.group_name_input)


def _build_group_type_section(dialog, is_file_group: bool, is_hidden: bool = False):
    dialog.detail_layout.addWidget(field_label(dialog.tr("Group Type")))

    dialog._group_type_btn_group = QButtonGroup(dialog)
    dialog.radio_normal = OptionCard(dialog.tr("General"), dialog.tr("Paste text or files"))
    dialog.radio_file = OptionCard(dialog.tr("Quick Launch"), dialog.tr("Open files or folders"))
    dialog.radio_hidden = OptionCard(dialog.tr("Hidden"), dialog.tr("Not shown on the panel"))
    form_token = getattr(dialog, "_detail_form_token", None)
    for index, card in enumerate((dialog.radio_normal, dialog.radio_file, dialog.radio_hidden)):
        dialog._group_type_btn_group.addButton(card, index)
        card.toggled.connect(lambda checked, token=form_token: dialog._on_group_type_toggled(checked, token))

    if is_hidden:
        dialog.radio_hidden.setChecked(True)
    elif is_file_group:
        dialog.radio_file.setChecked(True)
    else:
        dialog.radio_normal.setChecked(True)

    row = QHBoxLayout()
    row.setSpacing(dialog_scaled(10))
    row.addWidget(dialog.radio_normal)
    row.addWidget(dialog.radio_file)
    row.addWidget(dialog.radio_hidden)
    dialog.detail_layout.addLayout(row)
