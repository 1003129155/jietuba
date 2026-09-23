"""
截图工具栏排布对话框：拖左侧手柄调整顺序，下拉框选择每个按钮的显示方式

对话框只负责编辑；读写配置、重排工具栏由调用方 Toolbar._open_layout_dialog 完成。
拖动换位那套在 ui.reorderable_rows 里，和放大镜的颜色格式对话框共用。
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QApplication, QDialog, QHBoxLayout, QLabel, QVBoxLayout,
)

from core.i18n import make_tr
from core.ui_scale import configure_dialog_control, dialog_scaled, scale_dialog_font
from ui.fluent_lite import (
    BodyLabel, CaptionLabel, ComboBox, PrimaryPushButton, PushButton,
    TransparentPushButton, ui_tokens,
)
from ui.reorderable_rows import DraggableRow, ReorderableRowList
from ui.toolbar_layout import (
    DEFAULT_ORDER, HIDE, LOCKED, MORE, SHOW, default_layout, normalize_layout,
)

_tr = make_tr("ToolbarLayoutDialog")

# 行标题。工具栏按钮的 tooltip 带着快捷键说明，放进列表太长，这里另起短名
BUTTON_NAMES = {
    "long_screenshot": "Long screenshot",
    "save": "Save",
    "screenshot_translate": "Screenshot translate",
    "text_recognize": "Recognize text",
    "scan_code": "Scan code",
    "gif": "GIF recording",
    "pen": "Pen",
    "highlighter": "Highlighter",
    "mosaic": "Mosaic",
    "spotlight": "Spotlight",
    "arrow": "Arrow",
    "number": "Number",
    "rect": "Rectangle",
    "ellipse": "Ellipse",
    "text": "Text",
    "eraser": "Eraser",
    "undo": "Undo",
    "redo": "Redo",
    "cancel": "Cancel screenshot",
    "pin": "Pin image",
    "confirm": "Confirm",
}

MODE_NAMES = (
    (SHOW, "Always show"),
    (MORE, "In more menu"),
    (HIDE, "Always hide"),
)


class _Row(DraggableRow):
    """一行：拖动手柄、按钮图标、名称、显示方式下拉框"""

    def __init__(self, key, icon, parent=None):
        super().__init__(parent)
        self.key = key

        # 图标垫一块白底：工具栏图标是照着白底工具栏画的深色图标，深色主题下直接放看不清
        icon_label = QLabel(self)
        icon_label.setFixedSize(dialog_scaled(28), dialog_scaled(28))
        icon_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        icon_label.setPixmap(icon.pixmap(dialog_scaled(20), dialog_scaled(20)))
        icon_label.setStyleSheet(
            f"background: #FFFFFF; border-radius: {dialog_scaled(6)}px;"
        )

        self.combo = ComboBox(self)
        configure_dialog_control(self.combo)
        for mode, text in MODE_NAMES:
            self.combo.addItem(_tr(text), mode)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(
            dialog_scaled(8), dialog_scaled(3),
            dialog_scaled(8), dialog_scaled(3),
        )
        layout.setSpacing(dialog_scaled(10))
        layout.addWidget(self.grip)
        layout.addWidget(icon_label)
        name_label = BodyLabel(_tr(BUTTON_NAMES[key]), self)
        configure_dialog_control(name_label)
        layout.addWidget(name_label, 1)
        layout.addWidget(self.combo)

    def mode(self):
        return self.combo.currentData()

    def set_mode(self, mode):
        self.combo.setCurrentIndex(self.combo.findData(mode))


class ToolbarLayoutDialog(QDialog):
    """编辑截图工具栏排布。layout 为 [(按钮, 显示方式)]，icons 为 按钮 → QIcon。"""

    def __init__(self, layout, icons, parent=None):
        super().__init__(parent)
        scale_dialog_font(self)
        self.setWindowTitle(_tr("Customize Toolbar"))
        # 从全屏置顶的截图窗口里打开，和截图里的取色对话框一样置顶，免得被截图窗口盖住
        self.setWindowFlags(
            Qt.WindowType.Dialog
            | Qt.WindowType.WindowTitleHint
            | Qt.WindowType.WindowCloseButtonHint
            | Qt.WindowType.WindowStaysOnTopHint
        )
        self.setStyleSheet(f"QDialog {{ background: {ui_tokens(self).window}; }}")

        self._list = ReorderableRowList(self)

        # 固定按钮不显示灰色禁用行；这里只为真正可调整的按钮创建界面。
        self._rows = {key: _Row(key, icons[key]) for key in DEFAULT_ORDER if key not in LOCKED}

        hint = CaptionLabel(
            _tr("Drag the handle to reorder. Use the drop-down to change visibility."), self)
        configure_dialog_control(hint)

        reset_btn = TransparentPushButton(_tr("Restore defaults"), self)
        configure_dialog_control(reset_btn)
        reset_btn.clicked.connect(lambda: self._fill(default_layout()))
        ok_btn = PrimaryPushButton(_tr("OK"), self)
        configure_dialog_control(ok_btn)
        ok_btn.clicked.connect(self.accept)
        cancel_btn = PushButton(_tr("Cancel"), self)
        configure_dialog_control(cancel_btn)
        cancel_btn.clicked.connect(self.reject)

        buttons = QHBoxLayout()
        buttons.addWidget(reset_btn)
        buttons.addStretch(1)
        buttons.addWidget(ok_btn)
        buttons.addWidget(cancel_btn)

        root = QVBoxLayout(self)
        root.setContentsMargins(
            dialog_scaled(16), dialog_scaled(16),
            dialog_scaled(16), dialog_scaled(14),
        )
        root.setSpacing(dialog_scaled(10))
        root.addWidget(self._list, 1)
        root.addWidget(hint)
        root.addLayout(buttons)

        self._fill(layout)
        self._fit_height_to_rows()

    def entries(self):
        """编辑结果；不可调整的固定按钮按进入/重置对话框时的位置合并回来。"""
        entries = [(row.key, row.mode()) for row in self._list.rows()]
        for index, key in self._locked_slots:
            entries.insert(min(index, len(entries)), (key, SHOW))
        return normalize_layout(entries)

    def _fill(self, layout):
        """按排布摆放各行、设置下拉框"""
        normalized = normalize_layout(layout)
        self._locked_slots = [
            (index, key) for index, (key, _mode) in enumerate(normalized) if key in LOCKED
        ]
        editable = [(key, mode) for key, mode in normalized if key not in LOCKED]
        for index, (key, mode) in enumerate(editable):
            row = self._rows[key]
            self._list.add_row(row, index)
            row.set_mode(mode)

    def _fit_height_to_rows(self):
        """优先完整展示全部可调整行；只有超过屏幕可用高度时才让列表滚动。"""
        content_height = self._list.content_height()
        self._list.setFixedHeight(content_height)

        desired_height = self.sizeHint().height()
        screen = self.parentWidget().screen() if self.parentWidget() else QApplication.primaryScreen()
        if screen is not None:
            max_height = max(dialog_scaled(360), screen.availableGeometry().height() - dialog_scaled(40))
            if desired_height > max_height:
                non_list_height = desired_height - content_height
                content_height = max(dialog_scaled(180), max_height - non_list_height)
                self._list.setFixedHeight(content_height)
                desired_height = non_list_height + content_height

        self.resize(dialog_scaled(480), desired_height)
