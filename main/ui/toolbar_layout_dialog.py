"""
截图工具栏排布对话框：拖左侧手柄调整顺序，下拉框选择每个按钮的显示方式

对话框只负责编辑；读写配置、重排工具栏由调用方 Toolbar._open_layout_dialog 完成。

行没有做成 QListWidget 的条目再用内置拖放：InternalMove 会把被拖的条目删掉再插入，
setItemWidget 挂上去的下拉框会跟着丢失。这里每一行就是布局里的普通部件，拖动时
直接在布局里换位置。
"""

from functools import partial

from PySide6.QtCore import QPoint, QPointF, Qt, Signal
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import (
    QApplication, QDialog, QHBoxLayout, QLabel, QScrollArea, QVBoxLayout, QWidget,
)

from core import safe_event
from core.i18n import make_tr
from ui.fluent_lite import (
    BodyLabel, CaptionLabel, ComboBox, PrimaryPushButton, PushButton,
    SimpleCardWidget, TransparentPushButton, ui_tokens,
)
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


class _Grip(QWidget):
    """行首的拖动手柄（2×3 圆点）。只把按下、拖动、松开报出去，换位由对话框做。"""

    pressed = Signal()
    dragged = Signal(int)   # 鼠标的全局 y
    released = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(16, 28)
        self.setCursor(Qt.CursorShape.OpenHandCursor)

    @safe_event
    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            self.pressed.emit()

    @safe_event
    def mouseMoveEvent(self, event):
        # 按下后 Qt 会隐式抓住鼠标，拖出手柄、拖出对话框也照样收得到移动事件
        if event.buttons() == Qt.MouseButton.LeftButton:
            self.dragged.emit(event.globalPosition().toPoint().y())

    @safe_event
    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setCursor(Qt.CursorShape.OpenHandCursor)
            self.released.emit()

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(ui_tokens(self).text_muted))
        center = QPointF(self.rect().center())
        for dx in (-3, 3):
            for dy in (-6, 0, 6):
                painter.drawEllipse(center + QPointF(dx, dy), 1.5, 1.5)
        painter.end()


class _Row(QWidget):
    """一行：拖动手柄、按钮图标、名称、显示方式下拉框"""

    def __init__(self, key, icon, parent=None):
        super().__init__(parent)
        self.key = key
        self._dragging = False

        self.grip = _Grip(self)

        # 图标垫一块白底：工具栏图标是照着白底工具栏画的深色图标，深色主题下直接放看不清
        icon_label = QLabel(self)
        icon_label.setFixedSize(28, 28)
        icon_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        icon_label.setPixmap(icon.pixmap(20, 20))
        icon_label.setStyleSheet("background: #FFFFFF; border-radius: 6px;")

        self.combo = ComboBox(self)
        for mode, text in MODE_NAMES:
            self.combo.addItem(_tr(text), mode)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 3, 8, 3)
        layout.setSpacing(10)
        layout.addWidget(self.grip)
        layout.addWidget(icon_label)
        layout.addWidget(BodyLabel(_tr(BUTTON_NAMES[key]), self), 1)
        layout.addWidget(self.combo)

    def mode(self):
        return self.combo.currentData()

    def set_mode(self, mode):
        self.combo.setCurrentIndex(self.combo.findData(mode))

    def set_dragging(self, dragging):
        """拖动中的行垫一层强调色，让用户看清自己拖的是哪一行"""
        self._dragging = dragging
        self.update()

    @safe_event
    def paintEvent(self, event):
        if not self._dragging:
            return
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(ui_tokens(self).accent_soft))
        painter.drawRoundedRect(self.rect(), 8, 8)
        painter.end()


class ToolbarLayoutDialog(QDialog):
    """编辑截图工具栏排布。layout 为 [(按钮, 显示方式)]，icons 为 按钮 → QIcon。"""

    def __init__(self, layout, icons, parent=None):
        super().__init__(parent)
        self.setWindowTitle(_tr("Customize Toolbar"))
        # 从全屏置顶的截图窗口里打开，和截图里的取色对话框一样置顶，免得被截图窗口盖住
        self.setWindowFlags(
            Qt.WindowType.Dialog
            | Qt.WindowType.WindowTitleHint
            | Qt.WindowType.WindowCloseButtonHint
            | Qt.WindowType.WindowStaysOnTopHint
        )
        self.setStyleSheet(f"QDialog {{ background: {ui_tokens(self).window}; }}")

        self._card = SimpleCardWidget()
        self._list_layout = QVBoxLayout(self._card)
        self._list_layout.setContentsMargins(6, 6, 6, 6)
        self._list_layout.setSpacing(0)
        self._list_layout.addStretch(1)

        # 固定按钮不显示灰色禁用行；这里只为真正可调整的按钮创建界面。
        self._rows = {}
        for key in DEFAULT_ORDER:
            if key in LOCKED:
                continue
            row = _Row(key, icons[key], self._card)
            row.grip.pressed.connect(partial(row.set_dragging, True))
            row.grip.dragged.connect(partial(self._drag_row, row))
            row.grip.released.connect(partial(row.set_dragging, False))
            self._rows[key] = row

        self._scroll = QScrollArea(self)
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        self._scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")
        self._scroll.setWidget(self._card)

        hint = CaptionLabel(
            _tr("Drag the handle to reorder. Use the drop-down to change visibility."), self)

        reset_btn = TransparentPushButton(_tr("Restore defaults"), self)
        reset_btn.clicked.connect(lambda: self._fill(default_layout()))
        ok_btn = PrimaryPushButton(_tr("OK"), self)
        ok_btn.clicked.connect(self.accept)
        cancel_btn = PushButton(_tr("Cancel"), self)
        cancel_btn.clicked.connect(self.reject)

        buttons = QHBoxLayout()
        buttons.addWidget(reset_btn)
        buttons.addStretch(1)
        buttons.addWidget(ok_btn)
        buttons.addWidget(cancel_btn)

        root = QVBoxLayout(self)
        root.setContentsMargins(16, 16, 16, 14)
        root.setSpacing(10)
        root.addWidget(self._scroll, 1)
        root.addWidget(hint)
        root.addLayout(buttons)

        self._fill(layout)
        self._fit_height_to_rows()

    def entries(self):
        """编辑结果；不可调整的固定按钮按进入/重置对话框时的位置合并回来。"""
        entries = [(row.key, row.mode()) for row in self._ordered_rows()]
        for index, key in self._locked_slots:
            entries.insert(min(index, len(entries)), (key, SHOW))
        return normalize_layout(entries)

    def _ordered_rows(self):
        items = (self._list_layout.itemAt(i) for i in range(self._list_layout.count()))
        return [item.widget() for item in items if item.widget() is not None]

    def _fill(self, layout):
        """按排布摆放各行、设置下拉框"""
        normalized = normalize_layout(layout)
        self._locked_slots = [
            (index, key) for index, (key, _mode) in enumerate(normalized) if key in LOCKED
        ]
        editable = [(key, mode) for key, mode in normalized if key not in LOCKED]
        for index, (key, mode) in enumerate(editable):
            row = self._rows[key]
            self._place(row, index)
            row.set_mode(mode)

    def _place(self, row, index):
        self._list_layout.removeWidget(row)
        self._list_layout.insertWidget(index, row)

    def _fit_height_to_rows(self):
        """优先完整展示全部可调整行；只有超过屏幕可用高度时才让列表滚动。"""
        self._list_layout.activate()
        content_height = self._list_layout.sizeHint().height() + 2
        self._scroll.setFixedHeight(content_height)

        desired_height = self.sizeHint().height()
        screen = self.parentWidget().screen() if self.parentWidget() else QApplication.primaryScreen()
        if screen is not None:
            max_height = max(360, screen.availableGeometry().height() - 40)
            if desired_height > max_height:
                non_list_height = desired_height - content_height
                content_height = max(180, max_height - non_list_height)
                self._scroll.setFixedHeight(content_height)
                desired_height = non_list_height + content_height

        self.resize(480, desired_height)

    def _drag_row(self, row, global_y):
        """被拖的行跟着鼠标换位：它该排第几，就看其余行里有几行的中线在鼠标上方"""
        y = self._card.mapFromGlobal(QPoint(0, global_y)).y()
        index = sum(
            1 for other in self._ordered_rows()
            if other is not row and other.geometry().center().y() < y
        )
        if self._list_layout.indexOf(row) != index:
            self._place(row, index)
            # 立刻按新顺序算出各行几何，下一次鼠标移动才能基于新位置判断
            self._list_layout.activate()
        # 拖到可见区域外时让滚动跟上
        self._scroll.ensureWidgetVisible(row, 0, 0)
