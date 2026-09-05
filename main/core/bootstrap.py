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


def _read_instance_record(pid_file: str):
    """读取实例记录文件，返回 (pid, create_time)；解析失败返回 None。

    create_time 为 None 表示这是旧版本写下的、只有 PID 的记录。
    """
    try:
        with open(pid_file, 'r', encoding='utf-8') as f:
            lines = [ln.strip() for ln in f.read().splitlines() if ln.strip()]
        if not lines:
            return None
        pid = int(lines[0])
        create_time = int(lines[1]) if len(lines) > 1 else None
        return pid, create_time
    except Exception:
        # 文件损坏或格式不认识：当作没有记录处理，交由调用方覆盖重写
        return None


def _is_our_old_instance(record, my_pid: int, my_image_name: str, frozen: bool) -> bool:
    """判断记录指向的进程是否确实是本程序的旧实例。

    这个校验是必需的：PID 文件很容易变成陈旧文件——它的清理挂在 atexit 上，
    而被 TerminateProcess 杀死（本模块终止旧实例正是这么做的）或崩溃退出时
    atexit 都不会执行。Windows 又会激进地回收 PID，若只凭 PID 就终止，
    很可能误杀一个恰好复用了该 PID 的无关进程。
    """
    from core.platform_utils import get_process_identity

    pid, create_time = record
    if pid == my_pid:
        return False

    live = get_process_identity(pid)
    if live is None:
        return False  # 进程已不存在或无权访问 —— 陈旧记录，忽略
    live_create_time, live_image_name = live

    if create_time is not None:
        # (PID, 创建时间) 唯一确定一个进程，这是最可靠的判据
        return live_create_time == create_time

    # 旧版本写的记录没有创建时间，退化到核对映像名：目标必须和自己是同一个可执行文件。
    # 开发环境下映像名是 python.exe，这个判据太弱（会命中任何无关的 python 进程），
    # 因此只在打包后启用；开发环境宁可放过旧实例，也不冒误杀的风险。
    if not frozen or not my_image_name:
        return False
    return live_image_name.lower() == my_image_name.lower()


def ensure_single_instance():
    """
    确保单实例运行：确认有本程序的旧实例在运行时，终止它再继续启动。

    实例记录文件（系统临时目录下）除 PID 外还记录进程创建时间，
    因为 PID 在 Windows 上会被回收复用，单凭 PID 无法判断目标是不是
    当初写入记录的那个进程。校验之后，陈旧的记录文件就只是无害的垃圾。
    """
    import tempfile
    from core.platform_utils import terminate_process_by_pid, get_process_identity
    from core.logger import log_debug, log_exception, log_info, log_warning, T

    frozen = getattr(sys, 'frozen', False)
    exe_name = os.path.basename(sys.executable if frozen else sys.argv[0])
    # 打包后用 exe 名，开发时固定用 jietuba_app，避免与其他 python 进程冲突
    pid_name = exe_name if frozen else "jietuba_app"
    pid_file = os.path.join(tempfile.gettempdir(), f"{pid_name}.pid")

    my_pid = os.getpid()
    my_identity = get_process_identity(my_pid)
    my_create_time = my_identity[0] if my_identity else None
    my_image_name = my_identity[1] if my_identity else os.path.basename(sys.executable)

    # 确认旧实例身份后再终止
    record = _read_instance_record(pid_file) if os.path.exists(pid_file) else None
    if record:
        try:
            if _is_our_old_instance(record, my_pid, my_image_name, frozen):
                if terminate_process_by_pid(record[0]):
                    log_info(T("已终止旧实例 (PID {old_pid})", old_pid=record[0]), "SingleInstance")
                else:
                    log_warning(T("旧实例 (PID {old_pid}) 终止失败", old_pid=record[0]), "SingleInstance")
            else:
                log_debug(
                    T("实例记录 (PID {old_pid}) 未通过身份校验，不终止", old_pid=record[0]),
                    "SingleInstance",
                )
        except Exception as e:
            log_exception(e, T("终止旧实例"))

    # 写入当前实例记录：PID + 创建时间
    try:
        with open(pid_file, 'w', encoding='utf-8') as f:
            print(my_pid, file=f)
            if my_create_time is not None:
                print(my_create_time, file=f)
    except Exception as e:
        log_exception(e, T("写入实例记录"))

    # 程序正常退出时删除记录文件（被强杀或崩溃时不会执行，此时留下的陈旧记录由上面的校验兜住）
    import atexit

    def _cleanup():
        try:
            current = _read_instance_record(pid_file)
            if current and current[0] == my_pid:
                os.remove(pid_file)
        except Exception as e:
            log_exception(e, T("清理实例记录"))

    atexit.register(_cleanup)


# 持有 onefile 资源文件的 fd。必须是模块级变量：一旦被回收，
# 文件就重新可删，保护随之失效。
_pinned_resource_fds = []


def pin_bundled_resources():
    r"""锁定 onefile 解压出的资源文件，避免运行期被系统清理工具删除。

    背景：onefile 打包把资源解压到 %TEMP%\_MEIxxxxxx，而 Windows 存储感知
    的"临时文件"清理会在若干天后扫这个目录。已加载的 DLL/.pyd 因为被进程
    锁住而幸存，但 svg、.qm 这类"读完就关"的数据文件会被整个删掉。症状是
    程序连续运行数天后工具栏图标突然全部空白、语言切换失效；更糟的是尚未
    导入的 .pyd 也会被删，首次使用对应功能时直接 ImportError。

    做法：启动时对 _MEIPASS 下每个文件持有一个只读 fd。Windows 上被打开的
    文件无法被其它进程删除，于是所有资源都获得与已加载 DLL 同等的保护。

    几个前提已实测确认：
      * fd 在 Python 中默认不可继承（PEP 446），子进程（如打开文件位置用的
        explorer）拿不到，不会在本进程退出后继续锁住目录；
      * 持有 fd 不影响 Qt 读取同一文件，也不影响 PyInstaller 的退出清理
        （进程结束时由操作系统统一关闭 fd，父进程随后正常删除目录）；
      * 成本为每个文件 1 个句柄，百来个文件耗时约 1.5 ms。
    """
    import sys as _sys

    meipass = getattr(_sys, "_MEIPASS", None)
    if not meipass or not os.path.isdir(meipass):
        return  # 非 onefile 运行（开发环境或 onedir），资源本就在稳定位置

    from core.logger import log_debug, log_warning, T

    pinned = failed = 0
    for root, _dirs, files in os.walk(meipass):
        for name in files:
            try:
                _pinned_resource_fds.append(
                    os.open(os.path.join(root, name),
                            os.O_RDONLY | getattr(os, "O_BINARY", 0))
                )
                pinned += 1
            except OSError:
                # 个别文件打不开（权限/占用）不影响其余文件的保护
                failed += 1

    if failed:
        log_warning(T("资源锁定：成功 {pinned} 个，失败 {failed} 个", pinned=pinned, failed=failed), "Bootstrap")
    else:
        log_debug(T("资源锁定：已保护 {pinned} 个打包资源文件", pinned=pinned), "Bootstrap")


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
        from core.logger import log_exception, T

        if not self._steps:
            return
        step = self._steps.pop(0)
        try:
            is_async = step()
        except Exception as e:
            log_exception(e, T("预加载步骤执行失败"))
            is_async = False
        # 如果不是异步任务（或者失败了），立即调度下一个
        # singleShot(0) 让事件循环处理一轮再继续，避免长时间占住主线程
        if not is_async:
            QTimer.singleShot(0, self._run_next)
    
    # ---------- 具体预加载步骤 ----------
    
    def _preload_toolbar_assets(self):
        """在主线程预热截图工具栏，避免首次截图创建工具栏卡顿"""
        from core.logger import log_debug, log_warning, T
        try:
            from PySide6.QtWidgets import QWidget
            from ui.toolbar import Toolbar
            dummy_parent = QWidget()
            dummy_parent.hide()
            toolbar = Toolbar(dummy_parent)
            toolbar.hide()
            toolbar.deleteLater()
            dummy_parent.deleteLater()
            log_debug(T("工具栏预加载完成"), "Preload")
        except Exception as e:
            log_warning(T("工具栏预加载失败: {e}", e=e), "Preload")
    
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
        from core.logger import log_debug, log_info, log_warning, log_exception, T

        class ScreenshotPreloadThread(QThread):
            def run(self):
                try:
                    log_debug(T("开始预加载截图相关模块..."), "Preload")

                    # 1. 预加载并预热 mss（屏幕截图库）
                    import mss
                    with mss.mss() as sct:
                        sct.grab({"left": 0, "top": 0, "width": 1, "height": 1})
                    log_debug(T("mss 模块已加载并预热"), "Preload")

                    # 2. 预加载 canvas 模块（场景、视图、图形项）
                    log_debug(T("canvas 模块已加载"), "Preload")

                    # 3. 预加载 tools 模块（所有绘图工具）
                    log_debug(T("tools 模块已加载"), "Preload")

                    # 4. 预加载智能选区依赖（win32gui）
                    try:
                        # 预加载：这些导入的目的就是把模块提前装进 import 缓存，
                        # 让首次截图时不必等待，因此"未使用"是预期的。
                        import win32gui  # noqa: F401
                        import win32con  # noqa: F401
                        from capture.window_finder import WindowFinder  # noqa: F401
                        log_debug(T("win32gui 模块已加载"), "Preload")
                    except ImportError:
                        log_debug(T("win32gui 未安装，跳过"), "Preload")

                    # 4.5 预加载 win32clipboard
                    try:
                        import win32clipboard  # noqa: F401  预加载，见上
                        log_debug(T("win32clipboard 模块已加载"), "Preload")
                    except ImportError:
                        log_debug(T("win32clipboard 未安装，跳过"), "Preload")

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
                        log_debug(T("PNG 编码器已预热"), "Preload")
                    except Exception as e:
                        log_exception(e, T("预热PNG编码器"))

                    # 5. 预加载 UI 组件
                    log_debug(T("UI 组件已加载"), "Preload")

                    # 6. 预加载 capture 服务
                    log_debug(T("CaptureService 已加载"), "Preload")

                    # 7. 预加载 GIF 录制模块
                    try:
                        log_debug(T("GIF 模块已加载"), "Preload")
                    except Exception as e:
                        log_warning(T("GIF 模块预加载失败: {e}", e=e), "Preload")

                    # 8. 预加载长截图模块
                    try:
                        log_debug(T("长截图模块已加载"), "Preload")
                    except Exception as e:
                        log_warning(T("长截图模块预加载失败: {e}", e=e), "Preload")

                    log_info(T("截图模块预加载完成"), "Preload")

                except Exception as e:
                    log_warning(T("截图模块预加载失败: {e}", e=e), "Preload")
        
        # 保持线程引用在 MainApp 上，quit_app 需要等待它结束
        self.app._screenshot_preload_thread = ScreenshotPreloadThread(self.app)
        self.app._screenshot_preload_thread.finished.connect(self._run_next)
        self.app._screenshot_preload_thread.start()
        return True  # 异步任务
    
    def _preload_ocr_engine(self):
        """预加载 OCR 模块和引擎（在后台线程中完成，避免阻塞主线程）"""
        from core.logger import log_debug, log_info, log_warning, T
        try:
            if not self.config.get_ocr_enabled():
                log_debug(T("OCR 功能已禁用，跳过预加载"), "OCR")
                return

            log_info(T("开始在后台线程预加载 OCR 模块和引擎..."), "OCR")

            from PySide6.QtCore import QThread

            class OCRPreloadThread(QThread):
                def run(self):
                    try:
                        from ocr import is_ocr_available, initialize_ocr

                        if not is_ocr_available():
                            log_debug(T("OCR 模块不可用（无OCR版本）"), "OCR")
                            return

                        if initialize_ocr():
                            log_info(T("OCR 预加载成功"), "OCR")
                        else:
                            log_warning(T("OCR 引擎预加载失败"), "OCR")
                    except ImportError:
                        log_debug(T("OCR 模块不存在（无OCR版本）"), "OCR")
                    except Exception as e:
                        log_warning(T("OCR 预加载失败: {e}", e=e), "OCR")

            self.app._ocr_preload_thread = OCRPreloadThread(self.app)
            self.app._ocr_preload_thread.finished.connect(self._run_next)
            self.app._ocr_preload_thread.start()
            return True  # 异步任务

        except Exception as e:
            log_warning(T("OCR 引擎预加载失败: {e}", e=e), "OCR")
    
    def preload_settings(self):
        """预加载设置窗口（也供 MainApp 在语言切换/打开设置时调用）"""
        from core.logger import log_debug, T
        from ui.settings_ui import SettingsDialog

        if not self.app.settings_window:
            log_debug(T("预加载设置窗口..."), "Preload")
            current_hotkey = self.config.get_hotkey()
            self.app.settings_window = SettingsDialog(self.config, current_hotkey)
            self.app.settings_window.accepted.connect(self.app.on_settings_accepted)
            self.app.settings_window.wizard_requested.connect(self.app._on_wizard_requested)
            log_debug(T("设置窗口预加载完成"), "Preload")
    
    def _init_clipboard_manager(self):
        """按当前设置初始化剪贴板监听。"""
        self.app.set_clipboard_monitoring_enabled(
            self.config.get_clipboard_enabled()
        )
    
    def _show_main_window_on_start(self):
        """根据配置决定启动时是否显示主界面（设置窗口或欢迎向导）"""
        from core.logger import log_exception, T
        try:
            # 首次运行：显示欢迎向导
            if self.config.is_first_run():
                from ui.welcome import WelcomeWizard
                self.app.hotkey_system.unregister_all()
                wizard = WelcomeWizard(self.config)
                wizard.exec()
                self.app.update_hotkey()
                self.app.setup_tray()
                self.app._setup_pin_tray_updates()
                return

            # 非首次运行：预加载完成后再注册热键，避免启动预加载期间触发卡顿。
            self.app.update_hotkey()
            self.app.setup_tray()
            self.app._setup_pin_tray_updates()

            # 根据用户设置决定是否显示设置窗口
            if self.config.should_show_main_window_on_start():
                self.app.open_settings()
            else:
                self._preload_clipboard_window()
        except Exception as e:
            log_exception(e, T("启动时显示主界面"))
        finally:
            if hasattr(self.config, "mark_as_run"):
                self.config.mark_as_run()

    def _preload_clipboard_window(self):
        """后台启动时预创建剪贴板窗口。"""
        from core.logger import log_debug, log_warning, T

        if not self.config.get_clipboard_enabled():
            log_debug(T("剪贴板功能已禁用，跳过剪贴板窗口预创建"), "Clipboard")
            return

        try:
            if self.app.clipboard_window:
                return

            from clipboard import ClipboardWindow
            self.app.clipboard_window = ClipboardWindow()
            self.app.clipboard_window.hide()
            log_debug(T("剪贴板窗口预创建完成（未显示）"), "Clipboard")
        except Exception as e:
            log_warning(T("剪贴板窗口预创建失败: {e}", e=e), "Clipboard")


# ===================== 入口 =====================

def run():
    """应用入口点：执行所有启动准备，然后运行主应用"""
    from core.platform_utils import set_app_user_model_id

    # 1. 环境准备（必须在 Qt 之前）
    setup_environment()

    # 2. 模块路径
    ensure_module_path()

    # 3. 锁定打包资源，防止运行期被系统清理工具删除
    pin_bundled_resources()

    # 4. Windows 任务栏图标（必须在 QApplication 创建之前）
    set_app_user_model_id("jietuba.app")

    # 5. 单实例检查
    ensure_single_instance()

    # 6. 启动主应用
    from main_app import MainApp
    main = MainApp()
    main.run()


if __name__ == "__main__":
    run()
  
