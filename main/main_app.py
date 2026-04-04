"""应用主程序 - 系统托盘集成和全局快捷键管理

负责一次性初始化和管理应用的生命周期，包括系统托盘图标、快捷键钩子、
多窗口实例管理和启动流程。
"""

import sys
import os
import ctypes

from core import safe_event


from PySide6.QtWidgets import QApplication, QSystemTrayIcon, QMenu, QStyle
from PySide6.QtGui import QIcon, QAction, QPixmap, QPainter, QPen, QBrush, QColor
from PySide6.QtCore import QObject, Qt, QRect, QPoint
from ui.dialogs import show_warning_dialog, show_error_dialog

from core.shortcut_manager import HotkeySystem
from core.constants import CSS_FONT_FAMILY_UI
from settings import get_tool_settings_manager
from ui.screenshot_window import ScreenshotWindow
from core.logger import (
    setup_logger, get_logger,
    log_debug, log_info, log_warning, log_error, log_exception
)

class CustomTrayMenu(QMenu):
    @safe_event
    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.RightButton:
            # 阻止右键在托盘菜单上确认操作
            return
        super().mouseReleaseEvent(event)

def create_app_icon():
    """创建应用程序图标 - 加载SVG"""
    from core.resource_manager import ResourceManager
    icon_path = ResourceManager.get_resource_path("svg/托盘.svg")
    
    if os.path.exists(icon_path):
        # 加载SVG并放大
        pixmap = QPixmap(64, 64)  # 放大到64x64
        pixmap.fill(Qt.GlobalColor.transparent)
        
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        
        icon_pixmap = QIcon(icon_path).pixmap(64, 64)
        painter.drawPixmap(0, 0, icon_pixmap)
        painter.end()
        
        return QIcon(pixmap)

class MainApp(QObject):
    def __init__(self):
        super().__init__()
        self.app = QApplication(sys.argv)
        self.app.setQuitOnLastWindowClosed(False)
        
        # Config - 使用统一的设置管理器
        self.config_manager = get_tool_settings_manager()
        
        # Logger - 日志初始化，
        setup_logger(self.config_manager)
        self._logger = get_logger()
        self.app.aboutToQuit.connect(self._on_about_to_quit)
        
        # 初始化翻译系统
        from core.i18n import I18nManager
        # 检查是否有保存的语言设置，如果没有则使用系统语言
        saved_lang = self.config_manager.get_app_setting("language", "__NOT_SET__")
        if saved_lang == "__NOT_SET__":
            # 第一次启动，检测系统语言
            saved_lang = I18nManager.get_system_language()
            self.config_manager.set_app_setting("language", saved_lang)
            log_info(f"首次启动，检测到系统语言: {saved_lang}", "I18n")
        I18nManager.load_language(saved_lang)
        log_info(f"语言设置: {I18nManager.get_current_language_name()}", "I18n")
        
        # 连接语言切换信号，用于更新托盘菜单等 UI
        I18nManager.instance().language_changed.connect(self._on_language_changed)
        
        # 初始化主题颜色管理器
        from core.theme import get_theme
        get_theme().init(self.config_manager)
        
        # 输出DPI信息用于调试
        try:
            from PySide6.QtGui import QGuiApplication
            primary_screen = QGuiApplication.primaryScreen()
            if primary_screen:
                dpr = primary_screen.devicePixelRatio()
                logical_dpi = primary_screen.logicalDotsPerInch()
                physical_dpi = primary_screen.physicalDotsPerInch()
                log_debug(f"Device Pixel Ratio: {dpr}", "DPI")
                log_debug(f"Logical DPI: {logical_dpi}", "DPI")
                log_debug(f"Physical DPI: {physical_dpi}", "DPI")
        except Exception as e:
            log_warning(f"无法获取DPI信息: {e}", "DPI")
        
        # 热键初始（首次运行时跳过，等向导完成后再注册；非首次直接注册）
        self.hotkey_system = HotkeySystem()
        if not self.config_manager.is_first_run():
            self.update_hotkey()
        
        # 系统托盘
        self.setup_tray()
        
        # 窗口实例
        self.settings_window = None
        self.screenshot_window = None
        self.clipboard_window = None
        # 剪贴板管理器
        self.clipboard_manager = None
        
        # 启动预加载链（字体 → 截图模块 → 工具栏 → OCR → 设置窗口 → 剪贴板 → 显示主界面）
        from core.bootstrap import PreloadManager
        self._preloader = PreloadManager(self)
        self._preloader.build_and_start()

    def _on_about_to_quit(self):
        """应用退出前收尾"""
        try:
            if hasattr(self, "_logger") and self._logger:
                self._logger.close()
        except Exception as e:
            log_exception(e, "关闭logger")

    def _on_wizard_requested(self):
        """设置窗口请求打开向导：隐藏设置窗口、注销热键，再显示向导，完成后恢复"""
        log_info("向导请求：隐藏设置窗口并注销热键", "MainApp")

        # 1. 隐藏设置窗口
        if self.settings_window:
            self.settings_window.hide()

        # 2. 注销所有热键（向导期间不拦截快捷键）
        self.hotkey_system.unregister_all()

        # 3. 显示向导
        try:
            from ui.welcome import WelcomeWizard
            wizard = WelcomeWizard(self.config_manager)
            wizard.exec()
        except Exception as e:
            log_exception(e, "向导启动失败")

        # 4. 向导结束后重新注册热键
        self.update_hotkey()
        log_info("向导完成，热键已恢复", "MainApp")

    def _on_language_changed(self, lang_code: str):
        """语言切换时更新所有 UI 元素"""
        log_debug(f"语言已切换到: {lang_code}，更新 UI", "I18n")
        
        # 更新托盘菜单
        self._update_tray_menu()
        
        # 重新创建设置窗口（因为设置窗口是预加载的，需要重建才能更新翻译）
        if self.settings_window:
            was_visible = self.settings_window.isVisible()
            self.settings_window.close()
            self.settings_window.deleteLater()
            self.settings_window = None
            
            # 重新创建设置窗口
            self._preloader.preload_settings()
            
            # 如果之前是显示状态，重新显示
            if was_visible:
                self.settings_window.show()
                self.settings_window.activateWindow()
        
        # 关闭翻译窗口（下次打开时会用新语言创建）
        from translation import TranslationManager
        manager = TranslationManager.instance()
        if manager._dialog:
            manager._dialog.close()

    def _create_tray_menu(self) -> QMenu:
        """创建托盘菜单"""
        from core.theme import get_theme
        tc = get_theme().theme_color_hex
        menu = CustomTrayMenu()
        menu.setStyleSheet(f"""
            QMenu {{
                background-color: white;
                border: 1px solid #ccc;
                border-radius: 4px;
                padding: 4px;
                font-family: {CSS_FONT_FAMILY_UI};
                font-size: 9pt;
                color: #000000;
            }}
            QMenu::item {{
                padding: 6px 12px;
                border-radius: 3px;
                color: #000000;
                background-color: transparent;
            }}
            QMenu::item:selected {{
                background-color: {tc};
                color: #ffffff;
            }}
            QMenu::item:disabled {{
                color: #9e9e9e;
            }}
            QMenu::separator {{
                height: 1px;
                background: #ddd;
                margin: 4px 6px;
            }}
        """)

        action_screenshot = QAction(self.tr("Screenshot"), self)
        action_screenshot.triggered.connect(self.start_screenshot)
        menu.addAction(action_screenshot)

        action_clipboard = QAction(self.tr("Clipboard"), self)
        action_clipboard.triggered.connect(self.open_clipboard_window)
        menu.addAction(action_clipboard)

        action_translate = QAction(self.tr("Translation"), self)
        action_translate.triggered.connect(self.open_translator)
        menu.addAction(action_translate)

        action_settings = QAction(self.tr("Settings"), self)
        action_settings.triggered.connect(self.open_settings)
        menu.addAction(action_settings)

        menu.addSeparator()

        action_quit = QAction(self.tr("Exit"), self)
        action_quit.triggered.connect(self.quit_app)
        menu.addAction(action_quit)

        return menu

    def _update_tray_menu(self):
        """重建托盘菜单（用于语言切换后刷新）"""
        if not hasattr(self, 'tray_icon') or not self.tray_icon:
            return

        # 更新 tooltip
        self.tray_icon.setToolTip(self.tr("jietuba - Click to screenshot"))

        # 重建菜单
        self.tray_icon.setContextMenu(self._create_tray_menu())

    def update_hotkey(self, show_error: bool = False):
        """
        更新所有全局热键（截图热键 + 剪切板热键）
        
        Args:
            show_error: 是否显示错误提示（设置保存时为 True，启动时为 False）
        """
        
        # 注销所有已注册的热键
        self.hotkey_system.unregister_all()
        
        failed_hotkeys = []  # 收集注册失败的热键
        
        # 注册截图热键
        hotkey = self.config_manager.get_hotkey()
        if hotkey:
            if self.hotkey_system.register_hotkey(hotkey, self.start_screenshot):
                log_info(f"截图热键已注册: {hotkey}", "Hotkey")
            else:
                log_warning(f"截图热键注册失败: {hotkey}", "Hotkey")
                failed_hotkeys.append((self.tr("Screenshot"), hotkey))

        # 注册截图备用热键
        hotkey_2 = self.config_manager.get_hotkey_2()
        if hotkey_2:
            if self.hotkey_system.register_hotkey(hotkey_2, self.start_screenshot):
                log_info(f"截图备用热键已注册: {hotkey_2}", "Hotkey")
            else:
                log_warning(f"截图备用热键注册失败: {hotkey_2}", "Hotkey")
                failed_hotkeys.append((self.tr("Screenshot (2)"), hotkey_2))
        
        # 注册剪切板热键（如果剪切板功能启用）
        if self.config_manager.get_clipboard_enabled():
            clipboard_hotkey = self.config_manager.get_clipboard_hotkey()
            if clipboard_hotkey:
                if self.hotkey_system.register_hotkey(clipboard_hotkey, self.open_clipboard_window):
                    log_info(f"剪贴板热键已注册: {clipboard_hotkey}", "Hotkey")
                else:
                    log_warning(f"剪贴板热键注册失败: {clipboard_hotkey}", "Hotkey")
                    failed_hotkeys.append((self.tr("Clipboard"), clipboard_hotkey))
            
            # 注册剪贴板备用热键
            clipboard_hotkey_2 = self.config_manager.get_clipboard_hotkey_2()
            if clipboard_hotkey_2:
                if self.hotkey_system.register_hotkey(clipboard_hotkey_2, self.open_clipboard_window):
                    log_info(f"剪贴板备用热键已注册: {clipboard_hotkey_2}", "Hotkey")
                else:
                    log_warning(f"剪贴板备用热键注册失败: {clipboard_hotkey_2}", "Hotkey")
                    failed_hotkeys.append((self.tr("Clipboard (2)"), clipboard_hotkey_2))

            # 额外尝试注册 Win+V，失败也不提示
            if self.hotkey_system.register_hotkey("win+v", self.open_clipboard_window):
                log_info("剪贴板备用热键已注册: win+v", "Hotkey")
            else:
                log_info("剪贴板备用热键 win+v 注册失败（可能被系统占用）", "Hotkey")
        
        # 如果有注册失败的热键且需要显示提示
        if show_error and failed_hotkeys:
            self._show_hotkey_error(failed_hotkeys)
    
    def _show_hotkey_error(self, failed_hotkeys: list):
        """显示热键注册失败的提示"""
        
        lines = []
        for name, key in failed_hotkeys:
            lines.append(f"• {name}: {key}")
        
        msg = self.tr("The following hotkeys failed to register:") + "\n\n"
        msg += "\n".join(lines)
        msg += "\n\n" + self.tr("The hotkey may be occupied by other programs. Please try a different combination.")
        
        log_debug(f"显示热键错误提示: {failed_hotkeys}", "Hotkey")
        
        show_warning_dialog(
            None,
            self.tr("Hotkey Registration Failed"),
            msg,
        )

    def setup_tray(self):
        if not QSystemTrayIcon.isSystemTrayAvailable():
            show_error_dialog(None, "Error", "System tray not available")
        self.tray_icon = QSystemTrayIcon(self)

        # Use custom icon
        icon = create_app_icon()
        self.tray_icon.setIcon(icon)

        self.tray_icon.setToolTip(self.tr("jietuba - Click to screenshot"))

        # Menu
        self.tray_icon.setContextMenu(self._create_tray_menu())
        self.tray_icon.activated.connect(self.on_tray_activated)
        self.tray_icon.show()

    def on_tray_activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.Trigger:
            self.start_screenshot()
            
    def start_screenshot(self):
        """启动截图 - 管理截图窗口生命周期"""
        
        # 已有截图窗口且会话活跃 → 忽略重复触发，并把焦点还给截图窗口
        if self.screenshot_window and getattr(self.screenshot_window, '_session_active', False):
            log_debug("截图窗口已存在，忽略重复触发", "MainApp")
            self.screenshot_window.activateWindow()
            self.screenshot_window.raise_()
            # 如果有颜色选择器正在显示，重新提到截图窗口上方，防止被全屏窗口遮挡
            from PySide6.QtWidgets import QApplication, QColorDialog
            for w in QApplication.topLevelWidgets():
                if isinstance(w, QColorDialog) and w.isVisible():
                    w.raise_()
                    w.activateWindow()
                    return
            self.screenshot_window.setFocus()
            return
        
        # 后台截图线程正在运行时也忽略重复触发
        if getattr(self, '_capture_thread', None) and self._capture_thread.isRunning():
            log_debug("后台截图线程进行中，忽略重复触发", "MainApp")
            return

        # 关闭所有已打开的颜色选择器（避免其遮挡截图界面或触发焦点冲突）
        from PySide6.QtWidgets import QApplication, QColorDialog
        for w in QApplication.topLevelWidgets():
            if isinstance(w, QColorDialog) and w.isVisible():
                w.reject()

        log_info("启动后台截图线程", "MainApp")
        
        # 在后台线程执行 mss.grab()，避免主线程被阻塞 100~500ms
        from PySide6.QtCore import QThread, Signal

        class CaptureThread(QThread):
            captured = Signal(object, object)  # (QImage, QRectF)

            def run(self):
                try:
                    from capture.capture_service import CaptureService
                    image, rect = CaptureService().capture_all_screens()
                    self.captured.emit(image, rect)
                except Exception as e:
                    log_exception(e, "后台截图失败")

        self._capture_thread = CaptureThread()
        self._capture_thread.captured.connect(self._on_capture_ready)
        self._capture_thread.start()

    def _on_capture_ready(self, image, rect):
        """后台截图完成后，在主线程创建或复用截图窗口"""
        log_debug("后台截图完成，准备截图窗口", "MainApp")
        
        if self.screenshot_window is not None:
            # 复用已有窗口（节省 ~250ms 的 UI 壳创建时间）
            log_debug("复用已有截图窗口", "MainApp")
            self.screenshot_window.prepare_new_session(image, rect)
        else:
            # 首次创建
            log_debug("首次创建截图窗口", "MainApp")
            self.screenshot_window = ScreenshotWindow(
                self.config_manager,
                prefetched_image=image,
                prefetched_rect=rect,
            )
    
    def open_settings(self):
        """打开设置窗口"""
        if not self.settings_window:
            # Fallback: 如果还没预加载，立即创建
            self._preloader.preload_settings()
        
        # 确保窗口在可见屏幕内
        self._ensure_window_on_screen(self.settings_window)
        
        self.settings_window.setWindowState(self.settings_window.windowState() & ~Qt.WindowState.WindowMinimized | Qt.WindowState.WindowActive)
        self.settings_window.show()
        self.settings_window.raise_()
        self.settings_window.activateWindow()

    def _ensure_window_on_screen(self, win):
        """检查窗口位置，若在所有屏幕外则居中显示"""
        from PySide6.QtWidgets import QApplication
        pos = win.pos()
        for screen in QApplication.screens():
            if screen.availableGeometry().contains(pos):
                return
        # 窗口不在任何屏幕内，重置到主屏幕中央
        screen = QApplication.primaryScreen()
        if screen:
            screen_rect = screen.availableGeometry()
            x = screen_rect.x() + (screen_rect.width() - win.width()) // 2
            y = screen_rect.y() + (screen_rect.height() - win.height()) // 2
            win.move(x, y)

    def on_settings_accepted(self):
        """设置保存后更新热键和剪贴板设置"""
        self.update_hotkey(show_error=True)
        
        # 通知剪贴板窗口重新加载设置
        if hasattr(self, 'clipboard_window') and self.clipboard_window:
            self.clipboard_window._load_settings()
            # 同时更新历史限制
            if hasattr(self, 'clipboard_manager') and self.clipboard_manager:
                self.clipboard_manager._apply_history_limit()
    
    def open_translator(self):
        """打开翻译窗口"""
        from translation import TranslationManager
        
        params = self.config_manager.get_translation_params()
        
        manager = TranslationManager.instance()
        manager.translate(
            text="",
            **params
        )
    
    def open_clipboard_window(self):
        """打开剪切板历史窗口"""
        
        try:
            from clipboard import ClipboardWindow
            
            # 如果窗口已存在且可见，则关闭
            if self.clipboard_window and self.clipboard_window.isVisible():
                self.clipboard_window.close()
                return
            
            # 如果窗口不存在，创建新窗口
            if not self.clipboard_window:
                self.clipboard_window = ClipboardWindow()
            
            self.clipboard_window.setWindowState(self.clipboard_window.windowState() & ~Qt.WindowState.WindowMinimized | Qt.WindowState.WindowActive)
            self.clipboard_window.show()
            self.clipboard_window.raise_()
            self.clipboard_window.activateWindow()
            log_debug("剪切板窗口已打开", "Clipboard")
            
        except Exception as e:
            log_exception(e, "打开剪切板窗口失败")
        
    def quit_app(self):
        # 完全销毁缓存的截图窗口
        if self.screenshot_window:
            try:
                self.screenshot_window.full_destroy()
            except Exception as e:
                log_exception(e, "销毁截图窗口")
            self.screenshot_window = None
        
        # 关闭剪贴板窗口
        if self.clipboard_window:
            try:
                self.clipboard_window.close()
            except Exception as e:
                log_exception(e, "关闭剪贴板窗口")
            self.clipboard_window = None

        # 停止剪贴板监听
        if self.clipboard_manager and self.clipboard_manager.is_available:
            try:
                self.clipboard_manager.stop_monitoring()
            except Exception as e:
                log_exception(e, "停止剪贴板监听")
        
        # 关闭设置窗口
        if self.settings_window:
            try:
                self.settings_window.close()
            except Exception as e:
                log_exception(e, "关闭设置窗口")
            self.settings_window = None

        # 等待预加载线程结束（最多 2 秒，避免卡退出）
        for attr in ('_screenshot_preload_thread', '_ocr_preload_thread', '_capture_thread'):
            thread = getattr(self, attr, None)
            if thread and thread.isRunning():
                thread.wait(2000)

        self.hotkey_system.unregister_all()
        self.app.quit()
        
    def run(self):
        sys.exit(self.app.exec())

if __name__ == "__main__":
    from core.bootstrap import run
    run()

 