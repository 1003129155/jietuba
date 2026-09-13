# -*- coding: utf-8 -*-
"""
第3页 — 剪贴板管理快捷键设置

上半部：剪贴板内容类型、内容分组与快速启动的连续动画
下半部：快捷键设置
"""

from PySide6.QtWidgets import (
    QVBoxLayout, QHBoxLayout, QLabel, QPushButton, QWidget, QSizePolicy
)
from PySide6.QtCore import Qt, QTimer, QRectF, QPointF
from PySide6.QtGui import (
    QPainter, QColor, QPen, QFont, QFontMetrics, QPainterPath, QPolygonF,
)
from core import safe_event
from core.i18n import make_tr
from core.logger import log_exception, T
from ui.fluent_lite import SpinBox, ComboBox

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
    # 插画里的字号按真实窗口的默认值等比缩放
    BASE_FONT_SIZE = 17
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
        self._connect_clipboard_appearance()
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

    # ── 跟随真实剪贴板外观 ────────────────────────────
    # 预览是派生状态：订阅主题管理器的信号，而不是让三个下拉框各自回来戳一下。
    # 设置页改这三项时走的也是同一组信号，两边因此自动一致。

    def _connect_clipboard_appearance(self):
        self._font_scale = 1.0
        self._opacity_factor = 1.0
        self._refresh_appearance_from_config()
        try:
            from clipboard.ui.theme.themes import get_theme_manager
            manager = get_theme_manager()
        except Exception as e:
            log_exception(e, T("订阅剪贴板外观变化"))
            return
        manager.theme_changed.connect(lambda *_: self.update())
        manager.font_size_changed.connect(self._on_font_size_changed)
        manager.opacity_changed.connect(self._on_opacity_changed)

    def _refresh_appearance_from_config(self):
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            self._on_font_size_changed(config.get_clipboard_font_size())
            self._on_opacity_changed(config.get_clipboard_window_opacity())
        except Exception as e:
            log_exception(e, T("读取剪贴板外观设置"))

    def _on_font_size_changed(self, size: int):
        self._font_scale = max(0.5, min(2.0, int(size) / self.BASE_FONT_SIZE))
        self.update()

    def _on_opacity_changed(self, percent: int):
        # 配置语义是「数值越大越透明」，0 表示完全不透明。下限留一点，
        # 免得最高档把预览画成一片空白。
        percent = max(0, min(100, int(percent)))
        self._opacity_factor = max(0.3, 1.0 - percent / 100.0)
        self.update()

    def set_animation_time(self, elapsed_ms: int):
        """设置演示时间，供预览和自动化测试稳定检查各阶段。"""
        self._elapsed_ms = max(0, int(elapsed_ms)) % self.DURATION_MS
        self.update()

    def _font(self, pixel_size: int, *, bold=False) -> QFont:
        """使用系统字体回退，保证中、日、韩文字不会变成方框。

        插画里的字号是按真实窗口比例画的，跟随「字体大小」设置整体缩放，
        用户改档位时预览会跟着变粗变细。
        """
        font = QFont()
        font.setPixelSize(max(7, round(pixel_size * self._font_scale)))
        font.setBold(bold)
        return font

    @staticmethod
    def _with_alpha(color, alpha: int) -> QColor:
        result = QColor(color)
        result.setAlpha(max(0, min(255, int(alpha))))
        return result

    def _draw_centered_text(
        self, p: QPainter, rect: QRectF, text: str, color, size=12, bold=False
    ):
        # 跟着 _font 一起改成实例方法：字号要乘上当前的「字体大小」档位
        p.setPen(QColor(color))
        p.setFont(self._font(size, bold=bold))
        p.drawText(rect, Qt.AlignmentFlag.AlignCenter, text)

    # 本插画的语义色 → ThemeColors 的字段名。
    #
    # 配色直接取自真正的剪贴板主题，不再另抄一份：抄的那份一旦和 themes.py
    # 漂移，向导里的预览就会和真实剪贴板对不上，而且没有任何迹象能让人发现。
    # 这也是「选了主题，左边预览立刻变成那个样子」的前提。
    PALETTE_FIELDS = {
        "primary": "bg_primary",
        "secondary": "bg_secondary",
        "alternate": "bg_tertiary",
        "hover": "bg_hover",
        "selected": "bg_selected",
        "text": "text_primary",
        "muted": "text_tertiary",
        "border": "border_primary",
        "accent": "accent_primary",
        "shortcut": "shortcut_key_color",
        "danger": "error",
        "success": "success",
    }

    @classmethod
    def _palette(cls, _welcome_theme=None):
        """当前剪贴板主题的配色；取不到时退回浅色默认值。"""
        try:
            from clipboard.ui.theme.themes import get_theme_manager
            colors = get_theme_manager().get_current_theme().colors
        except Exception as e:
            log_exception(e, T("读取剪贴板主题配色"))
            from clipboard.ui.theme.themes import ThemeColors
            colors = ThemeColors()
        return {
            key: getattr(colors, field)
            for key, field in cls.PALETTE_FIELDS.items()
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
        fade = 1.0
        if self._elapsed_ms < 350:
            fade = max(0.05, self._elapsed_ms / 350.0)
        elif self._elapsed_ms > self.DURATION_MS - 500:
            fade = max(0.05, (self.DURATION_MS - self._elapsed_ms) / 500.0)
        # 窗口透明度是剪贴板窗口自身的属性，一并乘进来，预览才如实反映它
        p.setOpacity(fade * self._opacity_factor)
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


class _ThemeSwatchRow(QWidget):
    """一排主题色块，点选即应用。

    设置窗口那边用的是「一枚色块按钮 + 弹出菜单」，这里直接摊开：向导有横向
    空间，而且左边就是实时预览，摊开选比点开菜单再关掉更顺。两处共用同一张
    色板（clipboard.ui.theme.themes.PRESET_THEME_SWATCHES），不会各画各的。
    """

    SWATCH_W = 30
    SWATCH_H = 24

    def __init__(self, current: str, on_pick, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent;")
        from clipboard.ui.theme.themes import PRESET_THEME_SWATCHES

        self._on_pick = on_pick
        self._buttons = {}
        self._current = current if current in PRESET_THEME_SWATCHES else "light"

        row = QHBoxLayout(self)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(6)
        for name, (accent, background) in PRESET_THEME_SWATCHES.items():
            button = QPushButton(self)
            button.setFixedSize(self.SWATCH_W, self.SWATCH_H)
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            button.setToolTip(name)
            button.clicked.connect(lambda _checked=False, n=name: self._pick(n))
            self._buttons[name] = (button, accent, background)
            row.addWidget(button)
        row.addStretch()
        self._restyle()

    def current(self) -> str:
        return self._current

    def _pick(self, name: str):
        if name == self._current:
            return
        self._current = name
        self._restyle()
        self._on_pick(name)

    def _restyle(self, _tokens=None):
        theme = welcome_theme()
        for name, (button, accent, background) in self._buttons.items():
            selected = name == self._current
            border = theme.accent if selected else theme.border_strong
            width = 2 if selected else 1
            button.setStyleSheet(f"""
                QPushButton {{
                    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                        stop:0 {background}, stop:0.5 {background},
                        stop:0.5 {accent}, stop:1 {accent});
                    border: {width}px solid {border};
                    border-radius: 4px;
                }}
                QPushButton:hover {{ border: 2px solid {theme.accent}; }}
            """)

    # BasePage 在主题切换时会遍历子控件调用这个钩子
    def _apply_welcome_child_theme(self, tokens=None):
        self._restyle(tokens)


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

        self._build_appearance_controls(layout)

    def _build_appearance_controls(self, layout: QVBoxLayout):
        """主题 / 字体大小 / 透明度。

        这三项都是即改即生效：通过 ThemeManager 写配置并广播，左边的预览订阅
        了同一组信号，所以选什么当场就能看到。也因此它们不需要在 save() 里再
        写一次。
        """
        from clipboard.ui.theme.themes import get_theme_manager
        theme_manager = get_theme_manager()

        layout.addSpacing(14)
        self._appearance_lbl = QLabel(_tr("剪贴板外观"))
        set_welcome_label_style(
            self._appearance_lbl, role="primary", font_size=14, weight=600
        )
        layout.addWidget(self._appearance_lbl)
        layout.addSpacing(6)

        # 当前主题问 ThemeManager 而不是问配置：写也是走它（set_theme 自己负责
        # 落盘并广播），读写同源才不会出现「配置里是 A、界面上高亮 B」。
        self._theme_row = _ThemeSwatchRow(
            theme_manager.get_current_theme().name, theme_manager.set_theme
        )
        row, self._theme_lbl, _ = self._make_setting_row_with_refs(
            _settings_tr("Theme"), self._theme_row
        )
        layout.addWidget(row)

        layout.addSpacing(8)
        self._font_combo = ComboBox()
        self._font_combo.setFixedWidth(124)
        self._font_combo.setFixedHeight(32)
        self._font_combo.setCursor(Qt.CursorShape.PointingHandCursor)
        for size in self._config.get_clipboard_font_size_options():
            self._font_combo.addItem(f"{size}px", userData=size)
        index = self._font_combo.findData(self._config.get_clipboard_font_size())
        if index >= 0:
            self._font_combo.setCurrentIndex(index)
        self._font_combo.currentIndexChanged.connect(self._on_font_size_picked)
        row, self._font_lbl, _ = self._make_setting_row_with_refs(
            _settings_tr("Font Size"), self._font_combo
        )
        layout.addWidget(row)

        layout.addSpacing(8)
        self._opacity_combo = ComboBox()
        self._opacity_combo.setFixedWidth(124)
        self._opacity_combo.setFixedHeight(32)
        self._opacity_combo.setCursor(Qt.CursorShape.PointingHandCursor)
        self._opacity_options = list(
            self._config.get_clipboard_window_opacity_options()
        )
        self._populate_opacity_combo()
        index = self._opacity_combo.findData(
            self._config.get_clipboard_window_opacity()
        )
        if index >= 0:
            self._opacity_combo.setCurrentIndex(index)
        self._opacity_combo.currentIndexChanged.connect(self._on_opacity_picked)
        row, self._opacity_lbl, _ = self._make_setting_row_with_refs(
            _settings_tr("Opacity"), self._opacity_combo
        )
        layout.addWidget(row)

    def _populate_opacity_combo(self):
        """0 档显示成「不透明」而不是 0%，与设置窗口一致。"""
        blocked = self._opacity_combo.blockSignals(True)
        current = self._opacity_combo.currentData()
        self._opacity_combo.clear()
        for percent in self._opacity_options:
            label = _settings_tr("Opaque") if percent == 0 else f"{percent}%"
            self._opacity_combo.addItem(label, userData=percent)
        if current is not None:
            index = self._opacity_combo.findData(current)
            if index >= 0:
                self._opacity_combo.setCurrentIndex(index)
        self._opacity_combo.blockSignals(blocked)

    def _on_font_size_picked(self, index: int):
        size = self._font_combo.itemData(index)
        if size is None:
            return
        from clipboard.ui.theme.themes import get_theme_manager
        self._config.set_clipboard_font_size(size)
        get_theme_manager().notify_font_size_changed(size)

    def _on_opacity_picked(self, index: int):
        percent = self._opacity_combo.itemData(index)
        if percent is None:
            return
        from clipboard.ui.theme.themes import get_theme_manager
        self._config.set_clipboard_window_opacity(percent)
        get_theme_manager().notify_opacity_changed(percent)

    def retranslate(self):
        self.title_label.setText(_tr("剪贴板管理"))
        self.subtitle_label.setText(_clipboard_subtitle())
        if hasattr(self, "_appearance_lbl"):
            self._appearance_lbl.setText(_tr("剪贴板外观"))
        if hasattr(self, "_theme_lbl"):
            self._theme_lbl.setText(_settings_tr("Theme"))
        if hasattr(self, "_font_lbl"):
            self._font_lbl.setText(_settings_tr("Font Size"))
        if hasattr(self, "_opacity_lbl"):
            self._opacity_lbl.setText(_settings_tr("Opacity"))
        if hasattr(self, "_opacity_combo"):
            # 「不透明」这一档是翻译出来的，语言切换后要重新生成
            self._populate_opacity_combo()
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
    w._stack.setCurrentIndex(3)   # 跳到剪贴板页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 
