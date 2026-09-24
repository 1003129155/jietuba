# -*- coding: utf-8 -*-

"""导入导出表单构造器。

负责在管理窗口中构建 CSV 导入导出的界面控件，包含编码选择、
按钮布局和说明文本，不直接处理文件读写逻辑。
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QFrame, QHBoxLayout, QVBoxLayout

from core.ui_scale import dialog_scaled
from ui.fluent_lite import ComboBox, FluentIcon, PushButton as FluentPushButton
from ui.settings_ui.components import IconBadge, apply_theme_text_style
from .form_widgets import field_hint, field_label


def _section_card(dialog, icon, title: str, description: str) -> tuple:
    s = dialog_scaled
    card = QFrame()
    card.setObjectName("ManageSectionCard")
    layout = QVBoxLayout(card)
    layout.setContentsMargins(s(18), s(16), s(18), s(16))
    layout.setSpacing(s(12))

    header = QHBoxLayout()
    header.setSpacing(s(12))
    header.addWidget(IconBadge(icon, "neutral", card))
    text_box = QVBoxLayout()
    text_box.setSpacing(s(2))
    title_label = field_label(title, top_gap=False)
    apply_theme_text_style(title_label, 14, bold=True)
    text_box.addWidget(title_label)
    text_box.addWidget(field_hint(description))
    header.addLayout(text_box, 1)
    layout.addLayout(header)
    return card, layout


def _action_button(dialog, text: str, handler) -> FluentPushButton:
    button = FluentPushButton(text)
    button.setCursor(Qt.CursorShape.PointingHandCursor)
    button.setMinimumWidth(dialog_scaled(140))
    button.clicked.connect(handler)
    return button


def build_import_export_form(dialog):
    """构建导入导出表单。"""
    export_card, export_layout = _section_card(
        dialog, FluentIcon.SAVE, dialog.tr("Export"), dialog.tr("Export all saved content to CSV file")
    )
    export_row = QHBoxLayout()
    export_row.setSpacing(dialog_scaled(10))
    encoding_label = field_hint(dialog.tr("Encoding:"))
    encoding_label.setWordWrap(False)
    export_row.addWidget(encoding_label)
    dialog.export_encoding_combo = ComboBox()
    dialog.export_encoding_combo.addItem("UTF-8 (BOM)", userData="utf-8-sig")
    dialog.export_encoding_combo.addItem("UTF-8", userData="utf-8")
    dialog.export_encoding_combo.addItem("Shift_JIS", userData="shift_jis")
    dialog.export_encoding_combo.addItem("GBK", userData="gbk")
    export_row.addWidget(dialog.export_encoding_combo)
    export_row.addStretch()
    export_row.addWidget(_action_button(dialog, dialog.tr("Export to CSV"), dialog._export_to_csv))
    export_layout.addLayout(export_row)
    dialog.detail_layout.addWidget(export_card)

    import_card, import_layout = _section_card(
        dialog, FluentIcon.DOWNLOAD, dialog.tr("Import"),
        dialog.tr("Import content from CSV file (Group, Content, Title)"),
    )
    format_hint = field_hint(
        dialog.tr("Column 1: Group Name") + "\n"
        + dialog.tr("Column 2: Content") + "\n"
        + dialog.tr("Column 3: Title (optional)")
    )
    import_row = QHBoxLayout()
    import_row.addWidget(format_hint, 1)
    import_row.addWidget(
        _action_button(dialog, dialog.tr("Import from CSV"), dialog._import_from_csv),
        0, Qt.AlignmentFlag.AlignBottom,
    )
    import_layout.addLayout(import_row)
    dialog.detail_layout.addWidget(import_card)

    dialog.detail_layout.addStretch()
