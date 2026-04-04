"""
启动引导模块 - 应用的完整启动流程

职责分两阶段：
  阶段 1（Pre-Qt）：环境变量、DPI 感知、崩溃钩子、单实例、模块路径
  阶段 2（Post-Qt）：PreloadManager 链式预加载字体/模块/工具栏/OCR/设置窗口/剪贴板
"""

import sys
import os

# ===================== 安全补丁：避免 platform 模块启动 cmd.exe =====================
# platform.version() → uname() → _syscmd_ver() 会调用 subprocess("ver", shell=True)
# PyInstaller 打包后，从 %TEMP% 解压 + 启动 cmd.exe 的组合会被杀毒软件误报
# 用 sys.getwindowsversion()（纯 Win32 API）替代，结果完全一致且不创建子进程
if sys.platform == "win32":
    import platform as _platform
    _orig_syscmd_ver = _platform._syscmd_ver
    def _safe_syscmd_ver(system='', release='', version='',
                         supported_platforms=('win32', 'win16', 'dos')):
        if sys.platform == 'win32':
            wv = sys.getwindowsversion()
            return ('Microsoft Windows', str(wv.major),
                    f'{wv.major}.{wv.minor}.{wv.build}')
        return _orig_syscmd_ver(system, release, version, supported_platforms)
    _platform._syscmd_ver = _safe_syscmd_ver


# ===================== 阶段 1：Pre-Qt 环境准备 =====================

def setup_environment():
    """设置环境变量和 DPI（必须在导入 PySide6 之前调用）"""
    # 全局崩溃捕获
    from core.crash_handler import install_crash_hooks
    install_crash_hooks()

    # 禁用 Qt 的高 DPI 自动缩放，不然桌面设置缩放比例不是100%就会让画面变得奇怪
    # 必须在导入 PySide6 之前设置
    os.environ["QT_ENABLE_HIGHDPI_SCALING"] = "0"
    os.environ["QT_SCALE_FACTOR"] = "1"
    # 抑制 Qt 的 DPI 警告（因为我们手动控制 DPI）
    os.environ["QT_LOGGING_RULES"] = "qt.qpa.window=false"

    # 设置 DPI 感知（在 Qt 初始化之前）
    from core.platform_utils import set_dpi_awareness
    set_dpi_awareness()


def ensure_module_path():
    """确保 main/ 目录在 sys.path 中"""
    current_dir = os.path.dirname(os.path.abspath(__file__))
    if current_dir not in sys.path:
        sys.path.insert(0, current_dir)


def ensure_single_instance():
    """
    确保单实例运行：若已有同名进程在运行则将其终止，再继续启动。
    使用 PID 文件（存放在系统临时目录）记录当前进程 ID。
    """
    import tempfile
    from core.platform_utils import terminate_process_by_pid
    from core.logger import log_exception

    exe_name = os.path.basename(sys.executable if getattr(sys, 'frozen', False) else sys.argv[0])
    # 打包后用 exe 名，开发时固定用 jietuba_app，避免与其他 python 进程冲突
    pid_name = exe_name if getattr(sys, 'frozen', False) else "jietuba_app"
    pid_file = os.path.join(tempfile.gettempdir(), f"{pid_name}.pid")

    # 读取旧 PID 并尝试终止旧进程
    if os.path.exists(pid_file):
        try:
            with open(pid_file, 'r') as f:
                old_pid = int(f.read().strip())
            if old_pid != os.getpid():
                try:
                    terminate_process_by_pid(old_pid)
                except Exception as e:
                    log_exception(e, "终止旧进程")
        except Exception as e:
            log_exception(e, "读取PID文件")

    # 写入当前 PID
    try:
        with open(pid_file, 'w') as f:
            f.write(str(os.getpid()))
    except Exception as e:
        log_exception(e, "写入PID文件")

    # 程序退出时删除 PID 文件
    import atexit
    def _cleanup():
        try:
            if os.path.exists(pid_file):
                with open(pid_file, 'r') as f:
                    stored = int(f.read().strip())
                if stored == os.getpid():
                    os.remove(pid_file)
        except Exception as e:
            log_exception(e, "清理PID文件")
    atexit.register(_cleanup)


# ===================== 阶段 2：Post-Qt 预加载管理 =====================

class PreloadManager:
    """
    管理应用启动后的链式预加载
    
    执行顺序：字体 → 截图模块(子线程) → 工具栏 → OCR(子线程) → 设置窗口 → 剪贴板 → 显示主界面
    主线程任务用 singleShot(0) 衔接（让事件循环处理一轮再继续）
    子线程任务用 finished 信号衔接
    """
    
    def __init__(self, app):
        """
        Args:
            app: MainApp 实例，预加载结果（窗口、管理器、线程引用）会设置到 app 上
        """
        self.app = app
        self.config = app.config_manager
        self._steps = []
    
    def build_and_start(self):
        """根据配置构建预加载步骤链，然后启动"""
        from PySide6.QtCore import QTimer
        from core.platform_utils import request_trim_working_set
        
        cfg = self.config
        if cfg.get_app_setting("preload_fonts", True):
            self._steps.append(self._preload_fonts)
        if cfg.get_app_setting("preload_screenshot", True):
            self._steps.append(self._preload_screenshot_modules)
        if cfg.get_app_setting("preload_toolbar", True):
            self._steps.append(self._preload_toolbar_assets)
        if cfg.get_app_setting("preload_ocr", True):
            self._steps.append(self._preload_ocr_engine)
        if cfg.get_app_setting("preload_settings", True):
            self._steps.append(self.preload_settings)
        if cfg.get_app_setting("preload_clipboard", True):
            self._steps.append(self._init_clipboard_manager)
        # 最后：显示主界面 + 释放工作集（始终执行）
        self._steps.append(self._show_main_window_on_start)
        self._steps.append(lambda: request_trim_working_set(1000))
        # 启动链式预加载（50ms 后开始，让事件循环先稳定）
        QTimer.singleShot(50, self._run_next)
    
    def _run_next(self):
        """链式调度器：取出队列头部的任务并执行。
        
        主线程任务（字体、工具栏、设置窗口等）同步执行后立即调度下一个。
        子线程任务（截图模块、OCR）返回 True 表示"异步进行中，finished 信号会触发下一步"。
        """
        from PySide6.QtCore import QTimer
        from core.logger import log_exception
        
        if not self._steps:
            return
        step = self._steps.pop(0)
        try:
            is_async = step()
        except Exception as e:
            log_exception(e, "预加载步骤执行失败")
            is_async = False
        # 如果不是异步任务（或者失败了），立即调度下一个
        # singleShot(0) 让事件循环处理一轮再继续，避免长时间占住主线程
        if not is_async:
            QTimer.singleShot(0, self._run_next)
    
    # ---------- 具体预加载步骤 ----------
    
    def _preload_fonts(self):
        """预加载系统字体列表（首次调用 QFontDatabase.families()）"""
        from PySide6.QtGui import QFontDatabase
        from core.logger import log_debug
        log_debug("预加载系统字体列表...", "Preload")
        fonts = QFontDatabase.families()
        log_debug(f"字体预加载完成，共 {len(fonts)} 个字体", "Preload")
    
    def _preload_toolbar_assets(self):
        """在主线程预热截图工具栏，避免首次截图创建工具栏卡顿"""
        from core.logger import log_debug
        try:
            from PySide6.QtWidgets import QWidget
            from ui.toolbar import Toolbar
            dummy_parent = QWidget()
            dummy_parent.hide()
            toolbar = Toolbar(dummy_parent)
            toolbar.hide()
            toolbar.deleteLater()
            dummy_parent.deleteLater()
            log_debug("工具栏预加载完成", "Preload")
        except Exception as e:
            log_debug(f"工具栏预加载异常: {e}", "Preload")
    
    def _preload_screenshot_modules(self):
        """
        在后台线程预加载截图相关模块
        
        首次截图时需要加载大量模块，在低配电脑上会导致明显卡顿：
        1. mss - 屏幕截图库，首次导入需要初始化 Windows API
        2. canvas 模块 - CanvasScene, CanvasView, 各种图形项
        3. tools 模块 - 9 个绘图工具类
        4. win32gui - 智能选区依赖
        5. CursorManager, SmartEditController 等
        
        通过在后台线程预加载这些模块，可以让首次截图更流畅
        """
        from PySide6.QtCore import QThread
        from core.logger import log_debug, log_info, log_exception
        
        class ScreenshotPreloadThread(QThread):
            def run(self):
                try:
                    log_debug("开始预加载截图相关模块...", "Preload")
                    
                    # 1. 预加载并预热 mss（屏幕截图库）
                    import mss
                    with mss.mss() as sct:
                        sct.grab({"left": 0, "top": 0, "width": 1, "height": 1})
                    log_debug("mss 模块已加载并预热", "Preload")
                    
                    # 2. 预加载 canvas 模块（场景、视图、图形项）
                    from canvas import CanvasScene, CanvasView, SelectionModel
                    from canvas.items import BackgroundItem, SelectionItem
                    from canvas.items import StrokeItem, RectItem, EllipseItem, ArrowItem, TextItem, NumberItem
                    from canvas.smart_edit_controller import SmartEditController
                    from canvas.handle_editor import LayerEditor
                    log_debug("canvas 模块已加载", "Preload")
                    
                    # 3. 预加载 tools 模块（所有绘图工具）
                    from tools import (
                        ToolController, ToolContext, CursorTool,
                        PenTool, RectTool, EllipseTool, ArrowTool,
                        TextTool, NumberTool, HighlighterTool, EraserTool
                    )
                    from tools.cursor_manager import CursorManager
                    log_debug("tools 模块已加载", "Preload")
                    
                    # 4. 预加载智能选区依赖（win32gui）
                    try:
                        import win32gui
                        import win32con
                        from capture.window_finder import WindowFinder
                        log_debug("win32gui 模块已加载", "Preload")
                    except ImportError:
                        log_debug("win32gui 未安装，跳过", "Preload")
                    
                    # 4.5 预加载 win32clipboard
                    try:
                        import win32clipboard
                        log_debug("win32clipboard 模块已加载", "Preload")
                    except ImportError:
                        log_debug("win32clipboard 未安装，跳过", "Preload")
                    
                    # 4.6 预热 PNG 编码器
                    try:
                        from PySide6.QtGui import QImage
                        from PySide6.QtCore import QBuffer, QIODeviceBase
                        _tiny = QImage(1, 1, QImage.Format.Format_ARGB32)
                        _tiny.fill(0)
                        _buf = QBuffer()
                        _buf.open(QIODeviceBase.OpenModeFlag.WriteOnly)
                        _tiny.save(_buf, "PNG")
                        _buf.close()
                        del _tiny, _buf
                        log_debug("PNG 编码器已预热", "Preload")
                    except Exception as e:
                        log_exception(e, "预热PNG编码器")
                    
                    # 5. 预加载 UI 组件
                    from ui.toolbar import Toolbar
                    from ui.magnifier import MagnifierOverlay
                    log_debug("UI 组件已加载", "Preload")
                    
                    # 6. 预加载 capture 服务
                    from capture.capture_service import CaptureService
                    log_debug("CaptureService 已加载", "Preload")

                    # 7. 预加载 GIF 录制模块
                    try:
                        from gif import GifRecordWindow
                        log_debug("GIF 模块已加载", "Preload")
                    except Exception as e:
                        log_debug(f"GIF 模块预加载异常: {e}", "Preload")

                    # 8. 预加载长截图模块
                    try:
                        from stitch.scroll_window import ScrollCaptureWindow
                        log_debug("长截图模块已加载", "Preload")
                    except Exception as e:
                        log_debug(f"长截图模块预加载异常: {e}", "Preload")
                    
                    log_info("截图模块预加载完成", "Preload")
                    
                except Exception as e:
                    log_debug(f"截图模块预加载异常: {e}", "Preload")
        
        # 保持线程引用在 MainApp 上，quit_app 需要等待它结束
        self.app._screenshot_preload_thread = ScreenshotPreloadThread(self.app)
        self.app._screenshot_preload_thread.finished.connect(self._run_next)
        self.app._screenshot_preload_thread.start()
        return True  # 异步任务
    
    def _preload_ocr_engine(self):
        """预加载 OCR 模块和引擎（在后台线程中完成，避免阻塞主线程）"""
        from core.logger import log_debug, log_info, log_warning
        try:
            if not self.config.get_ocr_enabled():
                log_debug("OCR 功能已禁用，跳过预加载", "OCR")
                return
            
            log_info("开始在后台线程预加载 OCR 模块和引擎...", "OCR")
            
            from PySide6.QtCore import QThread
            
            class OCRPreloadThread(QThread):
                def run(self):
                    try:
                        from ocr import is_ocr_available, initialize_ocr
                        
                        if not is_ocr_available():
                            log_debug("OCR 模块不可用（无OCR版本）", "OCR")
                            return
                        
                        if initialize_ocr():
                            log_info("OCR 预加载成功", "OCR")
                        else:
                            log_warning("OCR 引擎预加载失败", "OCR")
                    except ImportError:
                        log_debug("OCR 模块不存在（无OCR版本）", "OCR")
                    except Exception as e:
                        log_debug(f"OCR 预加载异常: {e}", "OCR")
            
            self.app._ocr_preload_thread = OCRPreloadThread(self.app)
            self.app._ocr_preload_thread.finished.connect(self._run_next)
            self.app._ocr_preload_thread.start()
            return True  # 异步任务
            
        except Exception as e:
            log_debug(f"OCR 引擎预加载异常（可能是无OCR版本）: {e}", "OCR")
    
    def preload_settings(self):
        """预加载设置窗口（也供 MainApp 在语言切换/打开设置时调用）"""
        from core.logger import log_debug
        from ui.settings_ui import SettingsDialog
        
        if not self.app.settings_window:
            log_debug("预加载设置窗口...", "Preload")
            current_hotkey = self.config.get_hotkey()
            self.app.settings_window = SettingsDialog(self.config, current_hotkey)
            self.app.settings_window.accepted.connect(self.app.on_settings_accepted)
            self.app.settings_window.wizard_requested.connect(self.app._on_wizard_requested)
            log_debug("设置窗口预加载完成", "Preload")
    
    def _init_clipboard_manager(self):
        """初始化剪贴板管理器和快捷键"""
        from core.logger import log_debug, log_info, log_warning
        
        if not self.config.get_clipboard_enabled():
            log_debug("剪贴板功能已禁用", "Clipboard")
            return
        
        try:
            from clipboard import ClipboardManager
            
            self.app.clipboard_manager = ClipboardManager()
            
            if self.app.clipboard_manager.is_available:
                def on_clipboard_change(item):
                    if self.app.clipboard_window:
                        self.app.clipboard_window.notify_new_content()
                
                self.app.clipboard_manager.start_monitoring(callback=on_clipboard_change)
                log_info("剪贴板监听已启动", "Clipboard")
            else:
                log_warning("剪贴板管理器不可用（pyclipboard 未安装）", "Clipboard")
        except ImportError:
            log_debug("clipboard 模块不存在", "Clipboard")
        except Exception as e:
            log_warning(f"剪贴板初始化失败: {e}", "Clipboard")
    
    def _show_main_window_on_start(self):
        """根据配置决定启动时是否显示主界面（设置窗口或欢迎向导）"""
        from core.logger import log_exception, log_debug
        try:
            # 首次运行：显示欢迎向导
            if self.config.is_first_run():
                from ui.welcome import WelcomeWizard
                self.app.hotkey_system.unregister_all()
                wizard = WelcomeWizard(self.config)
                wizard.exec()
                self.app.update_hotkey()
                return

            # 非首次运行：根据用户设置决定是否显示设置窗口
            if self.config.should_show_main_window_on_start():
                self.app.open_settings()
            else:
                self._init_settings_window_offscreen()
        except Exception as e:
            log_exception(e, "启动时显示主界面")
        finally:
            if hasattr(self.config, "mark_as_run"):
                self.config.mark_as_run()
    
    def _init_settings_window_offscreen(self):
        """
        当"启动时显示主界面"开关关闭时调用。
        将设置窗口移到屏幕外极远处，短暂 show() 以触发 Qt 内部初始化
        （showEvent / refresh_settings 等），然后立刻 hide() 并将窗口
        位置重置回屏幕中央，保证下次手动打开时位置正常。
        """
        from core.logger import log_debug
        try:
            if not self.app.settings_window:
                self.preload_settings()

            win = self.app.settings_window
            win.move(-99999, -99999)
            win.show()
            win.hide()

            from PySide6.QtWidgets import QApplication
            screen = QApplication.primaryScreen()
            if screen:
                screen_rect = screen.availableGeometry()
                win_size = win.size()
                x = screen_rect.x() + (screen_rect.width() - win_size.width()) // 2
                y = screen_rect.y() + (screen_rect.height() - win_size.height()) // 2
                win.move(x, y)

            log_debug("设置窗口已完成离屏初始化", "Preload")
        except Exception as e:
            log_debug(f"离屏初始化设置窗口异常: {e}", "Preload")


# ===================== 入口 =====================

def run():
    """应用入口点：执行所有启动准备，然后运行主应用"""
    from core.platform_utils import set_app_user_model_id

    # 1. 环境准备（必须在 Qt 之前）
    setup_environment()

    # 2. 模块路径
    ensure_module_path()

    # 3. Windows 任务栏图标（必须在 QApplication 创建之前）
    set_app_user_model_id("jietuba.app")

    # 4. 单实例检查
    ensure_single_instance()

    # 5. 启动主应用
    from main_app import MainApp
    main = MainApp()
    main.run()


if __name__ == "__main__":
    run()
  