# -*- coding: utf-8 -*-
"""
设置窗口 — 共享 UI 组件库
"""
from PySide6.QtWidgets import (
    QWidget, QFrame, QVBoxLayout, QHBoxLayout, QLabel, QSizePolicy,
)
from PySide6.QtCore import QRect, QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QPainter
from core import safe_event
from core.ui_scale import dialog_scaled
from core.ui_theme import get_ui_theme

from ui.fluent_lite import (
    SwitchButton, SimpleCardWidget, SwitchSettingCard as _SwitchSettingCard, SettingCardGroup as _SettingCardGroupBase,
)
from ui.fluent_lite.theme import css_color, tinted_icon


# 四个全局功能各有固定色相，饱和度压低，和蓝灰强调色放在一起不跳；
# 其余设置行统一用 neutral。每项是 ((浅色底, 浅色图标), (深色底, 深色图标))。
FEATURE_TONES = {
    "capture": (("#F3E3E3", "#B0575A"), ("rgba(227, 154, 156, 0.16)", "#E39A9C")),
    "clipboard": (("#E1EEE6", "#4E8A67"), ("rgba(140, 201, 165, 0.15)", "#8CC9A5")),
    "pin": (("#F5EAD9", "#9E6B28"), ("rgba(224, 176, 112, 0.15)", "#E0B070")),
    "translate": (("#E9E4F3", "#7A63A8"), ("rgba(183, 163, 227, 0.16)", "#B7A3E3")),
    "neutral": (("#E3EAF0", "#526D85"), ("rgba(111, 143, 171, 0.20)", "#AFC3D4")),
}


def theme_color(light: str, dark: str) -> str:
    """Return one of two values for the effective application theme."""
    return dark if get_ui_theme().is_dark else light


def theme_surface_color() -> str:
    return theme_color("#FAFBFC", "#202124")


def theme_border_color() -> str:
    return theme_color("rgba(255, 255, 255, 0.76)", "rgba(255, 255, 255, 0.08)")


def theme_input_background() -> str:
    return theme_color("rgba(255, 255, 255, 0.78)", "#2B2D31")


def theme_popup_background() -> str:
    return theme_color("#FFFFFF", "#2A2C30")


def theme_popup_hover_background() -> str:
    return theme_color("#EAF2FA", "#36393F")


def theme_text_style(font_size: int = 14, bold: bool = False, extra: str = "") -> str:
    weight = " font-weight: 600;" if bold else ""
    suffix = f" {extra.strip()}" if extra.strip() else ""
    color = get_ui_theme().tokens.text
    return f"font-size: {dialog_scaled(font_size)}px; color: {color}; background: transparent;{weight}{suffix}"


def theme_caption_style(font_size: int = 13, extra: str = "") -> str:
    suffix = f" {extra.strip()}" if extra.strip() else ""
    color = get_ui_theme().tokens.text_muted
    return f"font-size: {dialog_scaled(font_size)}px; color: {color}; background: transparent;{suffix}"


def apply_theme_text_style(
    widget: QWidget,
    font_size: int = 14,
    bold: bool = False,
    extra: str = "",
    caption: bool = False,
):
    """Apply and register a semantic text style for runtime refresh."""
    widget._ui_theme_text_spec = (font_size, bold, extra, caption)
    style = (
        theme_caption_style(font_size, extra)
        if caption
        else theme_text_style(font_size, bold, extra)
    )
    widget.setStyleSheet(style)


def refresh_theme_widget_styles(root: QWidget):
    """Refresh semantic text styles registered below a top-level widget."""
    for widget in (root, *root.findChildren(QWidget)):
        spec = getattr(widget, "_ui_theme_text_spec", None)
        if spec is not None:
            apply_theme_text_style(widget, *spec)


def theme_menu_style() -> str:
    return f"""
        QMenu {{
            background-color: {theme_popup_background()};
            color: {get_ui_theme().tokens.text};
            border: 1px solid {theme_border_color()};
            border-radius: {dialog_scaled(6)}px;
            padding: {dialog_scaled(4)}px 0;
        }}
        QMenu::item {{
            padding: {dialog_scaled(6)}px {dialog_scaled(20)}px;
            font-size: {dialog_scaled(14)}px;
            color: {get_ui_theme().tokens.text};
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

    def __init__(self, title, parent=None):
        super().__init__(title, parent)
        # 竖直方向不吃富余空间：页面末尾有 addStretch()，本该由它吸收多出来的高度。
        # 默认策略下组会把富余摊进卡片之间，表现为组顶部凭空多一段空白，
        # 同时把后面的卡片顶出可视区。
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Maximum)

    def refreshHeight(self):
        """按当前可见的卡片重算最小高度。

        只在 showEvent 里算一次是不够的：卡片显隐变化后（翻译页切换服务商时，
        同一个组里换成行数不同的另一批凭证），旧的 minimumHeight 会留着不动，
        表现为组顶部多出一段空白、页面底部的卡片被切掉。
        """
        # 列宽影响左侧说明的折行数，因而影响高度，要先于高度算。
        self._sync_control_column()
        # 上下限一起锁死。只设 minimum 的话，组内的 _card_container 是默认策略，
        # 会把页面分给组的富余高度全吃进去，摊在卡片之间。
        self.setFixedHeight(self.cardLayout.heightForWidth(self.width()) + dialog_scaled(46))

    @safe_event
    def showEvent(self, e):
        super().showEvent(e)
        # 显示后 card.height() 才准确，重新算高度
        self.refreshHeight()

    @safe_event
    def resizeEvent(self, e):
        super().resizeEvent(e)
        # 宽度变了，带换行描述的卡片行数会变，高度得跟着重算
        self.refreshHeight()

def make_row(label, ctrl_widget: QWidget) -> QHBoxLayout:
    """创建统一的「标签 — 控件」行。"""
    row = QHBoxLayout()
    row.setSpacing(10)
    if isinstance(label, str):
        lbl = QLabel(label)
        apply_theme_text_style(lbl, 14)
    else:
        lbl = label
    row.addWidget(lbl, 1)
    row.addWidget(ctrl_widget)
    return row


def make_card_title(text: str) -> QLabel:
    """创建卡片标题"""
    lbl = QLabel(text)
    apply_theme_text_style(lbl, 15, bold=True)
    return lbl


def adjust_button_width(button, min_width: int = 0, horizontal_padding: int = 28):
    """按当前文字和图标内容调整按钮宽度。"""
    button.ensurePolished()
    content_width = button.sizeHint().width() + dialog_scaled(horizontal_padding)
    button.setMinimumWidth(max(dialog_scaled(min_width), content_width))


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
        t = get_ui_theme().tokens
        painter = QPainter(self)
        painter.setRenderHints(QPainter.RenderHint.Antialiasing)
        rect = self.rect().adjusted(1, 1, -1, -1)
        if t.is_dark:
            painter.setBrush(QColor(43, 47, 52, 245))
            painter.setPen(QColor(255, 255, 255, 31))
        else:
            painter.setBrush(QColor(255, 255, 255, 46))
            painter.setPen(QColor(255, 255, 255, 76))
        painter.drawRoundedRect(rect, dialog_scaled(11), dialog_scaled(11))


class IconBadge(QWidget):
    """设置行左侧的图标块。

    tone 取 FEATURE_TONES 的键时画带底色的方块；为 None 时只画图标，
    muted 决定图标用次要文字色还是正文色。
    """

    def __init__(self, icon, tone="neutral", parent=None, *, size=36, icon_size=20, muted=True):
        super().__init__(parent)
        self._icon = icon
        self._tone = tone
        self._icon_size = icon_size
        self._muted = muted
        self.setFixedSize(dialog_scaled(size), dialog_scaled(size))
        get_ui_theme().theme_changed.connect(self.update)

    @safe_event
    def paintEvent(self, e):
        t = get_ui_theme().tokens
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self._tone is None:
            fg = t.text_muted if self._muted else t.text
        else:
            bg, fg = FEATURE_TONES[self._tone][1 if t.is_dark else 0]
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(css_color(bg))
            radius = dialog_scaled(10)
            painter.drawRoundedRect(QRectF(self.rect()), radius, radius)
        if self._icon is None:
            return
        side = dialog_scaled(self._icon_size)
        target = QRect(0, 0, side, side)
        target.moveCenter(self.rect().center())
        tinted_icon(self._icon, fg).paint(painter, target)


class _Separator(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedHeight(1)
        get_ui_theme().theme_changed.connect(self.update)

    @safe_event
    def paintEvent(self, e):
        QPainter(self).fillRect(self.rect(), css_color(get_ui_theme().tokens.separator))


class SectionCard(QFrame):
    """标题放在卡片内的设置分组：图标 + 标题 + 一句说明，下面逐行排列。

    用普通布局而不是 SettingCardGroup 的 ExpandLayout，高度跟着内容走，
    不需要手动 setFixedHeight。
    """

    def __init__(self, icon, title, caption="", parent=None):
        super().__init__(parent)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.bodyLayout = QVBoxLayout(self)
        self.bodyLayout.setContentsMargins(dialog_scaled(20), 0, dialog_scaled(20), dialog_scaled(4))
        self.bodyLayout.setSpacing(0)

        header = QWidget(self)
        header.setFixedHeight(dialog_scaled(50))
        header_layout = QHBoxLayout(header)
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(dialog_scaled(10))
        header_layout.addWidget(
            IconBadge(icon, None, header, size=20, icon_size=19, muted=False)
        )
        self.titleLabel = QLabel(title, header)
        apply_theme_text_style(self.titleLabel, 15, bold=True)
        header_layout.addWidget(self.titleLabel)
        self.captionLabel = None
        if caption:
            self.captionLabel = QLabel(caption, header)
            apply_theme_text_style(self.captionLabel, 12, caption=True)
            header_layout.addWidget(self.captionLabel)
        header_layout.addStretch(1)
        self.bodyLayout.addWidget(header)
        get_ui_theme().theme_changed.connect(self.update)

    def addRow(self, widget: QWidget):
        """加一行，上方带分隔线。"""
        add_separated_row(self.bodyLayout, widget)

    def addWidget(self, widget: QWidget):
        """加一块不带分隔线的内容（标签页切换条这类）。"""
        self.bodyLayout.addWidget(widget)

    @safe_event
    def paintEvent(self, e):
        t = get_ui_theme().tokens
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(css_color(t.border))
        painter.setBrush(css_color(t.surface))
        radius = dialog_scaled(15)
        painter.drawRoundedRect(QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius)


def add_separated_row(layout, widget: QWidget):
    """往竖排布局里加一行，上方带一条主题色分隔线。"""
    layout.addWidget(_Separator(widget.parentWidget()))
    layout.addWidget(widget)


def make_switch_card(dialog, icon, title, content, checked, attr_name, parent=None):
    """创建 SwitchSettingCard 并将其绑定到 dialog 属性。

    SwitchSettingCard 自带 isChecked()/setChecked()，
    所以直接赋给 dialog.attr_name 即可兼容 accept/reset/refresh。
    """
    card = _SwitchSettingCard(icon, title, content, parent=parent)
    card.setChecked(checked)
    setattr(dialog, attr_name, card)
    return card
 
