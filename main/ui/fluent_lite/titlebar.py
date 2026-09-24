"""Shared Fluent title bar for the application's standalone windows."""

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import QLabel
from qframelesswindow import TitleBar, TitleBarButton
from qframelesswindow.utils import toggleMaxState

from core.ui_scale import widget_scaled as _px
from core.ui_theme import get_ui_theme

from .theme import FONT_FAMILY, ui_tokens


class _FluentCaptionButton(TitleBarButton):
    """Scale-aware caption button shared by every Fluent window."""

    MINIMIZE = "minimize"
    MAXIMIZE = "maximize"
    CLOSE = "close"

    def __init__(self, kind, parent=None):
        super().__init__(parent)
        self.kind = kind
        self._is_maximized = False

    def setMaxState(self, is_maximized):
        is_maximized = bool(is_maximized)
        if self._is_maximized == is_maximized:
            return
        self._is_maximized = is_maximized
        self.update()

    def paintEvent(self, event):
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        color, background = self._getColors()

        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(background)
        painter.drawRect(self.rect())

        # The upstream qframelesswindow glyphs use fixed 46 x 32 coordinates.
        # Derive all geometry from this button instead so application scaling
        # cannot pull the mark away from its visual centre.
        factor = self.width() / 46.0
        icon_side = 10.0 * factor
        center = QRectF(self.rect()).center()
        left = center.x() - icon_side / 2.0
        top = center.y() - icon_side / 2.0

        pen = QPen(color, max(1.0, factor))
        pen.setCapStyle(Qt.PenCapStyle.SquareCap)
        pen.setJoinStyle(Qt.PenJoinStyle.MiterJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)

        if self.kind == self.MINIMIZE:
            y = center.y() + 2.0 * factor
            painter.drawLine(QPointF(left, y), QPointF(left + icon_side, y))
        elif self.kind == self.CLOSE:
            painter.drawLine(
                QPointF(left, top), QPointF(left + icon_side, top + icon_side)
            )
            painter.drawLine(
                QPointF(left + icon_side, top), QPointF(left, top + icon_side)
            )
        elif not self._is_maximized:
            painter.drawRect(QRectF(left, top, icon_side, icon_side))
        else:
            offset = 2.0 * factor
            restore_side = icon_side - offset
            painter.drawRect(QRectF(left, top + offset, restore_side, restore_side))
            painter.drawRect(QRectF(left + offset, top, restore_side, restore_side))


class FluentTitleBar(TitleBar):
    def __init__(self, parent):
        super().__init__(parent)
        self.setObjectName("fluentTitleBar")

        # Replace qframelesswindow's fixed-coordinate glyphs once here rather
        # than letting individual windows implement their own caption buttons.
        for button in (self.minBtn, self.maxBtn, self.closeBtn):
            self.hBoxLayout.removeWidget(button)
            button.hide()
            button.deleteLater()
        self.minBtn = _FluentCaptionButton(_FluentCaptionButton.MINIMIZE, self)
        self.maxBtn = _FluentCaptionButton(_FluentCaptionButton.MAXIMIZE, self)
        self.closeBtn = _FluentCaptionButton(_FluentCaptionButton.CLOSE, self)
        self.minBtn.clicked.connect(self.window().showMinimized)
        self.maxBtn.clicked.connect(lambda: toggleMaxState(self.window()))
        self.closeBtn.clicked.connect(self.window().close)
        self.hBoxLayout.addWidget(self.minBtn)
        self.hBoxLayout.addWidget(self.maxBtn)
        self.hBoxLayout.addWidget(self.closeBtn)

        self.setFixedHeight(32)
        self.buttonLayout = self.hBoxLayout
        self.iconLabel = QLabel(self)
        self.iconLabel.setFixedSize(22, 22)
        self.iconLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.titleLabel = QLabel(parent.windowTitle(), self)
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)
        self.hBoxLayout.insertWidget(0, self.iconLabel)
        self.hBoxLayout.insertWidget(1, self.titleLabel)
        parent.windowTitleChanged.connect(self.setTitle)
        parent.windowIconChanged.connect(self.setIcon)
        # Scope transparency to the title-bar container.  An unqualified
        # declaration cascades to child widgets and erases backgrounds such as
        # the translation window's logo, status badge and checked pin button.
        self.setStyleSheet(
            "QWidget#fluentTitleBar { background: transparent; }"
        )

    def _apply_theme(self, _tokens=None):
        tokens = ui_tokens(self)
        self.setFixedHeight(_px(self, 32))
        self.iconLabel.setFixedSize(_px(self, 22), _px(self, 22))
        for button in (self.minBtn, self.maxBtn, self.closeBtn):
            button.setFixedSize(_px(self, 46), _px(self, 32))
        self.titleLabel.setStyleSheet(
            f"color: {tokens.text}; font: {_px(self, 12)}px {FONT_FAMILY}; "
            "background: transparent;"
        )
        self._apply_caption_colors()
        self._place_centered_title()

    def _apply_caption_colors(self):
        """Caption-button colours, as a single overridable source.

        Windows with a palette of their own override this rather than
        re-colouring the buttons from outside: _apply_theme is connected to
        theme_changed, so anything applied from outside is painted back over.
        Overrides run before the subclass constructor finishes, so they must
        not depend on state that constructor sets up.
        """
        tokens = ui_tokens(self)
        for button in (self.minBtn, self.maxBtn, self.closeBtn):
            button.setNormalColor(QColor(tokens.text_muted))
            button.setHoverColor(QColor(tokens.text))
            button.setPressedColor(QColor(tokens.text))
            button.setNormalBackgroundColor(QColor(0, 0, 0, 0))
            button.setHoverBackgroundColor(QColor(tokens.surface_hover))
            button.setPressedBackgroundColor(QColor(tokens.accent_soft))
        self.closeBtn.setHoverColor(QColor("#FFFFFF"))
        self.closeBtn.setHoverBackgroundColor(QColor("#C42B1C"))

    def setTitle(self, title):
        self.titleLabel.setText(str(title))
        self._place_centered_title()

    def setIcon(self, icon):
        side = _px(self, 16)
        self.iconLabel.setPixmap(icon.pixmap(side, side))
        self._place_centered_title()

    def center_title(self):
        """图标和标题改为在整条标题栏里居中，不再贴左。"""
        self.hBoxLayout.removeWidget(self.iconLabel)
        self.hBoxLayout.removeWidget(self.titleLabel)
        self._title_centered = True
        self._place_centered_title()

    def _place_centered_title(self):
        if not getattr(self, "_title_centered", False):
            return
        self.titleLabel.adjustSize()
        gap = _px(self, 6)
        icon_w = self.iconLabel.width() + gap if self.iconLabel.pixmap() and not self.iconLabel.pixmap().isNull() else 0
        left = (self.width() - icon_w - self.titleLabel.width()) // 2
        if icon_w:
            self.iconLabel.move(left, (self.height() - self.iconLabel.height()) // 2)
        self.titleLabel.move(left + icon_w, (self.height() - self.titleLabel.height()) // 2)

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._place_centered_title()
