# -*- coding: utf-8 -*-
"""DeepL translation window with the FlashTrans dashboard UI."""

from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtCore import QEvent, QObject, QPoint, QRectF, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QCursor,
    QFont,
    QKeyEvent,
    QLinearGradient,
    QPainter,
    QPalette,
    QPen,
)
from PySide6.QtWidgets import (
    QAbstractButton,
    QApplication,
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)
from qframelesswindow import FramelessWindow

from core import log_debug, log_info
from core.i18n import make_tr
from core.ui_scale import configure_dialog_control, dialog_scaled, get_dialog_scale
from .ui.widgets import EllipsisAnimator
from core.resource_manager import ResourceManager
from settings import get_tool_settings_manager
from ui.fluent_lite import FluentTitleBar, TextEdit
from .languages import TRANSLATION_LANGUAGES


_tr = make_tr("TranslationDialog")


# Keep rectangular surfaces restrained.  Pills and circular controls use their
# own half-height radii below; applying those large radii to panes and popup
# surfaces makes Qt's stylesheet corners look swollen, especially when the
# standalone-window scale is above 100%.
WINDOW_CORNER_RADIUS = 8
SURFACE_CORNER_RADIUS = 8
CONTROL_CORNER_RADIUS = 6
# 服务名标签的最大宽度（未缩放）。自定义服务的名字是用户起的，不设上限会把
# 标题栏或底栏挤变形。
BADGE_MAX_WIDTH = 220


def set_badge_text(badge: QLabel, text: str, padding: int) -> None:
    """超出 BADGE_MAX_WIDTH 时截断成省略号，完整名字放到悬停提示里。

    padding 是样式表里标签单侧的水平内边距（未缩放）。
    """
    max_width = dialog_scaled(BADGE_MAX_WIDTH)
    badge.setMaximumWidth(max_width)
    # 字号和粗细来自样式表，先 polish 才能量得准
    badge.ensurePolished()
    shown = badge.fontMetrics().elidedText(
        text, Qt.TextElideMode.ElideRight, max_width - 2 * dialog_scaled(padding)
    )
    badge.setText(shown)
    badge.setToolTip(text if shown != text else "")


@dataclass(frozen=True)
class Palette:
    window: str
    field: str
    surface_strong: str
    text: str
    text_2: str
    text_3: str
    accent: str
    accent_hover: str
    accent_tint: str
    fill: str
    fill_hover: str
    fill_color: QColor
    fill_hover_color: QColor
    danger: str
    green: str


DARK = Palette(
    window="#45474e",
    field="#2c2d33",
    surface_strong="#555860",
    text="#f4f6f9",
    text_2="#9aa1ae",
    text_3="#686d79",
    accent="#3f8cff",
    accent_hover="#62a2ff",
    accent_tint="rgba(63, 140, 255, 0.16)",
    fill="rgba(125, 132, 148, 0.22)",
    fill_hover="rgba(125, 132, 148, 0.34)",
    fill_color=QColor(125, 132, 148, 56),
    fill_hover_color=QColor(125, 132, 148, 87),
    danger="#ff5449",
    green="#35d06a",
)

LIGHT = Palette(
    window="#f5f5f7",
    field="#ffffff",
    surface_strong="#ffffff",
    text="#1d1d1f",
    text_2="#73737a",
    text_3="#9999a1",
    accent="#007aff",
    accent_hover="#218cff",
    accent_tint="rgba(0, 122, 255, 0.12)",
    fill="rgba(120, 120, 128, 0.15)",
    fill_hover="rgba(120, 120, 128, 0.25)",
    fill_color=QColor(120, 120, 128, 38),
    fill_hover_color=QColor(120, 120, 128, 64),
    danger="#ff3b30",
    green="#34c759",
)


class VectorToolButton(QAbstractButton):
    """Crisp copy/trash button drawn by Qt instead of a font glyph."""

    def __init__(self, icon_name: str, tooltip: str, parent: QWidget | None = None):
        super().__init__(parent)
        self._icon_name = icon_name
        self._palette = DARK
        self.setToolTip(tooltip)
        self.setAccessibleName(tooltip)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedSize(dialog_scaled(32), dialog_scaled(30))

    def set_tooltip(self, tooltip: str) -> None:
        self.setToolTip(tooltip)
        self.setAccessibleName(tooltip)

    def apply_palette(self, palette: Palette) -> None:
        self._palette = palette
        self.update()

    def enterEvent(self, event) -> None:
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        self.update()
        super().leaveEvent(event)

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        if self.isDown():
            painter.setBrush(self._palette.fill_hover_color)
        elif self.underMouse():
            painter.setBrush(self._palette.fill_color)
        else:
            painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(
            QRectF(self.rect()).adjusted(
                dialog_scaled(2), dialog_scaled(2),
                -dialog_scaled(2), -dialog_scaled(2),
            ),
            dialog_scaled(9), dialog_scaled(9),
        )

        color = QColor(self._palette.text if self.underMouse() else self._palette.text_2)
        pen = QPen(color, 1.8)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)

        # 图标是一组互相咬合的手绘坐标，整体 scale() 而不是逐点取整，
        # 否则各点独立四舍五入会让复制/清空图标的相对比例跑偏。
        painter.save()
        painter.scale(get_dialog_scale().factor, get_dialog_scale().factor)
        if self._icon_name == "copy":
            painter.drawRoundedRect(QRectF(7.5, 6.5, 11, 12), 2, 2)
            painter.drawRoundedRect(QRectF(11.5, 10.5, 11, 12), 2, 2)
        else:
            painter.drawLine(8.5, 9.0, 21.5, 9.0)
            painter.drawLine(12.0, 6.0, 18.0, 6.0)
            painter.drawRoundedRect(QRectF(10.0, 11.0, 10.0, 12.0), 2, 2)
            painter.drawLine(13.5, 14.0, 13.5, 20.0)
            painter.drawLine(16.5, 14.0, 16.5, 20.0)
        painter.restore()


class TranslateButton(QAbstractButton):
    """Dashboard translation action."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._palette = DARK
        self._idle_text = _tr("Translate")
        self.setText(self._idle_text)
        self.setToolTip(_tr("Translate text"))
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedSize(dialog_scaled(96), dialog_scaled(34))

    def retranslate(self, busy: bool) -> None:
        self._idle_text = _tr("Translate")
        self.setText(_tr("Translating...") if busy else self._idle_text)
        self.setToolTip(_tr("Translate text"))
        self.setAccessibleName(_tr("Translate text"))
        self.update()

    def apply_palette(self, palette: Palette) -> None:
        self._palette = palette
        self.update()

    def enterEvent(self, event) -> None:
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        self.update()
        super().leaveEvent(event)

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        color = self._palette.accent_hover if self.underMouse() else self._palette.accent
        if not self.isEnabled():
            color = "#6689bb"
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(color))
        painter.drawRoundedRect(QRectF(self.rect()), dialog_scaled(17), dialog_scaled(17))

        painter.setPen(QColor("white"))
        painter.setFont(QFont("Microsoft YaHei UI", dialog_scaled(10), QFont.Weight.DemiBold))
        painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, self.text())


class PinIconButton(QAbstractButton):
    """Background-free always-on-top toggle using the pin SVG asset."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._palette = DARK
        self.icon_path = ResourceManager.get_icon_path("钉图.svg")
        self._icon = ResourceManager.get_icon(self.icon_path)
        self.setAccessibleName(_tr("Toggle always on top"))
        self.setCheckable(True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedSize(dialog_scaled(32), dialog_scaled(32))

    def apply_palette(self, palette: Palette) -> None:
        self._palette = palette
        self.update()

    def enterEvent(self, event) -> None:
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        self.update()
        super().leaveEvent(event)

    def paintEvent(self, event) -> None:
        del event
        side = dialog_scaled(20)
        pixmap = self._icon.pixmap(side, side)
        if pixmap.isNull():
            return

        if self.isChecked():
            color = self._palette.accent
        elif self.underMouse():
            color = self._palette.text
        else:
            color = self._palette.text_2

        icon_painter = QPainter(pixmap)
        icon_painter.setCompositionMode(
            QPainter.CompositionMode.CompositionMode_SourceIn
        )
        icon_painter.fillRect(pixmap.rect(), QColor(color))
        icon_painter.end()

        painter = QPainter(self)
        x = (self.width() - pixmap.width()) // 2
        y = (self.height() - pixmap.height()) // 2
        painter.drawPixmap(x, y, pixmap)


class DashboardTitleBar(FluentTitleBar):
    """Shared Fluent title bar plus the always-on-top control."""

    # The base constructor reaches _apply_caption_colors before the instance
    # has been handed a palette.
    _palette = DARK

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.hBoxLayout.setContentsMargins(dialog_scaled(12), 0, 0, 0)
        self.hBoxLayout.setSpacing(0)

        self.logo = self.iconLabel
        self.logo.setObjectName("appLogo")
        self.logo.setAccessibleName(_tr("Translation"))
        logo_icon = ResourceManager.get_icon(
            ResourceManager.get_icon_path("翻译.svg"), dialog_scaled(22)
        )
        if logo_icon.isNull():
            self.logo.setText(_tr("Translation"))
        else:
            logo_pixmap = logo_icon.pixmap(dialog_scaled(16), dialog_scaled(16))
            logo_painter = QPainter(logo_pixmap)
            logo_painter.setCompositionMode(
                QPainter.CompositionMode.CompositionMode_SourceIn
            )
            logo_painter.fillRect(logo_pixmap.rect(), QColor("white"))
            logo_painter.end()
            self.logo.setPixmap(logo_pixmap)

        self.app_name = self.titleLabel
        self.app_name.setText("jietuba")
        self.app_name.setObjectName("appName")

        self.pin_button = PinIconButton(self)
        self.pin_button.setObjectName("pinButton")
        self.update_translations()

        controls_index = self.hBoxLayout.indexOf(self.minBtn)
        self.hBoxLayout.insertWidget(controls_index, self.pin_button)
        self.hBoxLayout.insertSpacing(controls_index + 1, dialog_scaled(8))

    def update_translations(self) -> None:
        self.pin_button.setAccessibleName(_tr("Toggle always on top"))
        # Caption symbols are universally understood; keep their names for
        # accessibility without showing redundant hover bubbles.
        for button, name in (
            (self.minBtn, _tr("Minimize")),
            (self.maxBtn, _tr("Maximize")),
            (self.closeBtn, _tr("Close")),
        ):
            button.setToolTip("")
            button.setAccessibleName(name)

    def apply_palette(self, palette: Palette) -> None:
        self._palette = palette
        self.pin_button.apply_palette(palette)
        self._apply_caption_colors()

    def _apply_caption_colors(self):
        palette = self._palette
        for button in (self.minBtn, self.maxBtn, self.closeBtn):
            button.setNormalColor(QColor(palette.text_2))
            button.setHoverColor(QColor(palette.text))
            button.setPressedColor(QColor(palette.text))
            button.setNormalBackgroundColor(QColor(0, 0, 0, 0))
            button.setHoverBackgroundColor(palette.fill_hover_color)
            button.setPressedBackgroundColor(palette.fill_color)
        self.closeBtn.setHoverColor(QColor("white"))
        self.closeBtn.setHoverBackgroundColor(QColor(palette.danger))


class SourceEdit(TextEdit):
    translate_shortcut = Signal()

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter) and (
            event.modifiers() & Qt.KeyboardModifier.ControlModifier
        ):
            self.translate_shortcut.emit()
            return
        super().keyPressEvent(event)


class FocusPane(QFrame):
    """Add the focus-within state that QSS does not provide."""

    def watch_focus(self, child: QWidget) -> None:
        child.installEventFilter(self)

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:
        if event.type() in (QEvent.Type.FocusIn, QEvent.Type.FocusOut):
            self.setProperty("focused", event.type() == QEvent.Type.FocusIn)
            self.style().unpolish(self)
            self.style().polish(self)
        return super().eventFilter(watched, event)


class TranslationDialog(FramelessWindow):
    """Translation window retaining the existing manager-facing API."""

    translate_requested = Signal(str, str, str)
    _stay_on_top = True
    MINIMUM_WIDTH = 860
    MINIMUM_HEIGHT = 560
    DEFAULT_WIDTH = 1000
    DEFAULT_HEIGHT = 700

    def __init__(
        self,
        original_text: str = "",
        translated_text: str = "",
        parent: QWidget | None = None,
        position: QPoint | None = None,
        source_lang: str = "auto",
        target_lang: str = "ZH",
    ):
        super().__init__(parent)
        self.original_text = original_text
        self.translated_text = translated_text
        self.source_lang = source_lang or "auto"
        self._detected_source_lang = ""
        self._theme_name = "dark"
        self._palette = DARK
        self._tool_buttons: list[VectorToolButton] = []

        config = get_tool_settings_manager()
        saved_target_lang = config.get_app_setting("translation_target_lang", "")
        self.target_lang = saved_target_lang or target_lang or "ZH"
        self._is_on_top = TranslationDialog._stay_on_top

        self.setObjectName("dashboardWindow")
        self.setWindowTitle("jietuba")
        self.setMinimumSize(dialog_scaled(self.MINIMUM_WIDTH), dialog_scaled(self.MINIMUM_HEIGHT))
        self.resize(dialog_scaled(self.DEFAULT_WIDTH), dialog_scaled(self.DEFAULT_HEIGHT))
        self.setFont(QFont("Microsoft YaHei UI", dialog_scaled(10)))
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAutoFillBackground(False)
        self.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, self._is_on_top)
        # 改 WindowFlags 会重建原生窗口，无边框库加的 WS_THICKFRAME 和阴影随之丢失，
        # 边缘就拖不动了，要重新加回去
        self.updateFrameless()

        self.dashboard_title_bar = DashboardTitleBar(self)
        configure_dialog_control(self.dashboard_title_bar)
        self.setTitleBar(self.dashboard_title_bar)
        self.dashboard_title_bar.pin_button.setChecked(self._is_on_top)
        self.dashboard_title_bar.pin_button.clicked.connect(self._on_toggle_pin)
        self._update_pin_tooltip()

        self._build_ui()
        self.set_theme("dark")

        from core.i18n import I18nManager
        I18nManager.instance().language_changed.connect(self._on_language_changed)

        # TranslationManager supplies the selected provider and readiness.
        self.set_backend_badge(_tr("Engine not configured"), False)
        self._retranslate_ui()

        self._place_initial_window(position)

        log_debug("Translation dashboard created", "Translation")

    @classmethod
    def initial_size_for_available_geometry(cls, available_geometry) -> tuple[int, int]:
        """Return the initial size, reduced to the screen when possible."""
        minimum_width = dialog_scaled(cls.MINIMUM_WIDTH)
        minimum_height = dialog_scaled(cls.MINIMUM_HEIGHT)
        default_width = dialog_scaled(cls.DEFAULT_WIDTH)
        default_height = dialog_scaled(cls.DEFAULT_HEIGHT)
        width = max(minimum_width, min(default_width, available_geometry.width()))
        height = max(minimum_height, min(default_height, available_geometry.height()))
        return width, height

    def _place_initial_window(self, position: QPoint | None) -> None:
        """Place a new window on the requested screen or center it by the cursor."""
        reference = position if position is not None else QCursor.pos()
        screen = QApplication.screenAt(reference) or QApplication.primaryScreen()
        if screen is None:
            if position is not None:
                self.move(position)
            return

        available = screen.availableGeometry()
        width, height = self.initial_size_for_available_geometry(available)
        self.resize(width, height)

        if position is None:
            desired_x = available.left() + (available.width() - width) // 2
            desired_y = available.top() + (available.height() - height) // 2
        else:
            desired_x = position.x()
            desired_y = position.y()

        max_x = max(available.left(), available.left() + available.width() - width)
        max_y = max(available.top(), available.top() + available.height() - height)
        final_x = min(max(desired_x, available.left()), max_x)
        final_y = min(max(desired_y, available.top()), max_y)
        self.move(final_x, final_y)

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(
            dialog_scaled(18), self.dashboard_title_bar.height() + dialog_scaled(6),
            dialog_scaled(18), dialog_scaled(15),
        )
        root.setSpacing(0)

        language_bar = QHBoxLayout()
        language_bar.setContentsMargins(0, dialog_scaled(2), 0, dialog_scaled(14))
        language_bar.setSpacing(dialog_scaled(12))
        language_bar.addStretch()

        self.source_language = self._language_combo(include_auto=True)
        self.target_language = self._language_combo(include_auto=False)
        # Compatibility aliases used by the existing translation code.
        self.source_lang_combo = self.source_language
        self.target_lang_combo = self.target_language

        self._set_combo_data(self.source_language, self.source_lang, "auto")
        self._set_combo_data(self.target_language, self.target_lang, "ZH")
        self.target_language.currentIndexChanged.connect(self._on_target_lang_changed)

        self.swap_button = QPushButton("⇄")
        self.swap_button.setObjectName("swapButton")
        self.swap_button.setToolTip(_tr("Swap languages"))
        self.swap_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.swap_button.setFixedSize(dialog_scaled(36), dialog_scaled(36))
        self.swap_button.clicked.connect(self._swap_languages)

        language_bar.addWidget(self.source_language)
        language_bar.addWidget(self.swap_button)
        language_bar.addWidget(self.target_language)
        language_bar.addStretch()
        root.addLayout(language_bar)

        panes = QHBoxLayout()
        panes.setSpacing(dialog_scaled(13))

        self.source_pane = FocusPane()
        self.source_pane.setObjectName("sourcePane")
        source_layout = QVBoxLayout(self.source_pane)
        source_layout.setContentsMargins(
            dialog_scaled(10), dialog_scaled(10),
            dialog_scaled(10), dialog_scaled(8),
        )
        source_layout.setSpacing(dialog_scaled(2))

        self.source_edit = SourceEdit()
        self.source_edit.setObjectName("sourceEdit")
        self.source_edit.setAcceptRichText(False)
        self.source_edit.setPlaceholderText(self._source_placeholder())
        self.source_edit.setPlainText(self.original_text)
        self.source_edit.translate_shortcut.connect(self._request_translation)
        self.source_edit.textChanged.connect(self._update_count)
        self.source_pane.watch_focus(self.source_edit)
        source_layout.addWidget(self.source_edit, 1)

        source_tools = QHBoxLayout()
        source_tools.setSpacing(dialog_scaled(3))
        self.copy_source_button = self._tool_button("copy", _tr("Copy Original"))
        self.copy_source_button.clicked.connect(self._copy_source)
        self.clear_source_button = self._tool_button("trash", _tr("Clear Original"))
        self.clear_source_button.clicked.connect(self.source_edit.clear)
        self.character_count = QLabel("")
        self.character_count.setObjectName("characterCount")
        source_tools.addWidget(self.copy_source_button)
        source_tools.addWidget(self.clear_source_button)
        source_tools.addStretch()
        source_tools.addWidget(self.character_count)
        source_layout.addLayout(source_tools)

        self.target_pane = QFrame()
        self.target_pane.setObjectName("targetPane")
        target_layout = QVBoxLayout(self.target_pane)
        target_layout.setContentsMargins(
            dialog_scaled(10), dialog_scaled(10),
            dialog_scaled(10), dialog_scaled(8),
        )
        target_layout.setSpacing(dialog_scaled(2))

        self.target_edit = TextEdit()
        self.target_edit.setObjectName("targetEdit")
        self.target_edit.setAcceptRichText(False)
        self.target_edit.setReadOnly(True)
        self.target_edit.setPlaceholderText(_tr("Translation will appear here..."))
        self.target_edit.setPlainText(self.translated_text)
        self._loading_dots = EllipsisAnimator(
            self, on_tick=self.target_edit.setPlainText
        )
        target_layout.addWidget(self.target_edit, 1)

        target_tools = QHBoxLayout()
        target_tools.setSpacing(dialog_scaled(3))
        self.copy_target_button = self._tool_button("copy", _tr("Copy Translation"))
        self.copy_target_button.clicked.connect(self._copy_target)
        target_tools.addWidget(self.copy_target_button)
        target_tools.addStretch()
        target_layout.addLayout(target_tools)

        panes.addWidget(self.source_pane, 1)
        panes.addWidget(self.target_pane, 1)
        root.addLayout(panes, 1)

        footer = QHBoxLayout()
        footer.setContentsMargins(dialog_scaled(2), dialog_scaled(13), dialog_scaled(2), 0)
        self.backend_badge = QLabel(_tr("Engine not configured"), self)
        self.backend_badge.setObjectName("backendBadge")
        self.backend_badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.backend_badge.setFixedHeight(dialog_scaled(22))
        footer.addWidget(self.backend_badge)
        footer.addStretch()
        self.translate_button = TranslateButton()
        self.translate_btn = self.translate_button
        self.translate_button.clicked.connect(self._request_translation)
        footer.addWidget(self.translate_button)
        root.addLayout(footer)

        self._update_count()

    def _tool_button(self, icon_name: str, tooltip: str) -> VectorToolButton:
        button = VectorToolButton(icon_name, tooltip)
        self._tool_buttons.append(button)
        return button

    @staticmethod
    def _set_combo_data(combo: QComboBox, value: str, fallback: str) -> None:
        index = combo.findData(value)
        if index < 0:
            index = combo.findData(fallback)
        if index >= 0:
            combo.setCurrentIndex(index)

    @staticmethod
    def _language_combo(include_auto: bool) -> QComboBox:
        combo = QComboBox()
        combo.setObjectName("languageCombo")
        if include_auto:
            combo.addItem(_tr("Auto Detect"), "auto")
        for code, name in TRANSLATION_LANGUAGES.items():
            combo.addItem(name, code)
        combo.setFixedWidth(dialog_scaled(150))
        combo.setFixedHeight(dialog_scaled(36))
        return combo

    def _swap_languages(self) -> None:
        source_code = str(self.source_language.currentData())
        target_code = str(self.target_language.currentData())
        if source_code == "auto":
            source_code = self._detected_source_lang
        if not source_code:
            return

        source_index = self.source_language.findData(target_code)
        target_index = self.target_language.findData(source_code)
        if source_index < 0 or target_index < 0:
            return

        self.source_language.setCurrentIndex(source_index)
        self.target_language.setCurrentIndex(target_index)

        source_text = self.source_edit.toPlainText()
        target_text = self.target_edit.toPlainText()
        if target_text:
            self.source_edit.setPlainText(target_text)
            self.target_edit.setPlainText(source_text)
            self._set_target_state(error=False, loading=False)

    def _request_translation(self) -> None:
        text = self.source_edit.toPlainText().strip()
        if not text:
            self.source_edit.setFocus()
            return
        source_lang = str(self.source_language.currentData())
        target_lang = str(self.target_language.currentData())
        self.set_loading()
        self.translate_requested.emit(text, source_lang, target_lang)

    def _update_count(self) -> None:
        count = len(self.source_edit.toPlainText())
        count_text = _tr("%1 characters").replace("%1", f"{count:,}")
        self.character_count.setText(count_text if count else "")

    @staticmethod
    def _source_placeholder() -> str:
        return f'{_tr("Enter or paste text...")}\n\n⌃ Enter {_tr("Translate")}'

    def _on_language_changed(self, _lang_code: str) -> None:
        self._retranslate_ui()

    def _retranslate_ui(self) -> None:
        self.dashboard_title_bar.update_translations()
        self._update_pin_tooltip()
        self.swap_button.setToolTip(_tr("Swap languages"))
        self.source_edit.setPlaceholderText(self._source_placeholder())
        self.target_edit.setPlaceholderText(_tr("Translation will appear here..."))
        self.copy_source_button.set_tooltip(_tr("Copy Original"))
        self.clear_source_button.set_tooltip(_tr("Clear Original"))
        self.copy_target_button.set_tooltip(_tr("Copy Translation"))
        self.source_language.setItemText(0, _tr("Auto Detect"))
        self.translate_button.retranslate(not self.translate_button.isEnabled())
        self._update_count()
        if not getattr(self, "_backend_configured", False):
            self.backend_badge.setText(_tr("Engine not configured"))
        if self.target_edit.property("loading"):
            self.target_edit.setPlainText(_tr("Translating..."))
        elif self.target_edit.property("error") and hasattr(self, "_last_error_message"):
            self.target_edit.setPlainText(
                f'{_tr("Translation failed:")} {self._last_error_message}'
            )

    def _on_target_lang_changed(self, _index: int) -> None:
        target_lang = self.get_target_lang()
        if target_lang:
            config = get_tool_settings_manager()
            config.set_app_setting("translation_target_lang", target_lang)
            self.target_lang = target_lang
            log_debug(f"Target language saved: {target_lang}", "Translation")

    def _on_toggle_pin(self, checked: bool) -> None:
        self._is_on_top = checked
        TranslationDialog._stay_on_top = checked
        self._update_pin_tooltip()

        position = self.pos()
        was_visible = self.isVisible()
        self.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, checked)
        self.updateFrameless()
        self.move(position)
        if was_visible:
            self.show()
            self.raise_()
            self.activateWindow()
        log_debug(f"Always on top: {checked}", "Translation")

    def _update_pin_tooltip(self) -> None:
        state = _tr("Always on top: ON") if self._is_on_top else _tr("Always on top: OFF")
        self.dashboard_title_bar.pin_button.setToolTip(state)

    def _copy_source(self) -> None:
        text = self.source_edit.toPlainText()
        if text:
            QApplication.clipboard().setText(text)
            log_info("Copied original text", "Translation")

    def _copy_target(self) -> None:
        text = self.target_edit.toPlainText()
        if text:
            QApplication.clipboard().setText(text)
            log_info("Copied translation", "Translation")

    def _set_target_state(self, *, error: bool, loading: bool) -> None:
        self.target_edit.setProperty("error", error)
        self.target_edit.setProperty("loading", loading)
        self.target_edit.style().unpolish(self.target_edit)
        self.target_edit.style().polish(self.target_edit)

    def set_translation_result(self, translated_text: str, detected_lang: str = "") -> None:
        self._loading_dots.stop()
        self._set_target_state(error=False, loading=False)
        self.target_edit.setPlainText(translated_text)
        self.translated_text = translated_text
        if detected_lang:
            self._detected_source_lang = detected_lang
            self._set_combo_data(self.source_language, detected_lang, "auto")
        self.set_busy(False)

    def set_translation_error(self, error_msg: str) -> None:
        self._loading_dots.stop()
        self._last_error_message = error_msg
        self._set_target_state(error=True, loading=False)
        self.target_edit.setPlainText(f'{_tr("Translation failed:")} {error_msg}')
        self.set_busy(False)

    def set_loading(self) -> None:
        self._set_target_state(error=False, loading=True)
        self.target_edit.setPlainText(_tr("Translating..."))
        # 这条路径以前是一行静止的文字，而小窗口那边的点在转——同一个应用
        # 两种表现，只因为动画当初只加在了小窗口上。
        self._loading_dots.start()
        self.set_busy(True)

    def set_busy(self, busy: bool) -> None:
        self.translate_button.setDisabled(busy)
        self.translate_button.retranslate(busy)

    def on_translation_finished(
        self,
        success: bool,
        translated_text: str,
        error: str,
        detected_lang: str = "",
    ) -> None:
        if success:
            self.set_translation_result(translated_text, detected_lang)
        else:
            self.set_translation_error(error)

    def get_source_lang(self) -> str:
        return str(self.source_language.currentData())

    def get_target_lang(self) -> str:
        return str(self.target_language.currentData())

    def update_content(
        self,
        text: str,
        target_lang: str | None = None,
        source_lang: str | None = None,
    ) -> None:
        self.original_text = text
        self.source_edit.setPlainText(text)
        if source_lang:
            self.source_lang = source_lang
            self._set_combo_data(self.source_language, source_lang, "auto")
        if target_lang:
            self._set_combo_data(self.target_language, target_lang, self.get_target_lang())

    def set_backend_badge(self, text: str, configured: bool = True) -> None:
        self._backend_configured = configured
        badge = self.backend_badge
        badge.setProperty("configured", configured)
        badge.style().unpolish(badge)
        badge.style().polish(badge)
        set_badge_text(badge, text, padding=10)

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
        painter.fillRect(self.rect(), Qt.GlobalColor.transparent)
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)

        radius = 0.0 if self.isMaximized() else dialog_scaled(WINDOW_CORNER_RADIUS)
        rect = QRectF(self.rect())
        if not self.isMaximized():
            rect.adjust(0.5, 0.5, -0.5, -0.5)
        if self._theme_name == "dark":
            background = QLinearGradient(0, 0, 0, min(dialog_scaled(190), self.height()))
            background.setColorAt(0.0, QColor("#4d4f56"))
            background.setColorAt(1.0, QColor("#41434a"))
            painter.setBrush(background)
        else:
            painter.setBrush(QColor(self._palette.window))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(rect, radius, radius)

    def set_theme(self, theme_name: str) -> None:
        self._theme_name = "light" if theme_name == "light" else "dark"
        self._palette = LIGHT if self._theme_name == "light" else DARK
        self.setStyleSheet(self._style_sheet(self._palette))
        self.dashboard_title_bar.apply_palette(self._palette)
        for button in self._tool_buttons:
            button.apply_palette(self._palette)
        self.translate_button.apply_palette(self._palette)

        source_palette = self.source_edit.palette()
        source_palette.setColor(QPalette.ColorRole.PlaceholderText, QColor(self._palette.text_3))
        self.source_edit.setPalette(source_palette)
        target_palette = self.target_edit.palette()
        target_palette.setColor(QPalette.ColorRole.PlaceholderText, QColor(self._palette.text_3))
        self.target_edit.setPalette(target_palette)
        self.update()

    @staticmethod
    def _style_sheet(p: Palette) -> str:
        s = dialog_scaled
        stroke = s(1)
        return f"""
        QWidget {{ color: {p.text}; font-size: {s(13)}px; }}
        QLabel#appLogo {{
            color: white; background: {p.accent}; border: none; border-radius: {s(7)}px;
            font-size: {s(9)}px; font-weight: 700;
        }}
        QLabel#appName {{ font-size: {s(13)}px; font-weight: 600; padding-left: {s(3)}px; }}
        QLabel#backendBadge {{
            color: {p.accent}; background: {p.accent_tint};
            border-radius: {s(11)}px; padding: 0 {s(10)}px; font-size: {s(11)}px; font-weight: 600;
        }}
        QLabel#backendBadge[configured="true"] {{ color: {p.green}; }}
        QComboBox#languageCombo {{
            color: {p.text}; background: {p.fill}; border: none; border-radius: {s(18)}px;
            padding: 0 {s(14)}px;
        }}
        QComboBox#languageCombo:hover {{ background: {p.fill_hover}; }}
        QComboBox#languageCombo::drop-down {{ border: none; width: {s(24)}px; }}
        QComboBox#languageCombo QAbstractItemView {{
            color: {p.text}; background: {p.surface_strong};
            border: {stroke}px solid {p.fill_hover}; border-radius: {s(CONTROL_CORNER_RADIUS)}px;
            padding: {s(5)}px;
            outline: none; selection-color: white; selection-background-color: {p.accent};
        }}
        QPushButton#swapButton {{
            color: {p.accent}; background: {p.accent_tint}; border: none;
            border-radius: {s(18)}px; font-size: {s(19)}px; font-weight: 500;
        }}
        QPushButton#swapButton:hover {{ background: {p.fill_hover}; }}
        QFrame#sourcePane, QFrame#targetPane {{
            background: {p.field}; border: {stroke}px solid transparent;
            border-radius: {s(SURFACE_CORNER_RADIUS)}px;
        }}
        QFrame#sourcePane[focused="true"] {{ border-color: {p.accent}; }}
        QTextEdit#sourceEdit, QTextEdit#targetEdit {{
            color: {p.text}; background: transparent; border: none;
            padding: {s(1)}px {s(3)}px; font-size: {s(15)}px;
            selection-color: white; selection-background-color: {p.accent};
        }}
        QTextEdit#targetEdit[error="true"] {{ color: {p.danger}; }}
        QTextEdit#targetEdit[loading="true"] {{ color: {p.text_2}; font-style: italic; }}
        QLabel#characterCount {{ color: {p.text_3}; font-size: {s(11)}px; padding-right: {s(5)}px; }}
        QScrollBar:vertical {{ background: transparent; width: {s(7)}px; margin: {s(2)}px; }}
        QScrollBar::handle:vertical {{
            background: {p.fill}; min-height: {s(28)}px; border-radius: {s(3)}px;
        }}
        QScrollBar::handle:vertical:hover {{ background: {p.fill_hover}; }}
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {{ background: transparent; }}
        """


class TranslationLoadingDialog(TranslationDialog):
    """Compatibility class used by :class:`TranslationManager`."""

    def __init__(
        self,
        original_text: str = "",
        parent: QWidget | None = None,
        position: QPoint | None = None,
        source_lang: str = "auto",
        target_lang: str = "ZH",
    ):
        super().__init__(
            original_text=original_text,
            translated_text="",
            parent=parent,
            position=position,
            source_lang=source_lang,
            target_lang=target_lang,
        )
        if original_text.strip():
            self.set_loading()
