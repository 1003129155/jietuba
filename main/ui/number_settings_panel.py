"""
序号工具设置面板
适用于：序号 (number)
"""
from PySide6.QtCore import QEvent, QPoint, QSize, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPixmap
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QStyleOptionGraphicsItem,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from canvas.items import NumberItem
from tools.number import NumberTool

from core.i18n import make_tr
from core.ui_scale import scaled
from .base_settings_panel import BaseSettingsPanel, HoverPopup, set_step_button_icon

# 样式弹出条与序号面板共用同一翻译上下文
_style_tr = make_tr("ArrowSettingsPanel")


def render_number_style_preview(
    style: str, color: QColor, side: int, number: int = 1
) -> QPixmap:
    """把一个真的 NumberItem 画进 pixmap 当预览图。

    直接复用图元自己的 paint，预览和实际画出来的东西才不会各画各的。
    """
    pixmap = QPixmap(side, side)
    pixmap.fill(Qt.GlobalColor.transparent)
    radius = side / 2.0 - 2.0
    item = NumberItem(int(number), QPoint(0, 0), radius, QColor(color), style)
    painter = QPainter(pixmap)
    try:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.translate(side / 2.0, side / 2.0)
        item.paint(painter, QStyleOptionGraphicsItem(), None)
    finally:
        painter.end()
    return pixmap


class NumberStylePopup(HoverPopup):
    """悬停在 ① 预览上弹出的样式选择条。"""

    style_selected = Signal(str)

    BASE_ITEM_SIDE = 20   # 与面板里其它小控件一个量级，34 太笨重
    BASE_ITEM_PADDING = 6
    # 这是样式选择器，不表示颜色。颜色由光标去预览，图标固定用中性色，
    # 否则选浅色标注时白底上的图标会看不见。
    PREVIEW_INK = QColor("#444444")

    def __init__(self, parent=None):
        super().__init__(parent)
        self.set_extra_stylesheet(
            # 选中态必须一眼盖过悬停态：弹出条以 ① 预览为中心对齐，鼠标移进来
            # 时几乎总会先擦过中间那两个按钮，两者长得像就会被误读成"它自己跳
            # 到空心圆了"。所以悬停只给极淡的底，选中给明显的蓝框——图标是深灰
            # 的，选中态不能用实心主题色去填，否则图样自己就看不见了。
            "QToolButton { border: 1px solid transparent; border-radius: 3px;"
            " background: transparent; }"
            "QToolButton:hover { background-color: #f2f8fd; border-color: #d8e8f5; }"
            "QToolButton:checked, QToolButton:checked:hover {"
            " background-color: #cce4f7; border: 2px solid #0078d7; }"
        )

        layout = QHBoxLayout(self)
        pad = scaled(4)
        layout.setContentsMargins(pad, pad, pad, pad)
        layout.setSpacing(scaled(2))

        self._buttons = {}
        for style, tip in (
            (NumberItem.STYLE_SOLID, "Solid"),
            (NumberItem.STYLE_HOLLOW_BG, "Hollow Circle"),
            (NumberItem.STYLE_HOLLOW_ALL, "Outline Only"),
            (NumberItem.STYLE_NO_CIRCLE, "Number Only"),
        ):
            button = QToolButton(self)
            button.setCheckable(True)
            button.setAutoRaise(True)
            button.setToolTip(_style_tr(tip))
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.clicked.connect(lambda _=False, st=style: self._choose(st))
            layout.addWidget(button)
            self._buttons[style] = button

        self.apply_scale()

    def apply_scale(self):
        """按当前比例重挂样式、重算格子大小并重画预览图"""
        super().apply_scale()
        if not getattr(self, "_buttons", None):
            return
        side = scaled(self.BASE_ITEM_SIDE)
        cell = side + scaled(self.BASE_ITEM_PADDING)
        for style, button in self._buttons.items():
            button.setFixedSize(cell, cell)
            button.setIconSize(QSize(side, side))
            button.setIcon(render_number_style_preview(style, self.PREVIEW_INK, side))
        self.adjustSize()

    def set_current_style(self, style: str):
        style = NumberItem.normalize_style(style)
        for key, button in self._buttons.items():
            button.setChecked(key == style)

    def _choose(self, style: str):
        self.set_current_style(style)
        self.style_selected.emit(style)
        self.hide()


class NumberSettingsPanel(BaseSettingsPanel):
    """序号工具设置面板 - 统一风格"""

    next_number_changed = Signal(int)
    style_changed = Signal(str)

    # 与 NumberTool 共用同一个范围：面板显示什么大小，就必须画出什么大小
    SIZE_RANGE = (NumberTool.MIN_WIDTH, NumberTool.MAX_WIDTH)
    SIZE_DEFAULT = 16
    SIZE_TOOLTIP = "Font Size"

    # 基准尺寸（100% 下的实际像素）
    BASE_NEXT_PREVIEW = 23
    BASE_NEXT_BTN_WIDTH = 16
    BASE_NEXT_BTN_HEIGHT = 13

    def _build_extra_controls(self, layout):
        self.next_preview = QLabel()
        self.next_preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        # 这里要显示的是下一个序号会长成什么样，所以连样式一起画，
        # 而不是用 CSS 画一个永远是空心圆的假框。
        self.next_preview.setStyleSheet("QLabel { background: transparent; }")
        layout.insertWidget(0, self.next_preview)

        self._next_value = 1
        self._current_style = NumberItem.DEFAULT_STYLE
        self._style_popup = None
        # 悬停在 ① 预览上弹出样式选择条
        self.next_preview.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.next_preview.setToolTip(self._tr("Number Style"))
        self.next_preview.installEventFilter(self)

        btn_wrap = QWidget()
        self._next_btn_layout = QVBoxLayout(btn_wrap)
        self._next_btn_layout.setContentsMargins(0, 0, 0, 0)

        self.next_up_btn = QToolButton()
        self.next_up_btn.setToolTip(self._tr("Next Number"))
        self._next_btn_layout.addWidget(self.next_up_btn)

        self.next_down_btn = QToolButton()
        self.next_down_btn.setToolTip(self._tr("Next Number"))
        self._next_btn_layout.addWidget(self.next_down_btn)

        layout.insertWidget(1, btn_wrap)

        self.next_up_btn.clicked.connect(lambda: self._change_next_number(1))
        self.next_down_btn.clicked.connect(lambda: self._change_next_number(-1))

    def _apply_scale_sizes(self):
        super()._apply_scale_sizes()
        preview = scaled(self.BASE_NEXT_PREVIEW)
        self.next_preview.setFixedSize(preview, preview)
        for direction, button in (("up", self.next_up_btn), ("down", self.next_down_btn)):
            set_step_button_icon(button, direction)
            button.setFixedSize(scaled(self.BASE_NEXT_BTN_WIDTH),
                                scaled(self.BASE_NEXT_BTN_HEIGHT))
        self._next_btn_layout.setSpacing(scaled(2))
        popup = getattr(self, "_style_popup", None)
        if popup is not None:
            popup.apply_scale()
        # 预览图是按 next_preview 的像素尺寸画的，改比例后要重画
        self._update_next_preview()

    def set_next_number(self, value: int):
        if not hasattr(self, "next_preview"):
            return
        value = max(1, int(value))
        if getattr(self, "_next_value", 1) == value:
            return
        self._next_value = value
        self._update_next_preview(value)

    def _update_next_preview(self, value: int = None):
        """把下一个序号按当前样式画出来。"""
        if not hasattr(self, "next_preview"):
            return
        value = self._next_value if value is None else int(value)
        self.next_preview.setPixmap(
            render_number_style_preview(
                self.current_style,
                NumberStylePopup.PREVIEW_INK,
                self.next_preview.width(),
                value,
            )
        )

    def _change_next_number(self, delta: int):
        value = max(1, int(getattr(self, "_next_value", 1)) + int(delta))
        self._next_value = value
        self._update_next_preview(value)
        self.next_number_changed.emit(int(value))

    # ------------------------------------------------------------------
    # 样式选择
    # ------------------------------------------------------------------

    def eventFilter(self, watched, event):
        if watched is getattr(self, "next_preview", None):
            if event.type() == QEvent.Type.Enter:
                self._show_style_popup()
            elif event.type() == QEvent.Type.Leave:
                popup = self._style_popup
                if popup is not None:
                    popup.close_soon()
        return super().eventFilter(watched, event)

    def _ensure_style_popup(self):
        if self._style_popup is None:
            popup = NumberStylePopup(self)
            popup.style_selected.connect(self._on_style_selected)
            self._style_popup = popup
        return self._style_popup

    def _show_style_popup(self):
        popup = self._ensure_style_popup()
        popup.set_current_style(self._current_style)
        popup.show_beside(self, self.next_preview)

    def _on_style_selected(self, style: str):
        self.set_style(style)
        self.style_changed.emit(style)

    def set_style(self, style: str):
        """设置当前样式（不触发信号）。"""
        style = NumberItem.normalize_style(style)
        self._current_style = style
        if self._style_popup is not None:
            self._style_popup.set_current_style(style)
        self._update_next_preview()

    @property
    def current_style(self) -> str:
        return getattr(self, "_current_style", NumberItem.DEFAULT_STYLE)
