# -*- coding: utf-8 -*-
"""
第2页 — 截图设置

上半部：工具图标轮播（紧凑版）
下半部：截图快捷键 + 截图保存位置
"""

from PySide6.QtWidgets import (
    QVBoxLayout, QHBoxLayout, QLabel, QWidget, QGraphicsOpacityEffect,
    QFileDialog, QSizePolicy,
)
from PySide6.QtCore import Qt, QSize, QTimer, QPropertyAnimation, QEasingCurve
from PySide6.QtGui import QIcon, QPixmap, QPainter, QColor, QPen
from ui.fluent_lite import PushButton, FluentIcon, LineEdit
from ui.fluent_lite.theme import to_qicon
from core.i18n import make_tr

if __package__:
    from .base_page import (
        BasePage, IllustrationArea, ACCENT, TEXT_PRIMARY, TEXT_SECOND, BG_ILLUS,
        welcome_theme, set_welcome_label_style, apply_welcome_label_style,
    )
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import (
        BasePage, IllustrationArea, ACCENT, TEXT_PRIMARY, TEXT_SECOND, BG_ILLUS,
        welcome_theme, set_welcome_label_style, apply_welcome_label_style,
    )


_tr = make_tr("WelcomeWizard")


# ── 工具列表：(svg路径, 中文名key, 中文描述key)
_TOOLS = [
    ("svg/画笔.svg",   "工具_画笔",   "工具描述_画笔"),
    ("svg/荧光笔.svg", "工具_荧光笔", "工具描述_荧光笔"),
    ("svg/箭头.svg",   "工具_箭头",   "工具描述_箭头"),
    ("svg/序号.svg",   "工具_序号",   "工具描述_序号"),
    ("svg/方框.svg",   "工具_矩形框", "工具描述_矩形框"),
    ("svg/圆框.svg",   "工具_椭圆框", "工具描述_椭圆框"),
    ("svg/文字.svg",   "工具_文字",   "工具描述_文字"),
    ("svg/橡皮.svg",   "工具_橡皮擦", "工具描述_橡皮擦"),
    ("svg/长截图.svg", "工具_长截图", "工具描述_长截图"),
    ("svg/gif.svg",    "工具_GIF录制","工具描述_GIF录制"),
    ("svg/翻译.svg",   "工具_截图翻译","工具描述_截图翻译"),
    ("svg/钉图.svg",   "工具_钉图",   "工具描述_钉图"),
    ("svg/下载.svg",   "工具_保存",   "工具描述_保存"),
    ("svg/确定.svg",   "工具_完成截图","工具描述_完成截图"),
]

_TOOL_ZH = {
    "工具_画笔":     "画笔",
    "工具_荧光笔":   "荧光笔",
    "工具_箭头":     "箭头",
    "工具_序号":     "序号",
    "工具_矩形框":   "矩形框",
    "工具_椭圆框":   "椭圆框",
    "工具_文字":     "文字",
    "工具_橡皮擦":   "橡皮擦",
    "工具_长截图":   "长截图",
    "工具_GIF录制":  "GIF 录制",
    "工具_截图翻译": "截图翻译",
    "工具_钉图":     "钉图",
    "工具_保存":     "保存",
    "工具_完成截图": "完成截图",
    "工具描述_画笔":     "自由绘制线条，Shift 画直线",
    "工具描述_荧光笔":   "半透明高亮，标记重点区域",
    "工具描述_箭头":     "绘制箭头，多种样式可选",
    "工具描述_序号":     "标注①②③…有序说明步骤",
    "工具描述_矩形框":   "圈出矩形区域，支持填充",
    "工具描述_椭圆框":   "圈出椭圆区域，支持填充",
    "工具描述_文字":     "在截图上添加文字说明",
    "工具描述_橡皮擦":   "擦除已绘制的标注内容",
    "工具描述_长截图":   "自动滚动拼接超长页面",
    "工具描述_GIF录制":  "录制屏幕区域为 GIF 动图",
    "工具描述_截图翻译": "OCR 识别文字并即时翻译",
    "工具描述_钉图":     "将截图悬浮置顶在屏幕上",
    "工具描述_保存":     "保存截图到本地文件",
    "工具描述_完成截图": "复制到剪贴板或保存文件",
}


def _tool_text(key: str) -> str:
    translated = _tr(key)
    if translated and translated != key:
        return translated
    return _TOOL_ZH.get(key, key)


# 双栏向导中的紧凑工具矩阵
_ICON_NORMAL  = 23
_ICON_ACTIVE  = 29
_BTN_NORMAL   = 36
_BTN_ACTIVE   = 44
_ANIM_MS      = 2000


def _get_path_fn():
    try:
        from core.resource_manager import ResourceManager
        return ResourceManager.get_resource_path
    except Exception:
        import os
        _base = os.path.normpath(
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
        )
        return lambda rel: os.path.join(_base, rel)


class _IconCell(QWidget):
    def __init__(self, svg_rel: str, get_path, parent=None):
        super().__init__(parent)
        self._get_path = get_path
        self._svg_rel = svg_rel
        self._active = False
        self.setFixedSize(_BTN_ACTIVE, _BTN_ACTIVE)
        self._lbl = QLabel(self)
        self._lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._lbl.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self._render(False)

    def _render(self, active: bool):
        theme = welcome_theme()
        icon_sz = _ICON_ACTIVE if active else _ICON_NORMAL
        btn_sz  = _BTN_ACTIVE  if active else _BTN_NORMAL
        offset = (_BTN_ACTIVE - btn_sz) // 2
        self._lbl.setGeometry(offset, offset, btn_sz, btn_sz)
        icon = to_qicon(self._get_path(self._svg_rel), self)
        if not icon.isNull():
            pix = icon.pixmap(QSize(icon_sz, icon_sz))
        else:
            pix = QPixmap(icon_sz, icon_sz)
            pix.fill(Qt.GlobalColor.transparent)
        bg = QPixmap(btn_sz, btn_sz)
        bg.fill(Qt.GlobalColor.transparent)
        painter = QPainter(bg)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        if active:
            painter.setPen(QPen(QColor(theme.accent), 2))
            painter.setBrush(QColor(theme.accent_soft))
        else:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(theme.panel_subtle))
        painter.drawRoundedRect(0, 0, btn_sz, btn_sz, 10, 10)
        ix = (btn_sz - icon_sz) // 2
        painter.drawPixmap(ix, ix, pix)
        painter.end()
        self._lbl.setPixmap(bg)

    def set_active(self, active: bool):
        if self._active == active:
            return
        self._active = active
        self._render(active)


class _ToolPreviewIllus(IllustrationArea):
    """紧凑版工具轮播插画区"""

    def _build_content(self):
        self._layout.setContentsMargins(14, 16, 14, 12)
        self._layout.setSpacing(5)

        self._get_path = _get_path_fn()
        self._cells: list[_IconCell] = []
        self._current = -1

        # 三行图标更适合横向向导里的窄预览栏
        rows = [_TOOLS[:5], _TOOLS[5:10], _TOOLS[10:]]
        for row_items in rows:
            row_w = QWidget()
            row_w.setStyleSheet("background: transparent;")
            row_l = QHBoxLayout(row_w)
            row_l.setContentsMargins(0, 0, 0, 0)
            row_l.setSpacing(3)
            row_l.addStretch()
            for svg_rel, _nk, _dk in row_items:
                cell = _IconCell(svg_rel, self._get_path, row_w)
                row_l.addWidget(cell)
                self._cells.append(cell)
            row_l.addStretch()
            self._layout.addWidget(row_w)

        # 名称 + 说明文字（紧凑）
        info_w = QWidget()
        info_w.setStyleSheet("background: transparent;")
        info_l = QVBoxLayout(info_w)
        info_l.setContentsMargins(8, 2, 8, 0)
        info_l.setSpacing(1)

        self._name_lbl = QLabel()
        self._name_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        set_welcome_label_style(
            self._name_lbl, role="accent", font_size=13, weight=700
        )
        self._desc_lbl = QLabel()
        self._desc_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._desc_lbl.setWordWrap(True)
        set_welcome_label_style(
            self._desc_lbl, role="muted", font_size=11, weight=400
        )

        self._opacity_effect = QGraphicsOpacityEffect(info_w)
        info_w.setGraphicsEffect(self._opacity_effect)
        self._fade_anim = QPropertyAnimation(self._opacity_effect, b"opacity", self)
        self._fade_anim.setDuration(300)
        self._fade_anim.setEasingCurve(QEasingCurve.Type.OutCubic)

        info_l.addWidget(self._name_lbl)
        info_l.addWidget(self._desc_lbl)
        self._layout.addWidget(info_w)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._advance)
        self._timer.start(_ANIM_MS)
        self._advance()

    def _advance(self):
        n = len(_TOOLS)
        if 0 <= self._current < n:
            self._cells[self._current].set_active(False)
        self._current = (self._current + 1) % n
        self._cells[self._current].set_active(True)
        _svg, name_key, desc_key = _TOOLS[self._current]
        self._name_lbl.setText(_tool_text(name_key))
        self._desc_lbl.setText(_tool_text(desc_key))
        self._opacity_effect.setOpacity(0.0)
        self._fade_anim.stop()
        self._fade_anim.setStartValue(0.0)
        self._fade_anim.setEndValue(1.0)
        self._fade_anim.start()

    def retranslate(self):
        if 0 <= self._current < len(_TOOLS):
            _svg, name_key, desc_key = _TOOLS[self._current]
            self._name_lbl.setText(_tool_text(name_key))
            self._desc_lbl.setText(_tool_text(desc_key))

    def _apply_welcome_child_theme(self, _tokens=None):
        for cell in self._cells:
            cell._render(cell._active)
        apply_welcome_label_style(self._name_lbl)
        apply_welcome_label_style(self._desc_lbl)
        self.update()


# ── 页面主体 ────────────────────────────────────────────
class ScreenshotHotkeyPage(BasePage):
    """第2页：截图设置（快捷键 + 保存位置）"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title=_tr("📸 截图设置").replace("📸", "").strip(),
            subtitle=_tr("设置快捷键与截图的默认保存位置。"),
            parent=parent,
        )

    def _create_illustration(self):
        return _ToolPreviewIllus(self)

    def _build_controls(self, layout: QVBoxLayout):
        if __package__:
            from ..hotkey_edit import HotkeyEdit
        else:
            import sys, os
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
            from hotkey_edit import HotkeyEdit

        # ── 快捷键区 ──────────────────────────────────
        self._hotkey_lbl = QLabel("快捷键（最多设置两个）")
        set_welcome_label_style(
            self._hotkey_lbl, role="primary", font_size=14, weight=600
        )
        layout.addWidget(self._hotkey_lbl)

        self._hotkey_desc = QLabel("单击输入框后，直接按下目标组合键即可录入。")
        self._hotkey_desc.setWordWrap(True)
        set_welcome_label_style(
            self._hotkey_desc, role="muted", font_size=12, weight=400
        )
        layout.addWidget(self._hotkey_desc)
        layout.addSpacing(4)

        # 两个快捷键横排
        hotkey_row = QHBoxLayout()
        hotkey_row.setSpacing(8)
        self._hotkey = HotkeyEdit()
        self._hotkey.setMinimumWidth(138)
        self._hotkey.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._hotkey.setText(self._config.get_hotkey())
        self._hotkey2 = HotkeyEdit()
        self._hotkey2.setMinimumWidth(138)
        self._hotkey2.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._hotkey2.setText(self._config.get_hotkey_2())
        hotkey_row.addWidget(self._hotkey, 1)
        hotkey_row.addWidget(self._hotkey2, 1)
        layout.addLayout(hotkey_row)

        layout.addSpacing(14)

        # ── 保存位置区 ────────────────────────────────
        self._save_lbl = QLabel("截图保存位置")
        set_welcome_label_style(
            self._save_lbl, role="primary", font_size=14, weight=600
        )
        layout.addWidget(self._save_lbl)

        self._save_desc = QLabel("确认截图后自动保存到该文件夹。")
        self._save_desc.setWordWrap(True)
        set_welcome_label_style(
            self._save_desc, role="muted", font_size=12, weight=400
        )
        layout.addWidget(self._save_desc)
        layout.addSpacing(4)

        # 路径输入框 + 浏览按钮横排
        path_row = QHBoxLayout()
        path_row.setSpacing(6)

        self._path_edit = LineEdit()
        self._path_edit.setText(self._config.get_screenshot_save_path())
        self._path_edit.setPlaceholderText("...")
        self._path_edit.setFixedHeight(36)
        self._path_edit.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

        self._browse_btn = PushButton(_tr("浏览"))
        self._browse_btn.setIcon(FluentIcon.FOLDER)
        self._browse_btn.setFixedHeight(36)
        self._browse_btn.setMinimumWidth(92)
        self._browse_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._browse_btn.clicked.connect(self._browse_path)

        path_row.addWidget(self._path_edit)
        path_row.addWidget(self._browse_btn)
        layout.addLayout(path_row)

    def _browse_path(self):
        current = self._path_edit.text().strip()
        folder = QFileDialog.getExistingDirectory(
            self, _tr("选择保存文件夹"), current or ""
        )
        if folder:
            self._path_edit.setText(folder)

    def retranslate(self):
        self.title_label.setText(_tr("📸 截图设置").replace("📸", "").strip())
        self.subtitle_label.setText(_tr("设置快捷键与截图的默认保存位置。"))
        if hasattr(self, "_hotkey_lbl"):
            self._hotkey_lbl.setText(_tr("快捷键（最多设置两个）"))
        if hasattr(self, "_hotkey_desc"):
            self._hotkey_desc.setText(_tr("单击输入框后，直接按下目标组合键即可录入。"))
        if hasattr(self, "_save_lbl"):
            self._save_lbl.setText(_tr("截图保存位置"))
        if hasattr(self, "_save_desc"):
            self._save_desc.setText(_tr("确认截图后自动保存到该文件夹。"))
        if hasattr(self, "_browse_btn"):
            self._browse_btn.setText(_tr("浏览"))
        if hasattr(self, "illus_area") and hasattr(self.illus_area, "retranslate"):
            self.illus_area.retranslate()

    def save(self):
        key = self._hotkey.text().strip()
        if key:
            self._config.set_hotkey(key)
        key2 = self._hotkey2.text().strip()
        self._config.set_hotkey_2(key2)
        path = self._path_edit.text().strip()
        if path:
            self._config.set_screenshot_save_path(path)
            import os
            os.makedirs(path, exist_ok=True)


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(1)
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 
