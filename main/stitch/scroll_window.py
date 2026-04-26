"""
jietuba_scroll.py - 滚动截图窗口模块

实现滚动长截图功能的窗口类,用于捕获滚动页面的多张截图。

主要功能:
- 显示半透明边框窗口标识截图区域
- 监听鼠标滚轮事件自动触发截图
- 实时显示已捕获的截图数量
- 支持手动/自动截图控制

主要类:
- ScrollCaptureWindow: 滚动截图窗口类

特点:
- 窗口透明,不拦截鼠标事件
- 使用 Windows API 监听鼠标滚轮
- 延迟截图机制避免滚动动画干扰
- 支持取消和完成截图操作

依赖模块:
- PyQt5: GUI框架
- PIL: 图像处理
- ctypes: Windows API调用
- pynput: 鼠标事件监听

使用方法:
    window = ScrollCaptureWindow(capture_rect, parent)
    window.finished.connect(on_finished)
    window.show()
"""

import os
import time
import ctypes
import io
from ctypes import wintypes
from datetime import datetime
from PySide6.QtWidgets import QWidget, QPushButton, QVBoxLayout, QHBoxLayout, QLabel, QApplication
from PySide6.QtCore import Qt, QRect, QTimer, Signal, QPoint, QMetaObject, Q_ARG
from PySide6.QtGui import QPainter, QPen, QColor, QPixmap, QGuiApplication, QImage
from typing import Optional
from PIL import Image

# Windows API 常量
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
from ctypes import wintypes
from datetime import datetime
from PySide6.QtWidgets import QWidget, QPushButton, QVBoxLayout, QHBoxLayout, QLabel, QApplication
from PySide6.QtCore import Qt, QRect, QTimer, Signal, QPoint, QMetaObject, Q_ARG, QSettings
from PySide6.QtGui import QPainter, QPen, QColor, QPixmap, QGuiApplication, QImage
from PIL import Image
import io

# 导入长截图拼接统一接口
from .jietuba_long_stitch import AllOverlapShrinkError
from .jietuba_long_stitch_unified import (
    configure as long_stitch_configure,
    normalize_engine_value,
)

# 长截图调试日志控制 - 使用新架构的配置
from settings import get_tool_settings_manager
from core.save import SaveService
from core import log_debug, log_info, log_warning, log_error, safe_event
from core.logger import log_exception
from .scroll_toolbar import FloatingToolbar  # 浮动工具栏（独立模块）

_config = get_tool_settings_manager()
_LONG_STITCH_DEBUG_ENABLED = False
del _config

_MODULE_TAG = "长截图"

def _long_stitch_print(*args, force: bool = False, **kwargs):
    """根据调试开关决定是否输出日志，使用统一日志系统"""
    # force=True 映射到 INFO 级别（始终显示）
    # force=False 映射到 DEBUG 级别（仅调试时显示）
    message = " ".join(str(arg) for arg in args)
    if force:
        log_info(message, module=_MODULE_TAG)
    elif _LONG_STITCH_DEBUG_ENABLED:
        log_debug(message, module=_MODULE_TAG)


def set_long_stitch_debug_enabled(enabled: bool):
    """供外部更新长截图调试日志开关"""
    global _LONG_STITCH_DEBUG_ENABLED
    _LONG_STITCH_DEBUG_ENABLED = bool(enabled)


def is_long_stitch_debug_enabled() -> bool:
    return _LONG_STITCH_DEBUG_ENABLED


# 覆盖模块内的 print，支持 force 强制输出
print = _long_stitch_print  # type: ignore

# 从配置文件读取长截图引擎设置
def _load_long_stitch_engine():
    """从配置文件加载长截图引擎设置"""
    from settings import get_tool_settings_manager
    config = get_tool_settings_manager()
    raw_engine = config.get_long_stitch_engine()
    engine = normalize_engine_value(raw_engine)
    
    if engine != raw_engine:
        config.set_long_stitch_engine(engine)
        print(f"📖 检测到长截图引擎旧值 {raw_engine}，已自动转换为 {engine}")
    else:
        print(f"📖 从配置加载长截图引擎: {engine}")
    return engine

def _load_long_stitch_config():
    """从配置文件加载所有长截图参数"""
    from settings import get_tool_settings_manager
    config_mgr = get_tool_settings_manager()
    
    raw_engine = config_mgr.get_long_stitch_engine()
    engine = normalize_engine_value(raw_engine)
    
    if engine != raw_engine:
        config_mgr.set_long_stitch_engine(engine)
        print(f"📖 检测到长截图引擎旧值 {raw_engine}，已自动转换为 {engine}")
    
    config = {
        'engine': engine,
        'sample_rate': config_mgr.settings.value('screenshot/rust_sample_rate', 0.6, type=float),
        'min_sample_size': config_mgr.settings.value('screenshot/rust_min_sample_size', 300, type=int),
        'max_sample_size': config_mgr.settings.value('screenshot/rust_max_sample_size', 800, type=int),
        'corner_threshold': config_mgr.settings.value('screenshot/rust_corner_threshold', 30, type=int),
        'descriptor_patch_size': config_mgr.settings.value('screenshot/rust_descriptor_patch_size', 9, type=int),
        'min_size_delta': config_mgr.settings.value('screenshot/rust_min_size_delta', 1, type=int),
        'try_rollback': config_mgr.settings.value('screenshot/rust_try_rollback', True, type=bool),
        'distance_threshold': config_mgr.settings.value('screenshot/rust_distance_threshold', 0.1, type=float),
        'ef_search': config_mgr.settings.value('screenshot/rust_ef_search', 32, type=int),
        'verbose': False,
    }

    set_long_stitch_debug_enabled(config['verbose'])
    
    print(f"📖 从配置加载长截图参数:")
    print(f"   引擎: {config['engine']}")
    print(f"   采样率: {config['sample_rate']}")
    print(f"   采样尺寸: {config['min_sample_size']}-{config['max_sample_size']}")
    print(f"   特征点阈值: {config['corner_threshold']}")
    print(f"   描述符大小: {config['descriptor_patch_size']}")
    print(f"   索引重建阈值: {config['min_size_delta']}")
    print(f"   回滚匹配: {config['try_rollback']}")
    print(f"   距离阈值: {config['distance_threshold']}")
    print(f"   HNSW搜索参数: {config['ef_search']}")
    print(f"   调试日志: {config['verbose']}")
    
    return config

# 配置拼接引擎（从配置文件读取）
_long_stitch_config = _load_long_stitch_config()
long_stitch_configure(
    engine=_long_stitch_config['engine'],
    direction=0,  # 垂直拼接
    sample_rate=_long_stitch_config['sample_rate'],
    min_sample_size=_long_stitch_config['min_sample_size'],
    max_sample_size=_long_stitch_config['max_sample_size'],
    corner_threshold=_long_stitch_config['corner_threshold'],
    descriptor_patch_size=_long_stitch_config['descriptor_patch_size'],
    min_size_delta=_long_stitch_config['min_size_delta'],
    try_rollback=_long_stitch_config['try_rollback'],
    distance_threshold=_long_stitch_config['distance_threshold'],
    ef_search=_long_stitch_config['ef_search'],
    verbose=_long_stitch_config['verbose'],
)

# Windows API 常量
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
WS_EX_LAYERED = 0x00080000


class PreviewPanel(QWidget):
    """实时预览面板，仅以透明背景展示拼接缩略图"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(Qt.WindowType.WindowStaysOnTopHint | Qt.WindowType.FramelessWindowHint | Qt.WindowType.Tool)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self._fixed_side = 190  # 固定边长度
        self._last_panel_size = None
        self.setFixedWidth(self._fixed_side)
        self.setFixedHeight(self._fixed_side)  # 初始正方形占位
        self._build_ui()
        self._set_placeholder()
        
        # 设置鼠标穿透，防止拦截滚轮事件
        self._setup_mouse_transparent()
    
    def _setup_mouse_transparent(self):
        """设置窗口鼠标穿透，不拦截滚轮事件"""
        try:
            hwnd = int(self.winId())
            user32 = ctypes.windll.user32
            ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_TRANSPARENT | WS_EX_LAYERED)
            print(f"[OK] PreviewPanel 已设置为鼠标穿透模式")
        except Exception as e:
            print(f"[WARN] 设置 PreviewPanel 鼠标穿透失败: {e}")
        self._capture_excluded = False

    def _build_ui(self):
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self.preview_label = QLabel()
        self.preview_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.preview_label.setFixedSize(self.width(), self.height())
        self.preview_label.setStyleSheet(
            "background: rgba(0, 0, 0, 0.25);"
            "border: 1px solid rgba(0, 0, 0, 0.8);"
            "border-radius: 8px;"
            "color: rgba(255, 255, 255, 0.85);"
            "font-size: 10pt;"
            "padding: 6px;"
        )
        layout.addWidget(self.preview_label)
        
        # 截图计数标签（左上角）
        self.count_label = QLabel("0", self.preview_label)
        self.count_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.count_label.setFixedSize(40, 24)
        self.count_label.setStyleSheet("""
            background: rgba(33, 150, 243, 0.9);
            color: white;
            border: 1px solid rgba(33, 150, 243, 1);
            border-radius: 4px;
            font-weight: bold;
            font-size: 11pt;
            padding: 2px;
        """)
        self.count_label.move(8, 8)
        
        self.warning_icon = QLabel("!", self.preview_label)
        self.warning_icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.warning_icon.setFixedSize(32, 32)
        self.warning_icon.setStyleSheet(
            "background: rgba(255, 255, 255, 0.9);"
            "color: #ff4d4f;"
            "border: 1px solid rgba(255, 77, 79, 0.65);"
            "border-radius: 16px;"
            "font-weight: 700;"
            "font-size: 20px;"
        )
        self.warning_icon.move(self.preview_label.width() - self.warning_icon.width() - 10, 10)
        self.warning_icon.hide()

    def set_capture_excluded(self, exclude: bool):
        """根据是否与截图区域重叠，设置截图排除"""
        if exclude == self._capture_excluded:
            return
        try:
            from core.platform_utils import set_window_exclude_from_capture
            set_window_exclude_from_capture(int(self.winId()), exclude)
            self._capture_excluded = exclude
        except Exception:
            pass

    def closeEvent(self, event):
        """关闭时还原截图排除"""
        if self._capture_excluded:
            self.set_capture_excluded(False)
        super().closeEvent(event)

    def _set_placeholder(self, scroll_direction="vertical", screenshot_count=0):
        self.preview_label.clear()
        self.preview_label.setText("")

    def _pil_to_qpixmap(self, pil_image):
        image = pil_image.convert("RGBA")
        width, height = image.size
        data = image.tobytes("raw", "RGBA")
        # PyQt6: Format_RGBA8888 → Format.Format_RGBA8888
        qimage = QImage(data, width, height, width * 4, QImage.Format.Format_RGBA8888)
        return QPixmap.fromImage(qimage.copy())

    def update_preview(self, pil_image, scroll_direction, screenshot_count):
        if pil_image is None:
            self._set_placeholder(scroll_direction, screenshot_count)
            return

        pixmap = self._pil_to_qpixmap(pil_image)
        img_w = pixmap.width()
        img_h = pixmap.height()
        if img_w <= 0 or img_h <= 0:
            return

        F = self._fixed_side  # 210

        # 获取屏幕可用空间用于限制面板最大尺寸
        screen = QApplication.primaryScreen()
        if self.parent() and hasattr(self.parent(), 'screen') and self.parent().screen():
            screen = self.parent().screen()
        max_screen = screen.geometry().height() - 60 if screen else 800

        if scroll_direction == "vertical":
            # 竖向：宽度固定F，高度按比例，但不超过屏幕
            panel_w = F
            panel_h = max(F, int(img_h * (F / img_w)))
            if panel_h > max_screen:
                panel_h = max_screen  # 超出屏幕则限制，缩放显示
        else:
            # 横向：高度固定F，宽度按比例，但不超过屏幕宽度
            max_screen_w = screen.geometry().width() - 60 if screen else 1400
            panel_h = F
            panel_w = max(F, int(img_w * (F / img_h)))
            if panel_w > max_screen_w:
                panel_w = max_screen_w

        if self._last_panel_size != (panel_w, panel_h):
            self.setFixedSize(panel_w, panel_h)
            self.preview_label.setFixedSize(panel_w, panel_h)
            self._last_panel_size = (panel_w, panel_h)

        if pixmap.size() == self.preview_label.size():
            self.preview_label.setPixmap(pixmap)
        else:
            scaled = pixmap.scaled(
                self.preview_label.size(),
                Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation
            )
            self.preview_label.setPixmap(scaled)
        self.preview_label.setText("")

        # 更新警告图标位置
        self.warning_icon.move(panel_w - self.warning_icon.width() - 10, 10)

    def show_warning(self, message: Optional[str] = None):
        self.warning_icon.raise_()
        if message:
            self.warning_icon.setToolTip(message)
        else:
            self.warning_icon.setToolTip("")
        self.warning_icon.show()

    def clear_warning(self):
        self.warning_icon.hide()
        self.warning_icon.setToolTip("")
    
    def update_count(self, count):
        """更新截图计数"""
        self.count_label.setText(str(count))

class ScrollCaptureWindow(QWidget):
    """滚动长截图窗口
    
    特性：
    - 带边框的透明窗口
    - 不拦截鼠标滚轮事件（鼠标可以直接操作后面的网页）
    - 监听全局滚轮事件，每次滚轮后1秒截图
    - 底部有完成和取消按钮
    """
    
    finished = Signal()  # 完成信号
    cancelled = Signal()  # 取消信号
    scroll_detected = Signal(int)  # 滚轮检测信号（用于线程安全通信），传递滚动距离
    
    def __init__(self, capture_rect, parent=None, config_manager=None):
        """初始化滚动截图窗口
        
        Args:
            capture_rect: QRect，截图区域（屏幕坐标）
            parent: 父窗口
            config_manager: 配置管理器（用于钉图功能）
        """
        super().__init__(parent)
        
        self.capture_rect = capture_rect
        self.config_manager = config_manager  # 保存配置管理器
        self.screenshots = []  # 存储截图的列表
        self.scroll_distances = []  # 存储每次滚动的距离（像素）
        self.current_scroll_distance = 0  # 当前累积的滚动距离
        
        # 保存目录（由外部设置）
        self.save_directory = None
        self.save_service = SaveService()
        
        # 🆕 截图方向: "vertical"(竖向) 或 "horizontal"(横向)
        self.scroll_direction = "vertical"
        
        # 🆕 滚动方向锁定: None=未锁定, "down"=向下, "up"=向上
        self.scroll_locked_direction = None
        
        # 🆕 横向模式的键盘监听器
        self.keyboard_listener = None
        self.horizontal_scroll_key_pressed = False  # 防止重复触发
        
        # 实时拼接相关
        self.stitched_result = None  # 当前拼接的结果图
        self.preview_warning_active = False
        self._original_cancel_on_shrink = None
        
        # 滚动检测相关
        self.last_scroll_time = 0  # 最后一次滚动的时间戳
        # 从配置读取滚动冷却时间
        settings = QSettings('Fandes', 'jietuba')
        self.scroll_cooldown = settings.value('screenshot/scroll_cooldown', 0.15, type=float)
        self.capture_mode = "immediate"  # 截图模式: "immediate"立即 或 "wait"等待停止
        
        # 去重相关
        self.last_screenshot_hash = None  # 上一张截图的哈希值（用于去重）
        self.duplicate_threshold = 0.95  # 相似度阈值（95%以上认为重复）
        
        # 定时器
        self.capture_timer = QTimer(self)  # 截图定时器
        self.capture_timer.setSingleShot(True)
        self.capture_timer.timeout.connect(self._do_capture)
        
        self.scroll_check_timer = QTimer(self)  # 滚动检测定时器
        self.scroll_check_timer.setInterval(100)  # 每100ms检查一次
        self.scroll_check_timer.timeout.connect(self._check_scroll_stopped)
        
        # 连接滚轮检测信号到主线程处理函数
        # 显式指定 QueuedConnection：pynput 回调运行在 threading.Thread 中
        # PySide6 对非 QThread 线程的 AutoConnection 检测不稳定，强制入队保证线程安全
        self.scroll_detected.connect(
            self._handle_scroll_in_main_thread,
            Qt.ConnectionType.QueuedConnection
        )
        
        self._setup_window()
        self._setup_ui()
        self._setup_mouse_hook()
        
        # 创建独立的浮动工具栏
        self._setup_floating_toolbar()

        # 创建实时拼接预览面板
        self._setup_preview_panel()
        
        # 添加窗口定位检查定时器
        self._position_fix_timer = QTimer()
        self._position_fix_timer.setSingleShot(True)
        self._position_fix_timer.timeout.connect(self._force_fix_window_position)
        self._position_fix_timer.start(200)  # 200ms后再次检查并修复
    
    def _get_correct_window_position(self, border_width):
        """获取正确的窗口位置。
        
        capture_rect 已经是真实屏幕坐标，窗口只需在此基础上向外扩展 border_width
        即可与截图区域完全对齐。不做任何单屏 clamp——跨屏选区本来就应该跨屏显示。
        曾有的 clamp 逻辑会在跨屏捕获时把窗口强制推到单个屏幕内，造成位置错误。
        """
        return (self.capture_rect.x() - border_width,
                self.capture_rect.y() - border_width)
        
    def _setup_window(self):
        """设置窗口属性"""
        # 设置窗口标志：无边框、置顶
        self.setWindowFlags(
            Qt.WindowType.WindowStaysOnTopHint | 
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool
        )
        
        # 设置窗口透明度和背景
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        # 设置关闭时自动销毁，防止内存泄漏
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        
        # 设置窗口位置和大小（基于截图区域）
        # 窗口区域 = 截图区域 + 底部按钮栏
        button_bar_height = 35  # 从50改为35，让按钮栏更窄
        
        # 为边框预留空间（但截图区域不包含边框）
        border_width = 3
        
        window_x, window_y = self._get_correct_window_position(border_width)
        
        final_width = self.capture_rect.width() + border_width * 2
        final_height = self.capture_rect.height() + border_width * 2
        
        # 不再包含按钮栏高度（工具栏已独立）
        self.setGeometry(
            window_x,
            window_y,
            final_width,
            final_height
        )
        
    def _setup_ui(self):
        """设置UI界面 - 只保留透明边框区域"""
        layout = QVBoxLayout(self)
        layout.setContentsMargins(3, 3, 3, 3)  # 为边框预留空间
        layout.setSpacing(0)
        
        # 透明区域（用于显示边框）
        self.transparent_area = QWidget()
        self.transparent_area.setFixedSize(
            self.capture_rect.width(),
            self.capture_rect.height()
        )
        layout.addWidget(self.transparent_area)
    
    def _setup_floating_toolbar(self):
        """创建并设置独立的浮动工具栏"""
        self.toolbar = FloatingToolbar(self)
        
        # 连接工具栏信号
        self.toolbar.direction_changed.connect(self._toggle_direction)
        self.toolbar.manual_capture.connect(self._on_manual_capture)
        self.toolbar.pin_clicked.connect(self._on_pin)
        self.toolbar.finish_clicked.connect(self._on_finish)
        self.toolbar.cancel_clicked.connect(self._on_cancel)
        
        self._position_floating_toolbar()
        self.toolbar.show()

    def _position_floating_toolbar(self):
        """根据屏幕边界将工具栏对齐到截图区域上方居中，支持上/下/左/右四向智能回退"""
        if not hasattr(self, 'toolbar') or self.toolbar is None:
            return
        margin = 10
        screen = self.screen()
        if screen is None:
            screen = QApplication.primaryScreen()
        screen_geometry = screen.geometry()
        tw = self.toolbar.width()
        th = self.toolbar.height()

        # 水平居中 x（供上下方案使用）
        x_center = self.x() + (self.width() - tw) // 2
        x_center = max(screen_geometry.left() + margin,
                       min(x_center, screen_geometry.right() - margin - tw))

        # 策略 1：截图区上方
        y_above = self.y() - th - margin
        if y_above >= screen_geometry.top() + margin:
            self.toolbar.move(x_center, y_above)
            return

        # 策略 2：截图区下方
        y_below = self.y() + self.height() + margin
        if y_below + th <= screen_geometry.bottom() - margin:
            self.toolbar.move(x_center, y_below)
            return

        # 策略 3/4：左侧 / 右侧（截图区占满纵向时）
        y_mid = self.y() + (self.height() - th) // 2
        y_mid = max(screen_geometry.top() + margin,
                    min(y_mid, screen_geometry.bottom() - margin - th))
        if self.x() - tw - margin >= screen_geometry.left() + margin:
            self.toolbar.move(self.x() - tw - margin, y_mid)
        else:
            x_right = min(self.x() + self.width() + margin,
                          screen_geometry.right() - margin - tw)
            x_right = max(screen_geometry.left() + margin, x_right)
            self.toolbar.move(x_right, y_mid)

    def _setup_preview_panel(self):
        """创建拼接结果预览面板"""
        self.preview_panel = PreviewPanel(self)
        self._position_preview_panel()
        self.preview_panel.show()
        self._refresh_preview_panel()

    def _position_preview_panel(self):
        """根据窗口位置调整预览面板，尽量贴近截图区域且避免进入截图区域和工具栏"""
        if not hasattr(self, 'preview_panel') or self.preview_panel is None:
            return
        panel = self.preview_panel
        margin = 14
        # PyQt6: 使用 screen() 代替 desktop()
        screen = self.screen()
        if screen is None:
            screen = QApplication.primaryScreen()
        screen_geometry = screen.geometry()
        screen_left = screen_geometry.x()
        screen_top = screen_geometry.y()
        screen_right = screen_geometry.x() + screen_geometry.width()
        screen_bottom = screen_geometry.y() + screen_geometry.height()
        
        # 截图区域的边界
        capture_left = self.x()
        capture_right = self.x() + self.width()
        capture_top = self.y()
        capture_bottom = self.y() + self.height()
        
        # 获取工具栏位置（用于避让）
        toolbar_rect = None
        if hasattr(self, 'toolbar') and self.toolbar is not None:
            toolbar_rect = QRect(
                self.toolbar.x(),
                self.toolbar.y(),
                self.toolbar.width(),
                self.toolbar.height()
            )
        
        def is_overlapping_toolbar(x, y):
            """检查预览面板是否与工具栏重叠"""
            if toolbar_rect is None:
                return False
            panel_rect = QRect(int(x), int(y), panel.width(), panel.height())
            return toolbar_rect.intersects(panel_rect)
        
        scroll_dir = getattr(self, 'scroll_direction', 'vertical')

        if scroll_dir == "vertical":
            # ===== 竖向截图：优先放左右，下边对齐 =====
            
            # 尝试1: 右边，下边对齐
            x_right = capture_right + margin
            if x_right + panel.width() <= screen_right - margin:
                x = x_right
                y = capture_bottom - panel.height()
                y = max(screen_top + margin, min(y, screen_bottom - panel.height() - margin))
                if not is_overlapping_toolbar(x, y):
                    panel.move(int(x), int(y))
                    return
            
            # 尝试2: 左边，下边对齐
            x_left = capture_left - panel.width() - margin
            if x_left >= screen_left + margin:
                x = x_left
                y = capture_bottom - panel.height()
                y = max(screen_top + margin, min(y, screen_bottom - panel.height() - margin))
                if not is_overlapping_toolbar(x, y):
                    panel.move(int(x), int(y))
                    return
            
            # 尝试3: 上边，水平居中
            y_top = capture_top - panel.height() - margin
            if toolbar_rect and toolbar_rect.bottom() >= y_top - margin:
                y_top = toolbar_rect.y() - panel.height() - margin
            if y_top >= screen_top + margin:
                x = capture_left + (self.width() - panel.width()) // 2
                x = max(screen_left + margin, min(x, screen_right - panel.width() - margin))
                if not is_overlapping_toolbar(x, y_top):
                    panel.move(int(x), int(y_top))
                    return
            
            # 尝试4: 下边，水平居中
            y_bottom = capture_bottom + margin
            if toolbar_rect and toolbar_rect.top() <= y_bottom + panel.height() + margin:
                y_bottom = toolbar_rect.bottom() + margin
            if y_bottom + panel.height() <= screen_bottom - margin:
                x = capture_left + (self.width() - panel.width()) // 2
                x = max(screen_left + margin, min(x, screen_right - panel.width() - margin))
                if not is_overlapping_toolbar(x, y_bottom):
                    panel.move(int(x), int(y_bottom))
                    return

        else:
            # ===== 横向截图：优先放上下，右边对齐 =====
            
            # 尝试1: 上边
            y_top = capture_top - panel.height() - margin
            if y_top >= screen_top + margin:
                x = capture_right - panel.width()
                x = max(screen_left + margin, min(x, screen_right - panel.width() - margin))
                if not is_overlapping_toolbar(x, y_top):
                    panel.move(int(x), int(y_top))
                    return
            
            # 尝试2: 下边（工具栏也在下面则再往下）
            y_bottom = capture_bottom + margin
            if toolbar_rect:
                tb_bottom = toolbar_rect.y() + toolbar_rect.height()
                if toolbar_rect.y() >= capture_bottom:
                    # 工具栏在截图区域下方，面板放到工具栏下面
                    y_bottom = max(y_bottom, tb_bottom + margin)
            if y_bottom + panel.height() <= screen_bottom - margin:
                x = capture_right - panel.width()
                x = max(screen_left + margin, min(x, screen_right - panel.width() - margin))
                panel.move(int(x), int(y_bottom))
                return
            
            # 尝试3: 右边
            x_right = capture_right + margin
            if x_right + panel.width() <= screen_right - margin:
                x = x_right
                y = capture_top + (self.height() - panel.height()) // 2
                y = max(screen_top + margin, min(y, screen_bottom - panel.height() - margin))
                if not is_overlapping_toolbar(x, y):
                    panel.move(int(x), int(y))
                    return
            
            # 尝试4: 左边
            x_left = capture_left - panel.width() - margin
            if x_left >= screen_left + margin:
                x = x_left
                y = capture_top + (self.height() - panel.height()) // 2
                y = max(screen_top + margin, min(y, screen_bottom - panel.height() - margin))
                if not is_overlapping_toolbar(x, y):
                    panel.move(int(x), int(y))
                    return
        
        # 兜底: 放在屏幕右上角（避免进入截图区域和工具栏）
        x = screen_right - panel.width() - margin
        y = screen_top + margin
        if is_overlapping_toolbar(x, y) and toolbar_rect:
            y = toolbar_rect.bottom() + margin
            if y + panel.height() > screen_bottom - margin:
                x = screen_left + margin
                y = screen_top + margin
        panel.move(int(x), int(y))

    def _refresh_preview_panel(self):
        """将最新拼接结果渲染到预览面板"""
        if not hasattr(self, 'preview_panel') or self.preview_panel is None:
            return
        screenshot_count = len(self.screenshots)
        display_image = None
        if self.stitched_result is not None:
            display_image = self.stitched_result
            # 向上/向左滚动模式：先翻转（顺序：先翻转再旋转）
            if self.scroll_locked_direction == "up" and screenshot_count >= 2:
                display_image = display_image.transpose(Image.FLIP_TOP_BOTTOM)
            if self.scroll_direction == "horizontal" and screenshot_count >= 2:
                display_image = display_image.rotate(90, expand=True)
        elif hasattr(self, '_last_screenshot') and self._last_screenshot is not None:
            # 内存优化：使用 _last_screenshot 代替直接访问列表
            display_image = self._last_screenshot
        self.preview_panel.update_preview(
            display_image,
            self.scroll_direction,
            screenshot_count
        )
        # 面板大小可能变化，重新定位到不遮挡截图区域的位置
        self._position_preview_panel()

    def _show_preview_warning(self, message: str):
        self.preview_warning_active = True
        if hasattr(self, 'preview_panel') and self.preview_panel is not None:
            self.preview_panel.show_warning(message)

    def _clear_preview_warning(self):
        if not self.preview_warning_active:
            return
        self.preview_warning_active = False
        if hasattr(self, 'preview_panel') and self.preview_panel is not None:
            self.preview_panel.clear_warning()

    def _handle_shrink_abort(self, screenshot_index: int):
        message = f"第 {screenshot_index} 张截图可能造成拼接收缩，已取消"
        print(f"🛑 {message}")
        if self.screenshots:
            self.screenshots.pop()
        if hasattr(self, 'preview_panel') and self.preview_panel:
            self.preview_panel.update_count(len(self.screenshots))
        self.current_scroll_distance = 0
        self._show_preview_warning(message)

    def _handle_stitch_failure(self, screenshot_index: int, detail: str):
        detail = detail or "拼接失败"
        message = f"第 {screenshot_index} 张图片拼接失败：{detail}"
        print(f"🗑️ 忽略第 {screenshot_index} 张截图，等待下一次滚动")
        if self.screenshots:
            try:
                self.screenshots.pop()
            except Exception as e:
                log_exception(e, "移除失败截图")
        if hasattr(self, 'preview_panel') and self.preview_panel:
            self.preview_panel.update_count(len(self.screenshots))
        self._show_preview_warning(message)
        
    def _setup_mouse_hook(self):
        """设置Windows鼠标钩子以监听全局滚轮事件"""
        try:
            # 使用Windows API设置窗口透明鼠标事件（需在主线程执行）
            hwnd = int(self.transparent_area.winId())
            user32 = ctypes.windll.user32
            ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_TRANSPARENT | WS_EX_LAYERED)
            print(f"[OK] 窗口已设置为鼠标穿透模式")

            # 将可能较慢的模块导入与监听器启动放到后台线程，避免首次阻塞UI
            import threading

            def _init_listener_bg():
                try:
                    from pynput import mouse  # 首次导入较慢，放后台

                    def on_scroll(x, y, dx, dy):
                        """滚轮事件回调（在pynput线程中）
                        dx: 横向滚动量（正值向右，负值向左）
                        dy: 纵向滚动量（正值向上，负值向下）
                        
                        注意:
                        - 横向模式: 监听 dx (横向滚轮) 和 dy (Shift+滚轮会产生横向滚动)
                        - 竖向模式: 只监听 dy (竖向滚轮)
                        """
                        if self._is_mouse_in_capture_area(x, y):
                            # 根据当前方向决定使用哪个滚动值
                            if self.scroll_direction == "horizontal":
                                # 横向模式：优先使用dx，也接受dy（Shift+滚轮）
                                scroll_val = dx if dx != 0 else (-dy if dy != 0 else 0)
                                
                                if scroll_val != 0:
                                    # 横向模式：方向由自动检测处理，所有方向都接受
                                    scroll_pixels = int(abs(scroll_val) * 25)
                                    
                                    if self.scroll_locked_direction is None:
                                        is_right = scroll_val > 0
                                        self.scroll_locked_direction = "down" if is_right else "up"
                                        arrow = "➡️" if is_right else "⬅️"
                                        print(f"{arrow} 锁定横向滚动方向: {'向右' if is_right else '向左'}")
                                    
                                    if ("down" if scroll_val > 0 else "up") == self.scroll_locked_direction:
                                        try:
                                            self.scroll_detected.emit(scroll_pixels)
                                        except Exception as e:
                                            print(f"[ERROR] 触发滚动信号失败: {e}", force=True)
                            else:
                                # 竖向模式：第一次滚动锁定方向，之后只接受同方向
                                if dy != 0:
                                    is_scroll_down = dy < 0  # pynput: dy<0=向下
                                    direction = "down" if is_scroll_down else "up"
                                    
                                    if self.scroll_locked_direction is None:
                                        # 第一次滚动，锁定方向
                                        self.scroll_locked_direction = direction
                                        arrow = "⬇️" if is_scroll_down else "⬆️"
                                        print(f"{arrow} 锁定滚动方向: {'向下' if is_scroll_down else '向上'}")
                                    
                                    if direction == self.scroll_locked_direction:
                                        scroll_pixels = int(abs(dy) * 25)
                                        try:
                                            self.scroll_detected.emit(scroll_pixels)
                                        except Exception as e:
                                            print(f"[ERROR] 触发滚动信号失败: {e}", force=True)
                                    else:
                                        # 反向滚动，忽略
                                        pass

                    # 创建并启动监听器（pynput内部也会使用线程）
                    self.mouse_listener = mouse.Listener(on_scroll=on_scroll)
                    self.mouse_listener.start()
                    print("[OK] 全局滚轮监听器已启动（竖向仅响应向下滚动，横向响应向右滚动和Shift+滚轮）")
                except Exception as e:
                    print(f"[ERROR] 设置鼠标钩子失败: {e}", force=True)
                    import traceback
                    traceback.print_exc()

            threading.Thread(target=_init_listener_bg, daemon=True).start()

        except Exception as e:
            print(f"[ERROR] 设置窗口鼠标穿透时出错: {e}", force=True)
            import traceback
            traceback.print_exc()
    
    def _toggle_direction(self):
        """切换截图方向（竖向/横向）"""
        if self.scroll_direction == "vertical":
            self.scroll_direction = "horizontal"
            self.toolbar.update_direction("horizontal")
            print("🔄 切换到横向截图模式")
        else:
            self.scroll_direction = "vertical"
            self.toolbar.update_direction("vertical")
            print("🔄 切换到竖向截图模式")
        
        # 重新配置拼接引擎
        self._reconfigure_stitch_engine()
        self._refresh_preview_panel()
        
        # 🆕 切换键盘监听器状态
        if self.scroll_direction == "horizontal":
            self._start_keyboard_listener()
        else:
            self._stop_keyboard_listener()
    
    def _send_horizontal_scroll(self):
        """发送横向滚动指令（向右滚动）"""
        try:
            import win32api
            import win32con
            
            # 使用Windows API发送横向滚动事件
            # MOUSEEVENTF_HWHEEL: 横向滚动事件
            # amount * 120: WHEEL_DELTA标准值
            amount = 1  # 向右滚动
            win32api.mouse_event(
                win32con.MOUSEEVENTF_HWHEEL,
                0, 0,
                amount * 120,  # WHEEL_DELTA
                0
            )
            print(f"[OK] 发送横向滚动指令: 向右滚动 {amount} 格")
            
        except Exception as e:
            print(f"[ERROR] 发送横向滚动失败: {e}", force=True)
            import traceback
            traceback.print_exc()
    
    def _start_keyboard_listener(self):
        """启动键盘监听器（用于横向模式）"""
        if self.keyboard_listener is not None:
            return  # 已经启动
        
        try:
            from pynput import keyboard
            
            def on_press(key):
                """按键按下回调"""
                try:
                    # 使用Shift键触发横向滚动+截图
                    if key == keyboard.Key.shift and not self.horizontal_scroll_key_pressed:
                        self.horizontal_scroll_key_pressed = True
                        print("⌨️ 检测到Shift按下，触发横向滚动+截图")
                        
                        # 发送横向滚动指令
                        self._send_horizontal_scroll()
                        
                        # 延迟后截图（给页面时间滚动）
                        QTimer.singleShot(int(self.scroll_cooldown * 1000), self._do_capture)
                        
                except Exception as e:
                    print(f"[ERROR] 处理按键事件失败: {e}", force=True)
            
            def on_release(key):
                """按键释放回调"""
                try:
                    if key == keyboard.Key.shift:
                        self.horizontal_scroll_key_pressed = False
                except Exception as e:
                    log_exception(e, "释放Shift键")
            
            # 创建并启动键盘监听器
            self.keyboard_listener = keyboard.Listener(
                on_press=on_press,
                on_release=on_release
            )
            self.keyboard_listener.start()
            print("[OK] 键盘监听器已启动（横向模式，按Shift触发）")
            
        except Exception as e:
            print(f"[ERROR] 启动键盘监听器失败: {e}", force=True)
            import traceback
            traceback.print_exc()
    
    def _stop_keyboard_listener(self):
        """停止键盘监听器"""
        if self.keyboard_listener is not None:
            try:
                self.keyboard_listener.stop()
                self.keyboard_listener = None
                print("[OK] 键盘监听器已停止")
            except Exception as e:
                print(f"[WARN] 停止键盘监听器时出错: {e}")
    
    def _reconfigure_stitch_engine(self):
        """重新配置拼接引擎方向"""
        try:
            from .jietuba_long_stitch_unified import configure, config
            
            # 横向和竖向都使用竖向拼接（direction=0）
            # 因为哈希匹配算法只支持竖向拼接
            # 横向截图时，图片会被旋转90度，拼接后再旋转回来
            direction = 0
            
            configure(
                engine=config.engine,
                direction=direction,
                sample_rate=config.sample_rate,
                min_sample_size=config.min_sample_size,
                max_sample_size=config.max_sample_size,
                verbose=True,
            )
            if self._original_cancel_on_shrink is None:
                self._original_cancel_on_shrink = config.cancel_on_shrink
            if not config.cancel_on_shrink:
                config.cancel_on_shrink = True
                print("🛑 启用拼接缩短保护：检测到风险时将取消本次拼接")
            
            mode_text = "横向截图（图片旋转90度+竖向拼接）" if self.scroll_direction == "horizontal" else "竖向截图（竖向拼接）"
            print(f"[OK] 拼接引擎已重新配置: {mode_text}")
            
            self._refresh_preview_panel()
                
        except Exception as e:
            print(f"[ERROR] 重新配置拼接引擎失败: {e}", force=True)
            import traceback
            traceback.print_exc()
    
    @safe_event
    def showEvent(self, event):
        """窗口显示事件 - 立即截取第一张图"""
        super().showEvent(event)
        
        # 验证窗口位置是否正确
        self._verify_window_position()

        # 延迟一次事件循环后强制将所有浮动子窗口提到 TOPMOST 栈顶，
        # 避免初始显示时被系统任务栏（同为 HWND_TOPMOST）压在下方。
        QTimer.singleShot(0, self._raise_all_topmost)

        # 使用QTimer延迟执行，确保窗口完全显示后再截图
        QTimer.singleShot(100, self._capture_initial_screenshot)

    def _raise_all_topmost(self):
        """将主窗口及所有浮动子窗口推到 TOPMOST z-order 顶部。"""
        self.raise_()
        if hasattr(self, 'toolbar') and self.toolbar is not None:
            self.toolbar.raise_()
        if hasattr(self, 'preview_panel') and self.preview_panel is not None:
            self.preview_panel.raise_()
    
    def _verify_window_position(self):
        """验证窗口位置是否正确"""
        try:
            app = QApplication.instance()
            
            # 获取窗口当前位置
            window_x = self.x()
            window_y = self.y()
            window_center = QPoint(window_x + self.width() // 2, window_y + self.height() // 2)
            
            # PyQt6: 找到窗口所在的显示器
            current_screen = app.screenAt(window_center)
            if current_screen is None:
                current_screen = app.primaryScreen()
            screen_geometry = current_screen.geometry()
            
            print(f"窗口位置验证:")
            print(f"   窗口位置: x={window_x}, y={window_y}")
            print(f"   窗口中心: x={window_center.x()}, y={window_center.y()}")
            print(f"   所在显示器: {current_screen}")
            print(f"   显示器范围: x={screen_geometry.x()}-{screen_geometry.x() + screen_geometry.width()}, y={screen_geometry.y()}-{screen_geometry.y() + screen_geometry.height()}")
            
            # 检查截图区域中心所在的显示器
            capture_center_x = self.capture_rect.x() + self.capture_rect.width() // 2
            capture_center_y = self.capture_rect.y() + self.capture_rect.height() // 2
            capture_center = QPoint(capture_center_x, capture_center_y)
            # PyQt6: 使用 screenAt() 代替 desktop.screenNumber()
            expected_screen = app.screenAt(capture_center)
            
            print(f"   截图区域中心: x={capture_center_x}, y={capture_center_y}")
            print(f"   期望显示器: {expected_screen}")
            
            if expected_screen and current_screen != expected_screen:
                print(f"[WARN] 警告: 窗口显示在显示器 {current_screen.name()}，但截图区域在不同的显示器")
                
                # 尝试移动窗口到截图区域所在的显示器
                capture_center_x = self.capture_rect.x() + self.capture_rect.width() // 2
                capture_center_y = self.capture_rect.y() + self.capture_rect.height() // 2
                capture_center = QPoint(capture_center_x, capture_center_y)
                target_screen = app.screenAt(capture_center)
                if target_screen is None:
                    target_screen = app.primaryScreen()
                
                target_screen_geometry = target_screen.geometry()
                # 计算在目标显示器上的相对位置
                relative_x = self.capture_rect.x() - 3  # border_width = 3
                relative_y = self.capture_rect.y() - 3
                
                # 确保不超出边界
                if (relative_x >= target_screen_geometry.x() and 
                    relative_y >= target_screen_geometry.y() and
                    relative_x + self.width() <= target_screen_geometry.x() + target_screen_geometry.width() and
                    relative_y + self.height() <= target_screen_geometry.y() + target_screen_geometry.height()):
                    
                    print(f"[FIX] 尝试移动窗口到正确位置: x={relative_x}, y={relative_y}")
                    self.move(relative_x, relative_y)
                    self.raise_()
                    self.activateWindow()
                else:
                    print(f"[WARN] 无法移动窗口到目标位置，可能会超出显示器边界")
            else:
                print("[OK] 窗口位置正确")
                
        except Exception as e:
            print(f"[ERROR] 验证窗口位置时出错: {e}", force=True)
    
    def _force_fix_window_position(self):
        """强制调整窗口位置。"""
        try:
            # 如果窗口不可见，先让它可见
            if not self.isVisible():
                print("[WARN] 检测到窗口不可见，强制显示")
                self.show()
                self.raise_()
                self.activateWindow()
                return
            
            app = QApplication.instance()
            
            # 获取窗口当前位置
            window_rect = self.geometry()
            
            # PyQt6: 检查窗口是否在任何显示器上可见
            visible_on_any_screen = False
            for screen in app.screens():
                screen_geometry = screen.geometry()
                if screen_geometry.intersects(window_rect):
                    visible_on_any_screen = True
                    break
            
            if not visible_on_any_screen:
                print("🚨 检测到窗口在所有显示器外，执行强制修复...")
                
                # 找到截图区域所在的显示器
                capture_center_x = self.capture_rect.x() + self.capture_rect.width() // 2
                capture_center_y = self.capture_rect.y() + self.capture_rect.height() // 2
                capture_center = QPoint(capture_center_x, capture_center_y)
                
                target_screen = app.screenAt(capture_center)
                if target_screen is None:
                    target_screen = app.primaryScreen()
                    print(f"[WARN] 截图区域不在任何显示器内，使用主显示器")
                
                target_geometry = target_screen.geometry()
                
                # 将窗口移动到目标显示器的中央
                new_x = target_geometry.x() + (target_geometry.width() - self.width()) // 2
                new_y = target_geometry.y() + (target_geometry.height() - self.height()) // 2
                
                print(f"[FIX] 强制移动窗口到显示器 {target_screen} 中央: x={new_x}, y={new_y}")
                self.move(new_x, new_y)
                self.raise_()
                self.activateWindow()
                
                # 更新窗口标题以提示用户
                self.setWindowTitle("長スクリーンショット - 位置が修正されました")
            else:
                print("[OK] 窗口位置验证通过")
                
        except Exception as e:
            print(f"[ERROR] 强制修复窗口位置时出错: {e}", force=True)
    
    def _capture_initial_screenshot(self):
        """截取初始截图（窗口显示时的区域内容）"""
        print("🎬 截取初始截图（第1张）...")
        self._do_capture()
        
        # 为初始截图生成哈希（用于后续去重）
        # 内存优化：使用 _last_screenshot 代替直接访问列表
        if len(self.screenshots) > 0 and self.capture_mode == "immediate":
            if hasattr(self, '_last_screenshot') and self._last_screenshot is not None:
                self.last_screenshot_hash = self._calculate_image_hash(self._last_screenshot)
        
        print(f"   初始截图完成，当前共 {len(self.screenshots)} 张")
    
    def _is_mouse_in_capture_area(self, x, y):
        """检查鼠标是否在截图区域内"""
        return (self.capture_rect.x() <= x <= self.capture_rect.x() + self.capture_rect.width() and
                self.capture_rect.y() <= y <= self.capture_rect.y() + self.capture_rect.height())
    
    def _handle_scroll_in_main_thread(self, scroll_distance):
        """在主线程中处理滚轮事件（立即截图模式）
        
        Args:
            scroll_distance: 滚动距离（像素）
        """
        import time
        
        # 累积滚动距离
        self.current_scroll_distance += scroll_distance
        
        # 更新最后滚动时间
        self.last_scroll_time = time.time()
        
        if self.capture_mode == "immediate":
            # 立即截图模式：延迟很短时间后截图（让滚动动画完成）
            # 横向模式需要额外增加0.15秒延迟
            delay = self.scroll_cooldown
            if self.scroll_direction == "horizontal":
                delay += 0.15
            if self.capture_timer.isActive():
                self.capture_timer.stop()
            self.capture_timer.start(int(delay * 1000))
            print(f"⚡ 检测到滚动，累积距离: {self.current_scroll_distance}px，{delay}秒后截图...")
        else:
            # 等待停止模式：启动检测定时器
            if not self.scroll_check_timer.isActive():
                self.scroll_check_timer.start()
                print("🔄 开始检测滚动停止...")
    
    def _check_scroll_stopped(self):
        """定期检查滚动是否已停止（仅在等待模式下使用）"""
        import time
        
        current_time = time.time()
        time_since_last_scroll = current_time - self.last_scroll_time
        
        # 如果距离上次滚动已经超过冷却时间
        if time_since_last_scroll >= self.scroll_cooldown:
            # 滚动已停止，停止检测定时器
            self.scroll_check_timer.stop()
            
            # 执行截图
            print(f"✋ 滚动已停止 ({time_since_last_scroll:.2f}秒)，开始截图...")
            self._do_capture()
        else:
            # 还在滚动，继续等待
            remaining = self.scroll_cooldown - time_since_last_scroll
            print(f"⏳ 等待滚动停止... (还需 {remaining:.1f}秒)", end='\r')
    
    def _calculate_image_hash(self, pil_image):
        """计算图片的感知哈希值（用于相似度比较）"""
        import hashlib
        
        # 缩小图片到8x8用于快速比较
        small_img = pil_image.resize((16, 16), Image.Resampling.LANCZOS)
        # 转为灰度
        gray_img = small_img.convert('L')
        # 计算平均值
        pixels = list(gray_img.getdata())
        avg = sum(pixels) / len(pixels)
        # 生成哈希（大于平均值为1，小于为0）
        hash_str = ''.join('1' if p > avg else '0' for p in pixels)
        return hash_str
    
    def _images_are_similar(self, hash1, hash2):
        """比较两个哈希值的相似度"""
        if hash1 is None or hash2 is None:
            return False
        
        # 计算汉明距离（不同位的数量）
        diff_bits = sum(c1 != c2 for c1, c2 in zip(hash1, hash2))
        similarity = 1 - (diff_bits / len(hash1))
        
        return similarity >= self.duplicate_threshold

    def _exclude_overlapping_ui(self, exclude: bool):
        """检测 UI 窗口是否与截图区域重叠，按需排除/恢复截图捕获"""
        from core.platform_utils import set_window_exclude_from_capture
        for widget in (getattr(self, 'toolbar', None), getattr(self, 'preview_panel', None)):
            if widget is None or not widget.isVisible():
                continue
            widget_rect = QRect(widget.x(), widget.y(), widget.width(), widget.height())
            if widget_rect.intersects(self.capture_rect):
                set_window_exclude_from_capture(int(widget.winId()), exclude)
    
    def _do_capture(self):
        """执行截图并实时拼接"""
        stitch_successful = True
        # 截图前：排除与截图区域重叠的 UI 窗口
        self._exclude_overlapping_ui(True)
        try:
            current_count = len(self.screenshots) + 1
            
            # 获取包含截图区域的屏幕
            app = QGuiApplication.instance()
            capture_center_x = self.capture_rect.x() + self.capture_rect.width() // 2
            capture_center_y = self.capture_rect.y() + self.capture_rect.height() // 2
            center_point = QPoint(capture_center_x, capture_center_y)
            
            screen = app.screenAt(center_point)
            if screen is None:
                print("[WARN] 截图区域不在任何显示器范围内，使用主显示器", force=True)
                screen = app.primaryScreen()
            
            screen_geometry = screen.geometry()
            
            # 将虚拟桌面坐标转换为相对于目标屏幕的坐标
            relative_x = self.capture_rect.x() - screen_geometry.x()
            relative_y = self.capture_rect.y() - screen_geometry.y()
            
            # 使用屏幕相对坐标截图
            pixmap = screen.grabWindow(
                0,
                relative_x,
                relative_y,
                self.capture_rect.width(),
                self.capture_rect.height()
            )
            
            if pixmap.isNull():
                print("[ERROR] 截图失败", force=True)
                return
            
            # PySide6: bits() 返回 memoryview，直接转 bytes
            qimage = pixmap.toImage()
            buffer = bytes(qimage.bits())
            pil_image = Image.frombytes(
                'RGBA',
                (qimage.width(), qimage.height()),
                buffer,
                'raw',
                'BGRA'
            ).convert('RGB')
            
            # 横向模式：从第2张图片开始旋转90度（顺时针）以便使用竖向拼接算法
            is_first_image = len(self.screenshots) == 0
            if self.scroll_direction == "horizontal" and not is_first_image:
                pil_image = pil_image.rotate(-90, expand=True)
            
            # 向上滚动模式：翻转图片，把"从下往上"变成"从上往下"
            if self.scroll_locked_direction == "up":
                pil_image = pil_image.transpose(Image.FLIP_TOP_BOTTOM)
            
            self._last_screenshot = pil_image
            self._screenshot_count = getattr(self, '_screenshot_count', 0) + 1
            
            # screenshots 列表只保留计数，不存储实际图像
            if not hasattr(self, '_screenshots_count_only'):
                self._screenshots_count_only = True
                self.screenshots.clear()
            self.screenshots.append(None)
            
            screenshot_count = len(self.screenshots)
            
            try:
                from .jietuba_long_stitch_unified import stitch_images, stitch_images_auto
                from .jietuba_long_stitch import AllOverlapShrinkError

                if self.stitched_result is None:
                    # 第一张图片
                    self.stitched_result = pil_image
                else:
                    # 增量拼接
                    # 横向模式：如果是第2张图片，需要先将第1张图片也旋转
                    if self.scroll_direction == "horizontal" and screenshot_count == 2:
                        self.stitched_result = self.stitched_result.rotate(-90, expand=True)
                    
                    # 向上/向左滚动模式：如果是第2张图片，需要先将第1张也翻转
                    if self.scroll_locked_direction == "up" and screenshot_count == 2:
                        self.stitched_result = self.stitched_result.transpose(Image.FLIP_TOP_BOTTOM)
                    
                    # 自动方向检测：第一次拼接时（方向未锁定），使用 Rust auto 接口
                    if self.scroll_locked_direction is None and screenshot_count == 2:
                        result, direction = stitch_images_auto(
                            self.stitched_result, pil_image, debug=False
                        )
                        if result is not None and direction == "reverse":
                            self.scroll_locked_direction = "up"
                            # Rust auto 返回的是翻转态（对翻转图片拼接的产物），
                            # 与 stitched_result 的全程翻转态约定一致，直接存储即可
                            self.stitched_result = result
                            arrow = "⬆️" if self.scroll_direction == "vertical" else "⬅️"
                            print(f"{arrow} 自动检测到反向滚动，已锁定")
                            result = "HANDLED"
                        elif result is not None:
                            self.scroll_locked_direction = "down"
                            self.stitched_result = result
                            result = "HANDLED"
                    else:
                        # 方向已锁定，正常拼接
                        try:
                            result = stitch_images([self.stitched_result, pil_image])
                        except AllOverlapShrinkError:
                            self._handle_shrink_abort(current_count)
                            return
                    
                    if result == "HANDLED":
                        pass  # 已在上面处理
                    elif result:
                        self.stitched_result = result
                    else:
                        print(f"[WARN] 第 {screenshot_count} 张拼接失败，未找到重叠区域", force=True)
                        stitch_successful = False
                        self._handle_stitch_failure(screenshot_count, "未找到可靠的重叠区域")
                        
            except Exception as e:
                print(f"[WARN] 第 {screenshot_count} 张拼接出错: {e}", force=True)
                import traceback
                traceback.print_exc()
                stitch_successful = False
                self._handle_stitch_failure(screenshot_count, f"算法异常：{e}")
                
                if self.stitched_result is None:
                    self.stitched_result = pil_image
            
            if stitch_successful:
                if len(self.screenshots) == 1:
                    self.scroll_distances.append(0)
                else:
                    self.scroll_distances.append(self.current_scroll_distance)
                self.current_scroll_distance = 0

                if hasattr(self, 'preview_panel') and self.preview_panel:
                    self.preview_panel.update_count(len(self.screenshots))

                # 只输出一行关键信息
                w, h = self.stitched_result.size
                print(f"📸 第 {screenshot_count} 张 → 拼接结果: {w}x{h}")
                self._clear_preview_warning()
            else:
                self.current_scroll_distance = 0
            
            self._refresh_preview_panel()
            
        except Exception as e:
            print(f"[ERROR] 截图时出错: {e}", force=True)
            import traceback
            traceback.print_exc()
        finally:
            # 截图完成：恢复 UI 窗口可被截图
            self._exclude_overlapping_ui(False)
    
    @safe_event
    def paintEvent(self, event):
        """绘制窗口边框"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        # 绘制半透明边框（在窗口边缘，不影响截图区域）
        pen = QPen(QColor(0, 120, 215), 3)  # 蓝色边框，3像素
        painter.setPen(pen)
        
        # 边框应该绘制在整个窗口的边缘
        # 窗口大小 = capture_rect + 边框(3px * 2)
        border_width = 3
        border_rect = QRect(
            1,  # 从窗口边缘开始
            1,
            self.width() - 2,  # 整个窗口宽度 - 2px（线宽的一半）
            self.height() - 2  # 整个窗口高度 - 2px
        )
        painter.drawRect(border_rect)
        
        painter.end()

    @safe_event
    def moveEvent(self, event):
        super().moveEvent(event)
        self._position_preview_panel()
        self._position_floating_toolbar()

    @safe_event
    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._position_preview_panel()
        self._position_floating_toolbar()
    
    def _on_finish(self):
        """完成按钮点击"""
        print(f"[OK] 完成长截图，共 {len(self.screenshots)} 张图片", force=True)
        
        # 横向模式：将拼接结果逆时针旋转90度还原
        # 只有在有2张及以上图片（发生了拼接）时才旋转
        # 如果只有1张图片，不需要旋转（第1张图片没有被旋转）
        # 向上/向左滚动模式：先翻转还原（必须在横向旋转之前）
        if (self.scroll_locked_direction == "up" and 
            self.stitched_result is not None and
            len(self.screenshots) >= 2):
            self.stitched_result = self.stitched_result.transpose(Image.FLIP_TOP_BOTTOM)
        
        # 横向模式：将拼接结果逆时针旋转90度还原
        if (self.scroll_direction == "horizontal" and 
            self.stitched_result is not None and 
            len(self.screenshots) >= 2):
            print(f"🔄 横向模式：将拼接结果逆时针旋转90度还原（共{len(self.screenshots)}张）")
            print(f"   旋转前尺寸: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
            self.stitched_result = self.stitched_result.rotate(90, expand=True)
            print(f"   旋转后尺寸: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
        elif self.scroll_direction == "horizontal" and len(self.screenshots) == 1:
            print(f"📸 横向模式：只有1张图片，无需旋转")
        
        # 自动保存文件
        self._save_result()
        
        # 复制到剪贴板
        self._copy_to_clipboard()
        
        self._cleanup()
        self.finished.emit()
        self.close()
    
    def set_save_directory(self, directory):
        """设置保存目录"""
        self.save_directory = directory
    
    def _save_result(self):
        """提交拼接结果的异步保存任务"""
        if self.stitched_result is None:
            print("[WARN] 没有拼接结果，跳过保存")
            return

        direction_suffix = "横" if self.scroll_direction == "horizontal" else "縦"
        target_dir = self.save_directory

        try:
            task_path = self.save_service.save_pil_async(
                self.stitched_result,
                directory=target_dir,
                prefix="長スクショ",
                suffix=direction_suffix,
                image_format="PNG"
            )
            if task_path:
                print(f"[SAVE] 长截图保存任务已提交: {task_path}")
            else:
                print("[ERROR] 无法提交长截图保存任务")
        except Exception as exc:
            print(f"[ERROR] 提交长截图保存任务失败: {exc}")
            import traceback
            traceback.print_exc()

    def _copy_to_clipboard(self):
        """将拼接结果复制到剪贴板"""
        if self.stitched_result is None:
            return
            
        try:
            # 转换为 QImage
            image = self.stitched_result.convert("RGBA")
            width, height = image.size
            data = image.tobytes("raw", "RGBA")
            
            # 创建 QImage (引用 data)
            qimage = QImage(data, width, height, width * 4, QImage.Format.Format_RGBA8888)
            
            # 复制到剪贴板（必须使用 copy() 创建深拷贝，避免 data 被回收后崩溃）
            clipboard = QApplication.clipboard()
            clipboard.setImage(qimage.copy())
            print("长截图已复制到剪贴板")
        except Exception as e:
            print(f"[ERROR] 复制到剪贴板失败: {e}")
            import traceback
            traceback.print_exc()
    
    def _on_manual_capture(self):
        """手动截图（从工具栏触发）"""
        try:
            print("🖱️ 用户手动触发截图...")
            # 立即执行截图
            self._do_capture()
        except Exception as e:
            print(f"[ERROR] 手动截图失败: {e}", force=True)
            import traceback
            traceback.print_exc()
    
    def _on_pin(self):
        """钉图按钮点击 - 将当前拼接结果钉到桌面，然后结束长截图"""
        print("钉图长截图结果...")
        
        # 检查 config_manager
        if self.config_manager is None:
            print("[ERROR] config_manager 未设置，无法创建钉图")
            return
        
        # 获取拼接结果
        result_image = self.stitched_result
        
        if result_image is None:
            print("[WARN] 没有拼接结果，无法钉图")
            return
        
        # 向上/向左滚动模式：先翻转还原（必须在横向旋转之前）
        if (self.scroll_locked_direction == "up" and
            len(self.screenshots) >= 2):
            result_image = result_image.transpose(Image.FLIP_TOP_BOTTOM)
        
        # 横向模式：旋转结果
        if (self.scroll_direction == "horizontal" and 
            len(self.screenshots) >= 2):
            print(f"🔄 横向模式：旋转图片...")
            result_image = result_image.rotate(90, expand=True)
        
        # 转换 PIL Image 到 QImage（使用原图，不缩放）
        try:
            from PySide6.QtGui import QImage
            from PySide6.QtCore import QPoint
            
            image_rgba = result_image.convert("RGBA")
            width, height = image_rgba.size
            data = image_rgba.tobytes("raw", "RGBA")
            
            qimage = QImage(data, width, height, width * 4, QImage.Format.Format_RGBA8888).copy()
            
            # 获取当前长截图窗口所在的屏幕（支持多屏幕）
            # 以长截图区域的中心为基准定位钉图窗口
            capture_center = self.capture_rect.center()
            pin_x = capture_center.x() - width // 2
            pin_y = capture_center.y() - height // 2
            
            position = QPoint(pin_x, pin_y)
            
            # 创建钉图（使用原图）
            from pin.pin_manager import PinManager
            pin_manager = PinManager.instance()
            
            pin_window = pin_manager.create_pin(
                image=qimage,
                position=position,
                config_manager=self.config_manager,
                drawing_items=None,  # 长截图不继承绘制项目
                selection_offset=None
            )
            
            log_debug(f"钉图已创建，位置: ({pin_x}, {pin_y})", module=_MODULE_TAG)
            
            # 钉图完成后，清理并关闭长截图窗口
            self._cleanup()
            self.finished.emit()
            self.close()
            
        except Exception as e:
            print(f"[ERROR] 创建钉图失败: {e}")
            import traceback
            traceback.print_exc()
    
    def _on_cancel(self):
        """取消按钮点击"""
        print("[ERROR] 取消长截图", force=True)
        self.screenshots.clear()
        self._cleanup()
        self.cancelled.emit()
        self.close()
    
    def _cleanup(self):
        """清理资源"""
        try:
            if hasattr(self, 'screenshots'):
                self.screenshots.clear()
                self.screenshots = []
            
            if hasattr(self, '_last_screenshot'):
                self._last_screenshot = None
            if hasattr(self, '_screenshot_count'):
                self._screenshot_count = 0
            
            if hasattr(self, 'stitched_result'):
                self.stitched_result = None
            
            import gc
            gc.collect()
                
            if self._original_cancel_on_shrink is not None:
                from .jietuba_long_stitch_unified import config as long_config
                long_config.cancel_on_shrink = self._original_cancel_on_shrink
                self._original_cancel_on_shrink = None

            # 关闭浮动工具栏
            if hasattr(self, 'toolbar') and self.toolbar:
                try:
                    self.toolbar.close()
                    print("[OK] 浮动工具栏已关闭")
                except Exception as e:
                    print(f"[WARN] 关闭工具栏时出错: {e}")
            
            # 关闭预览面板
            if hasattr(self, 'preview_panel') and self.preview_panel:
                try:
                    self.preview_panel.close()
                    print("[OK] 预览面板已关闭")
                except Exception as e:
                    print(f"[WARN] 关闭预览面板时出错: {e}")
                finally:
                    self.preview_panel = None

            # 停止所有定时器
            if hasattr(self, 'capture_timer'):
                self.capture_timer.stop()
            
            if hasattr(self, 'scroll_check_timer'):
                self.scroll_check_timer.stop()
            
            if hasattr(self, '_position_fix_timer'):
                self._position_fix_timer.stop()
            
            # 停止鼠标监听器
            if hasattr(self, 'mouse_listener'):
                self.mouse_listener.stop()
                print("[OK] 全局滚轮监听器已停止")
            
            # 🆕 停止键盘监听器
            self._stop_keyboard_listener()
            
        except Exception as e:
            print(f"[WARN] 清理资源时出错: {e}")
    
    @safe_event
    def closeEvent(self, event):
        """窗口关闭事件"""
        self._cleanup()
        super().closeEvent(event)
    
    def get_screenshots(self):
        """获取所有截图"""
        return self.screenshots
    
    def get_stitched_result(self):
        """获取实时拼接的结果图
        
        Returns:
            PIL.Image: 拼接好的完整图片，如果没有截图则返回None
            
        注意：
            - 竖向模式：返回原始拼接结果
            - 横向模式：返回旋转后的结果（在_on_finish中已处理）
        """
        return self.stitched_result
    
    def get_scroll_distances(self):
        """获取所有滚动距离记录
        
        Returns:
            List[int]: 滚动距离列表，每个元素表示相邻两张截图之间的估计滚动距离（像素）
        """
        return self.scroll_distances
 