# -*- coding: utf-8 -*-
"""
WelcomeWizard — 欢迎向导主窗口

承载 6 个页面，底部有进度点 + 上一步/下一步/完成按钮。
（本版本：左侧仅 Skip，右侧仅两个三角按钮 ◀ ▶）
"""

from PySide6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QWidget,
    QLabel, QStackedWidget, QFrame
)
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QPainter, QPen

from core.logger import log_exception
from core import safe_event
from qfluentwidgets import (
    PushButton as FluentPushButton,
    PrimaryPushButton,
    TransparentPushButton,
)

if __package__:
    from .base_page import ACCENT, ACCENT_DARK, TEXT_PRIMARY, TEXT_SECOND, BG_PAGE
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import ACCENT, ACCENT_DARK, TEXT_PRIMARY, TEXT_SECOND, BG_PAGE


# ── 进度点指示器 ─────────────────────────────────────────
class _DotIndicator(QWidget):
    def __init__(self, count: int, parent=None):
        super().__init__(parent)
        self._count = count
        self._current = 0
        self.setFixedHeight(16)
        self.setMinimumWidth(count * 20)

    def set_current(self, idx: int):
        self._current = idx
        self.update()

    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w = self.width()
        dot_r = 4
        gap = 14
        total_w = self._count * (dot_r * 2) + (self._count - 1) * (gap - dot_r * 2)
        x = (w - total_w) // 2
        cy = self.height() // 2
        for i in range(self._count):
            if i == self._current:
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(QColor(ACCENT))
                p.drawEllipse(x, cy - dot_r, dot_r * 2, dot_r * 2)
            else:
                p.setPen(QPen(QColor("#C0CDD8"), 1))
                p.setBrush(QColor("#E8EEF4"))
                p.drawEllipse(x, cy - dot_r + 1, (dot_r - 1) * 2, (dot_r - 1) * 2)
            x += gap


class WelcomeWizard(QDialog):
    """欢迎向导对话框"""

    PAGE_COUNT = 6
    WINDOW_W = 620
    WINDOW_H = 720

    def __init__(self, config_manager, parent=None):
        super().__init__(parent)
        self._config = config_manager
        self._current = 0

        # ── 第一步：检测并加载语言，必须在创建任何页面之前 ──
        self._init_language()

        self.setWindowTitle("欢迎使用截图吧")
        # 必须先设置 WindowFlags（会重建原生窗口句柄），再调用 setFixedSize，
        # 否则 setWindowFlags 会清除固定大小约束，导致拖动时布局反复重算、高度抖动。
        # MSWindowsFixedSizeDialogHint 在 Windows 上额外锁定窗口不可调整大小。
        self.setWindowFlags(
            Qt.WindowType.Dialog |
            Qt.WindowType.WindowTitleHint |
            Qt.WindowType.WindowCloseButtonHint |
            Qt.WindowType.MSWindowsFixedSizeDialogHint
        )
        self.setFixedSize(self.WINDOW_W, self.WINDOW_H)
        self.setStyleSheet(f"background: {BG_PAGE}; border: none;")

        self._build_ui()
        self._build_pages()
        self._update_nav()

        # ── 第二步：所有页面创建完成后，刷新一遍文字 ──
        # 这样页面显示的就是检测到的语言，而不是硬编码中文
        self.retranslate_ui()

    def _init_language(self):
        """程序启动时检测系统语言（或读取已保存语言），加载对应翻译文件。"""
        try:
            from core.i18n import I18nManager

            saved = self._config.get_app_setting("language", "") or ""
            if saved and saved in I18nManager.LANGUAGES:
                init_lang = saved
            else:
                init_lang = I18nManager.get_system_language()

            if I18nManager.get_current_language() != init_lang:
                I18nManager.load_language(init_lang)
        except Exception as e:
            log_exception(e, "WelcomeWizard 语言初始化")

    # ── UI 构建 ──────────────────────────────────────────
    def _build_ui(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # 页面堆叠区
        self._stack = QStackedWidget()
        root.addWidget(self._stack, 1)

        # 分隔线
        line = QFrame()
        line.setFrameShape(QFrame.Shape.HLine)
        line.setFixedHeight(1)
        line.setStyleSheet("background: #E8EEF4; border: none;")
        root.addWidget(line)

        # 底部导航栏
        nav = QWidget()
        nav.setFixedHeight(62)
        nav.setStyleSheet(f"background: {BG_PAGE};")
        nav_layout = QHBoxLayout(nav)
        nav_layout.setContentsMargins(24, 12, 24, 12)
        nav_layout.setSpacing(12)

        self._btn_skip = TransparentPushButton()
        self._btn_skip.setFixedHeight(36)
        self._btn_skip.setCursor(Qt.CursorShape.PointingHandCursor)
        self._btn_skip.clicked.connect(self._finish)

        self._dots = _DotIndicator(self.PAGE_COUNT)

        self._btn_back = FluentPushButton()
        self._btn_back.setObjectName("btnBack")
        self._btn_back.setFixedSize(96, 36)
        self._btn_back.setCursor(Qt.CursorShape.PointingHandCursor)
        self._btn_back.clicked.connect(self._go_back)

        self._btn_next = PrimaryPushButton()
        self._btn_next.setObjectName("btnNext")
        self._btn_next.setFixedSize(112, 36)
        self._btn_next.setCursor(Qt.CursorShape.PointingHandCursor)
        self._btn_next.clicked.connect(self._go_next)

        nav_layout.addWidget(self._btn_skip)
        nav_layout.addStretch()
        nav_layout.addWidget(self._dots)
        nav_layout.addStretch()
        nav_layout.addWidget(self._btn_back)
        nav_layout.addWidget(self._btn_next)

        root.addWidget(nav)

    def _build_pages(self):
        if __package__:
            from .page1_welcome import WelcomePage
            from .page2_screenshot import ScreenshotHotkeyPage
            from .page3_clipboard import ClipboardHotkeyPage
            from .page4_smart_select import SmartSelectPage
            from .page5_translation import TranslationPage
            from .page6_finish import FinishPage
        else:
            from page1_welcome import WelcomePage
            from page2_screenshot import ScreenshotHotkeyPage
            from page3_clipboard import ClipboardHotkeyPage
            from page4_smart_select import SmartSelectPage
            from page5_translation import TranslationPage
            from page6_finish import FinishPage

        self._pages = [
            WelcomePage(self._config),
            ScreenshotHotkeyPage(self._config),
            ClipboardHotkeyPage(self._config),
            SmartSelectPage(self._config),
            TranslationPage(self._config),
            FinishPage(self._config),
        ]
        for page in self._pages:
            self._stack.addWidget(page)

    # ── 导航逻辑 ─────────────────────────────────────────
    def _go_next(self):
        if self._current < self.PAGE_COUNT - 1:
            self._current += 1
            self._stack.setCurrentIndex(self._current)
            self._update_nav()
        else:
            self._finish()

    def _go_back(self):
        if self._current > 0:
            self._current -= 1
            self._stack.setCurrentIndex(self._current)
            self._update_nav()

    def _update_nav(self):
        self._dots.set_current(self._current)
        self._btn_back.setEnabled(self._current > 0)
        is_last = self._current == self.PAGE_COUNT - 1
        self._btn_skip.setVisible(not is_last)

        self._btn_back.setText(self.tr("上一步"))
        self._btn_back.setToolTip(self.tr("上一步"))
        self._btn_back.setIcon(None)

        if is_last:
            self._btn_next.setText(self.tr("完成"))
            self._btn_next.setToolTip(self.tr("完成"))
            self._btn_next.setIcon(None)
        else:
            self._btn_next.setText(self.tr("下一步"))
            self._btn_next.setToolTip(self.tr("下一步"))
            self._btn_next.setIcon(None)

    def retranslate_ui(self):
        """语言切换后刷新向导所有文字（导航按钮 + 各页面）。"""
        from PySide6.QtCore import QCoreApplication
        def tr(s): return QCoreApplication.translate("WelcomeWizard", s) or s

        # 刷新窗口标题
        self.setWindowTitle(tr("欢迎使用截图吧"))

        self._btn_skip.setText(tr("跳过"))
        self._update_nav()

        # 通知每个页面刷新
        for page in self._pages:
            if hasattr(page, "retranslate"):
                try:
                    page.retranslate()
                except Exception as e:
                    log_exception(e, "向导页面刷新翻译")

    def _save_all(self):
        """保存所有页面设置并标记向导已运行（仅执行一次）"""
        if getattr(self, "_saved", False):
            return
        self._saved = True
        for page in self._pages:
            try:
                page.save()
            except Exception as e:
                log_exception(e, f"WelcomeWizard save {page.__class__.__name__}")
        if hasattr(self._config, "mark_as_run"):
            self._config.mark_as_run()

    def _finish(self):
        """完成向导：保存设置，关闭对话框"""
        self._save_all()
        self.accept()

    # ── 关闭行为：关闭按钮也算完成 ──────────────────────
    @safe_event
    def closeEvent(self, event):
        # 无论是正常完成、Skip 还是点 X 关闭，都保存所有页面设置并标记已运行
        self._save_all()
        super().closeEvent(event)


if __name__ == "__main__":
    # 完整向导预览（所有6页 + 导航）
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import _dev_bootstrap
    mock = _dev_bootstrap()

    from PySide6.QtWidgets import QApplication
    from wizard import WelcomeWizard

    app = QApplication(sys.argv)
    w = WelcomeWizard(mock)
    w.show()
    sys.exit(app.exec()) 