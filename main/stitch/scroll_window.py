"""
jietuba_scroll.py - 滚动截图窗口模块

实现滚动长截图功能的窗口类,用于捕获滚动页面的多张截图。

主要功能:
- 显示半透明边框窗口标识截图区域
- 截图区画面一变就截图：滚轮、拖滚动条、拖内容、键盘翻页都一样
- 实时显示已捕获的截图数量
- 支持自动滚动

主要类:
- ScrollCaptureWindow: 滚动截图窗口类

特点:
- 窗口透明,不拦截鼠标事件
- 后台线程盯着截图区的变化（change_watch）
- 等画面停住再截，避开滚动动画中途的画面
- 支持取消和完成截图操作

依赖模块:
- PySide6: GUI框架
- PIL: 图像处理
- ctypes: Windows API调用
- core.input_hub: 横向模式下的 Shift 监听

使用方法:
    window = ScrollCaptureWindow(capture_rect, parent)
    window.finished.connect(on_finished)
    window.show()
"""

import ctypes
from PySide6.QtWidgets import QWidget, QVBoxLayout, QLabel, QApplication
from PySide6.QtCore import Qt, QRect, QRectF, QPointF, QSize, QTimer, Signal, QPoint
from PySide6.QtGui import (QPainter, QPainterPath, QPen, QColor, QLinearGradient, QPolygonF, QGuiApplication,
                           QImage)
from typing import Optional

# 导入长截图拼接统一接口
from .jietuba_long_stitch_unified import (
    configure as long_stitch_configure,
    normalize_engine_value,
)

from settings import get_tool_settings_manager
from capture.capture_service import grab_region_hdr, uses_hdr_engine
from core.save import SaveService
from core import log_debug, log_info, safe_event
from core.input_hub import input_hub
from core.logger import log_exception, T, LogMsg
from .scroll_toolbar import FloatingToolbar  # 浮动工具栏（独立模块）
from .incremental import Frame, IncrementalStitcher
from .auto_scroll import AutoScroller, activate_window_at, wheel_follows_focus
from .change_watch import ChangeWatch
from core.ui_theme import set_own_style
from core.ui_scale import scaled

_MODULE_TAG = "LongStitch"


def _log_stitch(*args, force: bool = False):
    """长截图模块的日志输出。

    force=True 记为 INFO（始终保留），否则记为 DEBUG（由全局日志级别过滤）。

    此前这里是 `print = _long_stitch_print`，在模块作用域覆盖了内置 print，
    读代码的人很容易把满屏的 print 误判成标准输出；而 DEBUG 分支还被一个
    恒为 False 的开关挡着，那批调试输出实际上永远不会执行。
    现在统一交给日志系统按级别过滤，调整日志级别即可看到。

    args 里可以混用普通字符串和 T() 构造的可翻译消息（LogMsg），
    LogMsg 在拼接前会先 render() 成当前语言的文本。
    """
    message = " ".join(arg.render() if isinstance(arg, LogMsg) else str(arg) for arg in args)
    if force:
        log_info(message, module=_MODULE_TAG)
    else:
        log_debug(message, module=_MODULE_TAG)


def _load_long_stitch_config():
    """从配置文件加载长截图参数（当前引擎仅支持 hash_rust，只保留其实际用到的参数）"""
    config_mgr = get_tool_settings_manager()

    raw_engine = config_mgr.get_long_stitch_engine()
    engine = normalize_engine_value(raw_engine)

    if engine != raw_engine:
        config_mgr.set_long_stitch_engine(engine)
        _log_stitch(T("📖 检测到长截图引擎旧值 {raw_engine}，已自动转换为 {engine}", raw_engine=raw_engine, engine=engine))

    config = {
        'engine': engine,
        'verbose': False,  # 传给拼接引擎，控制 Rust 侧的输出
        'ignore_top_pixels': config_mgr.get_long_stitch_ignore_top_pixels(),
    }

    _log_stitch(T(
        "📖 从配置加载长截图参数: 引擎={engine}, 顶部忽略={ignore_top_pixels}px",
        engine=config['engine'], ignore_top_pixels=config['ignore_top_pixels'],
    ))

    return config


# 配置拼接引擎（从配置文件读取）
_long_stitch_config = _load_long_stitch_config()
long_stitch_configure(
    engine=_long_stitch_config['engine'],
    verbose=_long_stitch_config['verbose'],
    ignore_top_pixels=_long_stitch_config['ignore_top_pixels'],
)

# Windows API 常量
_INPUT_WATCHER = "stitch"
STILL_CHECK_MS = 40  # 隔这么久再截一次，两次一样才算画面停住了
STILL_CHECK_TRIES = 6
# 接不上的提示晚这么久才出现：闪白、动画中途这类孤立的一张怪帧之后马上有好帧接上，提示不该一闪而过
WARNING_DELAY_MS = 600
_SHIFT_KEYS = (0xA0, 0xA1)  # 左右 Shift
_PREVIEW_SIDE = 190  # 预览面板的固定边长，缩略图按它生成

GWL_EXSTYLE = -20
WS_EX_TRANSPARENT = 0x00000020
WS_EX_LAYERED = 0x00080000


def _blank(frame, horizontal: bool) -> bool:
    """沿拼接方向没有任何变化，比如整屏刷白、正在重绘：没有能拼的内容，两次截到一样也不算停住。
    竖向看是否每一行都一样；横向拼的是列，看是否每一列都一样，即每一行内部只有一种颜色。"""
    bgra, width, height = frame
    stride = width * 4
    if horizontal:
        return all(bgra[row * stride:(row + 1) * stride] == bgra[row * stride:row * stride + 4] * width
                   for row in range(height))
    first = bgra[:stride]
    return all(bgra[row * stride:(row + 1) * stride] == first for row in range(1, height))


class PreviewPanel(QWidget):
    """实时预览面板：短边固定，按原比例显示拼接缩略图，并框出最新一帧的位置。

    缩略图放不下时只显示其中一段：最新一帧的框快出视口了才挪，被裁的一侧渐隐并画箭头。
    """

    _BOX_COLOR = QColor(82, 196, 26)
    _BASE_FADE = 28
    _BASE_MARGIN = 24

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowFlags(Qt.WindowType.WindowStaysOnTopHint | Qt.WindowType.FramelessWindowHint | Qt.WindowType.Tool)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self._fixed_side = scaled(_PREVIEW_SIDE)
        self._image: Optional[QImage] = None
        self._box: Optional[tuple[int, int]] = None
        self._vertical = True
        self._offset = 0
        self.setFixedSize(self._fixed_side, self._fixed_side)  # 初始正方形占位
        self._build_ui()
        
        # 设置鼠标穿透，防止拦截滚轮事件
        self._setup_mouse_transparent()
    
    def _setup_mouse_transparent(self):
        """设置窗口鼠标穿透，不拦截滚轮事件"""
        try:
            hwnd = int(self.winId())
            user32 = ctypes.windll.user32
            ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_TRANSPARENT | WS_EX_LAYERED)
            _log_stitch(T("[OK] PreviewPanel 已设置为鼠标穿透模式"))
        except Exception as e:
            _log_stitch(T("[WARN] 设置 PreviewPanel 鼠标穿透失败: {e}", e=e))
        self._capture_excluded = False

    def _build_ui(self):
        # 拼接失败时在顶部直接写出原因：面板鼠标穿透，悬停提示出不来
        self.warning_label = QLabel(self)
        self.warning_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.warning_label.setWordWrap(True)
        set_own_style(self.warning_label,
            "background: rgba(255, 77, 79, 0.92);"
            "color: white;"
            f"border-radius: {scaled(6)}px;"
            f"padding: {scaled(4)}px {scaled(8)}px;"
            f"font-size: {scaled(9)}pt;"
            "font-weight: 600;"
        )
        self.warning_label.hide()
        self._place_warning()

    def _place_warning(self):
        margin = scaled(8)
        self.warning_label.setFixedWidth(max(scaled(60), self.width() - 2 * margin))
        self.warning_label.adjustSize()
        self.warning_label.move(margin, margin)

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

    def update_preview(self, image: Optional[QImage], scroll_direction, box=None):
        """image 为拼接结果的缩略图（已还原朝向），box 为最新一帧在其长边上的 [起, 止) 像素范围。"""
        if image is None or image.isNull():
            self._image, self._box, self._offset = None, None, 0
            self.update()
            return

        self._image = image
        self._vertical = scroll_direction != "horizontal"
        if box is not None:
            self._box = box
        side = self._fixed_side
        total = image.height() if self._vertical else image.width()
        length = min(max(total, side), self._max_length())
        size = QSize(side, length) if self._vertical else QSize(length, side)
        if self.size() != size:
            self.setFixedSize(size)
        self._offset = self._follow(total, length)
        self._place_warning()
        self.update()

    def _max_length(self) -> int:
        screen = QApplication.primaryScreen()
        if self.parent() and hasattr(self.parent(), 'screen') and self.parent().screen():
            screen = self.parent().screen()
        if screen is None:
            return 800
        geometry = screen.geometry()
        return (geometry.height() if self._vertical else geometry.width()) - 60

    def _follow(self, total: int, length: int) -> int:
        """视口起点：放得下就从头显示；放不下时最新一帧的框快出视口了才挪，挪到框刚好露出并留点余量。"""
        if total <= length:
            return 0
        offset = min(self._offset, total - length)
        if self._box is not None:
            start, end = self._box
            margin = min(scaled(self._BASE_MARGIN), max(0, (length - (end - start)) // 2))
            if start < offset + margin:
                offset = start - margin
            elif end > offset + length - margin:
                offset = end + margin - length
        return max(0, min(offset, total - length))

    @safe_event
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        radius = scaled(8)
        outline = QPainterPath()
        outline.addRoundedRect(QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius)
        painter.fillPath(outline, QColor(0, 0, 0, 64))
        if self._image is not None:
            painter.setClipPath(outline)
            self._paint_content(painter)
            painter.setClipping(False)
        painter.setPen(QPen(QColor(0, 0, 0, 204), 1))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawPath(outline)
        painter.end()

    def _paint_content(self, painter: QPainter):
        image, vertical, offset = self._image, self._vertical, self._offset
        total = image.height() if vertical else image.width()
        length = self.height() if vertical else self.width()
        visible = min(total, length)
        lead = (length - visible) // 2  # 缩略图比面板短时居中
        if vertical:
            painter.drawImage(QRectF(0, lead, image.width(), visible), image,
                              QRectF(0, offset, image.width(), visible))
        else:
            painter.drawImage(QRectF(lead, 0, visible, image.height()), image,
                              QRectF(offset, 0, visible, image.height()))

        if self._box is not None:
            pen_width = scaled(2)
            inset = pen_width / 2
            start, end = (v - offset + lead for v in self._box)
            painter.setPen(QPen(self._BOX_COLOR, pen_width))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            if vertical:
                painter.drawRect(QRectF(inset, start + inset, self.width() - pen_width, end - start - pen_width))
            else:
                painter.drawRect(QRectF(start + inset, inset, end - start - pen_width, self.height() - pen_width))

        if offset > 0:
            self._paint_clipped_edge(painter, at_start=True)
        if offset + length < total:
            self._paint_clipped_edge(painter, at_start=False)

    def _paint_clipped_edge(self, painter: QPainter, at_start: bool):
        """被裁掉的一侧：渐隐，并画一个指向外侧的箭头。"""
        w, h, fade = self.width(), self.height(), scaled(self._BASE_FADE)
        arm, gap = scaled(6), scaled(10)
        if self._vertical:
            edge, inner = (0, fade) if at_start else (h, h - fade)
            gradient = QLinearGradient(0, edge, 0, inner)
            area = QRectF(0, min(edge, inner), w, fade)
            cx, cy = w / 2, (gap if at_start else h - gap)
            tip = -arm / 2 if at_start else arm / 2
            points = [QPointF(cx - arm, cy - tip), QPointF(cx, cy + tip), QPointF(cx + arm, cy - tip)]
        else:
            edge, inner = (0, fade) if at_start else (w, w - fade)
            gradient = QLinearGradient(edge, 0, inner, 0)
            area = QRectF(min(edge, inner), 0, fade, h)
            cx, cy = (gap if at_start else w - gap), h / 2
            tip = -arm / 2 if at_start else arm / 2
            points = [QPointF(cx - tip, cy - arm), QPointF(cx + tip, cy), QPointF(cx - tip, cy + arm)]
        gradient.setColorAt(0, QColor(0, 0, 0, 150))
        gradient.setColorAt(1, QColor(0, 0, 0, 0))
        painter.fillRect(area, gradient)
        pen = QPen(QColor(255, 255, 255, 230), scaled(2))
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.drawPolyline(QPolygonF(points))

    def show_warning(self, message: str):
        self.warning_label.setText(message)
        self._place_warning()
        self.warning_label.raise_()
        self.warning_label.show()

    def clear_warning(self):
        self.warning_label.hide()
    

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
        
        # 保存目录（由外部设置）
        self.save_directory = None
        self.save_service = SaveService()
        
        # 🆕 截图方向: "vertical"(竖向) 或 "horizontal"(横向)
        self.scroll_direction = "vertical"
        
        # 横向模式按 Shift 触发横向滚动；按住时的自动重复只算一次
        self.horizontal_scroll_key_pressed = False
        self._input_hub = None
        
        # 实时拼接相关
        self.stitched_result = None  # 完成或钉图时导出的长图（已还原朝向）
        self._latest_preview = None  # 后台送回的最新缩略图
        self._latest_box = None  # 最新一帧在缩略图长边上的范围
        self.preview_warning_active = False
        self._pending_warning = None
        self._warning_timer = QTimer(self)
        self._warning_timer.setSingleShot(True)
        self._warning_timer.timeout.connect(self._show_pending_warning)
        self._stitch_failing = False  # 一段连续接不上只记一条日志
        from .jietuba_long_stitch_unified import config as stitch_config
        self._stitcher = IncrementalStitcher(
            thumb_side=scaled(_PREVIEW_SIDE),
            # 0 一向被当作「用库的默认值 20」处理
            ignore_right_pixels=stitch_config.ignore_right_pixels or 20,
            ignore_top_pixels=stitch_config.ignore_top_pixels,
            parent=self,
        )
        self._stitcher.frame_done.connect(self._on_frame_stitched, Qt.ConnectionType.QueuedConnection)
        self._stitcher.crop_done.connect(self._on_cropped, Qt.ConnectionType.QueuedConnection)
        self._auto_scroller = AutoScroller(self._capture_when_still, parent=self)
        self._auto_scroller.stopped.connect(self._on_auto_scroll_stopped)
        self._change_watch = ChangeWatch(capture_rect, parent=self)
        self._change_watch.changed.connect(self._on_screen_changed, Qt.ConnectionType.QueuedConnection)
        
        # 开始时按截图引擎设置定下来，整段长截图不再切换；HDR 失败后剩下的帧走 grabWindow，
        # 见 _grab_capture_rect
        self._use_hdr = uses_hdr_engine()

        self._still_timer = QTimer(self)  # 等画面停住
        self._still_timer.setSingleShot(True)
        self._still_timer.timeout.connect(self._check_still)
        self._still_frame = None
        self._still_tries = 0
        self._still_callbacks = []
        
        self._setup_window()
        self._setup_ui()
        self._setup_input()
        
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
        self.toolbar.auto_scroll_clicked.connect(self._toggle_auto_scroll)
        self.toolbar.crop_requested.connect(self._crop)
        self.toolbar.pin_clicked.connect(self._on_pin)
        self.toolbar.finish_clicked.connect(self._on_finish)
        self.toolbar.cancel_clicked.connect(self._on_cancel)
        self.toolbar.set_result_size(self.capture_rect.width(), self.capture_rect.height())
        
        self._position_floating_toolbar()
        self.toolbar.show()

    def _position_floating_toolbar(self):
        """根据屏幕边界将工具栏对齐到截图区域上方居中，支持上/下/左/右四向智能回退"""
        if not hasattr(self, 'toolbar') or self.toolbar is None:
            return
        from core.ui_scale import scaled
        margin = scaled(10)
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
        """把最新的拼接缩略图显示到预览面板"""
        if not hasattr(self, 'preview_panel') or self.preview_panel is None:
            return
        self.preview_panel.update_preview(
            getattr(self, '_latest_preview', None),
            self.scroll_direction,
            getattr(self, '_latest_box', None),
        )
        # 面板大小可能变化，重新定位到不遮挡截图区域的位置
        self._position_preview_panel()

    def _show_preview_warning(self, message: str):
        self._warning_timer.stop()
        self._pending_warning = None
        self.preview_warning_active = True
        if hasattr(self, 'preview_panel') and self.preview_panel is not None:
            self.preview_panel.show_warning(message)

    def _show_pending_warning(self):
        if self._pending_warning is not None:
            self._show_preview_warning(self._pending_warning)

    def _clear_preview_warning(self):
        self._warning_timer.stop()
        self._pending_warning = None
        if not self.preview_warning_active:
            return
        self.preview_warning_active = False
        if hasattr(self, 'preview_panel') and self.preview_panel is not None:
            self.preview_panel.clear_warning()

    def _handle_stitch_failure(self, hint: str, immediate: bool = False):
        """丢掉接不上的这一帧；提示等 WARNING_DELAY_MS，期间有帧接上就不出现。"""
        self._stitch_failing = True
        if self.screenshots:
            try:
                self.screenshots.pop()
            except Exception as e:
                log_exception(e, T("移除失败截图"))
        if immediate or self.preview_warning_active:
            self._show_preview_warning(hint)
        else:
            self._pending_warning = hint
            if not self._warning_timer.isActive():
                self._warning_timer.start(WARNING_DELAY_MS)
        
    def _setup_input(self):
        """窗口鼠标穿透，并接上全局按键（横向模式的 Shift）。"""
        try:
            # 使用Windows API设置窗口透明鼠标事件（需在主线程执行）
            hwnd = int(self.transparent_area.winId())
            user32 = ctypes.windll.user32
            ex_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style | WS_EX_TRANSPARENT | WS_EX_LAYERED)
            _log_stitch(T("[OK] 窗口已设置为鼠标穿透模式"))
        except Exception as e:
            _log_stitch(T("[ERROR] 设置窗口鼠标穿透时出错: {e}", e=e), force=True)

        try:
            hub = input_hub()
            hub.key.connect(self._on_key, Qt.ConnectionType.QueuedConnection)
            self._input_hub = hub
        except Exception as e:
            _log_stitch(T("[ERROR] 接入全局按键失败: {e}", e=e), force=True)

    def _on_screen_changed(self):
        """截图区的画面变了：等它停住再截，自动滚动时由它决定什么时候截。"""
        if self._auto_scroller.running:
            return
        self._capture_when_still()

    def _toggle_auto_scroll(self):
        """开始或停止自动滚动：沿最近一次挪动的方向继续，还没挪过就往下或往右。"""
        scroller = self._auto_scroller
        if scroller.running:
            scroller.stop()
            return
        center = self.capture_rect.center()
        # 滚轮发给有焦点的窗口时，点完工具栏焦点在本程序这边，先把被截的窗口设为前台
        if wheel_follows_focus():
            activate_window_at(center.x(), center.y())
        scroller.start(center, self.scroll_direction == "horizontal")
        self.toolbar.set_auto_scrolling(True)
        self.update()
        _log_stitch(T("▶️ 开始自动滚动"))

    def _capture_pending_now(self):
        """停下自动滚动，把屏幕上现在的画面截下：自动滚动刚注入还没截的一步、还在等画面停住的，
        以及画面监视还没来得及发现的最后一次滚动。导出、裁剪前调用，它们才对准屏幕上停住的画面。"""
        self._auto_scroller.stop()
        self._auto_scroller.flush()
        if self._still_timer.isActive():
            self._flush_still()
        else:
            self._do_capture()

    def _on_auto_scroll_stopped(self, reason):
        if reason == "failed":
            self._show_pending_warning()  # 自动滚动因此停下，原因要马上让用户看到
        if getattr(self, 'toolbar', None) is not None:
            self.toolbar.set_auto_scrolling(False)
        self.update()
        _log_stitch(T("⏹️ 自动滚动已停止: {reason}", reason=reason))

    def _toggle_direction(self):
        """切换截图方向（竖向/横向）"""
        self._auto_scroller.stop()
        if self.scroll_direction == "vertical":
            self.scroll_direction = "horizontal"
            self.toolbar.update_direction("horizontal")
            _log_stitch(T("🔄 切换到横向截图模式"))
        else:
            self.scroll_direction = "vertical"
            self.toolbar.update_direction("vertical")
            _log_stitch(T("🔄 切换到竖向截图模式"))
        
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
            _log_stitch(T("[OK] 发送横向滚动指令: 向右滚动 {amount} 格", amount=amount))

        except Exception as e:
            _log_stitch(T("[ERROR] 发送横向滚动失败: {e}", e=e), force=True)
            import traceback
            traceback.print_exc()
    
    def _start_keyboard_listener(self):
        """横向模式：监听 Shift，按下时往右滚一格，截图交给画面监视。"""
        if self._input_hub is None:
            return
        self.horizontal_scroll_key_pressed = False
        self._input_hub.native.watch_keys(_INPUT_WATCHER, list(_SHIFT_KEYS))
        _log_stitch(T("[OK] 键盘监听器已启动（横向模式，按Shift触发）"))

    def _stop_keyboard_listener(self):
        if self._input_hub is not None:
            self._input_hub.native.unwatch_keys(_INPUT_WATCHER)
            _log_stitch(T("[OK] 键盘监听器已停止"))

    def _on_key(self, watcher, vk, pressed):
        """横向模式下的 Shift，已在主线程。"""
        if watcher != _INPUT_WATCHER or vk not in _SHIFT_KEYS or self.scroll_direction != "horizontal":
            return
        if not pressed:
            self.horizontal_scroll_key_pressed = False
            return
        if self.horizontal_scroll_key_pressed:
            return
        self.horizontal_scroll_key_pressed = True
        _log_stitch(T("⌨️ 检测到Shift按下，往右滚一格"))
        self._send_horizontal_scroll()

    def _reconfigure_stitch_engine(self):
        """重新配置拼接引擎（哈希匹配算法只支持竖向拼接，横向截图会先旋转90度再拼接后旋转回来）"""
        try:
            from .jietuba_long_stitch_unified import configure, config

            configure(
                engine=config.engine,
                verbose=True,
                ignore_top_pixels=config.ignore_top_pixels,
            )

            if self.scroll_direction == "horizontal":
                _log_stitch(T("[OK] 拼接引擎已重新配置: 横向截图（图片旋转90度+竖向拼接）"))
            else:
                _log_stitch(T("[OK] 拼接引擎已重新配置: 竖向截图（竖向拼接）"))

            self._refresh_preview_panel()

        except Exception as e:
            _log_stitch(T("[ERROR] 重新配置拼接引擎失败: {e}", e=e), force=True)
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
        QTimer.singleShot(0, self._drop_system_corners)

        # 使用QTimer延迟执行，确保窗口完全显示后再截图
        QTimer.singleShot(100, self._capture_initial_screenshot)

    def _drop_system_corners(self):
        """系统给窗口加的圆角会画进截图区的四个角，每一帧的角上都多一个灰点，拼出来两侧一串；
        预览面板离截图区只有十几像素，系统给它加的阴影伸出四十多像素，会落进截图区。
        原生窗口显示出来之后设才生效。"""
        from core.platform_utils import set_window_rounded_corners
        set_window_rounded_corners(int(self.winId()), False)
        panel = getattr(self, 'preview_panel', None)
        if panel is not None:
            set_window_rounded_corners(int(panel.winId()), False)

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
            
            _log_stitch(T("窗口位置验证:"))
            _log_stitch(T("   窗口位置: x={window_x}, y={window_y}", window_x=window_x, window_y=window_y))
            _log_stitch(T("   窗口中心: x={cx}, y={cy}", cx=window_center.x(), cy=window_center.y()))
            _log_stitch(T("   所在显示器: {current_screen}", current_screen=current_screen))
            _log_stitch(T(
                "   显示器范围: x={x_min}-{x_max}, y={y_min}-{y_max}",
                x_min=screen_geometry.x(), x_max=screen_geometry.x() + screen_geometry.width(),
                y_min=screen_geometry.y(), y_max=screen_geometry.y() + screen_geometry.height(),
            ))

            # 检查截图区域中心所在的显示器
            capture_center_x = self.capture_rect.x() + self.capture_rect.width() // 2
            capture_center_y = self.capture_rect.y() + self.capture_rect.height() // 2
            capture_center = QPoint(capture_center_x, capture_center_y)
            # PyQt6: 使用 screenAt() 代替 desktop.screenNumber()
            expected_screen = app.screenAt(capture_center)

            _log_stitch(T("   截图区域中心: x={capture_center_x}, y={capture_center_y}", capture_center_x=capture_center_x, capture_center_y=capture_center_y))
            _log_stitch(T("   期望显示器: {expected_screen}", expected_screen=expected_screen))

            if expected_screen and current_screen != expected_screen:
                _log_stitch(T("[WARN] 警告: 窗口显示在显示器 {screen_name}，但截图区域在不同的显示器", screen_name=current_screen.name()))
                
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
                    
                    _log_stitch(T("[FIX] 尝试移动窗口到正确位置: x={relative_x}, y={relative_y}", relative_x=relative_x, relative_y=relative_y))
                    self.move(relative_x, relative_y)
                    self.raise_()
                    self.activateWindow()
                else:
                    _log_stitch(T("[WARN] 无法移动窗口到目标位置，可能会超出显示器边界"))
            else:
                _log_stitch(T("[OK] 窗口位置正确"))

        except Exception as e:
            _log_stitch(T("[ERROR] 验证窗口位置时出错: {e}", e=e), force=True)
    
    def _force_fix_window_position(self):
        """强制调整窗口位置。"""
        try:
            # 如果窗口不可见，先让它可见
            if not self.isVisible():
                _log_stitch(T("[WARN] 检测到窗口不可见，强制显示"))
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
                _log_stitch(T("🚨 检测到窗口在所有显示器外，执行强制修复..."))
                
                # 找到截图区域所在的显示器
                capture_center_x = self.capture_rect.x() + self.capture_rect.width() // 2
                capture_center_y = self.capture_rect.y() + self.capture_rect.height() // 2
                capture_center = QPoint(capture_center_x, capture_center_y)
                
                target_screen = app.screenAt(capture_center)
                if target_screen is None:
                    target_screen = app.primaryScreen()
                    _log_stitch(T("[WARN] 截图区域不在任何显示器内，使用主显示器"))
                
                target_geometry = target_screen.geometry()
                
                # 将窗口移动到目标显示器的中央
                new_x = target_geometry.x() + (target_geometry.width() - self.width()) // 2
                new_y = target_geometry.y() + (target_geometry.height() - self.height()) // 2
                
                _log_stitch(T("[FIX] 强制移动窗口到显示器 {target_screen} 中央: x={new_x}, y={new_y}", target_screen=target_screen, new_x=new_x, new_y=new_y))
                self.move(new_x, new_y)
                self.raise_()
                self.activateWindow()
                
                # 更新窗口标题以提示用户
                self.setWindowTitle(self.tr("Long screenshot - window position corrected"))
            else:
                _log_stitch(T("[OK] 窗口位置验证通过"))

        except Exception as e:
            _log_stitch(T("[ERROR] 强制修复窗口位置时出错: {e}", e=e), force=True)
    
    def _capture_initial_screenshot(self):
        """截取初始截图（窗口显示时的区域内容），之后画面一变就截。"""
        _log_stitch(T("🎬 截取初始截图（第1张）..."))
        self._do_capture()
        
        _log_stitch(T("   初始截图完成，当前共 {count} 张", count=len(self.screenshots)))
        if self._stitcher is not None:  # 显示后 100ms 内就取消了的不再开
            self._change_watch.start()

    def _exclude_overlapping_ui(self, exclude: bool):
        """检测 UI 窗口是否与截图区域重叠，按需排除/恢复截图捕获"""
        from core.platform_utils import set_window_exclude_from_capture
        for widget in (getattr(self, 'toolbar', None), getattr(self, 'preview_panel', None)):
            if widget is None or not widget.isVisible():
                continue
            widget_rect = QRect(widget.x(), widget.y(), widget.width(), widget.height())
            if widget_rect.intersects(self.capture_rect):
                set_window_exclude_from_capture(int(widget.winId()), exclude)
    
    def _grab_capture_rect(self) -> Optional[QImage]:
        """抓取 capture_rect，失败返回 None。

        先走 HDR；失败后本次长截图剩下的帧都用 grabWindow，不再来回切换：拼接靠相邻帧
        重叠部分逐像素一致，而两条路径抓出的像素值并不完全相同。
        """
        if self._use_hdr:
            try:
                return grab_region_hdr(self.capture_rect)
            except Exception as e:
                self._use_hdr = False
                _log_stitch(T("HDR 截图失败，本次长截图改用 GDI: {error}", error=e), force=True)

        # 获取包含截图区域的屏幕
        app = QGuiApplication.instance()
        capture_center_x = self.capture_rect.x() + self.capture_rect.width() // 2
        capture_center_y = self.capture_rect.y() + self.capture_rect.height() // 2
        center_point = QPoint(capture_center_x, capture_center_y)

        screen = app.screenAt(center_point)
        if screen is None:
            _log_stitch(T("[WARN] 截图区域不在任何显示器范围内，使用主显示器"), force=True)
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
            return None
        return pixmap.toImage()

    def _grab_frame(self) -> Optional[tuple]:
        """抓一帧截图区，返回 (BGRA 字节, 宽, 高)，抓不到返回 None。"""
        # 截图前：排除与截图区域重叠的 UI 窗口
        self._exclude_overlapping_ui(True)
        try:
            qimage = self._grab_capture_rect()
            if qimage is None:
                _log_stitch(T("[ERROR] 截图失败"), force=True)
                return None
            image = qimage.convertToFormat(QImage.Format.Format_RGB32)
            width, height = image.width(), image.height()
            return bytes(image.constBits())[:width * height * 4], width, height
        except Exception as e:
            _log_stitch(T("[ERROR] 截图时出错: {e}", e=e), force=True)
            import traceback
            traceback.print_exc()
            return None
        finally:
            # 截图完成：恢复 UI 窗口可被截图
            self._exclude_overlapping_ui(False)

    def _submit_frame(self, frame) -> Optional[int]:
        """把抓到的一帧交给后台拼接，返回帧序号；结论由 _on_frame_stitched 处理"""
        stitcher = getattr(self, '_stitcher', None)
        if stitcher is None or frame is None:
            return None
        bgra, width, height = frame
        # screenshots 列表只保留计数，不存储实际图像
        self.screenshots.append(None)
        # 不知道这一帧之前往哪边滚，拼接按最近一次挪动的方向取舍
        stitcher.submit(Frame(
            bgra=bgra,
            width=width,
            height=height,
            index=len(self.screenshots),
            scroll_direction=self.scroll_direction,
        ))
        return len(self.screenshots)

    def _do_capture(self) -> Optional[int]:
        """立刻截图并提交给后台拼接，返回帧序号，截不到返回 None"""
        if getattr(self, '_stitcher', None) is None:
            return None
        return self._submit_frame(self._grab_frame())

    def _capture_when_still(self, done=None):
        """等画面停住再截图提交：隔 STILL_CHECK_MS 连截两次，一样才交给拼接，一直在变就最多截 STILL_CHECK_TRIES 次。

        平滑滚动的程序在动画中途常有一块还没重绘完，截进长图就是一行上下错开的字。
        done 在提交后以帧序号（截不到为 None）调用；正在等的时候再来请求，只追加回调。
        """
        if done is not None:
            self._still_callbacks.append(done)
        if self._still_timer.isActive():
            return
        self._still_frame = self._grab_frame() if getattr(self, '_stitcher', None) is not None else None
        if self._still_frame is None:
            self._finish_still(None)
            return
        self._still_tries = 1
        self._still_timer.start(STILL_CHECK_MS)

    def _check_still(self):
        frame = self._grab_frame()
        if frame is None:
            self._still_frame = None
            self._finish_still(None)
            return
        self._still_tries += 1
        still = frame[0] == self._still_frame[0] and not _blank(frame, self.scroll_direction == "horizontal")
        if still or self._still_tries >= STILL_CHECK_TRIES:
            self._still_frame = None
            self._finish_still(self._submit_frame(frame))
        else:
            self._still_frame = frame
            self._still_timer.start(STILL_CHECK_MS)

    def _finish_still(self, index):
        callbacks, self._still_callbacks = self._still_callbacks, []
        for done in callbacks:
            done(index)

    def _flush_still(self):
        """还在等画面停住的那一帧立刻截下提交。"""
        if self._still_timer.isActive():
            self._still_timer.stop()
            self._still_frame = None
            self._finish_still(self._do_capture())

    def _on_frame_stitched(self, result):
        """后台拼接完一帧，已在主线程。"""
        if getattr(self, '_stitcher', None) is None:
            return

        if result.ok:
            self.toolbar.set_result_size(*result.size)
            _log_stitch(T("📸 第 {screenshot_count} 张 → 拼接结果: {w}x{h}",
                          screenshot_count=result.index, w=result.width, h=result.height))
            self._stitch_failing = False
            self._clear_preview_warning()
        elif result.error:
            _log_stitch(T("[WARN] 第 {screenshot_count} 张拼接出错: {e}",
                          screenshot_count=result.index, e=result.error), force=True)
            self._handle_stitch_failure(self.tr("Stitching failed"), immediate=True)
        else:
            if not self._stitch_failing:
                _log_stitch(T("[WARN] 第 {screenshot_count} 张拼接失败，未找到重叠区域",
                              screenshot_count=result.index))
            self._handle_stitch_failure(self.tr("Couldn't join. Scroll back"))

        if result.preview is not None:
            self._latest_preview = result.preview
            self._latest_box = result.box
        self._refresh_preview_panel()
        self._auto_scroller.on_frame(result)

    def _crop(self, side):
        """把长图裁到当前画面：side 为 "top" 去掉之前的内容，"bottom" 去掉之后的内容。"""
        stitcher = getattr(self, '_stitcher', None)
        if stitcher is None:
            return
        self._capture_pending_now()
        stitcher.crop(side)

    def _on_cropped(self, result):
        """后台裁剪完，已在主线程。"""
        if getattr(self, '_stitcher', None) is None:
            return
        if not result.ok:
            _log_stitch(T("[WARN] 裁剪出错: {e}", e=result.error), force=True)
            return
        _log_stitch(T("✂️ 裁剪后: {w}x{h}", w=result.width, h=result.height))
        self.toolbar.set_result_size(*result.size)
        self._latest_preview = result.preview
        self._latest_box = result.box
        self._refresh_preview_panel()
        self._auto_scroller.on_frame(result)

    def _export_result(self):
        """等后台处理完已提交的帧，取出还原朝向后的完整长图。"""
        stitcher = getattr(self, '_stitcher', None)
        if stitcher is None:
            return self.stitched_result
        self.stitched_result = stitcher.export()
        return self.stitched_result
    
    @safe_event
    def paintEvent(self, event):
        """绘制窗口边框"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        # 绘制半透明边框（在窗口边缘，不影响截图区域）；自动滚动时变绿
        scroller = getattr(self, '_auto_scroller', None)
        color = QColor(82, 196, 26) if scroller is not None and scroller.running else QColor(0, 120, 215)
        pen = QPen(color, 3)
        painter.setPen(pen)
        
        # 边框应该绘制在整个窗口的边缘
        # 窗口大小 = capture_rect + 边框(3px * 2)
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
        _log_stitch(T("[OK] 完成长截图，共 {count} 张图片", count=len(self.screenshots)), force=True)
        self._capture_pending_now()
        self._export_result()

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
            _log_stitch(T("[WARN] 没有拼接结果，跳过保存"))
            return

        direction_suffix = self.tr("Horizontal") if self.scroll_direction == "horizontal" else self.tr("Vertical")
        target_dir = self.save_directory

        try:
            task_path = self.save_service.save_pil_async(
                self.stitched_result,
                directory=target_dir,
                prefix=self.tr("Long Screenshot"),
                suffix=direction_suffix,
                image_format="PNG"
            )
            if task_path:
                _log_stitch(T("[SAVE] 长截图保存任务已提交: {task_path}", task_path=task_path))
            else:
                _log_stitch(T("[ERROR] 无法提交长截图保存任务"))
        except Exception as exc:
            _log_stitch(T("[ERROR] 提交长截图保存任务失败: {exc}", exc=exc))
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
            _log_stitch(T("长截图已复制到剪贴板"))
        except Exception as e:
            _log_stitch(T("[ERROR] 复制到剪贴板失败: {e}", e=e))
            import traceback
            traceback.print_exc()
    
    def _on_pin(self):
        """钉图按钮点击 - 将当前拼接结果钉到桌面，然后结束长截图"""
        _log_stitch(T("钉图长截图结果..."))
        self._capture_pending_now()

        # 检查 config_manager
        if self.config_manager is None:
            _log_stitch(T("[ERROR] config_manager 未设置，无法创建钉图"))
            return

        # 获取拼接结果（导出时已还原朝向）
        result_image = self._export_result()

        if result_image is None:
            _log_stitch(T("[WARN] 没有拼接结果，无法钉图"))
            return

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
            
            # 不保留返回值：PinManager 自身会持有窗口引用，这里不需要
            pin_manager.create_pin(
                image=qimage,
                position=position,
                config_manager=self.config_manager,
                drawing_items=None,  # 长截图不继承绘制项目
                selection_offset=None
            )
            
            log_debug(T("钉图已创建，位置: ({pin_x}, {pin_y})", pin_x=pin_x, pin_y=pin_y), module=_MODULE_TAG)
            
            # 钉图完成后，清理并关闭长截图窗口
            self._cleanup()
            self.finished.emit()
            self.close()
            
        except Exception as e:
            _log_stitch(T("[ERROR] 创建钉图失败: {e}", e=e))
            import traceback
            traceback.print_exc()
    
    def _on_cancel(self):
        """取消按钮点击"""
        _log_stitch(T("[ERROR] 取消长截图"), force=True)
        self.screenshots.clear()
        self._cleanup()
        self.cancelled.emit()
        self.close()
    
    def _cleanup(self):
        """清理资源"""
        try:
            watch = getattr(self, '_change_watch', None)
            if watch is not None:
                watch.stop()
            scroller = getattr(self, '_auto_scroller', None)
            if scroller is not None:
                scroller.cancel()
            if hasattr(self, 'screenshots'):
                self.screenshots.clear()
                self.screenshots = []
            
            
            if hasattr(self, 'stitched_result'):
                self.stitched_result = None

            stitcher, self._stitcher = getattr(self, '_stitcher', None), None
            if stitcher is not None:
                stitcher.close()
            self._latest_preview = None
            self._latest_box = None
            
            import gc
            gc.collect()
                
            # 关闭浮动工具栏
            if hasattr(self, 'toolbar') and self.toolbar:
                try:
                    self.toolbar.close()
                    _log_stitch(T("[OK] 浮动工具栏已关闭"))
                except Exception as e:
                    _log_stitch(T("[WARN] 关闭工具栏时出错: {e}", e=e))
            
            # 关闭预览面板
            if hasattr(self, 'preview_panel') and self.preview_panel:
                try:
                    self.preview_panel.close()
                    _log_stitch(T("[OK] 预览面板已关闭"))
                except Exception as e:
                    _log_stitch(T("[WARN] 关闭预览面板时出错: {e}", e=e))
                finally:
                    self.preview_panel = None

            # 停止所有定时器
            if hasattr(self, '_still_timer'):
                self._still_timer.stop()
                self._still_callbacks = []
            if hasattr(self, '_warning_timer'):
                self._warning_timer.stop()
            
            
            if hasattr(self, '_position_fix_timer'):
                self._position_fix_timer.stop()
            
            hub, self._input_hub = self._input_hub, None
            if hub is not None:
                hub.key.disconnect(self._on_key)
                hub.native.unwatch_keys(_INPUT_WATCHER)

        except Exception as e:
            _log_stitch(T("[WARN] 清理资源时出错: {e}", e=e))
    
    @safe_event
    def closeEvent(self, event):
        """窗口关闭事件"""
        self._cleanup()
        super().closeEvent(event)
    