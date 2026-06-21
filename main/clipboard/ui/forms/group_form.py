# -*- coding: utf-8 -*-

"""分组表单构造器。

负责新建和编辑分组时的基础控件布局，包括名称输入、分组类型切换
以及图标选择区入口，供 ManageDialog 复用。
"""

from PySide6.QtWidgets import QButtonGroup, QHBoxLayout

from qfluentwidgets import BodyLabel, LineEdit, RadioButton

try:
    from ...services.group_service import get_group_display_icon
except ImportError:
    from services.group_service import get_group_display_icon


def build_new_group_form(dialog):
    """构建新建分组表单。"""
    name_label = BodyLabel(dialog.tr("Group Name"))
    dialog.detail_layout.addWidget(name_label)

    dialog.group_name_input = LineEdit()
    dialog.group_name_input.setPlaceholderText(dialog.tr("Enter group name..."))
    dialog.detail_layout.addWidget(dialog.group_name_input)

    _build_group_type_section(dialog, is_file_group=False, is_hidden=False)

    icon_label = BodyLabel(dialog.tr("Select Icon"))
    dialog.detail_layout.addWidget(icon_label)
    dialog._create_emoji_picker()


def build_edit_group_form(dialog, group):
    """构建编辑分组表单。"""
    name_label = BodyLabel(dialog.tr("Group Name"))
    dialog.detail_layout.addWidget(name_label)

    dialog.group_name_input = LineEdit()
    dialog.group_name_input.setText(group.name)
    dialog.detail_layout.addWidget(dialog.group_name_input)

    is_file_group = group.group_type == 1
    is_hidden = group.group_type == 2
    _build_group_type_section(dialog, is_file_group=is_file_group, is_hidden=is_hidden)

    icon_label = BodyLabel(dialog.tr("Select Icon"))
    dialog.detail_layout.addWidget(icon_label)
    current_icon = get_group_display_icon(group.icon, is_file_group, is_hidden=is_hidden)
    dialog._create_emoji_picker(current_icon)


def _build_group_type_section(dialog, is_file_group: bool, is_hidden: bool = False):
    type_label = BodyLabel(dialog.tr("Group Type"))
    dialog.detail_layout.addWidget(type_label)

    dialog._group_type_btn_group = QButtonGroup(dialog)
    dialog.radio_normal = RadioButton(dialog.tr("General Group"))
    dialog.radio_file = RadioButton(dialog.tr("Quick Launch Group"))
    dialog.radio_hidden = RadioButton(dialog.tr("Hidden Group"))
    dialog._group_type_btn_group.addButton(dialog.radio_normal, 0)
    dialog._group_type_btn_group.addButton(dialog.radio_file, 1)
    dialog._group_type_btn_group.addButton(dialog.radio_hidden, 2)
    form_token = getattr(dialog, "_detail_form_token", None)
    dialog.radio_normal.toggled.connect(lambda checked, token=form_token: dialog._on_group_type_toggled(checked, token))
    dialog.radio_file.toggled.connect(lambda checked, token=form_token: dialog._on_group_type_toggled(checked, token))
    dialog.radio_hidden.toggled.connect(lambda checked, token=form_token: dialog._on_group_type_toggled(checked, token))

    if is_hidden:
        dialog.radio_hidden.setChecked(True)
    elif is_file_group:
        dialog.radio_file.setChecked(True)
    else:
        dialog.radio_normal.setChecked(True)

    radio_row = QHBoxLayout()
    radio_row.addWidget(dialog.radio_normal)
    radio_row.addWidget(dialog.radio_file)
    radio_row.addWidget(dialog.radio_hidden)
    radio_row.addStretch()
    dialog.detail_layout.addLayout(radio_row)
