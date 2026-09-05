# -*- coding: utf-8 -*-
"""
第3页 — 剪贴板管理快捷键设置

上半部：剪贴板内容类型、内容分组与快速启动的连续动画
下半部：快捷键设置
"""

from PySide6.QtWidgets import (
    QVBoxLayout, QLabel, QWidget, QSizePolicy
)
from PySide6.QtCore import Qt, QTimer, QRectF, QPointF
from PySide6.QtGui import (
    QPainter, QColor, QPen, QFont, QFontMetrics, QPainterPath, QPolygonF,
)
from core import safe_event
from core.i18n import make_tr
from ui.fluent_lite import SpinBox

if __package__:
    from .base_page import (
        BasePage, IllustrationArea,
        welcome_theme, set_welcome_label_style,
    )
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import (
        BasePage, IllustrationArea,
        welcome_theme, set_welcome_label_style,
    )


_tr = make_tr("WelcomeWizard")
_settings_tr = make_tr("SettingsDialog")
_clipboard_tr = make_tr("ClipboardWindow")


def _clipboard_subtitle():
    """复用现有翻译，但不强制将说明拆成两行。"""
    return _tr(
        "自动记录每一次复制，随时召唤历史内容。\n"
        "支持文本、图片、文件，还能分组管理。"
    ).replace("\n", " ")


# ── 剪贴板功能连续动画 ─────────────────────────────────────
class _ClipboardFeatureAnimation(QWidget):
    """按真实剪贴板窗口布局演示内容类型和分组切换。"""

    DURATION_MS = 14200
    TYPE_SCAN_END_MS = 6000
    CONTENT_GROUP_START_MS = 7200
    QUICK_FOCUS_START_MS = 9000
    QUICK_GROUP_START_MS = 10200

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent;")
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        self.setMinimumHeight(280)
        self._elapsed_ms = 0
        self._refresh_text()
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(30)

    def _refresh_text(self):
        self._history_group = _tr("历史记录")
        self._content_group = _tr("内容分组")
        self._quick_group = _tr("快速启动")
        self._search = _clipboard_tr("Search")
        self._type_labels = [_tr("文字"), _tr("图片"), _tr("文件")]
        self._type_rows = [
            ("text", _tr("会议时间改为 14:00"), "WeChat · 14:20"),
            ("image", _tr("产品封面.png"), "Photos · 14:18"),
            ("file", _tr("项目说明.pdf"), "Explorer · 14:15"),
        ]
        self._content_rows = [
            ("text", _tr("感谢您的联系"), "Edge · 10:32"),
            ("image", _tr("设计预览.png"), "Photos · 10:18"),
            ("text", _tr("下周一 10:00 开会"), "Notepad · 09:45"),
        ]
        self._quick_rows = [
            ("app_edge", "Microsoft Edge", ""),
            ("app_code", "Visual Studio Code", ""),
            ("app_note", "Notepad", ""),
        ]

    def retranslate(self):
        self._refresh_text()
        self.update()

    def _tick(self):
        self._elapsed_ms = (self._elapsed_ms + self._timer.interval()) % self.DURATION_MS
        self.update()

    def set_animation_time(self, elapsed_ms: int):
        """设置演示时间，供预览和自动化测试稳定检查各阶段。"""
        self._elapsed_ms = max(0, int(elapsed_ms)) % self.DURATION_MS
        self.update()

    @staticmethod
    def _font(pixel_size: int, *, bold=False) -> QFont:
        """使用系统字体回退，保证中、日、韩文字不会变成方框。"""
        font = QFont()
        font.setPixelSize(pixel_size)
        font.setBold(bold)
        return font

    @staticmethod
    def _with_alpha(color, alpha: int) -> QColor:
        result = QColor(color)
        result.setAlpha(max(0, min(255, int(alpha))))
        return result

    @classmethod
    def _draw_centered_text(
        cls, p: QPainter, rect: QRectF, text: str, color, size=12, bold=False
    ):
        p.setPen(QColor(color))
        p.setFont(cls._font(size, bold=bold))
        p.drawText(rect, Qt.AlignmentFlag.AlignCenter, text)

    @staticmethod
    def _palette(theme):
        """复刻 clipboard/ui/theme/themes.py 的默认明暗配色。"""
        if theme.is_dark:
            return {
                "primary": "#1E1E1E",
                "secondary": "#3B3B3D",
                "alternate": "#2D2D30",
                "hover": "#3E3E42",
                "selected": "#384D5F",
                "text": "#E0E0E0",
                "muted": "#888888",
                "border": "#3E3E42",
                "accent": "#007ACC",
                "shortcut": "#9CA2C5",
                "danger": "#F44336",
                "success": "#4CAF50",
            }
        return {
            "primary": "#FFFFFF",
            "secondary": "#E0EAF0",
            "alternate": "#F5F6F8",
            "hover": "#F0F0F0",
            "selected": "#DCE6ED",
            "text": "#333333",
            "muted": "#999999",
            "border": "#E0E0E0",
            "accent": "#6F8FAB",
            "shortcut": "#58738C",
            "danger": "#F44336",
            "success": "#4CAF50",
        }

    def _draw_toolbar_button(
        self, p: QPainter, rect: QRectF, icon: str, palette, *, selected=False, focused=False
    ):
        if focused:
            pulse = 0.5 + 0.5 * abs(((self._elapsed_ms % 900) / 450.0) - 1.0)
            ring = rect.adjusted(-3, -3, 3, 3)
            p.setBrush(self._with_alpha(palette["accent"], 22 + int(28 * pulse)))
            p.setPen(QPen(self._with_alpha(palette["accent"], 90), 1))
            p.drawRoundedRect(ring, 5, 5)

        if selected:
            p.setBrush(QColor(palette["selected"]))
            p.setPen(QPen(QColor(palette["danger"]), 1.5))
            p.drawRoundedRect(rect, 4, 4)

        stroke = QColor(palette["accent"] if selected or focused else palette["muted"])
        cx, cy = rect.center().x(), rect.center().y()
        if icon == "clipboard":
            body = QRectF(cx - 7, cy - 8, 14, 17)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(stroke, 1.5))
            p.drawRoundedRect(body, 2, 2)
            p.drawRoundedRect(QRectF(cx - 4, cy - 10, 8, 4), 1.5, 1.5)
            p.drawLine(int(cx - 4), int(cy - 2), int(cx + 4), int(cy - 2))
            p.drawLine(int(cx - 4), int(cy + 2), int(cx + 3), int(cy + 2))
        elif icon == "folder":
            path = QPainterPath()
            path.moveTo(cx - 9, cy - 6)
            path.lineTo(cx - 2, cy - 6)
            path.lineTo(cx + 1, cy - 3)
            path.lineTo(cx + 9, cy - 3)
            path.lineTo(cx + 8, cy + 7)
            path.lineTo(cx - 9, cy + 7)
            path.closeSubpath()
            p.setBrush(self._with_alpha(stroke, 45))
            p.setPen(QPen(stroke, 1.5))
            p.drawPath(path)
        elif icon == "bolt":
            bolt = QPolygonF([
                QPointF(cx + 1, cy - 10),
                QPointF(cx - 7, cy + 1),
                QPointF(cx - 1, cy + 1),
                QPointF(cx - 3, cy + 10),
                QPointF(cx + 8, cy - 3),
                QPointF(cx + 2, cy - 3),
            ])
            p.setBrush(stroke)
            p.setPen(Qt.PenStyle.NoPen)
            p.drawPolygon(bolt)
        elif icon == "plus":
            p.setPen(QPen(QColor(palette["success"]), 1.8))
            p.drawLine(int(cx - 5), int(cy), int(cx + 5), int(cy))
            p.drawLine(int(cx), int(cy - 5), int(cx), int(cy + 5))
            p.setPen(QPen(QColor(palette["success"]), 1, Qt.PenStyle.DashLine))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(rect.adjusted(2, 2, -2, -2), 3, 3)
        elif icon == "close":
            p.setPen(QPen(stroke, 1.5))
            p.drawLine(int(cx - 5), int(cy - 5), int(cx + 5), int(cy + 5))
            p.drawLine(int(cx + 5), int(cy - 5), int(cx - 5), int(cy + 5))

    def _draw_group_label(
        self,
        p: QPainter,
        target: QRectF,
        canvas: QRectF,
        text: str,
        palette,
    ):
        font = self._font(11)
        width = QFontMetrics(font).horizontalAdvance(text) + 18
        center_x = target.center().x()
        left = max(2.0, min(self.width() - width - 2.0, center_x - width / 2))
        rect = QRectF(
            left,
            canvas.top() - 29,
            width,
            24,
        )
        p.setBrush(QColor(palette["primary"]))
        p.setPen(QPen(QColor(palette["border"]), 1))
        p.drawRoundedRect(rect, 4, 4)
        self._draw_centered_text(p, rect, text, palette["text"], size=11)

        # 标签位于窗口外的留白区，用一个小箭头指向当前分组图标。
        pointer_x = max(rect.left() + 8, min(rect.right() - 8, center_x))
        pointer = QPolygonF([
            QPointF(pointer_x - 4, rect.bottom()),
            QPointF(pointer_x + 4, rect.bottom()),
            QPointF(center_x, canvas.top() - 1),
        ])
        p.setBrush(QColor(palette["primary"]))
        p.setPen(QPen(QColor(palette["border"]), 1))
        p.drawPolygon(pointer)

    def _draw_image_thumbnail(self, p: QPainter, rect: QRectF, palette):
        p.setBrush(QColor("#DDECF4"))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRect(rect)
        p.setBrush(QColor("#F3B75D"))
        p.drawEllipse(QRectF(rect.right() - 12, rect.y() + 5, 6, 6))
        hill = QPolygonF([
            QPointF(rect.x(), rect.bottom()),
            QPointF(rect.x() + 11, rect.y() + 17),
            QPointF(rect.x() + 18, rect.y() + 23),
            QPointF(rect.x() + 27, rect.y() + 13),
            QPointF(rect.right(), rect.bottom()),
        ])
        p.setBrush(QColor("#74B98A"))
        p.drawPolygon(hill)
        p.setPen(QPen(QColor(palette["border"]), 1))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRect(rect)

    def _draw_file_icon(self, p: QPainter, rect: QRectF, palette):
        color = QColor("#E5A23E")
        path = QPainterPath()
        path.moveTo(rect.x() + 2, rect.y() + 8)
        path.lineTo(rect.x() + 10, rect.y() + 8)
        path.lineTo(rect.x() + 13, rect.y() + 11)
        path.lineTo(rect.right() - 2, rect.y() + 11)
        path.lineTo(rect.right() - 3, rect.bottom() - 3)
        path.lineTo(rect.x() + 2, rect.bottom() - 3)
        path.closeSubpath()
        p.setBrush(self._with_alpha(color, 65))
        p.setPen(QPen(color, 1.3))
        p.drawPath(path)

    def _draw_row(
        self,
        p: QPainter,
        rect: QRectF,
        index: int,
        kind: str,
        text: str,
        metadata: str,
        palette,
        *,
        active=False,
        type_label="",
        scan_progress=0.0,
        quick_group=False,
    ):
        background = palette["selected"] if active else (
            palette["primary"] if index % 2 == 0 else palette["alternate"]
        )
        p.fillRect(rect, QColor(background))
        p.setPen(QPen(self._with_alpha(palette["border"], 150), 1))
        p.drawLine(int(rect.left()), int(rect.bottom()), int(rect.right()), int(rect.bottom()))

        if active:
            p.fillRect(QRectF(rect.x(), rect.y(), 3, rect.height()), QColor(palette["accent"]))
            scan_x = rect.x() + rect.width() * scan_progress
            p.setPen(QPen(self._with_alpha(palette["accent"], 125), 1.5))
            p.drawLine(int(scan_x), int(rect.top() + 3), int(scan_x), int(rect.bottom() - 3))

        badge_rect = QRectF(rect.x() + 4, rect.y(), 27, rect.height())
        self._draw_centered_text(
            p, badge_rect, f"{index + 1}:", palette["shortcut"], size=12, bold=True
        )

        content_left = rect.x() + 34
        if kind == "image":
            thumb_size = min(38, rect.height() - 10)
            thumb = QRectF(content_left, rect.y() + (rect.height() - thumb_size) / 2, thumb_size, thumb_size)
            self._draw_image_thumbnail(p, thumb, palette)
            content_left = thumb.right() + 7
        elif kind == "file" and not quick_group:
            icon_rect = QRectF(content_left, rect.y() + (rect.height() - 28) / 2, 28, 28)
            self._draw_file_icon(p, icon_rect, palette)
            content_left = icon_rect.right() + 5

        type_width = 0
        if type_label:
            label_font = self._font(12, bold=True)
            type_width = QFontMetrics(label_font).horizontalAdvance(type_label) + 14
            alpha = int(255 * min(1.0, scan_progress * 5.0, (1.0 - scan_progress) * 5.0))
            type_rect = QRectF(rect.right() - type_width - 8, rect.y() + 5, type_width, 20)
            p.setPen(self._with_alpha(palette["accent"], alpha))
            p.setFont(label_font)
            p.drawText(type_rect, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, type_label)

        text_rect = QRectF(
            content_left,
            rect.y() + 5,
            max(20, rect.right() - content_left - type_width - 9),
            21,
        )
        font = self._font(13)
        shown = QFontMetrics(font).elidedText(
            text, Qt.TextElideMode.ElideRight, int(text_rect.width())
        )
        p.setFont(font)
        p.setPen(QColor(palette["text"]))
        p.drawText(text_rect, Qt.AlignmentFlag.AlignVCenter, shown)

        if metadata:
            meta_font = self._font(9)
            p.setFont(meta_font)
            p.setPen(QColor(palette["muted"]))
            meta_rect = QRectF(
                rect.x() + 34,
                rect.bottom() - 20,
                rect.width() - 42,
                15,
            )
            p.drawText(meta_rect, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, metadata)

    def _draw_bottom_bar(self, p: QPainter, rect: QRectF, palette):
        p.fillRect(rect, QColor(palette["secondary"]))
        p.setPen(QPen(QColor(palette["border"]), 1))
        p.drawLine(int(rect.left()), int(rect.top()), int(rect.right()), int(rect.top()))

        # 时间筛选按钮（真实窗口左侧的向上箭头）。
        arrow_x = rect.x() + 17
        arrow_y = rect.center().y()
        p.setPen(QPen(QColor(palette["muted"]), 1.5))
        p.drawLine(int(arrow_x - 4), int(arrow_y + 2), int(arrow_x), int(arrow_y - 2))
        p.drawLine(int(arrow_x), int(arrow_y - 2), int(arrow_x + 4), int(arrow_y + 2))

        # 扁平搜索输入框。
        search_left = rect.x() + 34
        search_right = rect.right() - 34
        cy = rect.center().y()
        p.setPen(QPen(QColor(palette["muted"]), 1.2))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(QRectF(search_left + 3, cy - 5, 8, 8))
        p.drawLine(int(search_left + 10), int(cy + 2), int(search_left + 14), int(cy + 6))
        search_rect = QRectF(search_left + 19, rect.y(), search_right - search_left - 19, rect.height())
        p.setFont(self._font(11))
        p.setPen(QColor(palette["muted"]))
        p.drawText(search_rect, Qt.AlignmentFlag.AlignVCenter, self._search)

        # 设置齿轮用真实窗口相同的紧凑位置，使用简单矢量圆环。
        gear_center = QPointF(rect.right() - 17, cy)
        p.setPen(QPen(QColor(palette["muted"]), 1.3))
        p.drawEllipse(gear_center, 6, 6)
        p.drawEllipse(gear_center, 2, 2)

    def _draw_rows(
        self,
        p: QPainter,
        rows,
        *,
        canvas: QRectF,
        list_top: float,
        row_height: float,
        palette,
        opacity=1.0,
        scan_index=-1,
        scan_progress=0.0,
        quick_group=False,
    ):
        p.save()
        p.setOpacity(opacity)
        for index, (kind, text, metadata) in enumerate(rows):
            rect = QRectF(
                canvas.x() + 1,
                list_top + index * row_height,
                canvas.width() - 2,
                row_height,
            )
            active = index == scan_index
            self._draw_row(
                p,
                rect,
                index,
                kind,
                text,
                metadata,
                palette,
                active=active,
                type_label=self._type_labels[index] if active else "",
                scan_progress=scan_progress,
                quick_group=quick_group,
            )
        p.restore()

    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self._elapsed_ms < 350:
            p.setOpacity(max(0.05, self._elapsed_ms / 350.0))
        elif self._elapsed_ms > self.DURATION_MS - 500:
            p.setOpacity(max(0.05, (self.DURATION_MS - self._elapsed_ms) / 500.0))
        theme = welcome_theme()
        palette = self._palette(theme)

        # 顶部固定预留说明标签区域，标签不会覆盖真实窗口内容。
        label_band = 34.0
        max_canvas_height = max(120.0, self.height() - label_band - 4.0)
        canvas_width = min(
            max(80.0, self.width() - 12.0),
            max_canvas_height / 1.34,
        )
        canvas_height = min(max_canvas_height, canvas_width * 1.34)
        canvas = QRectF(
            (self.width() - canvas_width) / 2,
            label_band + (self.height() - label_band - canvas_height) / 2,
            canvas_width,
            canvas_height,
        )

        # 真实窗口只有 4px 圆角和 1px 边框。
        shadow = canvas.translated(0, 3)
        p.setBrush(self._with_alpha("#000000", 20))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRoundedRect(shadow, 4, 4)
        p.setBrush(QColor(palette["primary"]))
        p.setPen(QPen(QColor(palette["border"]), 1))
        p.drawRoundedRect(canvas, 4, 4)

        # 顶部横向分组栏：历史、内容分组、快速启动、添加、关闭。
        toolbar_h = 38.0
        toolbar = QRectF(canvas.x() + 1, canvas.y() + 1, canvas.width() - 2, toolbar_h)
        p.fillRect(toolbar, QColor(palette["secondary"]))
        p.setPen(QPen(QColor(palette["border"]), 1))
        p.drawLine(int(toolbar.left()), int(toolbar.bottom()), int(toolbar.right()), int(toolbar.bottom()))

        button_y = toolbar.y() + 5
        button_size = 28.0
        clipboard_btn = QRectF(toolbar.x() + 5, button_y, button_size, button_size)
        separator_x = clipboard_btn.right() + 5
        p.setPen(QPen(QColor(palette["border"]), 1))
        p.drawLine(int(separator_x), int(toolbar.y() + 7), int(separator_x), int(toolbar.bottom() - 7))
        content_btn = QRectF(separator_x + 6, button_y, button_size, button_size)
        quick_btn = QRectF(content_btn.right() + 5, button_y, button_size, button_size)
        add_btn = QRectF(quick_btn.right() + 5, button_y, button_size, button_size)
        close_btn = QRectF(toolbar.right() - button_size - 5, button_y, button_size, button_size)

        elapsed = self._elapsed_ms
        content_selected = self.CONTENT_GROUP_START_MS <= elapsed < self.QUICK_GROUP_START_MS
        quick_selected = elapsed >= self.QUICK_GROUP_START_MS
        content_focused = self.TYPE_SCAN_END_MS <= elapsed < self.CONTENT_GROUP_START_MS
        quick_focused = self.QUICK_FOCUS_START_MS <= elapsed < self.QUICK_GROUP_START_MS
        self._draw_toolbar_button(
            p, clipboard_btn, "clipboard", palette,
            selected=not content_selected and not quick_selected,
        )
        self._draw_toolbar_button(
            p, content_btn, "folder", palette,
            selected=content_selected, focused=content_focused,
        )
        self._draw_toolbar_button(
            p, quick_btn, "bolt", palette,
            selected=quick_selected, focused=quick_focused,
        )
        self._draw_toolbar_button(p, add_btn, "plus", palette)
        self._draw_toolbar_button(p, close_btn, "close", palette)

        if elapsed < self.CONTENT_GROUP_START_MS:
            rows = self._type_rows
            quick_group = False
        elif elapsed < self.QUICK_GROUP_START_MS:
            rows = self._content_rows
            quick_group = False
        else:
            rows = self._quick_rows
            quick_group = True

        bottom_h = 36.0
        bottom_bar = QRectF(
            canvas.x() + 1,
            canvas.bottom() - bottom_h,
            canvas.width() - 2,
            bottom_h - 1,
        )
        list_top = toolbar.bottom() + 1
        list_bottom = bottom_bar.top()
        p.fillRect(
            QRectF(canvas.x() + 1, list_top, canvas.width() - 2, list_bottom - list_top),
            QColor(palette["primary"]),
        )

        row_height = min(58.0, max(46.0, (list_bottom - list_top) / 4.15))
        scan_index = -1
        scan_progress = 0.0
        if elapsed < self.TYPE_SCAN_END_MS:
            scan_index = min(2, elapsed // 2000)
            scan_progress = (elapsed % 2000) / 2000.0

        transition_ms = 260
        if self.CONTENT_GROUP_START_MS <= elapsed < self.CONTENT_GROUP_START_MS + transition_ms:
            mix = (elapsed - self.CONTENT_GROUP_START_MS) / transition_ms
            self._draw_rows(
                p, self._type_rows, canvas=canvas, list_top=list_top,
                row_height=row_height, palette=palette, opacity=1.0 - mix,
            )
            self._draw_rows(
                p, self._content_rows, canvas=canvas, list_top=list_top,
                row_height=row_height, palette=palette, opacity=mix,
            )
        elif self.QUICK_GROUP_START_MS <= elapsed < self.QUICK_GROUP_START_MS + transition_ms:
            mix = (elapsed - self.QUICK_GROUP_START_MS) / transition_ms
            self._draw_rows(
                p, self._content_rows, canvas=canvas, list_top=list_top,
                row_height=row_height, palette=palette, opacity=1.0 - mix,
            )
            self._draw_rows(
                p, self._quick_rows, canvas=canvas, list_top=list_top,
                row_height=row_height, palette=palette, opacity=mix, quick_group=True,
            )
        else:
            self._draw_rows(
                p, rows, canvas=canvas, list_top=list_top,
                row_height=row_height, palette=palette,
                scan_index=scan_index, scan_progress=scan_progress,
                quick_group=quick_group,
            )

        self._draw_bottom_bar(p, bottom_bar, palette)

        # 始终在窗口上方说明当前区域，三种状态位置和样式保持一致。
        if elapsed < self.TYPE_SCAN_END_MS:
            label_target = clipboard_btn
            label_text = self._history_group
        elif elapsed < self.QUICK_FOCUS_START_MS:
            label_target = content_btn
            label_text = self._content_group
        else:
            label_target = quick_btn
            label_text = self._quick_group
        self._draw_group_label(
            p, label_target, canvas, label_text, palette
        )


class _ClipboardFeatureIllus(IllustrationArea):
    """欢迎页左侧的非交互剪贴板功能演示。"""

    def _build_content(self):
        self._layout.setContentsMargins(10, 10, 10, 10)
        self.animation = _ClipboardFeatureAnimation(self)
        self._layout.addWidget(self.animation, 1)

    def retranslate(self):
        self.animation.retranslate()

    def _apply_welcome_theme(self, tokens=None):
        super()._apply_welcome_theme(tokens)
        if hasattr(self, "animation"):
            self.animation.update()


# ── 页面主体 ────────────────────────────────────────────
class ClipboardHotkeyPage(BasePage):
    """第3页：剪贴板快捷键"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title="剪贴板管理",
            subtitle=_clipboard_subtitle(),
            parent=parent,
        )

    def _create_illustration(self):
        return _ClipboardFeatureIllus(self)

    def _build_controls(self, layout: QVBoxLayout):
        if __package__:
            from ..hotkey_edit import HotkeyEdit
        else:
            import sys, os
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
            from hotkey_edit import HotkeyEdit


        # 快捷键说明标签
        self._hotkey_lbl = QLabel(_tr("快捷键（最多设置两个）"))
        set_welcome_label_style(
            self._hotkey_lbl, role="primary", font_size=14, weight=600
        )

        self._hotkey_desc = QLabel(_tr("程序还会尝试注册 Win+V 作为额外备用。"))
        self._hotkey_desc.setWordWrap(True)
        set_welcome_label_style(
            self._hotkey_desc, role="muted", font_size=12, weight=400
        )

        # 两个快捷键输入框上下排列
        self._hotkey = HotkeyEdit()
        self._hotkey.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._hotkey.setText(self._config.get_clipboard_hotkey())

        self._hotkey2 = HotkeyEdit()
        self._hotkey2.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._hotkey2.setText(self._config.get_clipboard_hotkey_2())

        layout.addWidget(self._hotkey_lbl)
        layout.addWidget(self._hotkey_desc)
        layout.addSpacing(4)
        layout.addWidget(self._hotkey)
        layout.addSpacing(6)
        layout.addWidget(self._hotkey2)

        # 历史记录上限（与上方快捷键区保持相同的信息层级）
        layout.addSpacing(14)
        self._history_limit_lbl = QLabel(_settings_tr("History Limit"))
        set_welcome_label_style(
            self._history_limit_lbl, role="primary", font_size=14, weight=600
        )
        layout.addWidget(self._history_limit_lbl)

        self._history_limit_desc = QLabel(
            _settings_tr("Maximum number of items to keep (0 = unlimited)")
        )
        self._history_limit_desc.setWordWrap(True)
        set_welcome_label_style(
            self._history_limit_desc, role="muted", font_size=12, weight=400
        )
        layout.addWidget(self._history_limit_desc)
        layout.addSpacing(4)

        self._history_limit_spin = SpinBox()
        self._history_limit_spin.setRange(0, 10000)
        self._history_limit_spin.setValue(
            self._config.get_clipboard_history_limit()
        )
        self._history_limit_spin.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed
        )
        self._history_limit_spin.valueChanged.connect(
            self._config.set_clipboard_history_limit
        )
        layout.addWidget(self._history_limit_spin)

    def retranslate(self):
        self.title_label.setText(_tr("剪贴板管理"))
        self.subtitle_label.setText(_clipboard_subtitle())
        if hasattr(self, "_hotkey_lbl") and self._hotkey_lbl:
            self._hotkey_lbl.setText(_tr("快捷键（最多设置两个）"))
        if hasattr(self, "_hotkey_desc") and self._hotkey_desc:
            self._hotkey_desc.setText(_tr("程序还会尝试注册 Win+V 作为额外备用。"))
        if hasattr(self, "_history_limit_lbl"):
            self._history_limit_lbl.setText(_settings_tr("History Limit"))
        if hasattr(self, "_history_limit_desc"):
            self._history_limit_desc.setText(
                _settings_tr("Maximum number of items to keep (0 = unlimited)")
            )
        # 级联刷新插画区中的类型、分组和示例文字。
        if hasattr(self.illus_area, "retranslate"):
            self.illus_area.retranslate()

    def save(self):
        key = self._hotkey.text().strip()
        if key:
            self._config.set_clipboard_hotkey(key)
        key2 = self._hotkey2.text().strip()
        self._config.set_clipboard_hotkey_2(key2)
        self._config.set_clipboard_history_limit(
            self._history_limit_spin.value()
        )


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()
    mock.get_show_main_window = lambda: False
    mock.get_autostart = lambda: False

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(2)   # 跳到第3页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 
