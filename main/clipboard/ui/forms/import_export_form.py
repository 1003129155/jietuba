# -*- coding: utf-8 -*-

"""导入导出表单构造器。

负责在管理窗口中构建 CSV 导入导出的界面控件，包含编码选择、
按钮布局和说明文本，不直接处理文件读写逻辑。
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QFrame, QHBoxLayout

from qfluentwidgets import BodyLabel, CaptionLabel, ComboBox, PrimaryPushButton


def build_import_export_form(dialog):
    """构建导入导出表单。"""
    export_section = BodyLabel(dialog.tr("Export"))
    export_section.setStyleSheet("font-size: 14px; font-weight: 600; margin-top: 8px;")
    dialog.detail_layout.addWidget(export_section)

    export_desc = CaptionLabel(dialog.tr("Export all saved content to CSV file"))
    dialog.detail_layout.addWidget(export_desc)

    encoding_layout = QHBoxLayout()
    encoding_label = CaptionLabel(dialog.tr("Encoding:"))
    encoding_layout.addWidget(encoding_label)

    dialog.export_encoding_combo = ComboBox()
    dialog.export_encoding_combo.addItem("UTF-8 (BOM)", userData="utf-8-sig")
    dialog.export_encoding_combo.addItem("UTF-8", userData="utf-8")
    dialog.export_encoding_combo.addItem("Shift_JIS", userData="shift_jis")
    dialog.export_encoding_combo.addItem("GBK", userData="gbk")
    encoding_layout.addWidget(dialog.export_encoding_combo)
    encoding_layout.addStretch()
    dialog.detail_layout.addLayout(encoding_layout)

    export_btn = PrimaryPushButton(dialog.tr("📤 Export to CSV"))
    export_btn.setCursor(Qt.CursorShape.PointingHandCursor)
    export_btn.clicked.connect(dialog._export_to_csv)
    dialog.detail_layout.addWidget(export_btn)

    line = QFrame()
    line.setFrameShape(QFrame.Shape.HLine)
    line.setStyleSheet("background: #E8E8E8; margin: 16px 0;")
    line.setFixedHeight(1)
    dialog.detail_layout.addWidget(line)

    import_section = BodyLabel(dialog.tr("Import"))
    import_section.setStyleSheet("font-size: 14px; font-weight: 600; margin-top: 8px;")
    dialog.detail_layout.addWidget(import_section)

    import_desc = CaptionLabel(dialog.tr("Import content from CSV file (Group, Content, Title)"))
    dialog.detail_layout.addWidget(import_desc)

    import_btn = PrimaryPushButton(dialog.tr("📥 Import from CSV"))
    import_btn.setCursor(Qt.CursorShape.PointingHandCursor)
    import_btn.clicked.connect(dialog._import_from_csv)
    dialog.detail_layout.addWidget(import_btn)

    format_section = BodyLabel(dialog.tr("CSV Format"))
    format_section.setStyleSheet("font-size: 14px; font-weight: 600; margin-top: 24px;")
    dialog.detail_layout.addWidget(format_section)

    format_desc = CaptionLabel(
        dialog.tr("Column 1: Group Name") + "\n" +
        dialog.tr("Column 2: Content") + "\n" +
        dialog.tr("Column 3: Title (optional)")
    )
    dialog.detail_layout.addWidget(format_desc)

    dialog.detail_layout.addStretch()
