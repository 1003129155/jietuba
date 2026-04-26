# -*- coding: utf-8 -*-
"""
第3页 — 剪贴板管理快捷键设置

上半部：工具栏位置选择预览（左=右侧栏 / 右=顶部栏），可点击选择
下半部：开启开关 + 快捷键设置
"""

from PySide6.QtWidgets import (
    QVBoxLayout, QHBoxLayout, QLabel, QWidget, QFrame, QSizePolicy
)
from PySide6.QtCore import Qt, Signal, QTimer
from PySide6.QtGui import QPainter, QColor, QPen, QFont
from core import safe_event
from core.i18n import make_tr

if __package__:
    from .base_page import BasePage, IllustrationArea, ToggleSwitch, ACCENT, TEXT_PRIMARY, TEXT_SECOND
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import BasePage, IllustrationArea, ToggleSwitch, ACCENT, TEXT_PRIMARY, TEXT_SECOND


_tr = make_tr("WelcomeWizard")


# ── 剪贴板动画组件（复用原样式代码） ────────────────────────
class _ClipAnim(QWidget):
    _ITEM_KEYS = ["Hello World", "192.168.1.1", "会议纪要.docx", "https://example.com"]

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent;")
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)  # 让鼠标事件穿透到父卡片
        self._offset = 0.0
        self._refresh_items()
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(16)

    def _refresh_items(self):
        self.ITEMS = [_tr(key) for key in self._ITEM_KEYS]

    def retranslate(self):
        self._refresh_items()

    def _tick(self):
        # 窗口大小如果改变可能会引发抖动，使用整数避免浮点渲染像素抖动
        self._offset = (self._offset + 0.4)
        if self._offset >= 48:
            self._offset = 0.0
        self.update()

    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()

        item_h = 36
        item_w = min(w - 20, 260)
        
        # 确保尺寸计算不会因为微小的浮动而不断变化（取整）
        item_w = int(item_w)
        
        x = (w - item_w) // 2
        
        # 使用整数，并且限制在一定范围内绘制，避免因为窗口布局尺寸拉伸时的浮点计算抖动
        start_y = int((h - item_h * 3.5) / 2) - int(self._offset)

        for i, text in enumerate(self.ITEMS):
            y = start_y + i * (item_h + 6)
            if y < -item_h or y > h:
                continue

            # 卡片阴影感
            alpha = max(0, min(255, int(220 - abs(y - h // 2 + item_h) * 1.2)))
            p.setBrush(QColor(255, 255, 255, alpha))
            p.setPen(QPen(QColor(220, 228, 240, alpha), 1))
            p.drawRoundedRect(x, y, item_w, item_h, 6, 6)

            # 彩色左边条
            colors = ["#2196F3", "#4CAF50", "#FF9800", "#9C27B0"]
            p.setBrush(QColor(colors[i % len(colors)]))
            p.setPen(Qt.PenStyle.NoPen)
            p.drawRoundedRect(x, y, 4, item_h, 2, 2)

            # 文字
            p.setPen(QColor(TEXT_PRIMARY))
            p.setFont(QFont("Microsoft YaHei", 9))
            p.drawText(x + 14, y, item_w - 14, item_h,
                       Qt.AlignmentFlag.AlignVCenter, text)


# ── 大尺寸布局选择卡片 ─────────────────────────────────────
class _LayoutOption(QFrame):
    """
    可点击的布局预览大图区域。
    mode: "right" 工具栏在右侧  /  "top" 工具栏在顶部
    """
    clicked = Signal(str)

    def __init__(self, mode: str, label_text: str, parent=None):
        super().__init__(parent)
        self._mode = mode
        self._selected = False
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

        vl = QVBoxLayout(self)
        vl.setContentsMargins(8, 0, 8, 8)
        vl.setSpacing(6)

        self._inner_frame = QFrame()
        self._inner_frame.setObjectName("InnerFrame")

        # 内部采用真实布局排版动画列表和模拟工具栏
        if mode == "right":    
            hl = QHBoxLayout(self._inner_frame)
            hl.setContentsMargins(1, 1, 1, 1)
            hl.setSpacing(0)
            self._anim = _ClipAnim()
            self._toolbar = QFrame()
            self._toolbar.setFixedWidth(40)
            self._toolbar.setStyleSheet("background: #EBF2FA; border-top-right-radius: 7px; border-bottom-right-radius: 7px; border-left: 1px solid #DCE4F0;")
            self._toolbar.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
            self._add_mock_buttons(self._toolbar, is_vertical=True)
            hl.addWidget(self._anim, 1)
            hl.addWidget(self._toolbar, 0)
        else:
            vl2 = QVBoxLayout(self._inner_frame)
            vl2.setContentsMargins(1, 1, 1, 1)
            vl2.setSpacing(0)
            self._toolbar = QFrame()
            self._toolbar.setFixedHeight(40)
            self._toolbar.setStyleSheet("background: #EBF2FA; border-top-left-radius: 7px; border-top-right-radius: 7px; border-bottom: 1px solid #DCE4F0;")
            self._toolbar.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
            self._add_mock_buttons(self._toolbar, is_vertical=False)
            self._anim = _ClipAnim()
            vl2.addWidget(self._toolbar, 0)
            vl2.addWidget(self._anim, 1)

        vl.addWidget(self._inner_frame, 1)

        self._lbl = QLabel(label_text)
        self._lbl.setFixedHeight(24)
        self._lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        vl.addWidget(self._lbl, 0)

        self.set_selected(False)

    def _add_mock_buttons(self, parent_widget, is_vertical):
        l = QVBoxLayout() if is_vertical else QHBoxLayout()
        l.setContentsMargins(10, 10, 10, 10)
        l.setSpacing(10)
        if is_vertical:
            l.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter)
        else:
            l.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        parent_widget.setLayout(l)
        for _ in range(3):
            circle = QFrame()
            circle.setFixedSize(16, 16)
            circle.setStyleSheet("background: #C4D3E8; border-radius: 8px;")
            l.addWidget(circle)

    def set_selected(self, sel: bool):
        self._selected = sel
        if sel:
            self._inner_frame.setStyleSheet(f"""
                #InnerFrame {{
                    background: #FFFFFF;
                    border: 2px solid {ACCENT};
                    border-radius: 8px;
                }}
            """)
            self._lbl.setStyleSheet(f"color: {ACCENT}; font-weight: bold; font-size: 13px;")
        else:
            self._inner_frame.setStyleSheet("""
                #InnerFrame {
                    background: #F8FAFC;
                    border: 1px solid #DCE4F0;
                    border-radius: 8px;
                }
            """)
            self._lbl.setStyleSheet(f"color: {TEXT_PRIMARY}; font-weight: normal; font-size: 13px;")

    @safe_event
    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit(self._mode)
        super().mousePressEvent(event)

    def retranslate(self):
        self._anim.retranslate()


# ── 插画区：工具栏位置选择器 ────────────────────────────
class _ToolbarPositionIllus(IllustrationArea):
    """
    插画区分为左右两列大预览。点击直接改变工具栏位置配置。
    """
    position_changed = Signal(str)   # 供外部连接

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(parent)

    def _build_content(self):
        self._layout.setContentsMargins(20, 12, 20, 0)
        # 标题
        self._title_lbl = QLabel("请选择工具栏布局首选项")
        self._title_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._title_lbl.setStyleSheet(
            f"font-size: 14px; font-weight: 600; color: {TEXT_PRIMARY}; background: transparent;"
        )
        self._layout.addWidget(self._title_lbl)
        self._layout.addSpacing(12)

        # 两张大卡片横排
        row = QWidget()
        row.setStyleSheet("background: transparent;")
        row.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        hl = QHBoxLayout(row)
        hl.setContentsMargins(0, 0, 0, 0)
        hl.setSpacing(24)

        self._card_right = _LayoutOption("right", "工具栏在右侧")
        self._card_top   = _LayoutOption("top",   "工具栏在顶部")

        self._card_right.clicked.connect(self._on_select)
        self._card_top.clicked.connect(self._on_select)

        hl.addWidget(self._card_right, 1)
        hl.addWidget(self._card_top, 1)
        self._layout.addWidget(row, 1)

        # 初始化选中状态
        current = self._config.get_clipboard_group_bar_position()
        self._apply_selection(current)

    def _on_select(self, mode: str):
        self._config.set_clipboard_group_bar_position(mode)
        self._apply_selection(mode)
        self.position_changed.emit(mode)

    def _apply_selection(self, mode: str):
        self._card_right.set_selected(mode == "right")
        self._card_top.set_selected(mode == "top")

    def retranslate(self):
        self._title_lbl.setText(_tr("请选择工具栏布局首选项"))
        self._card_right._lbl.setText(_tr("工具栏在右侧"))
        self._card_top._lbl.setText(_tr("工具栏在顶部"))
        self._card_right.retranslate()
        self._card_top.retranslate()


# ── 页面主体 ────────────────────────────────────────────
class ClipboardHotkeyPage(BasePage):
    """第3页：剪贴板快捷键"""

    def __init__(self, config_manager, parent=None):
        self._config = config_manager
        super().__init__(
            title="剪贴板管理",
            subtitle="自动记录每一次复制，随时召唤历史内容。\n"
                     "支持文本、图片、文件，还能分组管理。",
            parent=parent,
        )

    def _create_illustration(self):
        return _ToolbarPositionIllus(self._config, self)

    def _build_controls(self, layout: QVBoxLayout):
        if __package__:
            from ..hotkey_edit import HotkeyEdit
        else:
            import sys, os
            sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
            from hotkey_edit import HotkeyEdit

        # 启用开关
        self._enable_toggle = ToggleSwitch()
        self._enable_toggle.setChecked(self._config.get_clipboard_enabled())
        row0, self._row0_lbl, self._row0_desc = self._make_setting_row_with_refs(
            "启用剪贴板管理",
            self._enable_toggle,
            "关闭后不会记录复制内容，快捷键也不会激活窗口。"
        )
        layout.addWidget(row0)

        from PySide6.QtWidgets import QWidget, QLabel

        # 快捷键说明标签
        self._hotkey_lbl = QLabel("快捷键（最多设置两个）")
        self._hotkey_lbl.setStyleSheet(
            f"font-size: 14px; font-weight: 600; color: {TEXT_PRIMARY}; background: transparent;"
        )

        self._hotkey_desc = QLabel("程序还会尝试注册 Win+V 作为额外备用。")
        self._hotkey_desc.setWordWrap(True)
        self._hotkey_desc.setStyleSheet(
            f"font-size: 12px; color: {TEXT_SECOND}; background: transparent;"
        )

        # 两个快捷键输入框上下排列
        self._hotkey = HotkeyEdit()
        self._hotkey.setFixedWidth(200)
        self._hotkey.setText(self._config.get_clipboard_hotkey())

        self._hotkey2 = HotkeyEdit()
        self._hotkey2.setFixedWidth(200)
        self._hotkey2.setText(self._config.get_clipboard_hotkey_2())

        layout.addWidget(self._hotkey_lbl)
        layout.addWidget(self._hotkey_desc)
        layout.addSpacing(4)
        layout.addWidget(self._hotkey)
        layout.addSpacing(6)
        layout.addWidget(self._hotkey2)

    def retranslate(self):
        self.title_label.setText(_tr("剪贴板管理"))
        self.subtitle_label.setText(_tr(
            "自动记录每一次复制，随时召唤历史内容。\n"
            "支持文本、图片、文件，还能分组管理。"))
        if hasattr(self, "_row0_lbl") and self._row0_lbl:
            self._row0_lbl.setText(_tr("启用剪贴板管理"))
        if hasattr(self, "_row0_desc") and self._row0_desc:
            self._row0_desc.setText(_tr("关闭后不会记录复制内容，快捷键也不会激活窗口。"))
        if hasattr(self, "_hotkey_lbl") and self._hotkey_lbl:
            self._hotkey_lbl.setText(_tr("快捷键（最多设置两个）"))
        if hasattr(self, "_hotkey_desc") and self._hotkey_desc:
            self._hotkey_desc.setText(_tr("程序还会尝试注册 Win+V 作为额外备用。"))
        # 级联刷新插画区（工具栏位置预览文字）
        if hasattr(self.illus_area, "retranslate"):
            self.illus_area.retranslate()

    def save(self):
        self._config.set_clipboard_enabled(self._enable_toggle.isChecked())
        key = self._hotkey.text().strip()
        if key:
            self._config.set_clipboard_hotkey(key)
        key2 = self._hotkey2.text().strip()
        self._config.set_clipboard_hotkey_2(key2)


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
    # 为 _MockConfig 增加模拟方法
    if hasattr(mock, '__class__'):
        mock.get_clipboard_group_bar_position = lambda: "right"
        mock.set_clipboard_group_bar_position = lambda mode: None
    w = WelcomeWizard(mock)
    w._stack.setCurrentIndex(2)   # 跳到第3页
    w._update_nav()
    w.show()
    sys.exit(app.exec())
 