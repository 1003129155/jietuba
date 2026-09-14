"""扫码结果窗口：左边是截图，每个码描出轮廓、标上序号；右边按序号逐个列出内容

两边靠同一个序号对应。鼠标停在任一边的某个码上，两边一起高亮——"当前是哪个码"只在
窗口的 _set_active 里改，图和卡片各自照着画，彼此不直接通知。

配色分两类：和截图上某个码对应的标记（轮廓、序号、选中卡片的描边）用截图主题色，与
选区框同色；卡片、按钮、滚动条这些普通控件跟应用界面主题走。
"""

import math
import time

from PySide6.QtCore import QPointF, QRectF, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import QColor, QCursor, QDesktopServices, QPainter, QPen, QTextOption, QTransform
from PySide6.QtWidgets import (
    QApplication, QFrame, QHBoxLayout, QScrollArea, QSizePolicy, QVBoxLayout, QWidget,
)

from core import log_info, safe_event
from core.i18n import make_tr
from core.logger import T
from core.theme import contrast_ink, get_theme
from core.ui_theme import get_ui_theme
from ui.dialogs import track_modeless_dialog
from ui.fluent_lite import (
    FONT_FAMILY, BodyLabel, CaptionLabel, FluentTitleBar, FrostedFramelessDialog,
    PrimaryPushButton, PushButton, SimpleCardWidget, TextEdit, scrollbar_qss, ui_tokens,
)

from .reader import read_codes

_tr = make_tr("BarcodeResultWindow")

_BADGE_RADIUS = 10


def show_barcode_result(image):
    """识别 image 里的码并弹出结果窗口。

    解码直接在主线程做，不开后台线程、也没有"识别中"的等待态：实测 2560×1440 整屏约
    65 ms、普通大小的选区十几 ms，截图界面一关窗口就带着结果出来，等待态只会一闪而过。
    """
    started = time.perf_counter()
    codes = read_codes(image)
    log_info(T("扫码完成: 识别到 {count} 个码, 耗时 {elapsed_ms:.0f} ms",
               count=len(codes), elapsed_ms=(time.perf_counter() - started) * 1000), "Barcode")

    window = BarcodeResultWindow(image, codes)
    track_modeless_dialog(window)
    window.show()
    window.raise_()
    window.activateWindow()
    return window


def _web_link(text):
    """内容是 http(s) 链接就返回能打开的 QUrl，否则 None。

    只认 http(s)：码里的内容不可信，file:、自定义协议之类交给系统去打开，可能直接拉起本地程序。
    """
    url = QUrl(text.strip(), QUrl.ParsingMode.StrictMode)
    if url.isValid() and url.scheme() in ("http", "https") and url.host():
        return url
    return None


def _paint_badge(painter, center, number):
    """序号圆标。图上和卡片上共用这一份画法，两边看着才是同一个标记"""
    color = get_theme().theme_color
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(color)
    painter.drawEllipse(center, _BADGE_RADIUS, _BADGE_RADIUS)
    font = painter.font()
    font.setPixelSize(12)
    font.setBold(True)
    painter.setFont(font)
    painter.setPen(contrast_ink(color))
    box = QRectF(center.x() - _BADGE_RADIUS, center.y() - _BADGE_RADIUS, 2 * _BADGE_RADIUS, 2 * _BADGE_RADIUS)
    painter.drawText(box, Qt.AlignmentFlag.AlignCenter, str(number))


class _CodeImageView(QWidget):
    """截图，每个码描出轮廓、标上序号。

    图只缩小不放大：小选区放大后码会糊，也看不出截的原本有多大。
    """

    hovered = Signal(int)   # 鼠标下是第几个码，不在任何码上为 -1

    def __init__(self, image, codes, parent=None):
        super().__init__(parent)
        self._image = image
        self._codes = codes
        self._active = -1
        self.setMouseTracking(True)
        self.setMinimumSize(240, 180)

    def set_active(self, index):
        if index != self._active:
            self._active = index
            self.update()

    def _image_to_widget(self):
        """原图像素坐标 → 控件坐标（等比缩到放得下，居中）。画图和鼠标命中只用这一个换算"""
        scale = min(self.width() / self._image.width(), self.height() / self._image.height(), 1.0)
        return QTransform(
            scale, 0, 0, scale,
            (self.width() - self._image.width() * scale) / 2,
            (self.height() - self._image.height() * scale) / 2,
        )

    @safe_event
    def paintEvent(self, event):
        transform = self._image_to_widget()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        image_rect = transform.mapRect(QRectF(self._image.rect()))
        painter.drawImage(image_rect, self._image)
        painter.setPen(QPen(QColor(ui_tokens(self).window_border), 1))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(image_rect)

        color = get_theme().theme_color
        for index, code in enumerate(self._codes):
            active = index == self._active
            fill = QColor(color)
            fill.setAlpha(110 if active else 40)
            painter.setPen(QPen(color, 3 if active else 2))
            painter.setBrush(fill)
            outline = transform.map(code.outline)
            painter.drawPolygon(outline)
            # 码贴着图边时，圆标会被控件边缘裁掉一半，往里收到完整可见
            corner = outline.boundingRect().topLeft()
            center = QPointF(
                min(max(corner.x(), _BADGE_RADIUS), self.width() - _BADGE_RADIUS),
                min(max(corner.y(), _BADGE_RADIUS), self.height() - _BADGE_RADIUS),
            )
            _paint_badge(painter, center, index + 1)
        painter.end()

    @safe_event
    def mouseMoveEvent(self, event):
        to_image, _invertible = self._image_to_widget().inverted()
        point = to_image.map(event.position())
        self.hovered.emit(next(
            (index for index, code in enumerate(self._codes)
             if code.outline.containsPoint(point, Qt.FillRule.OddEvenFill)),
            -1,
        ))

    @safe_event
    def leaveEvent(self, event):
        self.hovered.emit(-1)


class _Badge(QWidget):
    def __init__(self, number, parent=None):
        super().__init__(parent)
        self._number = number
        self.setFixedSize(2 * _BADGE_RADIUS, 2 * _BADGE_RADIUS)

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        _paint_badge(painter, QRectF(self.rect()).center(), self._number)
        painter.end()


class _CodeText(TextEdit):
    """识别出的内容：只读、可选中，高度跟着内容走，不出滚动条。

    不用 QLabel：QLabel 只在空格处折行，一长串没有空格的链接会把整栏撑宽或者被截断。
    基类用 fluent_lite 的 TextEdit，右键菜单才是应用里统一那份（跟主题、有翻译）。
    """

    def __init__(self, text, parent=None):
        super().__init__(parent)
        self.setReadOnly(True)
        self.setFrameShape(QFrame.Shape.NoFrame)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setWordWrapMode(QTextOption.WrapMode.WrapAtWordBoundaryOrAnywhere)
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        self.document().setDocumentMargin(0)
        self.document().documentLayout().documentSizeChanged.connect(
            lambda size: self.setFixedHeight(math.ceil(size.height())))
        self.setPlainText(text)
        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)

    def _apply_theme(self, _tokens=None):
        t = ui_tokens(self)
        self.setStyleSheet(
            f"QTextEdit {{ background: transparent; color: {t.text}; font: 13px {FONT_FAMILY}; "
            f"selection-background-color: {t.accent}; selection-color: {t.selected_text}; }}"
        )


class _CodeCard(SimpleCardWidget):
    """一个码：序号、格式、内容，以及复制 / 打开链接"""

    hovered = Signal(int)   # 鼠标进来发自己的下标，离开发 -1

    def __init__(self, index, code, parent=None):
        super().__init__(parent)
        self.index = index
        self.setProperty("active", False)

        header = QHBoxLayout()
        header.setSpacing(8)
        header.addWidget(_Badge(index + 1, self))
        header.addWidget(CaptionLabel(code.format_name, self), 1)

        self.copy_button = PushButton(_tr("Copy"), self)
        self.copy_button.clicked.connect(lambda: self._copy(code.text))
        buttons = QHBoxLayout()
        buttons.addStretch(1)
        buttons.addWidget(self.copy_button)
        self.open_button = None
        link = _web_link(code.text)
        if link is not None:
            self.open_button = PrimaryPushButton(_tr("Open link"), self)
            self.open_button.clicked.connect(lambda: QDesktopServices.openUrl(link))
            buttons.addWidget(self.open_button)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(6)
        layout.addLayout(header)
        layout.addWidget(_CodeText(code.text, self))
        layout.addLayout(buttons)

    def set_active(self, active):
        if self.property("active") != active:
            self.setProperty("active", active)
            self.style().unpolish(self)
            self.style().polish(self)

    def _copy(self, text):
        QApplication.clipboard().setText(text)
        self.copy_button.setText(_tr("Copied"))
        QTimer.singleShot(1500, self.copy_button, lambda: self.copy_button.setText(_tr("Copy")))

    def _apply_theme(self, _tokens=None):
        """底色、描边、圆角沿用 SimpleCardWidget，只追加选中态"""
        super()._apply_theme(_tokens)
        self.setStyleSheet(
            self.styleSheet()
            + f'QFrame#FluentLiteSimpleCard[active="true"] {{ border: 2px solid {get_theme().theme_color_hex}; }}'
        )

    @safe_event
    def enterEvent(self, event):
        super().enterEvent(event)
        self.hovered.emit(self.index)

    @safe_event
    def leaveEvent(self, event):
        super().leaveEvent(event)
        self.hovered.emit(-1)


class BarcodeResultWindow(FrostedFramelessDialog):
    """扫码结果。image 是选区底图，codes 是 read_codes 的结果（轮廓坐标都是 image 的像素坐标）"""

    PANEL_WIDTH = 300

    def __init__(self, image, codes, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        title_bar = FluentTitleBar(self)
        self.setTitleBar(title_bar)
        title_bar.iconLabel.hide()
        # 同欢迎向导：隐藏图标后标题会紧贴窗口左边，补回原生标题栏的留白
        title_bar.hBoxLayout.setContentsMargins(12, 0, 0, 0)
        self.setWindowTitle(_tr("Scan result"))

        self.image_view = _CodeImageView(image, codes, self)
        self.image_view.hovered.connect(self._on_image_hovered)

        if codes:
            summary = _tr("Found %1 code(s)").replace("%1", str(len(codes)))
        else:
            summary = _tr("No QR code or barcode found")
        self.summary_label = BodyLabel(summary, self)
        self.summary_label.setWordWrap(True)

        cards_host = QWidget()
        cards_layout = QVBoxLayout(cards_host)
        cards_layout.setContentsMargins(0, 0, 0, 0)
        cards_layout.setSpacing(8)
        self.cards = []
        for index, code in enumerate(codes):
            card = _CodeCard(index, code, cards_host)
            card.hovered.connect(self._set_active)
            cards_layout.addWidget(card)
            self.cards.append(card)
        if not codes:
            cards_layout.addWidget(
                CaptionLabel(_tr("Make sure the whole code is inside the selection."), cards_host))
        cards_layout.addStretch(1)

        self._scroll = QScrollArea(self)
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._scroll.setWidget(cards_host)

        panel = QWidget(self)
        panel.setFixedWidth(self.PANEL_WIDTH)
        panel_layout = QVBoxLayout(panel)
        panel_layout.setContentsMargins(0, 0, 0, 0)
        panel_layout.setSpacing(8)
        panel_layout.addWidget(self.summary_label)
        panel_layout.addWidget(self._scroll, 1)

        root = QHBoxLayout(self)
        root.setContentsMargins(12, title_bar.height() + 4, 12, 12)
        root.setSpacing(12)
        root.addWidget(self.image_view, 1)
        root.addWidget(panel)

        self._apply_theme()
        get_ui_theme().theme_changed.connect(self._apply_theme)
        self._place(image)

    def _apply_theme(self, _tokens=None):
        self._scroll.setStyleSheet(
            "QScrollArea { border: none; background: transparent; }"
            "QScrollArea > QWidget > QWidget { background: transparent; }"
            + scrollbar_qss(self)
        )

    def _place(self, image):
        """图 1:1 放得下就按原尺寸开窗，放不下就开到屏幕的八成；摆在鼠标所在屏幕的正中"""
        screen = QApplication.screenAt(QCursor.pos()) or QApplication.primaryScreen()
        available = screen.availableGeometry()
        margins = self.layout().contentsMargins()
        width = (image.width() + self.layout().spacing() + self.PANEL_WIDTH
                 + margins.left() + margins.right())
        height = image.height() + margins.top() + margins.bottom()
        self.resize(
            max(640, min(width, int(available.width() * 0.8))),
            max(420, min(height, int(available.height() * 0.8))),
        )
        self.move(available.center() - self.rect().center())

    def _set_active(self, index):
        """高亮第 index 个码，-1 为都不高亮"""
        self.image_view.set_active(index)
        for card in self.cards:
            card.set_active(card.index == index)

    def _on_image_hovered(self, index):
        self._set_active(index)
        # 只在从图这边指到码时才滚动列表：鼠标本来就在卡片上时滚动，会把卡片从鼠标底下挪走
        if index >= 0:
            self._scroll.ensureWidgetVisible(self.cards[index])
