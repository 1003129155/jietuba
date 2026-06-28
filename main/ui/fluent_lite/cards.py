"""Settings cards and groups implemented with native Qt layouts."""

from __future__ import annotations

from PySide6.QtCore import QSize, Qt, Signal
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QVBoxLayout, QWidget

from .labels import BodyLabel, CaptionLabel
from .switch import SwitchButton
from .theme import BORDER, FONT_FAMILY, SURFACE, SURFACE_HOVER, TEXT, TEXT_MUTED, to_qicon


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
        self.setObjectName("FluentLiteSettingCard")
        self.setMinimumHeight(62)
        self.setStyleSheet(f"""
            QFrame#FluentLiteSettingCard {{ background: transparent; border: none; border-radius: 12px; }}
            QFrame#FluentLiteSettingCard:hover {{ background: rgba(255,255,255,.44); border: none; }}
        """)
        self.hBoxLayout = QHBoxLayout(self)
        self.hBoxLayout.setContentsMargins(15, 9, 15, 9)
        self.hBoxLayout.setSpacing(13)

        self.iconLabel = QLabel(self)
        self.iconLabel.setFixedSize(24, 24)
        self.iconLabel.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.iconLabel.setPixmap(to_qicon(icon).pixmap(20, 20))
        self.iconLabel.setStyleSheet("background: transparent; border: none;")
        self.hBoxLayout.addWidget(self.iconLabel)

        self._text_layout = QVBoxLayout()
        self._text_layout.setContentsMargins(0, 0, 0, 0)
        self._text_layout.setSpacing(3)
        self.titleLabel = BodyLabel(str(title), self)
        self.titleLabel.setStyleSheet(f"color: {TEXT}; font: 600 13px {FONT_FAMILY}; background: transparent; border: none;")
        self._text_layout.addWidget(self.titleLabel)
        self.contentLabel = CaptionLabel("" if content is None else str(content), self)
        self.contentLabel.setWordWrap(True)
        self.contentLabel.setStyleSheet(f"color: {TEXT_MUTED}; font: 12px {FONT_FAMILY}; background: transparent; border: none;")
        self.contentLabel.setVisible(bool(content))
        self._text_layout.addWidget(self.contentLabel)
        self.hBoxLayout.addLayout(self._text_layout, 1)
        self.hBoxLayout.addStretch()

    def setTitle(self, title):
        self.titleLabel.setText(str(title))

    def setContent(self, content):
        self.contentLabel.setText("" if content is None else str(content))
        self.contentLabel.setVisible(bool(content))

    def setValue(self, value):
        self.setContent(value)

    def minimumSizeHint(self):
        return QSize(180, max(self.minimumHeight(), self.hBoxLayout.minimumSize().height()))


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
        self.vBoxLayout.setSpacing(10)
        self.titleLabel = QLabel(str(title), self)
        self.titleLabel.setStyleSheet(
            f"color: #000000; font: 600 12px {FONT_FAMILY}; padding: 2px 8px 0 8px; background: transparent;"
        )
        self.vBoxLayout.addWidget(self.titleLabel)
        self._card_container = QFrame(self)
        self._card_container.setObjectName("FluentLiteGroupBody")
        self._card_container.setStyleSheet("""
            QFrame#FluentLiteGroupBody {
                background: rgba(255, 255, 255, 0.62);
                border: 1px solid rgba(255, 255, 255, 0.88);
                border-radius: 15px;
            }
        """)
        self.cardLayout = ExpandLayout(self._card_container)
        self.cardLayout.setContentsMargins(2, 2, 2, 2)
        self.cardLayout.setSpacing(0)
        self.vBoxLayout.addWidget(self._card_container)
        self._cards = []

    def addSettingCard(self, card):
        if self._cards:
            separator = QFrame(self._card_container)
            separator.setFixedHeight(1)
            separator.setStyleSheet(
                "background: rgba(98, 116, 105, 0.13); border: none; margin-left: 52px;"
            )
            self.cardLayout.addWidget(separator)
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
        self.setStyleSheet(
            f"QFrame#FluentLiteSimpleCard {{ background: {SURFACE}; border: 1px solid rgba(255,255,255,.86); border-radius: 14px; }}"
        )
