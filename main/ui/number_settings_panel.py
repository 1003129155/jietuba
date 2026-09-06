"""
序号工具设置面板
适用于：序号 (number)
"""
from PySide6.QtCore import QEvent, QPoint, QRect, QSize, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QPainter, QPixmap
from PySide6.QtWidgets import (
    QApplication,
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
from .base_settings_panel import BaseSettingsPanel, PANEL_SCALE, set_step_button_icon

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


class NumberStylePopup(QWidget):
    """悬停在 ① 预览上弹出的样式选择条。"""

    style_selected = Signal(str)

    ITEM_SIDE = 20      # 与面板里其它小控件一个量级，34 太笨重
    # 这是样式选择器，不表示颜色。颜色由光标去预览，图标固定用中性色，
    # 否则选浅色标注时白底上的图标会看不见。
    PREVIEW_INK = QColor("#444444")
    CLOSE_DELAY_MS = 260

    def __init__(self, parent=None):
        super().__init__(parent)
        # 与设置面板同样的标志：浮在最上层且不抢焦点，否则一弹出面板就没了
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setObjectName("NumberStylePopup")
        # 配色沿用应用里设置面板的白底浅色，不要另造一套深色
        self.setStyleSheet(
            "#NumberStylePopup { background: white; border: 1px solid #ccc;"
            " border-radius: 3px; }"
            "QToolButton { border: 1px solid transparent; border-radius: 3px;"
            " background: transparent; }"
            "QToolButton:hover { background-color: #e5f3ff; border-color: #cce4f7; }"
            "QToolButton:checked { background-color: #cce4f7; border-color: #0078d7; }"
        )

        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(2)

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
            button.setFixedSize(self.ITEM_SIDE + 6, self.ITEM_SIDE + 6)
            button.setIconSize(QSize(self.ITEM_SIDE, self.ITEM_SIDE))
            button.setToolTip(_style_tr(tip))
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.clicked.connect(lambda _=False, st=style: self._choose(st))
            layout.addWidget(button)
            self._buttons[style] = button

        self._close_timer = QTimer(self)
        self._close_timer.setSingleShot(True)
        self._close_timer.setInterval(self.CLOSE_DELAY_MS)
        self._close_timer.timeout.connect(self.hide)
        self._render_previews()

    def _render_previews(self):
        for style, button in self._buttons.items():
            button.setIcon(
                render_number_style_preview(style, self.PREVIEW_INK, self.ITEM_SIDE)
            )

    def set_current_style(self, style: str):
        style = NumberItem.normalize_style(style)
        for key, button in self._buttons.items():
            button.setChecked(key == style)

    def _choose(self, style: str):
        self.set_current_style(style)
        self.style_selected.emit(style)
        self.hide()

    # 鼠标在"预览标签"和"弹出层"之间移动时不能立刻关，留一点缓冲
    def keep_open(self):
        self._close_timer.stop()

    def close_soon(self):
        self._close_timer.start()

    def enterEvent(self, event):
        self.keep_open()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self.close_soon()
        super().leaveEvent(event)


class NumberSettingsPanel(BaseSettingsPanel):
    """序号工具设置面板 - 统一风格"""

    next_number_changed = Signal(int)
    style_changed = Signal(str)

    # 与 NumberTool 共用同一个范围：面板显示什么大小，就必须画出什么大小
    SIZE_RANGE = (NumberTool.MIN_WIDTH, NumberTool.MAX_WIDTH)
    SIZE_DEFAULT = 16
    SIZE_TOOLTIP = "Font Size"

    def _build_extra_controls(self, layout):
        _sz = round(26 * PANEL_SCALE)
        self.next_preview = QLabel()
        self.next_preview.setFixedSize(_sz, _sz)
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
        btn_layout = QVBoxLayout(btn_wrap)
        btn_layout.setContentsMargins(0, 0, 0, 0)
        btn_layout.setSpacing(2)

        self.next_up_btn = QToolButton()
        set_step_button_icon(self.next_up_btn, "up")
        self.next_up_btn.setFixedSize(round(18 * PANEL_SCALE), round(14 * PANEL_SCALE))
        self.next_up_btn.setToolTip(self._tr("Next Number"))
        btn_layout.addWidget(self.next_up_btn)

        self.next_down_btn = QToolButton()
        set_step_button_icon(self.next_down_btn, "down")
        self.next_down_btn.setFixedSize(round(18 * PANEL_SCALE), round(14 * PANEL_SCALE))
        self.next_down_btn.setToolTip(self._tr("Next Number"))
        btn_layout.addWidget(self.next_down_btn)

        layout.insertWidget(1, btn_wrap)

        self.next_up_btn.clicked.connect(lambda: self._change_next_number(1))
        self.next_down_btn.clicked.connect(lambda: self._change_next_number(-1))
        self._update_next_preview(self._next_value)

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
        popup.keep_open()
        popup.adjustSize()

        popup.move(self._style_popup_position(popup))
        popup.show()
        popup.raise_()

    def _style_popup_position(self, popup) -> QPoint:
        """背离工具栏的方向弹；那一边放不下就换另一边。

        面板在工具栏下方就往下弹，在上方就往上弹，这样天然不会盖住一级/二级菜单。
        """
        gap = 4
        panel_rect = QRect(self.mapToGlobal(QPoint(0, 0)), self.size())
        preview_top_left = self.next_preview.mapToGlobal(QPoint(0, 0))

        screen = QApplication.screenAt(preview_top_left) or QApplication.primaryScreen()
        area = screen.availableGeometry()

        toolbar = getattr(self, "_owner_toolbar", None)
        below_toolbar = True
        if toolbar is not None:
            try:
                below_toolbar = panel_rect.top() >= toolbar.mapToGlobal(QPoint(0, 0)).y()
            except RuntimeError:
                pass

        outward = panel_rect.bottom() + gap if below_toolbar else panel_rect.top() - popup.height() - gap
        other = panel_rect.top() - popup.height() - gap if below_toolbar else panel_rect.bottom() + gap

        y = outward
        if not (area.top() <= y and y + popup.height() <= area.bottom()):
            y = other

        x = preview_top_left.x() + self.next_preview.width() // 2 - popup.width() // 2
        x = max(area.left(), min(x, area.right() - popup.width()))
        y = max(area.top(), min(y, area.bottom() - popup.height()))
        return QPoint(int(x), int(y))

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
