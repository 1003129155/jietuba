# -*- coding: utf-8 -*-
"""WelcomeWizard — 五步产品初始化向导。"""

from PySide6.QtWidgets import (
    QVBoxLayout, QHBoxLayout, QWidget,
    QLabel, QStackedWidget, QFrame
)
from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QIcon

from core.i18n import make_tr
from core.logger import log_exception, T
from core import safe_event
from core.ui_theme import UIThemeManager, get_ui_theme
from ui.fluent_lite import (
    PushButton as FluentPushButton,
    PrimaryPushButton,
    TransparentPushButton,
    FluentTitleBar,
    FrostedFramelessDialog,
)

from main_app import APP_VERSION

if __package__:
    from .base_page import (
        ACCENT, ACCENT_DARK, TEXT_PRIMARY, TEXT_SECOND, BG_PAGE,
        PRODUCT_NAME, brand_text, welcome_theme,
    )
else:
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from base_page import (
        ACCENT, ACCENT_DARK, TEXT_PRIMARY, TEXT_SECOND, BG_PAGE,
        PRODUCT_NAME, brand_text, welcome_theme,
    )


_tr = make_tr("WelcomeWizard")


# ── 左侧步骤项 ───────────────────────────────────────────
class _StepItem(QFrame):
    clicked = Signal(int)

    def __init__(self, index: int, parent=None):
        super().__init__(parent)
        self._index = index
        self._hovered = False
        self.setObjectName("StepItem")
        self.setFixedHeight(52)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

        row = QHBoxLayout(self)
        row.setContentsMargins(10, 7, 10, 7)
        row.setSpacing(11)

        self._number = QLabel(str(index + 1), self)
        self._number.setObjectName("StepNumber")
        self._number.setFixedSize(28, 28)
        self._number.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._number.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

        self._text = QLabel("", self)
        self._text.setObjectName("StepText")
        self._text.setWordWrap(False)
        self._text.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

        row.addWidget(self._number)
        row.addWidget(self._text, 1)
        self.set_state(False, False)

    def set_text(self, text: str):
        self._text.setText(text)
        self._text.setToolTip(text)
        self.setAccessibleName(text)

    def set_state(self, active: bool, completed: bool):
        self._active = active
        self._completed = completed
        self._apply_welcome_theme()

    def _apply_welcome_theme(self, _tokens=None):
        theme = welcome_theme()
        active = getattr(self, "_active", False)
        completed = getattr(self, "_completed", False)
        interactive = self._hovered or self.hasFocus()
        if active:
            self.setStyleSheet(f"""
                #StepItem {{
                    background: {theme.accent_soft};
                    border: 1px solid {theme.border_strong};
                    border-radius: 10px;
                }}
                #StepNumber {{
                    background: {theme.accent};
                    color: #FFFFFF;
                    border: none;
                    border-radius: 14px;
                    font-size: 12px;
                    font-weight: 700;
                }}
                #StepText {{
                    color: {theme.text};
                    background: transparent;
                    border: none;
                    font-size: 13px;
                    font-weight: 700;
                }}
            """)
        elif completed:
            background = theme.panel_subtle if interactive else "transparent"
            border = theme.border if interactive else "transparent"
            self.setStyleSheet(f"""
                #StepItem {{
                    background: {background};
                    border: 1px solid {border};
                    border-radius: 10px;
                }}
                #StepNumber {{
                    background: {theme.panel};
                    color: {theme.accent};
                    border: 1px solid {theme.border_strong};
                    border-radius: 14px;
                    font-size: 12px;
                    font-weight: 700;
                }}
                #StepText {{
                    color: {theme.text};
                    background: transparent;
                    border: none;
                    font-size: 13px;
                    font-weight: 600;
                }}
            """)
        else:
            background = theme.panel_subtle if interactive else "transparent"
            border = theme.border if interactive else "transparent"
            self.setStyleSheet(f"""
                #StepItem {{
                    background: {background};
                    border: 1px solid {border};
                    border-radius: 10px;
                }}
                #StepNumber {{
                    background: transparent;
                    color: {theme.text_soft};
                    border: 1px solid {theme.border_strong};
                    border-radius: 14px;
                    font-size: 12px;
                    font-weight: 600;
                }}
                #StepText {{
                    color: {theme.text_muted};
                    background: transparent;
                    border: none;
                    font-size: 13px;
                    font-weight: 500;
                }}
            """)

    def enterEvent(self, event):
        self._hovered = True
        self._apply_welcome_theme()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self._hovered = False
        self._apply_welcome_theme()
        super().leaveEvent(event)

    def focusInEvent(self, event):
        self._apply_welcome_theme()
        super().focusInEvent(event)

    def focusOutEvent(self, event):
        self._apply_welcome_theme()
        super().focusOutEvent(event)

    def mouseReleaseEvent(self, event):
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self.rect().contains(event.position().toPoint())
        ):
            self.clicked.emit(self._index)
        super().mouseReleaseEvent(event)

    def keyPressEvent(self, event):
        if event.key() in (
            Qt.Key.Key_Return,
            Qt.Key.Key_Enter,
            Qt.Key.Key_Space,
        ):
            self.clicked.emit(self._index)
            event.accept()
            return
        super().keyPressEvent(event)


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


class WelcomeWizard(FrostedFramelessDialog):
    """欢迎向导对话框"""

    PAGE_COUNT = 5
    WINDOW_W = 960
    WINDOW_H = 680

    def __init__(self, config_manager, parent=None):
        super().__init__(parent)
        self._config = config_manager
        self._current = 0
        self._theme_manager = get_ui_theme()

        self._setup_titlebar()

        # ── 第一步：检测并加载语言，必须在创建任何页面之前 ──
        self._init_language()

        self.setWindowTitle(brand_text(_tr("欢迎使用截图吧")))
        # 必须先设置 WindowFlags（会重建原生窗口句柄），再调用 setFixedSize，
        # 否则 setWindowFlags 会清除固定大小约束，导致拖动时布局反复重算、高度抖动。
        # MSWindowsFixedSizeDialogHint 在 Windows 上额外锁定窗口不可调整大小。
        self.setWindowFlags(self.windowFlags() | Qt.WindowType.MSWindowsFixedSizeDialogHint)
        self.setFixedSize(self.WINDOW_W, self.WINDOW_H)
        self.setStyleSheet("background: transparent; border: none;")

        self._build_ui()
        self._build_pages()
        self._theme_manager.theme_changed.connect(self._apply_welcome_theme)
        self._apply_welcome_theme(self._theme_manager.tokens)
        self._refresh_theme_scope()
        self._update_nav()

        # ── 第二步：所有页面创建完成后，刷新一遍文字 ──
        # 这样页面显示的就是检测到的语言，而不是硬编码中文
        self.retranslate_ui()

    def _setup_titlebar(self):
        title_bar = FluentTitleBar(self)
        self.setTitleBar(title_bar)
        title_bar.iconLabel.hide()
        title_bar.maxBtn.hide()
        title_bar.minBtn.hide()
        # 隐藏图标后标题会紧贴窗口左边，看起来像被裁歪了；保留原生标题栏留白。
        title_bar.hBoxLayout.setContentsMargins(12, 0, 0, 0)
        title_bar.setDoubleClickEnabled(False)

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
            log_exception(e, T("WelcomeWizard 语言初始化"))

    # ── UI 构建 ──────────────────────────────────────────
    def _build_ui(self):
        root = QVBoxLayout(self)
        title_height = self.titleBar.height() if getattr(self, "titleBar", None) else 32
        root.setContentsMargins(8, title_height + 4, 8, 8)
        root.setSpacing(0)

        shell = QFrame(self)
        shell.setObjectName("WizardSurface")
        self._shell = shell
        shell_layout = QHBoxLayout(shell)
        shell_layout.setContentsMargins(0, 0, 0, 0)
        shell_layout.setSpacing(0)

        # 左侧：品牌与流程导航
        sidebar = QFrame(shell)
        sidebar.setObjectName("WizardSidebar")
        sidebar.setFixedWidth(224)
        self._sidebar = sidebar
        side_layout = QVBoxLayout(sidebar)
        side_layout.setContentsMargins(20, 23, 20, 20)
        side_layout.setSpacing(4)

        brand_row = QHBoxLayout()
        brand_row.setSpacing(11)
        self._brand_icon = QLabel(sidebar)
        self._brand_icon.setFixedSize(38, 38)
        self._brand_icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        try:
            from core.resource_manager import ResourceManager
            icon = QIcon(ResourceManager.get_resource_path("svg/托盘.svg"))
            if not icon.isNull():
                self._brand_icon.setPixmap(icon.pixmap(24, 24))
        except Exception as e:
            log_exception(e, T("欢迎向导品牌图标"))

        brand_text = QVBoxLayout()
        brand_text.setSpacing(0)
        self._brand_name = QLabel(PRODUCT_NAME, sidebar)
        self._brand_meta = QLabel("PRODUCT SETUP", sidebar)
        brand_text.addWidget(self._brand_name)
        brand_text.addWidget(self._brand_meta)
        brand_row.addWidget(self._brand_icon)
        brand_row.addLayout(brand_text, 1)
        side_layout.addLayout(brand_row)
        side_layout.addSpacing(24)

        self._step_items = []
        for idx in range(self.PAGE_COUNT):
            item = _StepItem(idx, sidebar)
            item.clicked.connect(self._go_to_page)
            self._step_items.append(item)
            side_layout.addWidget(item)

        side_layout.addStretch()
        self._setup_meta = QLabel(f"v{APP_VERSION}", sidebar)
        self._setup_meta.setObjectName("SetupMeta")
        side_layout.addWidget(self._setup_meta)

        # 右侧：页面与导航
        right = QWidget(shell)
        right.setObjectName("WizardContent")
        self._right = right
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(0)

        self._stack = QStackedWidget(right)
        right_layout.addWidget(self._stack, 1)

        line = QFrame(right)
        line.setFixedHeight(1)
        self._nav_line = line
        right_layout.addWidget(line)

        nav = QWidget(right)
        nav.setFixedHeight(66)
        self._nav = nav
        nav_layout = QHBoxLayout(nav)
        nav_layout.setContentsMargins(30, 14, 30, 14)
        nav_layout.setSpacing(10)

        self._btn_skip = TransparentPushButton()
        self._btn_skip.setFixedHeight(36)
        self._btn_skip.setCursor(Qt.CursorShape.PointingHandCursor)
        self._btn_skip.clicked.connect(self._finish)

        self._page_count = QLabel(nav)

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
        nav_layout.addWidget(self._page_count)
        nav_layout.addSpacing(8)
        nav_layout.addWidget(self._btn_back)
        nav_layout.addWidget(self._btn_next)

        right_layout.addWidget(nav)
        shell_layout.addWidget(sidebar)
        shell_layout.addWidget(right, 1)
        root.addWidget(shell, 1)

    def _apply_welcome_theme(self, tokens=None):
        theme = welcome_theme()
        self.setPalette(UIThemeManager.build_palette(get_ui_theme().tokens))
        self._shell.setStyleSheet(f"""
            #WizardSurface {{
                background: {theme.page};
                border: 1px solid {theme.border};
                border-radius: 14px;
            }}
        """)
        self._sidebar.setStyleSheet(f"""
            #WizardSidebar {{
                background: {theme.sidebar};
                border: none;
                border-right: 1px solid {theme.border};
                border-radius: 14px;
            }}
        """)
        self._right.setStyleSheet(
            f"#WizardContent {{ background: {theme.page}; border: none; }}"
        )
        self._nav.setStyleSheet(f"background: {theme.page}; border: none;")
        self._nav_line.setStyleSheet(
            f"background: {theme.separator}; border: none;"
        )
        self._brand_icon.setStyleSheet(
            f"background: {theme.panel}; border: 1px solid {theme.border};"
            " border-radius: 10px;"
        )
        self._brand_name.setStyleSheet(
            f"font-size: 17px; font-weight: 700; color: {theme.text};"
            " background: transparent;"
        )
        self._brand_meta.setStyleSheet(
            f"font-size: 9px; font-weight: 600; letter-spacing: 1px;"
            f" color: {theme.text_soft}; background: transparent;"
        )
        self._setup_meta.setStyleSheet(
            f"font-size: 10px; font-weight: 600; color: {theme.text_soft};"
            " background: transparent; letter-spacing: 1px;"
        )
        self._page_count.setStyleSheet(
            f"font-size: 12px; font-weight: 600; color: {theme.text_muted};"
            " background: transparent;"
        )
        for item in self._step_items:
            item._apply_welcome_theme(tokens)
        for page in getattr(self, "_pages", []):
            page._apply_welcome_theme(tokens)
        self.update()

    def _build_pages(self):
        if __package__:
            from .page1_welcome import WelcomePage
            from .page2_screenshot import ScreenshotHotkeyPage
            from .page3_clipboard import ClipboardHotkeyPage
            from .page5_translation import TranslationPage
            from .page6_finish import FinishPage
        else:
            from page1_welcome import WelcomePage
            from page2_screenshot import ScreenshotHotkeyPage
            from page3_clipboard import ClipboardHotkeyPage
            from page5_translation import TranslationPage
            from page6_finish import FinishPage

        self._pages = [
            WelcomePage(self._config),
            ScreenshotHotkeyPage(self._config),
            ClipboardHotkeyPage(self._config),
            TranslationPage(self._config),
            FinishPage(self._config),
        ]
        for page in self._pages:
            self._stack.addWidget(page)
        self._refresh_step_labels()

    def _refresh_theme_scope(self):
        """Repolish controls that were constructed before being parented."""
        tokens = get_ui_theme().tokens
        for widget in self.findChildren(QWidget):
            apply_theme = getattr(widget, "_apply_theme", None)
            if callable(apply_theme):
                apply_theme(tokens)
            else:
                widget.update()

    # ── 导航逻辑 ─────────────────────────────────────────
    def _go_to_page(self, index: int):
        """统一处理侧栏、前进和后退触发的页面跳转。"""
        if not 0 <= index < self.PAGE_COUNT:
            return
        self._current = index
        self._stack.setCurrentIndex(index)
        self._update_nav()

    def _go_next(self):
        if self._current < self.PAGE_COUNT - 1:
            self._go_to_page(self._current + 1)
        else:
            self._finish()

    def _go_back(self):
        if self._current > 0:
            self._go_to_page(self._current - 1)

    def _update_nav(self):
        for idx, item in enumerate(self._step_items):
            item.set_state(idx == self._current, idx < self._current)
        self._page_count.setText(f"{self._current + 1} / {self.PAGE_COUNT}")
        self._btn_back.setEnabled(self._current > 0)
        is_last = self._current == self.PAGE_COUNT - 1
        self._btn_skip.setVisible(not is_last)

        self._btn_back.setText(_tr("上一步"))
        self._btn_back.setToolTip(_tr("上一步"))
        self._btn_back.setIcon(None)

        if is_last:
            self._btn_next.setText(_tr("完成"))
            self._btn_next.setToolTip(_tr("完成"))
            self._btn_next.setIcon(None)
        else:
            self._btn_next.setText(_tr("下一步"))
            self._btn_next.setToolTip(_tr("下一步"))
            self._btn_next.setIcon(None)

    def retranslate_ui(self):
        """语言切换后刷新向导所有文字（导航按钮 + 各页面）。"""
        # 刷新窗口标题
        self.setWindowTitle(brand_text(_tr("欢迎使用截图吧")))

        self._btn_skip.setText(_tr("跳过"))
        self._update_nav()

        # 通知每个页面刷新
        for page in self._pages:
            if hasattr(page, "retranslate"):
                try:
                    page.retranslate()
                except Exception as e:
                    log_exception(e, T("向导页面刷新翻译"))
        self._refresh_step_labels()

    def _refresh_step_labels(self):
        """让侧栏标题始终与页面语言和实际标题保持一致。"""
        if not hasattr(self, "_pages") or not hasattr(self, "_step_items"):
            return
        for item, page in zip(self._step_items, self._pages):
            title_label = getattr(page, "title_label", None)
            item.set_text(title_label.text() if title_label is not None else "")

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
