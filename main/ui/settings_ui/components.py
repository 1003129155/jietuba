# -*- coding: utf-8 -*-
"""
设置窗口 — 共享 UI 组件库
"""
from PySide6.QtWidgets import QWidget, QFrame, QVBoxLayout, QHBoxLayout, QLabel
from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtGui import QColor, QPainter
from core import safe_event

from ui.fluent_lite import (
    SwitchButton, SimpleCardWidget, BodyLabel, CaptionLabel,
    SwitchSettingCard as _SwitchSettingCard, FluentIcon,
    SettingCard as FluentSettingCard,
    SettingCardGroup as _SettingCardGroupBase,
)


def theme_color(light: str, dark: str) -> str:
    """返回当前主题下的颜色（固定亮色）。"""
    return light


def theme_surface_color() -> str:
    return theme_color("rgba(239, 244, 250, 0.91)", "#202124")


def theme_sidebar_color() -> str:
    return theme_color("rgba(246, 249, 252, 0.54)", "#25272B")


def theme_border_color() -> str:
    return theme_color("rgba(255, 255, 255, 0.76)", "rgba(255, 255, 255, 0.08)")


def theme_input_background() -> str:
    return theme_color("rgba(255, 255, 255, 0.78)", "#2B2D31")


def theme_popup_background() -> str:
    return theme_color("#FFFFFF", "#2A2C30")


def theme_popup_hover_background() -> str:
    return theme_color("#EAF2FA", "#36393F")


def theme_text_style(font_size: int = 13, bold: bool = False, extra: str = "") -> str:
    weight = " font-weight: 600;" if bold else ""
    suffix = f" {extra.strip()}" if extra.strip() else ""
    return f"font-size: {font_size}px; background: transparent;{weight}{suffix}"


def theme_caption_style(font_size: int = 12, extra: str = "") -> str:
    suffix = f" {extra.strip()}" if extra.strip() else ""
    return f"font-size: {font_size}px; background: transparent;{suffix}"


def theme_menu_style() -> str:
    return f"""
        QMenu {{
            background-color: {theme_popup_background()};
            border: 1px solid {theme_border_color()};
            border-radius: 6px;
            padding: 4px 0;
        }}
        QMenu::item {{
            padding: 6px 20px;
            font-size: 13px;
            background: transparent;
        }}
        QMenu::item:selected {{
            background-color: {theme_popup_hover_background()};
        }}
    """


class SettingCardGroup(_SettingCardGroupBase):
    """调整 SettingCardGroup 的最小高度计算。"""

    def addSettingCard(self, card: QWidget):
        super().addSettingCard(card)

    @safe_event
    def showEvent(self, e):
        super().showEvent(e)
        # 显示后 card.height() 才准确，重新算 minimumHeight
        h = self.cardLayout.heightForWidth(self.width()) + 46
        self.setMinimumHeight(h)

# ── 设置页通用样式常量 ────────────────────────────────
LBL_STYLE = "font-size: 13px; background-color: transparent;"
TITLE_STYLE = "font-size: 14px; font-weight: bold; background-color: transparent;"


def make_row(label, ctrl_widget: QWidget) -> QHBoxLayout:
    """创建统一的「标签 — 控件」行。"""
    row = QHBoxLayout()
    row.setSpacing(10)
    if isinstance(label, str):
        lbl = QLabel(label)
        lbl.setStyleSheet(LBL_STYLE)
    else:
        lbl = label
    row.addWidget(lbl, 1)
    row.addWidget(ctrl_widget)
    return row


def make_card_title(text: str) -> QLabel:
    """创建卡片标题"""
    lbl = QLabel(text)
    lbl.setStyleSheet(TITLE_STYLE)
    return lbl


def adjust_button_width(button, min_width: int = 0, horizontal_padding: int = 28):
    """按当前文字和图标内容调整按钮宽度。"""
    button.ensurePolished()
    content_width = button.sizeHint().width() + horizontal_padding
    button.setMinimumWidth(max(min_width, content_width))


class ToggleSwitch(SwitchButton):
    """Fluent SwitchButton 兼容层。"""
    toggled = Signal(bool)

    def __init__(self, parent=None, **kwargs):
        super().__init__(parent=parent)
        self.setOnText('')
        self.setOffText('')
        self.checkedChanged.connect(self.toggled)


class SettingCard(QFrame):
    """白底圆角卡片容器 — 旧版兼容"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("Card")
        self.setStyleSheet("""
            #Card {
                background-color: #FFFFFF;
                border-radius: 8px;
                border: 1px solid #E5E5E5;
            }
        """)
        self.layout = QVBoxLayout(self)
        self.layout.setContentsMargins(20, 20, 20, 20)
        self.layout.setSpacing(15)


class HLine(QFrame):
    """分割线"""
    def __init__(self):
        super().__init__()
        self.setFrameShape(QFrame.Shape.HLine)
        self.setFrameShadow(QFrame.Shadow.Sunken)
        self.setStyleSheet("background-color: #F0F0F0; border: none; max-height: 1px;")


# ── Fluent 辅助 ──────────────────────────────────────

class FluentCard(SimpleCardWidget):
    """基于 SimpleCardWidget 的自由布局 Fluent 卡片。
    用于需要复杂自定义内容的场景（路径+按钮、多行输入等）。
    """
    def __init__(self, parent=None):
        super().__init__(parent)
        self.vBoxLayout = QVBoxLayout(self)
        self.vBoxLayout.setContentsMargins(20, 16, 20, 16)
        self.vBoxLayout.setSpacing(8)


class TransparentCard(QFrame):
    """透明卡片 — 用于 SettingCardGroup 内需要多行/复杂布局的场景。

    不绘制背景和边框（避免灰色方块），但仍是独立 QFrame，
    ExpandLayout 可以正确定位它。使用后需调用 setFixedHeight() 确保
    ExpandLayout 能拿到正确高度。
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setStyleSheet("background: transparent; border: none;")


class WhiteCard(QFrame):
    """自定义可变高度卡片。

    使用轻量绘制逻辑避免样式表导致的局部背景异常，
    同时不受旧组件固定高度限制。
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, False)

    def minimumSizeHint(self):
        return QSize(0, max(self.minimumHeight(), self.layout().minimumSize().height() if self.layout() else 0))

    @safe_event
    def paintEvent(self, e):
        painter = QPainter(self)
        painter.setRenderHints(QPainter.RenderHint.Antialiasing)
        rect = self.rect().adjusted(1, 1, -1, -1)
        painter.setBrush(QColor(255, 255, 255, 46))
        painter.setPen(QColor(255, 255, 255, 76))
        painter.drawRoundedRect(rect, 11, 11)


def make_switch_card(dialog, icon, title, content, checked, attr_name, parent=None):
    """创建 SwitchSettingCard 并将其绑定到 dialog 属性。

    SwitchSettingCard 自带 isChecked()/setChecked()，
    所以直接赋给 dialog.attr_name 即可兼容 accept/reset/refresh。
    """
    card = _SwitchSettingCard(icon, title, content, parent=parent)
    card.setChecked(checked)
    setattr(dialog, attr_name, card)
    return card
 
