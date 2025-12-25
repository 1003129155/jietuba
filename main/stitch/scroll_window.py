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
import builtins
from ctypes import wintypes
from datetime import datetime
from PyQt6.QtWidgets import QWidget, QPushButton, QVBoxLayout, QHBoxLayout, QLabel, QApplication
from PyQt6.QtCore import Qt, QRect, QTimer, pyqtSignal, QPoint, QMetaObject, Q_ARG
from PyQt6.QtGui import QPainter, QPen, QColor, QPixmap, QGuiApplication, QImage
from typing import Optional
from PIL import Image

# Windows API 常量
GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
from ctypes import wintypes
from datetime import datetime
from PyQt6.QtWidgets import QWidget, QPushButton, QVBoxLayout, QHBoxLayout, QLabel, QApplication
from PyQt6.QtCore import Qt, QRect, QTimer, pyqtSignal, QPoint, QMetaObject, Q_ARG, QSettings
from PyQt6.QtGui import QPainter, QPen, QColor, QPixmap, QGuiApplication, QImage
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
_config = get_tool_settings_manager()
_LONG_STITCH_DEBUG_ENABLED = False
del _config

_BUILTIN_PRINT = builtins.print

def _long_stitch_print(*args, force: bool = False, **kwargs):
    """根据调试开关决定是否输出日志"""
    if _LONG_STITCH_DEBUG_ENABLED or force:
        _BUILTIN_PRINT(*args, **kwargs)


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
    
    # 🆕 如果检测到auto或rust，强制切换为hash_python
    if engine in ('auto', 'rust'):
        print(f"⚠️ 检测到已禁用的引擎 {engine}，自动切换为 hash_python")
        engine = 'hash_python'
        config.set_long_stitch_engine(engine)
    elif engine != raw_engine:
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
    
    # 🆕 如果检测到auto或rust，强制切换为hash_python
    if engine in ('auto', 'rust'):
        print(f"⚠️ 检测到已禁用的引擎 {engine}，自动切换为 hash_python")
        engine = 'hash_python'
        config_mgr.set_long_stitch_engine(engine)
    elif engine != raw_engine:
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

class FloatingToolbar(QWidget):
    """可拖动的浮动工具栏窗口"""
    
    # 信号定义
    direction_changed = pyqtSignal()
    manual_capture = pyqtSignal()
    finish_clicked = pyqtSignal()
    cancel_clicked = pyqtSignal()
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.parent_window = parent
        
        # 拖动相关
        self.dragging = False
        self.drag_position = QPoint()
        self.resize_mode = None  # None, 'left', 'right'
        self.resize_start_pos = QPoint()
        self.resize_start_geometry = QRect()
        
        self._setup_toolbar_window()
        self._setup_toolbar_ui()
        
    def _setup_toolbar_window(self):
        """设置工具栏窗口属性"""
        self.setWindowFlags(
            Qt.WindowType.WindowStaysOnTopHint | 
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        
        # 设置初始大小
        self.setFixedHeight(40)
        self.setMinimumWidth(500)
        
        # 注意：工具栏不设置鼠标穿透，因为需要响应按钮点击
        # 如果工具栏遮挡截图区域，位置调整逻辑会自动避开
        
    def _setup_toolbar_ui(self):
        """设置工具栏UI"""
        # 主容器
        container = QWidget()
        container.setStyleSheet("""
            QWidget {
                background-color: rgba(40, 40, 40, 230);
                border: 2px solid #555;
                border-radius: 5px;
            }
        """)
        
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.addWidget(container)
        
        # 工具栏布局
        toolbar_layout = QHBoxLayout(container)
        toolbar_layout.setContentsMargins(10, 5, 10, 5)
        toolbar_layout.setSpacing(8)
        
        # 左侧拖动手柄
        left_handle = QLabel("⋮⋮")
        left_handle.setStyleSheet("""
            color: #888; 
            font-size: 14pt; 
            font-weight: bold;
            padding: 0 5px;
        """)
        left_handle.setCursor(Qt.CursorShape.SizeHorCursor)
        left_handle.setToolTip("ドラッグして移動")
        toolbar_layout.addWidget(left_handle)
        self.left_handle = left_handle
        
        # 方向切换按钮
        self.direction_btn = QPushButton("↕️ 縦")
        self.direction_btn.setStyleSheet("""
            QPushButton {
                background-color: #2196F3;
                color: white;
                border: none;
                padding: 5px 10px;
                font-size: 9pt;
                border-radius: 3px;
                font-weight: bold;
                min-width: 50px;
            }
            QPushButton:hover {
                background-color: #1976D2;
            }
        """)
        self.direction_btn.clicked.connect(self.direction_changed.emit)
        toolbar_layout.addWidget(self.direction_btn)
        
        # 提示文字标签
        self.tip_label = QLabel("上から下へゆっくりスクロール")
        self.tip_label.setStyleSheet("color: #FFD700; font-size: 8pt; font-weight: bold;")
        toolbar_layout.addWidget(self.tip_label)
        
        toolbar_layout.addStretch()
        
        # 截图计数标签
        self.count_label = QLabel("スクショ: 0 枚")
        self.count_label.setStyleSheet("""
            color: white; 
            font-size: 9pt;
            padding: 5px 10px;
            border-radius: 3px;
            background-color: rgba(255, 255, 255, 0.1);
        """)
        self.count_label.setCursor(Qt.CursorShape.PointingHandCursor)
        self.count_label.setToolTip("クリックして手動でスクリーンショット")
        self.count_label.mousePressEvent = lambda event: self._on_count_label_clicked(event)
        toolbar_layout.addWidget(self.count_label)
        
        # 完成按钮
        self.finish_btn = QPushButton("完了")
        self.finish_btn.setStyleSheet("""
            QPushButton {
                background-color: #4CAF50;
                color: white;
                border: none;
                padding: 5px 10px;
                font-size: 9pt;
                border-radius: 3px;
                font-weight: bold;
                min-width: 50px;
            }
            QPushButton:hover {
                background-color: #45a049;
            }
        """)
        self.finish_btn.clicked.connect(self.finish_clicked.emit)
        toolbar_layout.addWidget(self.finish_btn)
        
        # 取消按钮
        self.cancel_btn = QPushButton("キャンセル")
        self.cancel_btn.setStyleSheet("""
            QPushButton {
                background-color: #f44336;
                color: white;
                border: none;
                padding: 5px 10px;
                font-size: 9pt;
                border-radius: 3px;
                min-width: 70px;
            }
            QPushButton:hover {
                background-color: #da190b;
            }
        """)
        self.cancel_btn.clicked.connect(self.cancel_clicked.emit)
        toolbar_layout.addWidget(self.cancel_btn)
        
        # 右侧拖动手柄
        right_handle = QLabel("⋮⋮")
        right_handle.setStyleSheet("""
            color: #888; 
            font-size: 14pt; 
            font-weight: bold;
            padding: 0 5px;
        """)
        right_handle.setCursor(Qt.CursorShape.SizeHorCursor)
        right_handle.setToolTip("ドラッグして移動")
        toolbar_layout.addWidget(right_handle)
        self.right_handle = right_handle
        
    def _on_count_label_clicked(self, event):
        """点击计数标签触发手动截图"""
        original_style = self.count_label.styleSheet()
        self.count_label.setStyleSheet("""
            color: white; 
            font-size: 9pt;
            padding: 5px 10px;
            border-radius: 3px;
            background-color: rgba(33, 150, 243, 200);
        """)
        self.manual_capture.emit()
        QTimer.singleShot(200, lambda: self.count_label.setStyleSheet(original_style))
        
    def update_count(self, count):
        """更新截图计数"""
        self.count_label.setText(f"スクショ: {count} 枚")
        
    def update_direction(self, direction):
        """更新方向显示"""
        if direction == "horizontal":
            self.direction_btn.setText("↔️ 横")
            self.tip_label.setText(" Shift、ボタン")
        else:
            self.direction_btn.setText("↕️ 縦")
            self.tip_label.setText(" 上から下へゆっくりスクロール")
    
    def mousePressEvent(self, event):
        """鼠标按下事件 - 开始拖动或调整大小"""
        if event.button() == Qt.MouseButton.LeftButton:
            # 检查是否点击在手柄上
            left_handle_rect = self.left_handle.geometry()
            right_handle_rect = self.right_handle.geometry()
            
            pos = event.pos()
            
            if left_handle_rect.contains(pos) or right_handle_rect.contains(pos):
                # 点击在手柄上 - 开始拖动
                self.dragging = True
                self.drag_position = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
                self.setCursor(Qt.CursorShape.ClosedHandCursor)
            elif pos.x() < 20:
                # 点击在左边缘 - 左侧调整大小
                self.resize_mode = 'left'
                self.resize_start_pos = event.globalPosition().toPoint()
                self.resize_start_geometry = self.geometry()
            elif pos.x() > self.width() - 20:
                # 点击在右边缘 - 右侧调整大小
                self.resize_mode = 'right'
                self.resize_start_pos = event.globalPosition().toPoint()
                self.resize_start_geometry = self.geometry()
                
    def mouseMoveEvent(self, event):
        """鼠标移动事件 - 执行拖动或调整大小"""
        if event.buttons() == Qt.MouseButton.LeftButton:
            if self.dragging:
                # 拖动窗口
                self.move(event.globalPosition().toPoint() - self.drag_position)
            elif self.resize_mode == 'left':
                # 从左边调整大小
                delta = event.globalPosition().toPoint() - self.resize_start_pos
                new_x = self.resize_start_geometry.x() + delta.x()
                new_width = self.resize_start_geometry.width() - delta.x()
                
                if new_width >= self.minimumWidth():
                    self.setGeometry(new_x, self.y(), new_width, self.height())
            elif self.resize_mode == 'right':
                # 从右边调整大小
                delta = event.globalPosition().toPoint() - self.resize_start_pos
                new_width = self.resize_start_geometry.width() + delta.x()
                
                if new_width >= self.minimumWidth():
                    self.resize(new_width, self.height())
        else:
            # 更新鼠标光标
            pos = event.pos()
            if pos.x() < 20 or pos.x() > self.width() - 20:
                self.setCursor(Qt.CursorShape.SizeHorCursor)
            else:
                self.setCursor(Qt.CursorShape.ArrowCursor)
                
    def mouseReleaseEvent(self, event):
        """鼠标释放事件 - 结束拖动或调整大小"""
        if event.button() == Qt.MouseButton.LeftButton:
            self.dragging = False
            self.resize_mode = None
            self.setCursor(Qt.CursorShape.ArrowCursor)

class PreviewPanel(QWidget):
    """实时预览面板，仅以透明背景展示拼接缩略图"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(Qt.WindowType.WindowStaysOnTopHint | Qt.WindowType.FramelessWindowHint | Qt.WindowType.Tool)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setFixedSize(210, 240)
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
            print(f"✅ PreviewPanel 已设置为鼠标穿透模式")
        except Exception as e:
            print(f"⚠️ 设置 PreviewPanel 鼠标穿透失败: {e}")

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
        scaled = pixmap.scaled(
            self.preview_label.size(),
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation
        )
        self.preview_label.setPixmap(scaled)
        self.preview_label.setText("")

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

class ScrollCaptureWindow(QWidget):
    """滚动长截图窗口
    
    特性：
    - 带边框的透明窗口
    - 不拦截鼠标滚轮事件（鼠标可以直接操作后面的网页）
    - 监听全局滚轮事件，每次滚轮后1秒截图
    - 底部有完成和取消按钮
    """
    
    finished = pyqtSignal()  # 完成信号
    cancelled = pyqtSignal()  # 取消信号
    scroll_detected = pyqtSignal(int)  # 滚轮检测信号（用于线程安全通信），传递滚动距离
    
    def __init__(self, capture_rect, parent=None):
        """初始化滚动截图窗口
        
        Args:
            capture_rect: QRect，截图区域（屏幕坐标）
            parent: 父窗口
        """
        super().__init__(parent)
        
        self.capture_rect = capture_rect
        self.screenshots = []  # 存储截图的列表
        self.scroll_distances = []  # 存储每次滚动的距离（像素）
        self.current_scroll_distance = 0  # 当前累积的滚动距离
        
        # 保存目录（由外部设置）
        self.save_directory = None
        self.save_service = SaveService()
        
        # 🆕 截图方向: "vertical"(竖向) 或 "horizontal"(横向)
        self.scroll_direction = "vertical"
        
        # 🆕 横向模式的键盘监听器
        self.keyboard_listener = None
        self.horizontal_scroll_key_pressed = False  # 防止重复触发
        
        # 实时拼接相关
        self.stitched_result = None  # 当前拼接的结果图
        self.preview_warning_active = False
        self._original_cancel_on_shrink = None
        
        # 🆕 会话级别的引擎状态（整个滚动截图期间保持一致）
        # None=未初始化, "rust"=特征匹配, "hash_rust"/"hash_python"=哈希匹配
        # 一旦设置后就不会改变（除非从rust失败切换到hash_rust）
        self.session_engine = None
        
        # 🚀 特征匹配专用：持久化的拼接器实例（增量拼接）
        self.rust_stitcher = None  # RustLongStitch 实例
        
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
        self.scroll_detected.connect(self._handle_scroll_in_main_thread)
        
        print(f"🎯 [ScrollCaptureWindow.__init__] 收到 capture_rect: x={capture_rect.x()}, y={capture_rect.y()}, w={capture_rect.width()}, h={capture_rect.height()}", force=True)
        
        self._setup_window()
        self._setup_ui()
        self._setup_mouse_hook()
        
        # 创建独立的浮动工具栏
        self._setup_floating_toolbar()

        # 创建实时拼接预览面板
        self._setup_preview_panel()
        
        print(f"🎯 [ScrollCaptureWindow.__init__] 窗口几何: x={self.x()}, y={self.y()}, w={self.width()}, h={self.height()}", force=True)
        
        # 添加强制窗口定位修复定时器（作为最后的保险）
        self._position_fix_timer = QTimer()
        self._position_fix_timer.setSingleShot(True)
        self._position_fix_timer.timeout.connect(self._force_fix_window_position)
        self._position_fix_timer.start(200)  # 200ms后再次检查并修复
    
    def _get_correct_window_position(self, border_width):
        """获取正确的窗口位置，修复多显示器环境下的定位问题"""
        try:
            # 注意：传入的capture_rect已经是真实坐标（在start_long_screenshot_mode中已转换）
            real_x = self.capture_rect.x()
            real_y = self.capture_rect.y()
            real_x1 = real_x + self.capture_rect.width()
            real_y1 = real_y + self.capture_rect.height()
            
            print(f"🎯 [长截图窗口] 截图区域坐标: ({real_x}, {real_y}) -> ({real_x1}, {real_y1})", force=True)
            
            # 使用父窗口的屏幕检测方法（与钉图窗口一致）
            target_screen = None
            screen_x, screen_y, screen_w, screen_h = 0, 0, 0, 0
            
            if (hasattr(self, 'parent') and self.parent and 
                hasattr(self.parent, 'get_screen_for_rect')):
                target_screen = self.parent.get_screen_for_rect(real_x, real_y, real_x1, real_y1)
                screen_rect = target_screen.geometry().getRect()
                screen_x, screen_y, screen_w, screen_h = screen_rect
                print(f"🎯 [长截图] 检测到目标显示器: x={screen_x}, y={screen_y}, w={screen_w}, h={screen_h}", force=True)
            else:
                # 回退到原来的方法 - PyQt6 版本
                # 使用左上角而不是中心来检测屏幕（避免跨显示器时检测错误）
                app = QApplication.instance()
                top_left_point = QPoint(real_x, real_y)
                
                # PyQt6: 使用 screenAt() 代替 desktop().screenNumber()
                screen = app.screenAt(top_left_point)
                if screen is None:
                    print(f"⚠️ 截图区域左上角({real_x}, {real_y})不在任何显示器范围内，使用主显示器", force=True)
                    screen = app.primaryScreen()
                else:
                    print(f"📺 截图区域左上角({real_x}, {real_y})位于显示器", force=True)
                
                screen_geometry = screen.geometry()
                screen_x, screen_y = screen_geometry.x(), screen_geometry.y()
                screen_w, screen_h = screen_geometry.width(), screen_geometry.height()
                print(f"📺 显示器几何信息: x={screen_x}, y={screen_y}, w={screen_w}, h={screen_h}", force=True)
            
            # 计算窗口位置（使用真实坐标，相对于截图区域，减去边框宽度）
            window_x = real_x - border_width
            window_y = real_y - border_width
            
            print(f"🎯 [长截图窗口] 计算初始位置: ({window_x}, {window_y}), border={border_width}", force=True)
            
            # 确保窗口在目标显示器的范围内
            # 检查窗口是否会超出显示器边界
            window_width = self.capture_rect.width() + border_width * 2
            window_height = self.capture_rect.height() + border_width * 2
            
            print(f"🎯 [长截图窗口] 窗口尺寸: {window_width}x{window_height}", force=True)
            print(f"🎯 [长截图窗口] 目标屏幕范围: x=[{screen_x}, {screen_x + screen_w}], y=[{screen_y}, {screen_y + screen_h}]", force=True)
            
            # 如果有父窗口的adjust_position_to_screen方法，直接使用它（与钉图窗口完全一致）
            if (hasattr(self, 'parent') and self.parent and 
                hasattr(self.parent, 'adjust_position_to_screen') and target_screen):
                window_x, window_y = self.parent.adjust_position_to_screen(
                    window_x, window_y, window_width, window_height, target_screen)
                print(f"🎯 [长截图] 使用钉图窗口相同的位置调整逻辑: ({window_x}, {window_y})", force=True)
            else:
                print(f"🎯 [长截图] 使用手动边界检查", force=True)
                # 回退到手动边界检查
                # 如果窗口超出右边界，调整x位置
                if window_x + window_width > screen_x + screen_w:
                    old_x = window_x
                    window_x = screen_x + screen_w - window_width
                    print(f"⚠️ 窗口超出右边界，调整x位置: {old_x} -> {window_x}", force=True)
                
                # 如果窗口超出下边界，调整y位置
                if window_y + window_height > screen_y + screen_h:
                    old_y = window_y
                    window_y = screen_y + screen_h - window_height
                    print(f"⚠️ 窗口超出下边界，调整y位置: {old_y} -> {window_y}", force=True)
                
                # 如果窗口超出左边界，调整x位置（考虑多显示器负坐标）
                if window_x < screen_x:
                    old_x = window_x
                    window_x = screen_x
                    print(f"⚠️ 窗口超出左边界，调整x位置: {old_x} -> {window_x}", force=True)
                
                # 如果窗口超出上边界，调整y位置
                if window_y < screen_y:
                    old_y = window_y
                    window_y = screen_y
                    print(f"⚠️ 窗口超出上边界，调整y位置: {old_y} -> {window_y}", force=True)
            
            print(f"✅ 长截图窗口最终位置: x={window_x}, y={window_y}, 尺寸: {window_width}x{window_height}", force=True)
            return window_x, window_y
            
        except Exception as e:
            print(f"❌ 计算窗口位置时出错: {e}", force=True)
            # 如果出错，使用原始位置（传入的capture_rect已经是真实坐标）
            fallback_x = self.capture_rect.x()
            fallback_y = self.capture_rect.y()
            
            return (fallback_x - border_width, fallback_y - border_width)
        
    def _setup_window(self):
        """设置窗口属性"""
        print(f"🎯 [_setup_window] 开始，capture_rect: ({self.capture_rect.x()}, {self.capture_rect.y()}, {self.capture_rect.width()}, {self.capture_rect.height()})", force=True)
        
        # 设置窗口标志：无边框、置顶
        self.setWindowFlags(
            Qt.WindowType.WindowStaysOnTopHint | 
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool
        )
        
        # 设置窗口透明度和背景
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        # ⚠️ 关键修复: 设置关闭时自动销毁，防止内存泄漏
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        
        # 设置窗口位置和大小（基于截图区域）
        # 窗口区域 = 截图区域 + 底部按钮栏
        button_bar_height = 35  # 从50改为35，让按钮栏更窄
        
        # 为边框预留空间（但截图区域不包含边框）
        border_width = 3
        
        # 修复多显示器窗口定位问题
        window_x, window_y = self._get_correct_window_position(border_width)
        
        final_width = self.capture_rect.width() + border_width * 2
        final_height = self.capture_rect.height() + border_width * 2
        
        print(f"🎯 [_setup_window] 调用 setGeometry: x={window_x}, y={window_y}, w={final_width}, h={final_height}", force=True)
        
        # 不再包含按钮栏高度（工具栏已独立）
        self.setGeometry(
            window_x,
            window_y,
            final_width,
            final_height
        )
        
        print(f"🎯 [_setup_window] setGeometry后实际位置: x={self.x()}, y={self.y()}, w={self.width()}, h={self.height()}", force=True)
        
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
        self.toolbar.finish_clicked.connect(self._on_finish)
        self.toolbar.cancel_clicked.connect(self._on_cancel)
        
        self._position_floating_toolbar()
        self.toolbar.show()

    def _position_floating_toolbar(self):
        """根据屏幕边界将工具栏对齐到截图区域上方居中"""
        if not hasattr(self, 'toolbar') or self.toolbar is None:
            return
        margin = 10
        # PyQt6: 使用 screen() 代替 desktop()
        screen = self.screen()
        if screen is None:
            screen = QApplication.primaryScreen()
        screen_geometry = screen.geometry()
        toolbar_width = self.toolbar.width()
        toolbar_height = self.toolbar.height()

        preferred_x = self.x() + (self.width() - toolbar_width) // 2
        min_x = screen_geometry.left() + margin
        max_x = screen_geometry.right() - margin - toolbar_width
        toolbar_x = max(min_x, min(preferred_x, max_x))

        preferred_y = self.y() - toolbar_height - margin
        min_y = screen_geometry.top() + margin
        max_y = screen_geometry.bottom() - margin - toolbar_height
        if preferred_y < min_y:
            fallback_y = self.y() + self.height() + margin
            toolbar_y = min(max_y, max(fallback_y, min_y))
        else:
            toolbar_y = min(max_y, preferred_y)

        self.toolbar.move(toolbar_x, toolbar_y)

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
        
        # 尝试1: 右边
        x_right = capture_right + margin
        if x_right + panel.width() <= screen_right - margin:
            # 右边有足够空间，垂直居中
            x = x_right
            y = capture_top + (self.height() - panel.height()) // 2
            y = max(screen_top + margin, min(y, screen_bottom - panel.height() - margin))
            if not is_overlapping_toolbar(x, y):
                panel.move(int(x), int(y))
                return
        
        # 尝试2: 左边
        x_left = capture_left - panel.width() - margin
        if x_left >= screen_left + margin:
            # 左边有足够空间，垂直居中
            x = x_left
            y = capture_top + (self.height() - panel.height()) // 2
            y = max(screen_top + margin, min(y, screen_bottom - panel.height() - margin))
            if not is_overlapping_toolbar(x, y):
                panel.move(int(x), int(y))
                return
        
        # 尝试3: 上边（避开工具栏）
        y_top = capture_top - panel.height() - margin
        # 如果工具栏在上方，需要进一步避让
        if toolbar_rect and toolbar_rect.bottom() >= y_top - margin:
            y_top = toolbar_rect.y() - panel.height() - margin
        
        if y_top >= screen_top + margin:
            # 上边有足够空间，水平居中
            x = capture_left + (self.width() - panel.width()) // 2
            x = max(screen_left + margin, min(x, screen_right - panel.width() - margin))
            y = y_top
            if not is_overlapping_toolbar(x, y):
                panel.move(int(x), int(y))
                return
        
        # 尝试4: 下边（避开工具栏）
        y_bottom = capture_bottom + margin
        # 如果工具栏在下方，需要进一步避让
        if toolbar_rect and toolbar_rect.top() <= y_bottom + panel.height() + margin:
            y_bottom = toolbar_rect.bottom() + margin
        
        if y_bottom + panel.height() <= screen_bottom - margin:
            # 下边有足够空间，水平居中
            x = capture_left + (self.width() - panel.width()) // 2
            x = max(screen_left + margin, min(x, screen_right - panel.width() - margin))
            y = y_bottom
            if not is_overlapping_toolbar(x, y):
                panel.move(int(x), int(y))
                return
        
        # 兜底: 放在屏幕右上角（避免进入截图区域和工具栏）
        x = screen_right - panel.width() - margin
        y = screen_top + margin
        # 如果还是与工具栏重叠，尝试调整到工具栏下方
        if is_overlapping_toolbar(x, y) and toolbar_rect:
            y = toolbar_rect.bottom() + margin
            if y + panel.height() > screen_bottom - margin:
                # 如果下方也放不下，放在左上角
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
            if self.scroll_direction == "horizontal" and screenshot_count >= 2:
                display_image = display_image.rotate(90, expand=True)
        elif self.screenshots:
            display_image = self.screenshots[-1]
        self.preview_panel.update_preview(
            display_image,
            self.scroll_direction,
            screenshot_count
        )

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
        if hasattr(self, 'toolbar') and self.toolbar:
            self.toolbar.update_count(len(self.screenshots))
        self.current_scroll_distance = 0
        self._show_preview_warning(message)

    def _handle_stitch_failure(self, screenshot_index: int, detail: str):
        detail = detail or "拼接失败"
        message = f"第 {screenshot_index} 张图片拼接失败：{detail}"
        print(f"🗑️ 忽略第 {screenshot_index} 张截图，等待下一次滚动")
        if self.screenshots:
            try:
                self.screenshots.pop()
            except Exception:
                pass
        if hasattr(self, 'toolbar') and self.toolbar:
            self.toolbar.update_count(len(self.screenshots))
        self._show_preview_warning(message)
        
    def _setup_mouse_hook(self):
        """设置Windows鼠标钩子以监听全局滚轮事件"""
        try:
            # 使用Windows API设置窗口透明鼠标事件（需在主线程执行）
            hwnd = int(self.transparent_area.winId())
            user32 = ctypes.windll.user32
            ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_TRANSPARENT | WS_EX_LAYERED)
            print(f"✅ 窗口已设置为鼠标穿透模式")

            # 将可能较慢的模块导入与监听器启动放到后台线程，避免首次阻塞UI
            import threading

            def _init_listener_bg():
                try:
                    from pynput import mouse  # 首次导入较慢，放后台

                    def on_scroll(x, y, dx, dy):
                        """滚轮事件回调（在pynput线程中）
                        dx: 横向滚动量（正值向右，负值向左）
                        dy: 纵向滚动量（正值向上，负值向下）
                        """
                        if self._is_mouse_in_capture_area(x, y):
                            # 根据当前方向决定使用哪个滚动值
                            if self.scroll_direction == "horizontal":
                                # 横向模式：使用dx
                                if dx != 0:
                                    scroll_pixels = int(abs(dx) * 25)
                                    print(f"🖱️ 检测到横向滚轮: ({x}, {y}), dx={dx}, 估算距离: {scroll_pixels}px")
                                    try:
                                        self.scroll_detected.emit(scroll_pixels)
                                    except Exception as e:
                                        print(f"❌ 触发滚动信号失败: {e}", force=True)
                            else:
                                # 竖向模式：使用dy
                                if dy != 0:
                                    scroll_pixels = int(abs(dy) * 25)
                                    print(f"🖱️ 检测到竖向滚轮: ({x}, {y}), dy={dy}, 估算距离: {scroll_pixels}px")
                                    try:
                                        self.scroll_detected.emit(scroll_pixels)
                                    except Exception as e:
                                        print(f"❌ 触发滚动信号失败: {e}", force=True)

                    # 创建并启动监听器（pynput内部也会使用线程）
                    self.mouse_listener = mouse.Listener(on_scroll=on_scroll)
                    self.mouse_listener.start()
                    print("✅ 全局滚轮监听器已启动（支持横向和竖向）")
                except Exception as e:
                    print(f"❌ 设置鼠标钩子失败: {e}", force=True)
                    import traceback
                    traceback.print_exc()

            threading.Thread(target=_init_listener_bg, daemon=True).start()

        except Exception as e:
            print(f"❌ 设置窗口鼠标穿透时出错: {e}", force=True)
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
            print(f"✅ 发送横向滚动指令: 向右滚动 {amount} 格")
            
        except Exception as e:
            print(f"❌ 发送横向滚动失败: {e}", force=True)
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
                    print(f"❌ 处理按键事件失败: {e}", force=True)
            
            def on_release(key):
                """按键释放回调"""
                try:
                    if key == keyboard.Key.shift:
                        self.horizontal_scroll_key_pressed = False
                except:
                    pass
            
            # 创建并启动键盘监听器
            self.keyboard_listener = keyboard.Listener(
                on_press=on_press,
                on_release=on_release
            )
            self.keyboard_listener.start()
            print("✅ 键盘监听器已启动（横向模式，按Shift触发）")
            
        except Exception as e:
            print(f"❌ 启动键盘监听器失败: {e}", force=True)
            import traceback
            traceback.print_exc()
    
    def _stop_keyboard_listener(self):
        """停止键盘监听器"""
        if self.keyboard_listener is not None:
            try:
                self.keyboard_listener.stop()
                self.keyboard_listener = None
                print("✅ 键盘监听器已停止")
            except Exception as e:
                print(f"⚠️ 停止键盘监听器时出错: {e}")
    
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
                corner_threshold=config.corner_threshold,
                descriptor_patch_size=config.descriptor_patch_size,
                min_size_delta=config.min_size_delta,
                try_rollback=config.try_rollback,
                distance_threshold=config.distance_threshold,
                ef_search=config.ef_search,
                verbose=True,
            )
            if self._original_cancel_on_shrink is None:
                self._original_cancel_on_shrink = config.cancel_on_shrink
            if not config.cancel_on_shrink:
                config.cancel_on_shrink = True
                print("🛑 启用拼接缩短保护：检测到风险时将取消本次拼接")
            
            mode_text = "横向截图（图片旋转90度+竖向拼接）" if self.scroll_direction == "horizontal" else "竖向截图（竖向拼接）"
            print(f"✅ 拼接引擎已重新配置: {mode_text}")
            
            # 如果已经有rust拼接器实例，需要重新创建
            if self.rust_stitcher is not None:
                print("🔄 重置拼接器实例...")
                self.rust_stitcher.clear()
                self.rust_stitcher = None
                self.session_engine = None
                self.stitched_result = None
            self._refresh_preview_panel()
                
        except Exception as e:
            print(f"❌ 重新配置拼接引擎失败: {e}", force=True)
            import traceback
            traceback.print_exc()
    
    def showEvent(self, event):
        """窗口显示事件 - 立即截取第一张图"""
        super().showEvent(event)
        
        print(f"🎯 [showEvent] 窗口已显示，实际位置: x={self.x()}, y={self.y()}, w={self.width()}, h={self.height()}", force=True)
        print(f"🎯 [showEvent] capture_rect: x={self.capture_rect.x()}, y={self.capture_rect.y()}, w={self.capture_rect.width()}, h={self.capture_rect.height()}", force=True)
        print(f"🎯 [showEvent] 窗口覆盖范围: ({self.x()}, {self.y()}) -> ({self.x() + self.width()}, {self.y() + self.height()})", force=True)
        print(f"🎯 [showEvent] 期望覆盖范围: ({self.capture_rect.x() - 3}, {self.capture_rect.y() - 3}) -> ({self.capture_rect.x() + self.capture_rect.width() + 3}, {self.capture_rect.y() + self.capture_rect.height() + 3})", force=True)
        
        # 验证窗口位置是否正确
        self._verify_window_position()
        
        # 使用QTimer延迟执行，确保窗口完全显示后再截图
        QTimer.singleShot(100, self._capture_initial_screenshot)
    
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
            
            print(f"🔍 窗口位置验证:")
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
                print(f"⚠️ 警告: 窗口显示在显示器 {current_screen.name()}，但截图区域在不同的显示器")
                
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
                    
                    print(f"🔧 尝试移动窗口到正确位置: x={relative_x}, y={relative_y}")
                    self.move(relative_x, relative_y)
                    self.raise_()
                    self.activateWindow()
                else:
                    print(f"⚠️ 无法移动窗口到目标位置，可能会超出显示器边界")
            else:
                print("✅ 窗口位置正确")
                
        except Exception as e:
            print(f"❌ 验证窗口位置时出错: {e}", force=True)
    
    def _force_fix_window_position(self):
        """强制修复窗口位置（最后的保险措施）"""
        try:
            # 如果窗口不可见，先让它可见
            if not self.isVisible():
                print("⚠️ 检测到窗口不可见，强制显示")
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
                    print(f"⚠️ 截图区域不在任何显示器内，使用主显示器")
                
                target_geometry = target_screen.geometry()
                
                # 将窗口移动到目标显示器的中央
                new_x = target_geometry.x() + (target_geometry.width() - self.width()) // 2
                new_y = target_geometry.y() + (target_geometry.height() - self.height()) // 2
                
                print(f"🔧 强制移动窗口到显示器 {target_screen} 中央: x={new_x}, y={new_y}")
                self.move(new_x, new_y)
                self.raise_()
                self.activateWindow()
                
                # 更新窗口标题以提示用户
                self.setWindowTitle("長スクリーンショット - 位置が修正されました")
            else:
                print("✅ 窗口位置验证通过")
                
        except Exception as e:
            print(f"❌ 强制修复窗口位置时出错: {e}", force=True)
    
    def _capture_initial_screenshot(self):
        """截取初始截图（窗口显示时的区域内容）"""
        print("🎬 截取初始截图（第1张）...")
        self._do_capture()
        
        # 为初始截图生成哈希（用于后续去重）
        if len(self.screenshots) > 0 and self.capture_mode == "immediate":
            self.last_screenshot_hash = self._calculate_image_hash(self.screenshots[0])
        
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
    
    def _do_capture(self):
        """执行截图并实时拼接"""
        stitch_successful = True
        try:
            current_count = len(self.screenshots) + 1
            print(f"\n📸 截取第 {current_count} 张图片", force=True)
            print(f"   区域（虚拟桌面坐标）: x={self.capture_rect.x()}, y={self.capture_rect.y()}, w={self.capture_rect.width()}, h={self.capture_rect.height()}", force=True)
            
            # 获取包含截图区域的屏幕
            app = QGuiApplication.instance()
            capture_center_x = self.capture_rect.x() + self.capture_rect.width() // 2
            capture_center_y = self.capture_rect.y() + self.capture_rect.height() // 2
            center_point = QPoint(capture_center_x, capture_center_y)
            
            screen = app.screenAt(center_point)
            if screen is None:
                print("⚠️ 截图区域不在任何显示器范围内，使用主显示器", force=True)
                screen = app.primaryScreen()
            
            screen_geometry = screen.geometry()
            print(f"   目标屏幕: {screen.name() if hasattr(screen, 'name') else 'primary'}, 几何: x={screen_geometry.x()}, y={screen_geometry.y()}, w={screen_geometry.width()}, h={screen_geometry.height()}", force=True)
            
            # 将虚拟桌面坐标转换为相对于目标屏幕的坐标
            relative_x = self.capture_rect.x() - screen_geometry.x()
            relative_y = self.capture_rect.y() - screen_geometry.y()
            
            print(f"   屏幕相对坐标: x={relative_x}, y={relative_y}, w={self.capture_rect.width()}, h={self.capture_rect.height()}", force=True)
            
            # 使用屏幕相对坐标截图
            pixmap = screen.grabWindow(
                0,
                relative_x,
                relative_y,
                self.capture_rect.width(),
                self.capture_rect.height()
            )
            
            print(f"   截图尺寸: {pixmap.width()}x{pixmap.height()}", force=True)
            
            if pixmap.isNull():
                print("❌ 截图失败", force=True)
                return
            
            # 将QPixmap转换为PIL Image - PyQt6: byteCount() → sizeInBytes()
            qimage = pixmap.toImage()
            buffer = qimage.bits().asstring(qimage.sizeInBytes())
            pil_image = Image.frombytes(
                'RGBA',
                (qimage.width(), qimage.height()),
                buffer,
                'raw',
                'BGRA'
            ).convert('RGB')
            
            # 🆕 横向模式：从第2张图片开始旋转90度（顺时针）以便使用竖向拼接算法
            # 第1张图片不旋转（如果只截1张就不需要拼接和旋转）
            # 第2张及以后的图片旋转后进行竖向拼接
            is_first_image = len(self.screenshots) == 0
            if self.scroll_direction == "horizontal" and not is_first_image:
                print(f"🔄 横向模式：将图片顺时针旋转90度（第{len(self.screenshots)+1}张）")
                pil_image = pil_image.rotate(-90, expand=True)  # -90度 = 顺时针90度
                print(f"   旋转后尺寸: {pil_image.size[0]}x{pil_image.size[1]}")
            elif self.scroll_direction == "horizontal" and is_first_image:
                print(f"📸 横向模式：第1张图片不旋转（如果只有1张则无需拼接）")
            
            # 添加到截图列表（仍保留列表，用于最后的备份）
            self.screenshots.append(pil_image)
            
            # 🆕 智能拼接策略：会话级别的引擎选择
            screenshot_count = len(self.screenshots)
            
            try:
                from .jietuba_long_stitch_unified import stitch_images, get_active_engine

                # 🎯 确定本次会话使用的引擎（首次拼接时确定，后续保持不变）
                if self.session_engine is None:
                    # 🆕 首次拼接：检测配置的引擎（只在第一次调用）
                    self.session_engine = get_active_engine()
                    print(f"\n🎮 [引擎选择] 初始引擎: {self.session_engine} ({'特征匹配' if self.session_engine == 'rust' else '哈希匹配'})")
                else:
                    # ✅ 后续拼接：使用已锁定的引擎
                    print(f"🔒 [引擎锁定] 继续使用: {self.session_engine} ({'特征匹配' if self.session_engine == 'rust' else '哈希匹配'})")
                
                # 根据会话引擎选择拼接策略
                if self.session_engine == "rust":
                    # 🚀 特征匹配：使用持久化的拼接器实例，真正的增量拼接
                    
                    # 首次创建拼接器实例
                    if self.rust_stitcher is None:
                        print(f"🔧 创建 RustLongStitch 拼接器实例...")
                        from .jietuba_long_stitch_rust import RustLongStitch
                        from .jietuba_long_stitch_unified import config
                        
                        self.rust_stitcher = RustLongStitch(
                            direction=config.direction,
                            sample_rate=config.sample_rate,
                            min_sample_size=config.min_sample_size,
                            max_sample_size=config.max_sample_size,
                            corner_threshold=config.corner_threshold,
                            descriptor_patch_size=config.descriptor_patch_size,
                            min_size_delta=config.min_size_delta,
                            try_rollback=config.try_rollback,
                            distance_threshold=config.distance_threshold,
                            ef_search=config.ef_search,
                        )
                        print(f"✅ 拼接器已创建，参数: corner_threshold={config.corner_threshold}, distance_threshold={config.distance_threshold}")
                    
                    # 增量添加新图片
                    print(f"🔗 增量添加第 {screenshot_count} 张图片（特征匹配）...")
                    overlap = self.rust_stitcher.add_image(pil_image, direction=1, debug=True)
                    
                    if screenshot_count == 1:
                        # 第一张图片
                        print(f"✅ 第一张图片已添加，尺寸: {pil_image.size[0]}x{pil_image.size[1]}")
                        # 临时导出查看当前状态
                        self.stitched_result = self.rust_stitcher.export()
                    elif overlap is not None:
                        # 成功找到重叠
                        print(f"✅ 成功匹配，重叠区域: {overlap} 像素")
                        # 临时导出查看当前状态
                        self.stitched_result = self.rust_stitcher.export()
                        if self.stitched_result:
                            print(f"✅ 当前拼接结果尺寸: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
                    else:
                        # ⚠️ 特征匹配失败 → 切换到哈希匹配
                        print(f"\n⚠️ 第 {screenshot_count} 张图片特征匹配失败！")
                        print("🔄 切换到哈希匹配算法（本次会话将一直使用哈希匹配）\n")
                        
                        # 导出当前成功的结果
                        if self.rust_stitcher:
                            temp_result = self.rust_stitcher.export()
                            if temp_result:
                                self.stitched_result = temp_result
                                print(f"📌 保留之前成功的结果: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
                        
                        # 清理rust拼接器并切换引擎
                        self.rust_stitcher.clear()
                        self.rust_stitcher = None
                        self.session_engine = "hash_rust"  # ✅ 永久切换到哈希匹配
                        
                        # 使用哈希匹配拼接当前图片
                        if self.stitched_result:
                            print(f"🔗 使用哈希匹配拼接新图片...")
                            from .jietuba_long_stitch_unified import stitch_images
                            try:
                                temp_result = stitch_images([self.stitched_result, pil_image])
                            except AllOverlapShrinkError:
                                self._handle_shrink_abort(current_count)
                                return
                            if temp_result:
                                self.stitched_result = temp_result
                                print(f"✅ 哈希匹配成功，结果尺寸: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
                            else:
                                print("⚠️ 哈希匹配也失败，保持原结果")
                                stitch_successful = False
                                self._handle_stitch_failure(screenshot_count, "未找到可靠的重叠区域")
                        else:
                            # 如果连第一张都没成功，直接用当前图片
                            self.stitched_result = pil_image
                            print("📌 使用当前截图作为基础")
                
                else:
                    # 哈希匹配：使用增量拼接（hash_rust 或 hash_python）
                    if self.stitched_result is None:
                        # 第一张图片
                        print(f"🔗 初始化第 {screenshot_count} 张图片（哈希匹配）...")
                        self.stitched_result = pil_image
                        print(f"✅ 第一张图片作为基础，尺寸: {pil_image.size[0]}x{pil_image.size[1]}")
                    else:
                        # 🚀 增量拼接：只拼接 [上次结果, 新截图]
                        print(f"🔗 增量拼接第 {screenshot_count} 张图片（哈希匹配）...")
                        
                        # 🆕 横向模式：如果是第2张图片，需要先将第1张图片也旋转
                        if self.scroll_direction == "horizontal" and screenshot_count == 2:
                            print(f"🔄 横向模式：第2张图片拼接前，先将第1张图片也旋转90度")
                            print(f"   第1张原尺寸: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
                            self.stitched_result = self.stitched_result.rotate(-90, expand=True)
                            print(f"   第1张旋转后: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
                        
                        from .jietuba_long_stitch_unified import stitch_images
                        try:
                            result = stitch_images([self.stitched_result, pil_image])
                        except AllOverlapShrinkError:
                            self._handle_shrink_abort(current_count)
                            return
                        if result:
                            self.stitched_result = result
                            print(f"✅ 拼接完成，当前结果尺寸: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
                        else:
                            print("⚠️ 增量拼接失败，保持原结果")
                            stitch_successful = False
                            self._handle_stitch_failure(screenshot_count, "未找到可靠的重叠区域")
                        
            except Exception as e:
                print(f"⚠️ 拼接出错: {e}")
                import traceback
                traceback.print_exc()
                stitch_successful = False
                self._handle_stitch_failure(screenshot_count, f"算法异常：{e}")
                
                # 拼接失败时的回退处理
                if self.stitched_result is None:
                    self.stitched_result = pil_image
                    print("⚠️ 使用当前截图作为初始结果")
            
            if stitch_successful:
                # 记录滚动距离（第一张截图距离为0，后续为累积距离）
                if len(self.screenshots) == 1:
                    self.scroll_distances.append(0)
                else:
                    self.scroll_distances.append(self.current_scroll_distance)
                    print(f"📏 记录滚动距离: {self.current_scroll_distance}px")
                self.current_scroll_distance = 0

                # 更新工具栏计数
                if hasattr(self, 'toolbar') and self.toolbar:
                    self.toolbar.update_count(len(self.screenshots))

                print(f"✅ 第 {len(self.screenshots)} 张截图完成 (尺寸: {pil_image.size[0]}x{pil_image.size[1]})")
                self._clear_preview_warning()
            else:
                # 失败截图已被忽略
                self.current_scroll_distance = 0
            
            self._refresh_preview_panel()
            
        except Exception as e:
            print(f"❌ 截图时出错: {e}", force=True)
            import traceback
            traceback.print_exc()
    
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

    def moveEvent(self, event):
        super().moveEvent(event)
        self._position_preview_panel()
        self._position_floating_toolbar()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._position_preview_panel()
        self._position_floating_toolbar()
    
    def _on_finish(self):
        """完成按钮点击"""
        print(f"✅ 完成长截图，共 {len(self.screenshots)} 张图片", force=True)
        
        # 🚀 如果使用特征匹配，导出最终结果
        if self.session_engine == "rust" and self.rust_stitcher is not None:
            print("📸 长截图完成，获取拼接结果...")
            try:
                final_result = self.rust_stitcher.export()
                if final_result:
                    self.stitched_result = final_result
                    print(f"✅ 获取拼接结果，图片大小: {final_result.size}")
                else:
                    print("⚠️  导出结果为空")
            except Exception as e:
                print(f"❌ 导出拼接结果失败: {e}", force=True)
        
        # 横向模式：将拼接结果逆时针旋转90度还原
        # 只有在有2张及以上图片（发生了拼接）时才旋转
        # 如果只有1张图片，不需要旋转（第1张图片没有被旋转）
        if (self.scroll_direction == "horizontal" and 
            self.stitched_result is not None and 
            len(self.screenshots) >= 2):
            print(f"🔄 横向模式：将拼接结果逆时针旋转90度还原（共{len(self.screenshots)}张）")
            print(f"   旋转前尺寸: {self.stitched_result.size[0]}x{self.stitched_result.size[1]}")
            self.stitched_result = self.stitched_result.rotate(90, expand=True)  # 90度 = 逆时针90度
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
            print("⚠️ 没有拼接结果，跳过保存")
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
                print(f"💾 长截图保存任务已提交: {task_path}")
            else:
                print("❌ 无法提交长截图保存任务")
        except Exception as exc:
            print(f"❌ 提交长截图保存任务失败: {exc}")
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
            
            # 复制到剪贴板
            # ⚠️ 关键修复: 必须使用 copy() 创建深拷贝
            # 否则 qimage 依赖局部变量 data，函数结束后 data 被回收会导致崩溃
            clipboard = QApplication.clipboard()
            clipboard.setImage(qimage.copy())
            print("📋 长截图已复制到剪贴板")
        except Exception as e:
            print(f"❌ 复制到剪贴板失败: {e}")
            import traceback
            traceback.print_exc()
    
    def _on_manual_capture(self):
        """手动截图（从工具栏触发）"""
        try:
            print("🖱️ 用户手动触发截图...")
            # 立即执行截图
            self._do_capture()
        except Exception as e:
            print(f"❌ 手动截图失败: {e}", force=True)
            import traceback
            traceback.print_exc()
    
    def _on_cancel(self):
        """取消按钮点击"""
        print("❌ 取消长截图", force=True)
        self.screenshots.clear()
        self._cleanup()
        self.cancelled.emit()
        self.close()
    
    def _cleanup(self):
        """清理资源"""
        try:
            # 🧹 显式清理大对象内存
            if hasattr(self, 'screenshots'):
                self.screenshots.clear()
                self.screenshots = []
            
            if hasattr(self, 'stitched_result'):
                self.stitched_result = None
                
            if self._original_cancel_on_shrink is not None:
                from .jietuba_long_stitch_unified import config as long_config
                long_config.cancel_on_shrink = self._original_cancel_on_shrink
                self._original_cancel_on_shrink = None
            # 🧹 清理特征匹配拼接器
            if hasattr(self, 'rust_stitcher') and self.rust_stitcher is not None:
                try:
                    self.rust_stitcher.clear()
                    print("✅ 已清理 RustLongStitch 拼接器")
                except Exception as e:
                    print(f"⚠️  清理拼接器时出错: {e}")
                finally:
                    self.rust_stitcher = None
            
            # 关闭浮动工具栏
            if hasattr(self, 'toolbar') and self.toolbar:
                try:
                    self.toolbar.close()
                    print("✅ 浮动工具栏已关闭")
                except Exception as e:
                    print(f"⚠️ 关闭工具栏时出错: {e}")
            
            # 关闭预览面板
            if hasattr(self, 'preview_panel') and self.preview_panel:
                try:
                    self.preview_panel.close()
                    print("✅ 预览面板已关闭")
                except Exception as e:
                    print(f"⚠️ 关闭预览面板时出错: {e}")
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
                print("✅ 全局滚轮监听器已停止")
            
            # 🆕 停止键盘监听器
            self._stop_keyboard_listener()
            
        except Exception as e:
            print(f"⚠️ 清理资源时出错: {e}")
    
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
