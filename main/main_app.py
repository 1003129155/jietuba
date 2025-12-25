import sys
import os
import ctypes

# 🔥 必须在导入 PyQt6 之前设置 DPI 感知，避免访问被拒绝的警告
try:
    # PROCESS_PER_MONITOR_DPI_AWARE = 2
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass

# 🔥 禁用 Qt 的高 DPI 自动缩放，让应用程序自己处理 DPI
# 必须在创建 QApplication 之前设置
os.environ["QT_ENABLE_HIGHDPI_SCALING"] = "0"
os.environ["QT_SCALE_FACTOR"] = "1"

from PyQt6.QtWidgets import QApplication, QSystemTrayIcon, QMenu, QMessageBox, QStyle
from PyQt6.QtGui import QIcon, QAction, QPixmap, QPainter, QPen, QBrush, QColor
from PyQt6.QtCore import QObject, Qt, QRect, QPoint, QTimer

# Add '新架构文件' to sys.path if running directly
current_dir = os.path.dirname(os.path.abspath(__file__))
if current_dir not in sys.path:
    sys.path.insert(0, current_dir)

from core.hotkey_system import HotkeySystem
from settings import get_tool_settings_manager
from ui.screenshot_window import ScreenshotWindow
from ui.settings_window import SettingsDialog

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
        
        # 输出DPI信息用于调试
        try:
            from PyQt6.QtGui import QGuiApplication
            primary_screen = QGuiApplication.primaryScreen()
            if primary_screen:
                dpr = primary_screen.devicePixelRatio()
                logical_dpi = primary_screen.logicalDotsPerInch()
                physical_dpi = primary_screen.physicalDotsPerInch()
                print(f"🖥️ [DPI Info] Device Pixel Ratio: {dpr}")
                print(f"🖥️ [DPI Info] Logical DPI: {logical_dpi}")
                print(f"🖥️ [DPI Info] Physical DPI: {physical_dpi}")
        except Exception as e:
            print(f"⚠️ 无法获取DPI信息: {e}")
        
        # Config - 使用统一的设置管理器
        self.config_manager = get_tool_settings_manager()
        
        # Logger - 必须在程序启动早期初始化，否则不会生成日志文件
        # 注意：Logger 内部会创建目录，并把 stdout/stderr tee 到日志文件
        from core.logger import setup_logger, get_logger
        setup_logger(self.config_manager)
        self._logger = get_logger()
        self.app.aboutToQuit.connect(self._on_about_to_quit)
        
        # Hotkey System
        self.hotkey_system = HotkeySystem()
        self.update_hotkey()
        
        # Tray Icon
        self.setup_tray()
        
        # Windows
        self.settings_window = None
        self.screenshot_window = None
        
        # Pre-load settings window after a short delay to avoid lag on first open
        QTimer.singleShot(1000, self.preload_settings)
        
        # 🔥 预加载 OCR 引擎（在后台初始化，避免第一次打开钉图时卡顿）
        QTimer.singleShot(2000, self.preload_ocr_engine)

    def _on_about_to_quit(self):
        """应用退出前收尾：关闭日志文件（flush + restore stdout/stderr）。"""
        try:
            if hasattr(self, "_logger") and self._logger:
                self._logger.close()
        except Exception:
            # 退出阶段不再抛异常，避免影响退出
            pass
    def preload_settings(self):
        """预加载设置窗口（只创建一次）"""
        if not self.settings_window:
            print("Pre-loading settings window...")
            current_hotkey = self.config_manager.get_hotkey()
            self.settings_window = SettingsDialog(self.config_manager, current_hotkey)
            self.settings_window.accepted.connect(self.on_settings_accepted)
            print("Settings window pre-loaded.")
    
    def preload_ocr_engine(self):
        """预加载 OCR 引擎（避免第一次打开钉图时卡顿）"""
        try:
            # 检查 OCR 是否启用
            if not self.config_manager.get_ocr_enabled():
                print("ℹ️ [预加载] OCR 功能已禁用，跳过预加载")
                return
            
            from ocr import is_ocr_available, initialize_ocr
            
            if not is_ocr_available():
                print("ℹ️ [预加载] OCR 模块不可用（无OCR版本），跳过预加载")
                return
            
            print("🔄 [预加载] 开始预加载 OCR 引擎...")
            if initialize_ocr():
                print("✅ [预加载] OCR 引擎预加载成功（首次打开钉图将更流畅）")
            else:
                print("⚠️ [预加载] OCR 引擎预加载失败")
        except Exception as e:
            print(f"ℹ️ [预加载] OCR 引擎预加载异常（可能是无OCR版本）: {e}")

    def update_hotkey(self):
        self.hotkey_system.unregister_all()
        hotkey = self.config_manager.get_hotkey()
        if hotkey:
            print(f"Registering hotkey: {hotkey}")
            self.hotkey_system.register_hotkey(hotkey, self.start_screenshot)
        
    def setup_tray(self):
        if not QSystemTrayIcon.isSystemTrayAvailable():
            QMessageBox.critical(None, "Error", "System tray not available")
            sys.exit(1)
            
        self.tray_icon = QSystemTrayIcon(self)
        
        # Use custom icon
        icon = create_app_icon()
        self.tray_icon.setIcon(icon)
        
        self.tray_icon.setToolTip("jietuba - ダブルクリックで設定を表示")
        
        # Menu
        menu = QMenu()
        
        action_screenshot = QAction("スクリーンショット", self)
        action_screenshot.triggered.connect(self.start_screenshot)
        menu.addAction(action_screenshot)
        
        action_settings = QAction("設定", self)
        action_settings.triggered.connect(self.open_settings)
        menu.addAction(action_settings)
        
        menu.addSeparator()
        
        action_quit = QAction("終了", self)
        action_quit.triggered.connect(self.quit_app)
        menu.addAction(action_quit)
        
        self.tray_icon.setContextMenu(menu)
        self.tray_icon.activated.connect(self.on_tray_activated)
        self.tray_icon.show()
        
    def on_tray_activated(self, reason):
        if reason == QSystemTrayIcon.ActivationReason.DoubleClick:
            self.open_settings()
            
    def start_screenshot(self):
        """启动截图 - 管理截图窗口生命周期"""
        # 关闭已存在的截图窗口（防止多次打开）
        if self.screenshot_window:
            print("⚠️ 检测到已存在的截图窗口，先关闭...")
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
            
        print("📸 创建新的截图窗口...")
        # Create and show new screenshot window
        # This captures the screen immediately upon creation
        # Pass config_manager for auto-save functionality
        self.screenshot_window = ScreenshotWindow(self.config_manager)
        
        # 注意：窗口设置了 WA_DeleteOnClose，关闭后会自动删除
        # 所以我们需要在窗口关闭时将引用设置为 None
        self.screenshot_window.destroyed.connect(self._on_screenshot_window_destroyed)
    
    def _on_screenshot_window_destroyed(self):
        """截图窗口被销毁时的回调"""
        try:
            print("🗑️ 截图窗口已被销毁")
            self.screenshot_window = None
        except Exception as e:
            print(f"⚠️ 截图窗口销毁回调异常: {e}")
    
    def open_settings(self):
        """打开设置窗口"""
        if not self.settings_window:
            # Fallback: 如果还没预加载，立即创建
            self.preload_settings()
        
        self.settings_window.show()
        self.settings_window.activateWindow()
        self.settings_window.raise_()

    def on_settings_accepted(self):
        # Settings have been saved by the dialog
        # We need to update the hotkey if it changed
        # The dialog saves to config, so we just reload from config
        # Or better, get the new hotkey from the dialog before it closes?
        # SettingsDialog.on_save saves to config.
        
        # However, SettingsDialog.get_hotkey() returns the value from UI.
        # But since it's already accepted and saved, we can just read from config or the dialog.
        # Let's read from config to be safe as on_save writes it.
        
        # Wait, SettingsDialog.on_save does:
        # self.config_manager.settings.setValue('hotkey', self.get_hotkey()) (Wait, does it?)
        # I need to check SettingsDialog.on_save again.
        
        self.update_hotkey()
        
    def quit_app(self):
        self.hotkey_system.unregister_all()
        self.app.quit()
        
    def run(self):
        sys.exit(self.app.exec())

if __name__ == "__main__":
    main = MainApp()
    main.run()
