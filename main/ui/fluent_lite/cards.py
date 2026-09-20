"""Settings cards and groups implemented with native Qt layouts."""

from __future__ import annotations

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QVBoxLayout, QWidget

from core.ui_scale import dialog_scaled
from core.ui_theme import get_ui_theme

from .labels import BodyLabel, CaptionLabel
from .switch import SwitchButton
from .theme import FONT_FAMILY, to_qicon, ui_tokens


class ExpandLayout(QVBoxLayout):
    """Small compatibility layout with a useful heightForWidth result."""

    def heightForWidth(self, width):
        margins = self.contentsMargins()
        available_width = max(0, width - margins.left() - margins.right())
        height = margins.top() + margins.bottom()
        visible = 0
        for index in range(self.count()):
            item = self.itemAt(index)
            widget = item.widget()
            if widget is not None and not widget.isHidden():
                # Setting cards contain word-wrapped descriptions.  Their
                # width-independent sizeHint may assume a very narrow text
                # column and report several phantom lines, making the whole
                # group much taller than its contents at the real width.
                preferred_height = (
                    widget.heightForWidth(available_width)
                    if widget.hasHeightForWidth()
                    else widget.sizeHint().height()
                )
                height += max(widget.minimumHeight(), preferred_height)
                visible += 1
        return height + max(0, visible - 1) * self.spacing()


class SettingCard(QFrame):
    def __init__(self, icon, title, content=None, parent=None):
        super().__init__(parent)
        self._theme_icon = icon
        self.setObjectName("FluentLiteSettingCard")
        self.setMinimumHeight(dialog_scaled(62))
        self.hBoxLayout = QHBoxLayout(self)
        self.hBoxLayout.setContentsMargins(
            dialog_scaled(15), dialog_scaled(9), dialog_scaled(15), dialog_scaled(9)
        )
        self.hBoxLayout.setSpacing(dialog_scaled(13))

        self.iconLabel = QLabel(self)
        self.iconLabel.setFixedSize(dialog_scaled(24), dialog_scaled(24))
        self.iconLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.iconLabel.setPixmap(to_qicon(icon).pixmap(dialog_scaled(20), dialog_scaled(20)))
        self.iconLabel.setStyleSheet("background: transparent; border: none;")
        self.hBoxLayout.addWidget(self.iconLabel)

        self._text_layout = QVBoxLayout()
        self._text_layout.setContentsMargins(0, 0, 0, 0)
        self._text_layout.setSpacing(dialog_scaled(3))
        self.titleLabel = BodyLabel(str(title), self)
        self._text_layout.addWidget(self.titleLabel)
        self.contentLabel = CaptionLabel("" if content is None else str(content), self)
        self.contentLabel.setWordWrap(True)
        self.contentLabel.setVisible(bool(content))
        self._text_layout.addWidget(self.contentLabel)
        self.hBoxLayout.addLayout(self._text_layout, 1)
        self.hBoxLayout.addStretch()
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        t = ui_tokens(self)
        self.setStyleSheet(f"""
            QFrame#FluentLiteSettingCard {{ background: transparent; border: none; border-radius: {dialog_scaled(12)}px; }}
            QFrame#FluentLiteSettingCard:hover {{ background: {t.surface_subtle}; border: none; }}
        """)
        self.iconLabel.setPixmap(
            to_qicon(self._theme_icon, self).pixmap(dialog_scaled(20), dialog_scaled(20))
        )
        self.titleLabel.setStyleSheet(
            f"color: {t.text}; font: 600 {dialog_scaled(13)}px {FONT_FAMILY}; "
            "background: transparent; border: none;"
        )
        self.contentLabel.setStyleSheet(
            f"color: {t.text_muted}; font: {dialog_scaled(12)}px {FONT_FAMILY}; "
            "background: transparent; border: none;"
        )

    def setTitle(self, title):
        self.titleLabel.setText(str(title))

    def setContent(self, content):
        self.contentLabel.setText("" if content is None else str(content))
        self.contentLabel.setVisible(bool(content))

    def setValue(self, value):
        self.setContent(value)

    def minimumSizeHint(self):
        return QSize(dialog_scaled(180), max(self.minimumHeight(), self.hBoxLayout.minimumSize().height()))


class SwitchSettingCard(SettingCard):
    checkedChanged = Signal(bool)

    def __init__(self, icon, title, content=None, configItem=None, parent=None):
        super().__init__(icon, title, content, parent)
        self.switchButton = SwitchButton(self)
        self.switchButton.checkedChanged.connect(self._on_checked_changed)
        self.hBoxLayout.addWidget(self.switchButton, 0, Qt.AlignmentFlag.AlignVCenter)

    def _on_checked_changed(self, checked):
        self.checkedChanged.emit(checked)

    def setValue(self, value):
        self.setChecked(value)

    def setChecked(self, checked):
        self.switchButton.setChecked(checked)

    def isChecked(self):
        return self.switchButton.isChecked()


class SettingCardGroup(QWidget):
    def __init__(self, title, parent=None):
        super().__init__(parent)
        self.setObjectName("FluentLiteSettingCardGroup")
        self.vBoxLayout = QVBoxLayout(self)
        self.vBoxLayout.setContentsMargins(0, 0, 0, 0)
        self.vBoxLayout.setSpacing(dialog_scaled(10))
        self.titleLabel = QLabel(str(title), self)
        self.vBoxLayout.addWidget(self.titleLabel)
        self._card_container = QFrame(self)
        self._card_container.setObjectName("FluentLiteGroupBody")
        self.cardLayout = ExpandLayout(self._card_container)
        self.cardLayout.setContentsMargins(
            dialog_scaled(2), dialog_scaled(2), dialog_scaled(2), dialog_scaled(2)
        )
        self.cardLayout.setSpacing(0)
        self.vBoxLayout.addWidget(self._card_container)
        self._cards = []
        self._separators = []
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        t = ui_tokens(self)
        self.titleLabel.setStyleSheet(
            f"color: {t.text}; font: 600 {dialog_scaled(12)}px {FONT_FAMILY}; "
            f"padding: {dialog_scaled(2)}px {dialog_scaled(8)}px 0 {dialog_scaled(8)}px; background: transparent;"
        )
        self._card_container.setStyleSheet(f"""
            QFrame#FluentLiteGroupBody {{
                background: {t.surface};
                border: 1px solid {t.border};
                border-radius: {dialog_scaled(15)}px;
            }}
        """)
        for separator in self._separators:
            separator.setStyleSheet(
                f"background: {t.separator}; border: none; margin-left: {dialog_scaled(52)}px;"
            )

    def addSettingCard(self, card):
        if self._cards:
            separator = QFrame(self._card_container)
            separator.setFixedHeight(dialog_scaled(1))
            separator.setStyleSheet(
                f"background: {ui_tokens(self).separator}; border: none; margin-left: {dialog_scaled(52)}px;"
            )
            self.cardLayout.addWidget(separator)
            self._separators.append(separator)
        card.setParent(self._card_container)
        self.cardLayout.addWidget(card)
        self._cards.append(card)
        self.adjustSize()

    def addSettingCards(self, cards):
        for card in cards:
            self.addSettingCard(card)

    def adjustSize(self):
        self.setMinimumHeight(self.vBoxLayout.sizeHint().height())
        self.updateGeometry()

    def minimumSizeHint(self):
        # Card contents may contain long translated text and fixed-size controls.
        # Let a QScrollArea compress the group horizontally instead of creating
        # a distracting horizontal scrollbar.
        return QSize(0, self.vBoxLayout.minimumSize().height())

    def showEvent(self, event):
        super().showEvent(event)
        self.adjustSize()


class SimpleCardWidget(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("FluentLiteSimpleCard")
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        t = ui_tokens(self)
        self.setStyleSheet(
            f"QFrame#FluentLiteSimpleCard {{ background: {t.surface}; "
            f"border: 1px solid {t.border}; border-radius: 14px; }}"
        )
