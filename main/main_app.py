import sys
import os
import ctypes
import traceback

# ============================================================================
# 关键修复：在 PyInstaller 打包环境中预加载 MSVC CRT 运行时库
# 必须在导入任何原生扩展模块（如 ocr_rs）之前执行
# 这解决了 MNN 在 PyInstaller 环境中 ACCESS_VIOLATION 崩溃的问题
# ============================================================================
def _preload_crt_for_pyinstaller():
    """预加载 MSVC CRT 运行时库，解决 ocr_rs (MNN) 在 PyInstaller 环境崩溃问题"""
    if getattr(sys, 'frozen', False):
        # 在日志系统初始化前使用临时日志文件
        import time
        log_dir = os.path.join(os.getenv('LOCALAPPDATA', ''), 'Jietuba', 'Logs')
        os.makedirs(log_dir, exist_ok=True)
        log_file = os.path.join(log_dir, f'crt_preload_{time.strftime("%Y%m%d")}.log')
        
        with open(log_file, 'a', encoding='utf-8') as f:
            f.write(f'\n[{time.strftime("%H:%M:%S")}] CRT 预加载开始...\n')
            
            loaded = []
            failed = []
            for dll in ["ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"]:
                try:
                    ctypes.CDLL(dll)
                    loaded.append(dll)
                except OSError as e:
                    failed.append(f"{dll}: {e}")
            
            f.write(f'[{time.strftime("%H:%M:%S")}] 成功加载: {loaded}\n')
            if failed:
                f.write(f'[{time.strftime("%H:%M:%S")}] 加载失败: {failed}\n')

_preload_crt_for_pyinstaller()
# ============================================================================

# 必须在导入 PyQt6 之前设置 DPI 感知，避免访问被拒绝的警告
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

# 禁用 Qt 的高 DPI 自动缩放（必须在创建 QApplication 之前设置）
os.environ["QT_ENABLE_HIGHDPI_SCALING"] = "0"
os.environ["QT_SCALE_FACTOR"] = "1"


def global_exception_handler(exc_type, exc_value, exc_tb):
    """全局未处理异常捕获"""
    error_msg = ''.join(traceback.format_exception(exc_type, exc_value, exc_tb))
    print(f"\n{'='*60}")
    print("[ERROR] 未处理的异常:")
    print(error_msg)
    print('='*60)
    
    # 尝试写入日志文件
    try:
        from core.logger import get_logger
        logger = get_logger()
        if logger and logger._ready:
            logger.error(f"未处理的异常:\n{error_msg}", "CRASH")
    except Exception:
        pass

sys.excepthook = global_exception_handler

from PyQt6.QtWidgets import QApplication, QSystemTrayIcon, QMenu, QMessageBox, QStyle
from PyQt6.QtGui import QIcon, QAction, QPixmap, QPainter, QPen, QBrush, QColor
from PyQt6.QtCore import QObject, Qt, QRect, QPoint, QTimer

# 添加模块路径
current_dir = os.path.dirname(os.path.abspath(__file__))
if current_dir not in sys.path:
    sys.path.insert(0, current_dir)

from core.hotkey_system import HotkeySystem
from settings import get_tool_settings_manager
from ui.screenshot_window import ScreenshotWindow
from ui.settings_window import SettingsDialog
from core.logger import log_exception

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
    
    # Fallback: 如果找不到文件，使用代码绘制相机样式
    # 创建32x32的图标
    pixmap = QPixmap(32, 32)
    pixmap.fill(Qt.GlobalColor.transparent)  # 透明背景
    
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    
    # 设置画笔和画刷
    pen = QPen(Qt.GlobalColor.black, 2)
    painter.setPen(pen)
    
    # 画相机主体 (矩形)
    camera_body = QRect(4, 12, 24, 16)
    painter.fillRect(camera_body, Qt.GlobalColor.darkGray)
    painter.drawRect(camera_body)
    
    # 画镜头 (圆形)
    lens_center = QPoint(16, 20)
    painter.setBrush(QBrush(Qt.GlobalColor.black))
    painter.drawEllipse(lens_center, 6, 6)
    
    # 画镜头内圈
    painter.setBrush(QBrush(Qt.GlobalColor.lightGray))
    painter.drawEllipse(lens_center, 4, 4)
    
    # 画闪光灯/取景器
    painter.setBrush(QBrush(Qt.GlobalColor.white))
    painter.drawRect(22, 14, 4, 3)
    
    painter.end()
    return QIcon(pixmap)

class MainApp(QObject):
    def __init__(self):
        super().__init__()
        self.app = QApplication(sys.argv)
        self.app.setQuitOnLastWindowClosed(False)
        
        # Config - 使用统一的设置管理器
        self.config_manager = get_tool_settings_manager()
        
        # Logger - 必须在程序启动早期初始化，否则不会生成日志文件
        from core.logger import setup_logger, get_logger, log_debug, log_info, log_warning
        setup_logger(self.config_manager)
        self._logger = get_logger()
        self.app.aboutToQuit.connect(self._on_about_to_quit)
        
        # 🌐 初始化翻译系统
        from core.i18n import I18nManager
        # 检查是否有保存的语言设置，如果没有则使用系统语言
        # 使用特殊标记 "__NOT_SET__" 来检测是否是第一次启动
        saved_lang = self.config_manager.get_app_setting("language", "__NOT_SET__")
        if saved_lang == "__NOT_SET__":
            # 第一次启动，检测系统语言
            saved_lang = I18nManager.get_system_language()
            self.config_manager.set_app_setting("language", saved_lang)
            log_info(f"首次启动，检测到系统语言: {saved_lang}", "I18n")
        I18nManager.load_language(saved_lang)
        log_info(f"语言设置: {I18nManager.get_current_language_name()}", "I18n")
        
        # 🌐 连接语言切换信号，用于更新托盘菜单等 UI
        I18nManager.instance().language_changed.connect(self._on_language_changed)
        
        # 输出DPI信息用于调试
        try:
            from PyQt6.QtGui import QGuiApplication
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
        
        # Hotkey System
        self.hotkey_system = HotkeySystem()
        self.update_hotkey()
        
        # 系统托盘
        self.setup_tray()
        
        # 窗口实例
        self.settings_window = None
        self.screenshot_window = None
        self.clipboard_window = None
        
        # 剪贴板管理器
        self.clipboard_manager = None
        
        # 延迟预加载其他组件，避免启动卡顿
        QTimer.singleShot(1000, self.preload_settings)
        QTimer.singleShot(1500, self.init_clipboard_manager)

    def _on_about_to_quit(self):
        """应用退出前收尾"""
        try:
            if hasattr(self, "_logger") and self._logger:
                self._logger.close()
        except Exception:
            pass

    def preload_settings(self):
        """预加载设置窗口"""
        from core.logger import log_debug
        if not self.settings_window:
            log_debug("预加载设置窗口...", "MainApp")
            current_hotkey = self.config_manager.get_hotkey()
            self.settings_window = SettingsDialog(self.config_manager, current_hotkey)
            self.settings_window.accepted.connect(self.on_settings_accepted)
            log_debug("设置窗口预加载完成", "MainApp")
    
    def preload_ocr_engine(self):
        """预加载 OCR 模块和引擎（同步方式，避免后台线程问题）"""
        from core.logger import log_info, log_warning, log_debug, log_error
        try:
            if not self.config_manager.get_ocr_enabled():
                log_debug("OCR 功能已禁用，跳过预加载", "OCR")
                return
            
            log_info("开始预加载 OCR 模块和引擎...", "OCR")
            
            # 获取用户配置的 OCR 引擎
            ocr_engine = self.config_manager.get_ocr_engine()
            
            # 直接在主线程中同步初始化（避免 QThread 在 PyInstaller 环境下的问题）
            try:
                from ocr import is_ocr_available, initialize_ocr, set_ocr_engine
                
                if not is_ocr_available():
                    log_debug("OCR 模块不可用（无OCR版本）", "OCR")
                    return
                
                # 设置引擎类型
                set_ocr_engine(ocr_engine)
                log_info(f"设置 OCR 引擎: {ocr_engine}", "OCR")
                
                if initialize_ocr():
                    log_info("OCR 预加载成功", "OCR")
                else:
                    log_warning("OCR 引擎预加载失败", "OCR")
            except ImportError as e:
                log_debug(f"OCR 模块不存在（无OCR版本）: {e}", "OCR")
            except Exception as e:
                import traceback
                log_error(f"OCR 预加载异常: {e}\n{traceback.format_exc()}", "OCR")
            
        except Exception as e:
            log_debug(f"OCR 引擎预加载异常（可能是无OCR版本）: {e}", "OCR")

    def init_clipboard_manager(self):
        """初始化剪贴板管理器和快捷键"""
        from core.logger import log_info, log_debug, log_warning
        
        # 检查是否启用剪贴板功能
        if not self.config_manager.get_clipboard_enabled():
            log_debug("剪贴板功能已禁用", "Clipboard")
            return
        
        try:
            from clipboard import ClipboardManager
            
            self.clipboard_manager = ClipboardManager()
            
            if self.clipboard_manager.is_available:
                # 定义回调函数，用于通知窗口刷新
                def on_clipboard_change(item):
                    # 如果剪贴板窗口存在，通知它刷新
                    if self.clipboard_window:
                        self.clipboard_window.notify_new_content()
                
                # 启动监听（Rust 后台线程），传入回调
                self.clipboard_manager.start_monitoring(callback=on_clipboard_change)
                log_info("剪贴板监听已启动", "Clipboard")
                
                # 注意：剪贴板热键在 update_hotkey() 中统一注册
            else:
                log_warning("剪贴板管理器不可用（pyclipboard 未安装）", "Clipboard")
        except ImportError:
            log_debug("clipboard 模块不存在", "Clipboard")
        except Exception as e:
            log_warning(f"剪贴板初始化失败: {e}", "Clipboard")

    def open_clipboard_window(self):
        """打开剪贴板历史窗口"""
        from core.logger import log_debug
        
        try:
            from clipboard import ClipboardWindow
            
            # 如果窗口已存在且可见，则关闭
            if self.clipboard_window and self.clipboard_window.isVisible():
                self.clipboard_window.close()
                return
            
            # 如果窗口不存在，创建新窗口
            if not self.clipboard_window:
                self.clipboard_window = ClipboardWindow()
            
            self.clipboard_window.show()
            self.clipboard_window.activateWindow()
            log_debug("剪贴板窗口已打开", "Clipboard")
            
        except Exception as e:
            from core.logger import log_exception
            log_exception(e, "打开剪贴板窗口失败")

    def _on_language_changed(self, lang_code: str):
        """语言切换时更新所有 UI 元素"""
        from core.logger import log_debug
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
            self.preload_settings()
            
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
        """创建托盘菜单（公共方法，避免重复代码）"""
        menu = QMenu()
        
        action_screenshot = QAction(self.tr("Screenshot"), self)
        action_screenshot.triggered.connect(self.start_screenshot)
        menu.addAction(action_screenshot)
        
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
        self.tray_icon.setToolTip(self.tr("jietuba - Double click to open settings"))
        
        # 重建菜单
        self.tray_icon.setContextMenu(self._create_tray_menu())

    def update_hotkey(self, show_error: bool = False):
        """
        更新所有全局热键（截图热键 + 剪贴板热键）
        
        Args:
            show_error: 是否显示错误提示（设置保存时为 True，启动时为 False）
        """
        from core.logger import log_info, log_warning
        
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
        
        # 注册剪贴板热键（如果剪贴板功能启用）
        if self.config_manager.get_clipboard_enabled():
            clipboard_hotkey = self.config_manager.get_clipboard_hotkey()
            if clipboard_hotkey:
                if self.hotkey_system.register_hotkey(clipboard_hotkey, self.open_clipboard_window):
                    log_info(f"剪贴板热键已注册: {clipboard_hotkey}", "Hotkey")
                else:
                    log_warning(f"剪贴板热键注册失败: {clipboard_hotkey}", "Hotkey")
                    failed_hotkeys.append((self.tr("Clipboard"), clipboard_hotkey))
        
        # 如果有注册失败的热键且需要显示提示
        if show_error and failed_hotkeys:
            self._show_hotkey_error(failed_hotkeys)
    
    def _show_hotkey_error(self, failed_hotkeys: list):
        """显示热键注册失败的提示"""
        from core.logger import log_debug
        
        lines = []
        for name, key in failed_hotkeys:
            lines.append(f"• {name}: {key}")
        
        msg = self.tr("The following hotkeys failed to register:") + "\n\n"
        msg += "\n".join(lines)
        msg += "\n\n" + self.tr("The hotkey may be occupied by other programs. Please try a different combination.")
        
        log_debug(f"显示热键错误提示: {failed_hotkeys}", "Hotkey")
        
        # 使用 QMessageBox 显示错误（更可靠）
        QMessageBox.warning(
            None,
            self.tr("Hotkey Registration Failed"),
            msg,
            QMessageBox.StandardButton.Ok
        )
        
    def setup_tray(self):
        if not QSystemTrayIcon.isSystemTrayAvailable():
            QMessageBox.critical(None, "Error", "System tray not available")
            sys.exit(1)
            
        self.tray_icon = QSystemTrayIcon(self)
        
        # Use custom icon
        icon = create_app_icon()
        self.tray_icon.setIcon(icon)
        
        self.tray_icon.setToolTip(self.tr("jietuba - Double click to open settings"))
        
        # Menu - 使用公共方法创建菜单
        self.tray_icon.setContextMenu(self._create_tray_menu())
        self.tray_icon.activated.connect(self.on_tray_activated)
        self.tray_icon.show()
        
    def on_tray_activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.DoubleClick:
            self.open_settings()
            
    def start_screenshot(self):
        """启动截图 - 管理截图窗口生命周期"""
        from core.logger import log_info, log_warning, log_debug
        
        # 关闭已存在的截图窗口（防止多次打开）
        if self.screenshot_window:
            log_warning("检测到已存在的截图窗口，先关闭...", "MainApp")
            # 调用 cleanup_and_close 确保资源被释放
            if hasattr(self.screenshot_window, 'cleanup_and_close'):
                self.screenshot_window.cleanup_and_close()
            else:
                self.screenshot_window.close()
                self.screenshot_window.deleteLater()
            self.screenshot_window = None
            
        # Hide settings if open (optional, but good for focus)
        if self.settings_window and self.settings_window.isVisible():
            self.settings_window.hide()
            
        log_info("创建新的截图窗口", "MainApp")
        # Create and show new screenshot window
        self.screenshot_window = ScreenshotWindow(self.config_manager)
        
        # 注意：窗口设置了 WA_DeleteOnClose，关闭后会自动删除
        self.screenshot_window.destroyed.connect(self._on_screenshot_window_destroyed)
    
    def _on_screenshot_window_destroyed(self):
        """截图窗口被销毁时的回调"""
        from core.logger import log_debug
        try:
            log_debug("截图窗口已销毁", "MainApp")
            self.screenshot_window = None
        except Exception as e:
            log_exception(e, "截图窗口销毁回调")
    
    def open_settings(self):
        """打开设置窗口"""
        if not self.settings_window:
            # Fallback: 如果还没预加载，立即创建
            self.preload_settings()
        
        self.settings_window.show()
        self.settings_window.activateWindow()
        self.settings_window.raise_()

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
        from core.i18n import I18nManager
        
        api_key = ""
        if self.config_manager and hasattr(self.config_manager, 'get_deepl_api_key'):
            api_key = self.config_manager.get_deepl_api_key() or ""
        
        # 优先读取注册表保存的翻译目标语言
        saved_target_lang = self.config_manager.get_app_setting("translation_target_lang", "")
        if saved_target_lang:
            target_lang = saved_target_lang
        else:
            # 如果没有保存过，根据当前应用语言设置目标语言
            app_lang = I18nManager.get_current_language()
            lang_map = {"zh": "ZH", "en": "EN", "ja": "JA"}
            target_lang = lang_map.get(app_lang, "ZH")
        
        use_pro = False
        if self.config_manager and hasattr(self.config_manager, 'get_deepl_use_pro'):
            use_pro = self.config_manager.get_deepl_use_pro()
        
        # 获取翻译参数设置
        # split_sentences: "nonewlines"=忽略换行按标点分句, "0"=不分句
        split_sentences_enabled = True
        preserve_formatting = True
        if self.config_manager and hasattr(self.config_manager, 'get_translation_split_sentences'):
            split_sentences_enabled = self.config_manager.get_translation_split_sentences()
        if self.config_manager and hasattr(self.config_manager, 'get_translation_preserve_formatting'):
            preserve_formatting = self.config_manager.get_translation_preserve_formatting()
        
        # 转换为 DeepL API 参数: 开启时用 nonewlines（忽略换行），关闭时用 0（不分句）
        split_sentences = "nonewlines" if split_sentences_enabled else "0"
        
        manager = TranslationManager.instance()
        manager.translate(
            text="",
            api_key=api_key,
            target_lang=target_lang,
            use_pro=use_pro,
            split_sentences=split_sentences,
            preserve_formatting=preserve_formatting
        )
        
    def quit_app(self):
        # 停止剪贴板监听
        if self.clipboard_manager and self.clipboard_manager.is_available:
            try:
                self.clipboard_manager.stop_monitoring()
            except Exception:
                pass
        
        self.hotkey_system.unregister_all()
        self.app.quit()
        
    def run(self):
        sys.exit(self.app.exec())

if __name__ == "__main__":
    main = MainApp()
    main.run()
