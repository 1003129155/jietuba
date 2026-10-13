"""
钉图窗口 - 核心窗口类

架构说明：
- PinWindow：主窗口，只负责窗口管理和子控件布局
- PinCanvasView：唯一内容渲染者，使用 Qt 的 GPU 加速渲染
- PinCanvas：画布核心，包含工具信号路由
- PinOCRManager：OCR 初始化和线程管理
- PinThumbnailMode：缩略图模式逻辑
- PinControlButtons：控制按钮管理器
- PinHoverControls：控制按钮和工具栏的显隐
- PinContextMenu：右键菜单管理器
- PinTranslationHelper：翻译功能助手
"""

import math

from PySide6.QtWidgets import QWidget, QLabel, QApplication, QRubberBand
from PySide6.QtCore import Qt, QPoint, QPointF, QTimer, Signal, QRect, QRectF, QEvent
from PySide6.QtGui import (
    QCursor, QPixmap, QImage, QPainter, QMouseEvent, QWheelEvent, QKeyEvent,
    QTransform,
)
from .pin_canvas_view import PinCanvasView
from .pin_controls import PinControlButtons
from .pin_hover import PinHoverControls
from .pin_context_menu import PinContextMenu
from .pin_translation import PinTranslationHelper
from .pin_border_overlay import PinBorderOverlay
from .pin_ocr_manager import PinOCRManager
from .pin_thumbnail import PinThumbnailMode
from .pin_image_transform import PinImageTransform
from core import log_debug, log_info, log_warning, log_error, safe_event
from core.theme import get_theme
from core.logger import log_exception, T
from core.clipboard_utils import deliver_image_async
from core.platform_utils import set_window_click_through, set_window_rounded_corners
from core.qt_utils import blocking_modal
from settings.tool_settings import PIN_MOUSE_ACTIONS, get_pin_mouse_binding
from .pin_shortcut import mouse_binding_matches


# 滚轮和设置给出的不透明度都不低于这个值，免得钉图被调到看不见
MIN_OPACITY = 0.15
MAX_SCALE = 4.0
# 缩放后窗口较短的一边不小于这么多像素，免得缩成看不见的一点
MIN_SIDE = 50
# 比屏幕还大的图钉出来时缩到屏幕的这个比例，四周留出一圈好抓住拖动
AUTO_SHRINK_RATIO = 0.95
# 滚轮缩放时窗口上保持不动的点，按窗口宽高的比例；mouse 取鼠标所在位置
ZOOM_ANCHORS = {
    "top_left": (0.0, 0.0), "top_right": (1.0, 0.0),
    "bottom_left": (0.0, 1.0), "bottom_right": (1.0, 1.0),
    "center": (0.5, 0.5), "mouse": None,
}
# 边缘这么宽（按 96 DPI 计）的一圈按下拖动是调大小，不是移动
RESIZE_MARGIN = 6
_RESIZE_CURSORS = {
    (-1, 0): Qt.CursorShape.SizeHorCursor, (1, 0): Qt.CursorShape.SizeHorCursor,
    (0, -1): Qt.CursorShape.SizeVerCursor, (0, 1): Qt.CursorShape.SizeVerCursor,
    (-1, -1): Qt.CursorShape.SizeFDiagCursor, (1, 1): Qt.CursorShape.SizeFDiagCursor,
    (1, -1): Qt.CursorShape.SizeBDiagCursor, (-1, 1): Qt.CursorShape.SizeBDiagCursor,
}
# 锁定后这些鼠标手势归哪一类禁止项；右键菜单里的同名项是明确操作，照常执行
LOCKED_MOUSE_ACTIONS = {"close": "click_close", "reset": "move", "thumbnail": "move"}


class PinWindow(QWidget):
    """
    钉图窗口 - 可拖动、缩放、编辑的置顶图像窗口

    核心特性:
    - 无边框置顶窗口 + 描边效果
    - 拖动移动 / 滚轮缩放
    - 鼠标悬停显示控制按钮
    - ESC 关闭 / R 缩略图模式
    - 支持绘图编辑（委托给 PinCanvas）
    - OCR 文字选择（委托给 PinOCRManager）
    """

    # 信号
    closed = Signal()  # 窗口关闭信号

    def __init__(self, image: QImage, position: QPoint, config_manager,
                 drawing_items=None, selection_offset=None, number_next=None):
        """
        Args:
            image: 选区底图（只包含选区的纯净背景，不含绘制）
            position: 初始位置（全局坐标）
            config_manager: 配置管理器
            drawing_items: 绘制项目列表（从截图窗口继承）
            selection_offset: 选区在原场景中的偏移量
            number_next: 源场景的下一个序号值（用于同步计数器）
        """
        super().__init__()

        self.config_manager = config_manager
        self.drawing_items = drawing_items or []
        self.selection_offset = selection_offset or QPoint(0, 0)

        # ====== 描边样式参数 ======
        self.border_enabled = bool(config_manager.get_app_setting("pin_auto_border")) if config_manager else True
        self._square_corners = False   # 是否已让系统去掉圆角
        self.corner = 0
        self.border_width = 2
        tc = get_theme().theme_color
        tc.setAlpha(200)
        self.border_color = tc

        # ====== 窗口状态 ======
        self._is_closed = False
        self._is_dragging = False
        self._is_editing = False
        self._drag_start_pos = QPoint()
        self._drag_start_window_pos = QPoint()
        self._mouse_gesture = None
        self._mouse_swallow_release = None
        self._mouse_band = None
        self._mouse_click_timer = QTimer(self)
        self._mouse_click_timer.setSingleShot(True)
        self._mouse_click_timer.timeout.connect(self._finish_mouse_click)
        self._pending_mouse_action = None
        self._pending_mouse_position = None

        # ====== 锁定 / 鼠标穿透 ======
        self._locked = False
        self._click_through = False
        self._click_through_opacity = self._opacity_setting("pin_click_through_opacity")
        self._opacity_before_click_through = None
        self._click_through_bar = None

        # ====== 拖边缘调大小 ======
        self._resize_edge = None          # 正在拖的边，(sx, sy) 各取 -1/0/1
        self._resize_start_pos = QPoint()
        self._resize_start_geometry = QRect()
        self._resize_cursor_owner = None  # 当前显示调整指针的控件

        # ====== 设置窗口属性 ======
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool |
            Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        # 透明背景不随描边开关变，开关描边都是同一种窗口
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setMouseTracking(True)

        # ====== 底图 ======
        self._orig_size = image.size()
        self._base_pixmap = QPixmap.fromImage(image)
        self.base_image = None  # 释放 QImage

        # ====== 缩放 ======
        self.scale_factor = 1.0
        self._view_scale_x = 1.0
        self._view_scale_y = 1.0
        self._last_background_scale_size = None

        self._scale_timer = QTimer(self)
        self._scale_timer.setSingleShot(True)
        self._scale_timer.setInterval(80)
        self._scale_timer.timeout.connect(self._apply_smooth_scaling)
        self._is_scaling = False

        # ====== 窗口透明度 ======
        self._win_opacity = self._opacity_setting("pin_default_opacity")
        if self._win_opacity < 1.0:
            self.setWindowOpacity(self._win_opacity)

        # ====== 缩放百分比提示 ======
        self._zoom_label = QLabel(self)
        self._zoom_label.setStyleSheet(
            ".QLabel {"
            "  color: #2EC4B6;"
            "  background: rgba(0, 0, 0, 160);"
            "  border-radius: 4px;"
            "  padding: 2px 6px;"
            "  font-size: 12px;"
            "  font-weight: bold;"
            "}"
        )
        self._zoom_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._zoom_label.hide()
        self._zoom_hide_timer = QTimer(self)
        self._zoom_hide_timer.setSingleShot(True)
        self._zoom_hide_timer.setInterval(1000)
        self._zoom_hide_timer.timeout.connect(self._zoom_label.hide)

        self.view = None

        # ====== 初始几何 ======
        self.setGeometry(self._initial_geometry(position, image.size()))

        # ====== UI 组件 ======
        self.setup_ui()

        # ====== 画布 ======
        from .pin_canvas import PinCanvas
        self.canvas = PinCanvas(self, self._orig_size, image)
        if self.drawing_items:
            self.canvas.initialize_from_items(self.drawing_items, self.selection_offset, number_next)

        # ====== CanvasView ======
        self._cross_tool_selection_enabled = bool(
            self.config_manager.get_cross_tool_selection_enabled()
        ) if self.config_manager else False
        self.view = PinCanvasView(
            self.canvas.scene, self, self.canvas,
            cross_tool_select=self._cross_tool_selection_enabled,
        )
        self.view.setParent(self)
        self.view.setGeometry(0, 0, self.width(), self.height())
        self.view.set_corner_radius(self.corner)
        self._update_view_transform()
        self.view.viewport().installEventFilter(self)
        if self.scale_factor != 1.0:
            self._scale_timer.start()

        # ====== 工具栏（按需创建） ======
        self.toolbar = None
        self.hover_controls = PinHoverControls(self)

        # ====== OCR 管理器 ======
        self._ocr_mgr = PinOCRManager(self, config_manager)

        # ====== 缩略图模式 ======
        self._thumbnail = PinThumbnailMode(self)

        # ====== 图像变换管理器 ======
        self._image_transform = PinImageTransform()

        # ====== 描边 Overlay（单圈主题色，无阴影）======
        self.border_overlay = None
        if self.border_enabled:
            self.border_overlay = PinBorderOverlay(
                self, corner_radius=self.corner, border_color=self.border_color)
            self.border_overlay.setGeometry(0, 0, self.width(), self.height())
            self.border_overlay.raise_()

        # ====== 显示 ======
        self.show()
        self.update_button_positions()

        # ====== 注册到全局快捷键控制器 ======
        from .pin_shortcut import PinShortcutController
        PinShortcutController.instance().register(self)

        # 延迟 300ms 初始化 OCR（等钉图窗口完全显示后再启动，识别在子线程中运行，不阻塞主线程）
        QTimer.singleShot(300, self._ocr_mgr.init_now)

        log_info(
            T(
                "创建成功: {width}x{height}, 位置: ({x}, {y})",
                width=image.width(), height=image.height(),
                x=position.x(), y=position.y(),
            ),
            "PinWindow",
        )
        if self.drawing_items:
            log_debug(T("继承了 {count} 个绘制项目（向量数据）", count=len(self.drawing_items)), "PinWindow")

    # ==================================================================
    # 兼容属性：让外部通过 pin_window.ocr_text_layer 访问
    # ==================================================================

    @property
    def ocr_text_layer(self):
        return self._ocr_mgr.ocr_text_layer if hasattr(self, '_ocr_mgr') else None

    @property
    def _ocr_has_result(self):
        return self._ocr_mgr.has_result if hasattr(self, '_ocr_mgr') else False

    @property
    def _text_selection_enabled(self):
        return self._ocr_mgr.text_selection_enabled if hasattr(self, '_ocr_mgr') else False

    @property
    def _thumbnail_mode(self):
        return self._thumbnail.active if hasattr(self, '_thumbnail') else False

    # ==================================================================
    # UI 设置
    # ==================================================================

    def setup_ui(self):
        """设置 UI 布局"""
        self.setup_control_buttons()

    def setup_control_buttons(self):
        """设置控制按钮"""
        self._control_buttons = PinControlButtons(self)
        self.close_button = self._control_buttons.close_button
        self.toolbar_toggle_button = self._control_buttons.toolbar_toggle_button
        self._control_buttons.connect_signals(
            close_handler=lambda: self.close_unless_locked("click_close"),
            toggle_toolbar_handler=self.toggle_toolbar,
        )
        self._context_menu = PinContextMenu(self)
        self._translation_helper = PinTranslationHelper(self, self.config_manager)
        self.update_button_positions()

    def update_button_positions(self):
        """更新按钮位置"""
        if hasattr(self, '_control_buttons'):
            self._control_buttons.update_positions(self.width())

    def raise_control_buttons(self):
        """后建的子控件默认叠在最上层，会盖住右上角按钮、接走它们的点击，建完要调这里。"""
        self._control_buttons.raise_all()

    # ==================================================================
    # 外观设置
    # ==================================================================

    def refresh_appearance(self):
        """设置页保存后让已打开的钉图跟上外观设置"""
        if self._is_closed:
            return
        # 正在穿透的钉图保持当前不透明度，下次进入穿透才用新值
        self._click_through_opacity = self._opacity_setting("pin_click_through_opacity")
        self._apply_window_corners()
        self.hover_controls.sync()

    def _pin_setting(self, key):
        from settings.tool_settings import ToolSettingsManager
        cfg = getattr(self, 'config_manager', None)
        return cfg.get_app_setting(key) if cfg else ToolSettingsManager.APP_DEFAULT_SETTINGS[key]

    def _opacity_setting(self, key) -> float:
        return max(MIN_OPACITY, min(1.0, float(self._pin_setting(key))))

    def _initial_geometry(self, position: QPoint, size) -> QRect:
        """比所在屏幕还大的图按比例缩到屏幕的 95% 并居中，其余按原尺寸放在 position。"""
        if self._pin_setting("pin_auto_shrink"):
            screen = QApplication.screenAt(position) or QApplication.primaryScreen()
            area = screen.geometry() if screen is not None else QRect()
            if not area.isEmpty() and (size.width() > area.width() or size.height() > area.height()):
                self.scale_factor = min(AUTO_SHRINK_RATIO * area.width() / size.width(),
                                        AUTO_SHRINK_RATIO * area.height() / size.height())
                w = max(1, round(size.width() * self.scale_factor))
                h = max(1, round(size.height() * self.scale_factor))
                return QRect(area.x() + (area.width() - w) // 2, area.y() + (area.height() - h) // 2, w, h)
        return QRect(position, size)

    def _apply_window_corners(self):
        rounded = bool(self.config_manager and self.config_manager.get_app_setting("pin_rounded_corners"))
        # 没关过圆角就不碰系统设置，保持 Windows 默认行为
        if rounded and not self._square_corners:
            return
        set_window_rounded_corners(int(self.winId()), rounded)
        self._square_corners = not rounded

    @safe_event
    def showEvent(self, event):
        super().showEvent(event)
        # 每次显示系统都会把圆角恢复成默认（隐藏再显示、切换置顶都会），要重新设置
        self._apply_window_corners()
        if self._click_through:
            # 切换置顶时 Qt 重设窗口标志，会把穿透样式一并清掉
            set_window_click_through(int(self.winId()), True)
            self._click_through_bar.show()

    @safe_event
    def hideEvent(self, event):
        super().hideEvent(event)
        # 工具条是独立窗口，不会随钉图一起隐藏
        if self._click_through_bar is not None:
            self._click_through_bar.hide()

    # ==================================================================
    # 窗口拖动
    # ==================================================================

    def start_window_drag(self, global_pos: QPoint):
        self._is_dragging = True
        self._drag_start_pos = global_pos
        self._drag_start_window_pos = self.pos()
        if not PinWindow._lock_blocks(self, "move"):
            self.setCursor(Qt.CursorShape.ClosedHandCursor)

    def update_window_drag(self, global_pos: QPoint):
        if not self._is_dragging:
            return
        delta = global_pos - self._drag_start_pos
        if PinWindow._lock_blocks(self, "move"):
            # 单击不提示，真拖起来才提示，一次拖动只提示一次
            if delta.manhattanLength() >= QApplication.startDragDistance():
                self._is_dragging = False
                self.show_locked_hint()
            return
        self.move(self._drag_start_window_pos + delta)
        if self.toolbar and self.toolbar.isVisible():
            self.toolbar.sync_with_pin_window()

    def end_window_drag(self):
        if self._is_dragging:
            self._is_dragging = False
            self.setCursor(Qt.CursorShape.ArrowCursor)

    # ==================================================================
    # 拖边缘调大小
    # ==================================================================

    def _resize_edge_at(self, pos: QPoint):
        """窗口坐标 pos 落在哪条边或哪个角的热区，返回 (sx, sy)；此时不能调大小也返回 None。"""
        if (self._is_closed or self._thumbnail_mode or self._is_dragging
                or (self.canvas and self.canvas.is_editing)
                or not self._pin_setting("pin_border_resize") or self._lock_blocks("move")):
            return None
        screen = self.screen()
        margin = round(RESIZE_MARGIN * (screen.logicalDotsPerInch() if screen else 96) / 96)
        # 小钉图中间至少留一半给拖动
        margin = min(margin, self.width() // 4, self.height() // 4)
        sx = -1 if pos.x() < margin else (1 if pos.x() >= self.width() - margin else 0)
        sy = -1 if pos.y() < margin else (1 if pos.y() >= self.height() - margin else 0)
        return (sx, sy) if sx or sy else None

    def _handle_border_resize(self, obj, event) -> bool:
        """边缘热区里的悬停换指针、按下拖动调大小；返回 True 表示事件已处理。"""
        kind = event.type()
        if self._resize_edge is not None:
            if kind == QEvent.Type.MouseMove:
                self._update_border_resize(event.globalPosition().toPoint())
            elif kind == QEvent.Type.MouseButtonRelease and event.button() == Qt.MouseButton.LeftButton:
                self._end_border_resize()
            return kind in (QEvent.Type.MouseMove, QEvent.Type.MouseButtonPress,
                            QEvent.Type.MouseButtonRelease, QEvent.Type.MouseButtonDblClick)
        if kind == QEvent.Type.MouseMove:
            # 拖着别的东西（移动窗口、选文字）经过边缘时不换指针
            if event.buttons() != Qt.MouseButton.NoButton:
                return False
            edge = self._resize_edge_at(obj.mapTo(self, event.position().toPoint()))
            self._show_resize_cursor(obj, edge)
            return edge is not None
        if (kind != QEvent.Type.MouseButtonPress or event.button() != Qt.MouseButton.LeftButton
                or event.modifiers() != Qt.KeyboardModifier.NoModifier):
            return False
        edge = self._resize_edge_at(obj.mapTo(self, event.position().toPoint()))
        if edge is None:
            return False
        self._resize_edge = edge
        self._resize_start_pos = event.globalPosition().toPoint()
        self._resize_start_geometry = self.geometry()
        return True

    def _show_resize_cursor(self, obj, edge):
        if edge is not None:
            obj.setCursor(_RESIZE_CURSORS[edge])
            self._resize_cursor_owner = obj
        elif self._resize_cursor_owner is not None:
            # 编辑中指针归绘制工具管，不去覆盖它
            if not (self.canvas and self.canvas.is_editing):
                self._resize_cursor_owner.setCursor(Qt.CursorShape.ArrowCursor)
            self._resize_cursor_owner = None

    def _update_border_resize(self, global_pos: QPoint):
        """保持宽高比缩放，拖动那条边（角）的对边（对角）不动。"""
        sx, sy = self._resize_edge
        delta = global_pos - self._resize_start_pos
        start = self._resize_start_geometry
        base = PinWindow._display_base_size(self)
        start_scale = start.width() / base.width()
        candidates = []
        if sx:
            candidates.append((start.width() + sx * delta.x()) / base.width())
        if sy:
            candidates.append((start.height() + sy * delta.y()) / base.height())
        # 拖角时跟着变化大的那个方向走
        scale = max(candidates, key=lambda value: abs(value - start_scale))
        fx = 1.0 if sx < 0 else 0.0
        fy = 1.0 if sy < 0 else 0.0
        anchor = QPointF(start.x() + fx * start.width(), start.y() + fy * start.height())
        PinWindow._scale_around(self, scale, fx, fy, anchor)

    def _end_border_resize(self):
        self._resize_edge = None

    # ==================================================================
    # 视图 / 缩放
    # ==================================================================

    def update_display(self):
        if hasattr(self, 'view') and self.view:
            self.view.viewport().update()
        else:
            self.update()

    def _update_view_transform(self):
        if not getattr(self, 'view', None) or not getattr(self, 'canvas', None):
            return
        scene_rect = self.canvas.scene.sceneRect()
        if scene_rect.width() == 0 or scene_rect.height() == 0:
            return
        self.view.resetTransform()
        cr = self.content_rect()
        scale_x = cr.width() / scene_rect.width()
        scale_y = cr.height() / scene_rect.height()
        self._view_scale_x = float(scale_x)
        self._view_scale_y = float(scale_y)

        transform = getattr(self, '_image_transform', None)
        if transform and transform.has_transform:
            t = transform.build_view_transform(
                cr.width(), cr.height(),
                scene_rect.width(), scene_rect.height())
            self.view.setTransform(t)
        else:
            self.view.scale(scale_x, scale_y)

        # 画笔宽度按图片像素存储，光标要跟随视觉缩放才能预览真实落笔粗细。
        # 取行列式而不是 scale_x：旋转 90°/270° 时宽高互换，scale_x 不是实际缩放。
        cursor_mgr = getattr(self.view, 'cursor_manager', None)
        if cursor_mgr:
            cursor_mgr.update_view_scale(math.sqrt(abs(self.view.transform().determinant())))

    def _refresh_background_for_scale(self):
        if not getattr(self, 'canvas', None) or not getattr(self.canvas, 'scene', None):
            return
        background_item = getattr(self.canvas.scene, 'background', None)
        if background_item is None or self._view_scale_x <= 0 or self._view_scale_y <= 0:
            return
        if not getattr(self, '_base_pixmap', None):
            return

        # 计算背景预缩放尺寸
        # 目标：让预缩放后的像素密度匹配实际显示分辨率
        # 旋转 90°/270° 时，场景 x 轴映射到显示 y 轴，反之亦然
        # 所以每个场景轴的有效显示缩放需要交换
        transform = getattr(self, '_image_transform', None)
        cr = self.content_rect()
        scene_rect = self.canvas.scene.sceneRect()

        if transform and transform.is_rotated_90_or_270:
            # 场景 x 轴 → 显示 y 轴，场景 y 轴 → 显示 x 轴
            bg_scale_x = cr.height() / scene_rect.width()
            bg_scale_y = cr.width() / scene_rect.height()
        else:
            bg_scale_x = self._view_scale_x
            bg_scale_y = self._view_scale_y

        tw = max(1, int(round(self._orig_size.width() * bg_scale_x)))
        th = max(1, int(round(self._orig_size.height() * bg_scale_y)))
        target_size = (tw, th)
        if self._last_background_scale_size == target_size:
            return
        scaled = self._base_pixmap.scaled(
            tw, th,
            Qt.AspectRatioMode.IgnoreAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        # 只换渲染用的位图，不能走 update_image()：那会把这张按显示分辨率
        # 重采样的位图灌进背景的"内容"缓存，马赛克的缩小图届时就会按显示
        # 分辨率而不是原图分辨率切块，缩放后马赛克内容会错位/错误。
        background_item.set_display_pixmap(
            scaled,
            QTransform.fromScale(1.0 / bg_scale_x, 1.0 / bg_scale_y),
        )
        self._last_background_scale_size = target_size

    def content_rect(self) -> QRectF:
        """内容区域（整个窗口）"""
        return QRectF(self.rect())

    # ==================================================================
    # Qt 事件
    # ==================================================================

    @safe_event
    def resizeEvent(self, event):
        if hasattr(self, 'view') and self.view:
            self.view.setGeometry(0, 0, self.width(), self.height())
            if self._thumbnail_mode:
                self._thumbnail.update_view()
            else:
                self._update_view_transform()
            self.view._update_viewport_mask()

        if not self._thumbnail_mode:
            self.update_button_positions()
            if self.toolbar and self.toolbar.isVisible():
                self.toolbar.sync_with_pin_window()

        # OCR 层
        if not self._thumbnail_mode and hasattr(self, '_ocr_mgr'):
            cr = self.content_rect()
            self._ocr_mgr.update_geometry(cr.toRect())

        # 描边 Overlay
        if hasattr(self, 'border_overlay') and self.border_overlay:
            self.border_overlay.setGeometry(0, 0, self.width(), self.height())
            self.border_overlay.raise_()

        self._sync_click_through_bar()
        super().resizeEvent(event)

    @safe_event
    def moveEvent(self, event):
        super().moveEvent(event)
        self._sync_click_through_bar()

    def _sync_click_through_bar(self):
        bar = self._click_through_bar
        if bar is not None and bar.isVisible():
            bar.follow_pin()

    @safe_event
    def paintEvent(self, event):
        # View 是唯一的内容渲染者，这里不画任何东西
        pass

    @safe_event
    def mousePressEvent(self, event: QMouseEvent):
        if self._handle_mouse_gesture(event):
            event.accept()
            return
        self.hover_controls.set_pin_hovered(True)
        if event.button() == Qt.MouseButton.LeftButton and not (self.canvas and self.canvas.is_editing):
            self.start_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        super().mousePressEvent(event)

    @safe_event
    def mouseMoveEvent(self, event: QMouseEvent):
        if self._handle_mouse_gesture(event):
            event.accept()
            return
        self.hover_controls.set_pin_hovered(True)
        if self._is_dragging:
            self.update_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        super().mouseMoveEvent(event)

    @safe_event
    def mouseReleaseEvent(self, event: QMouseEvent):
        if self._handle_mouse_gesture(event):
            event.accept()
            return
        if event.button() == Qt.MouseButton.LeftButton and self._is_dragging:
            self.end_window_drag()
            event.accept()
            return
        elif event.button() == Qt.MouseButton.RightButton:
            self.show_context_menu(event.globalPosition().toPoint())
            event.accept()
            return
        super().mouseReleaseEvent(event)

    @safe_event
    def wheelEvent(self, event: QWheelEvent):
        if self._thumbnail_mode:
            event.ignore()
            return
        # Alt+滚轮在 Windows 上会被当成横向滚动送来
        delta = event.angleDelta().y() or event.angleDelta().x()
        if delta == 0:
            event.ignore()
            return
        config = getattr(self, 'config_manager', None)
        for action in ("zoom", "fine_zoom", "opacity"):
            if mouse_binding_matches(get_pin_mouse_binding(config, action), event, "wheel"):
                PinWindow._adjust_mouse_value(self, action, delta)
                return
        event.ignore()

    def _adjust_mouse_value(self, action, delta):
        if action == "opacity":
            step = PinWindow._pin_setting(self, "pin_opacity_step") / 100
            if delta < 0:
                step = -step
            self._win_opacity = max(MIN_OPACITY, min(1.0, round(self._win_opacity + step, 4)))
            self.setWindowOpacity(self._win_opacity)
            self._show_hint_label(f"α {round(self._win_opacity * 100)}%")
        elif action in ("zoom", "fine_zoom"):
            if PinWindow._lock_blocks(self, "move"):
                self.show_locked_hint()
                return
            step = PinWindow._pin_setting(self, "pin_zoom_step" if action == "zoom" else "pin_fine_zoom_step") / 100
            # 每格按原图尺寸加减固定的百分点，来回滚动总能回到原来的大小
            scale = round(self.scale_factor + (step if delta > 0 else -step), 4)
            fraction = ZOOM_ANCHORS.get(PinWindow._pin_setting(self, "pin_zoom_anchor"), (0.0, 0.0))
            if fraction is None:
                pos = self.mapFromGlobal(QCursor.pos())
                fraction = (min(max(pos.x() / max(1, self.width()), 0.0), 1.0),
                            min(max(pos.y() / max(1, self.height()), 0.0), 1.0))
            PinWindow._scale_around(self, scale, *fraction)

    def _display_base_size(self):
        """100% 时的窗口尺寸，旋转 90°/270° 后宽高互换。"""
        transform = getattr(self, '_image_transform', None)
        return transform.display_size(self._orig_size) if transform else self._orig_size

    def _scale_around(self, scale, fx, fy, anchor=None):
        """缩放到 scale，窗口上 (fx, fy) 比例处的点停在 anchor，默认就是它现在的位置。"""
        base = PinWindow._display_base_size(self)
        min_scale = max(MIN_SIDE / base.width(), MIN_SIDE / base.height())
        scale = max(min_scale, min(scale, MAX_SCALE))
        if abs(scale - 1.0) < 1e-6:
            scale = 1.0
        # 始终从原图尺寸计算，避免按当前整数窗口尺寸反复取整
        nw = max(1, round(base.width() * scale))
        nh = max(1, round(base.height() * scale))
        if anchor is None:
            geo = self.geometry()
            anchor = QPointF(geo.x() + fx * geo.width(), geo.y() + fy * geo.height())
        self.scale_factor = scale
        self._is_scaling = True
        self.setGeometry(round(anchor.x() - fx * nw), round(anchor.y() - fy * nh), nw, nh)
        if self.canvas:
            self.canvas.invalidate_cache()
        self.update()
        self._scale_timer.start()
        self._show_zoom_percent()

    def _blocked_by_modal(self) -> bool:
        # 鼠标移动也会走到这里，先用 activeModalWidget() 排除没有模态窗口的常见情况
        return QApplication.activeModalWidget() is not None and blocking_modal(self) is not None

    def handle_mouse_alternative_key(self, event):
        from core.shortcut_manager import event_key, _split_modifiers

        key = event_key(event)
        if key not in (Qt.Key.Key_Plus, Qt.Key.Key_Equal, Qt.Key.Key_Minus):
            return False
        if self._blocked_by_modal() or self._thumbnail_mode:
            return False
        modifier_options = [event.modifiers()]
        # Prefer an explicit Shift binding, then allow Shift used to type '+'.
        if key == Qt.Key.Key_Plus:
            modifier_options.append(event.modifiers() & ~Qt.KeyboardModifier.ShiftModifier)
        for modifiers in modifier_options:
            for action in ("zoom", "fine_zoom", "opacity"):
                binding = get_pin_mouse_binding(self.config_manager, action)
                if not binding:
                    continue
                expected, _parts = _split_modifiers(binding)
                if modifiers == expected:
                    self._adjust_mouse_value(action, 120 if key != Qt.Key.Key_Minus else -120)
                    return True
        return False

    def _matching_mouse_action(self, event, gesture):
        for action, _label, _default, kind in PIN_MOUSE_ACTIONS:
            if kind != "wheel" and mouse_binding_matches(
                get_pin_mouse_binding(self.config_manager, action), event, gesture
            ):
                if action == "copy_text":
                    layer = self.ocr_text_layer
                    if not layer or not layer.get_selected_text():
                        continue
                return action
        return None

    def _run_mouse_action(self, action, position=None):
        if self._is_closed:
            return
        blocked_by = LOCKED_MOUSE_ACTIONS.get(action)
        if blocked_by and self._lock_blocks(blocked_by):
            self.show_locked_hint()
            return
        if action == "close":
            self.close_window()
        elif action == "reset" and not self._thumbnail_mode:
            self.reset_to_original_size()
        elif action == "thumbnail":
            self.toggle_thumbnail_mode()
        elif action == "copy_text" and self.ocr_text_layer:
            self.ocr_text_layer._copy_selected_text()
        elif action == "context_menu" and position is not None:
            self.show_context_menu(position)

    def _finish_mouse_click(self):
        action = self._pending_mouse_action
        position = self._pending_mouse_position
        self._pending_mouse_action = None
        self._pending_mouse_position = None
        self._run_mouse_action(action, position)

    def _handle_mouse_gesture(self, event):
        if self._is_closed or (self.canvas and self.canvas.is_editing):
            return False
        if self._blocked_by_modal():
            return False
        kind = event.type()
        if kind not in (QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonDblClick,
                        QEvent.Type.MouseMove, QEvent.Type.MouseButtonRelease):
            return False
        names = {Qt.MouseButton.LeftButton: "left", Qt.MouseButton.MiddleButton: "middle",
                 Qt.MouseButton.RightButton: "right"}
        button = event.button()
        gesture = names.get(button)
        if kind == QEvent.Type.MouseButtonRelease and button == self._mouse_swallow_release:
            self._mouse_swallow_release = None
            return True
        if kind == QEvent.Type.MouseButtonDblClick and gesture:
            action = self._matching_mouse_action(event, "double" + gesture)
            if action:
                self._mouse_click_timer.stop()
                self._pending_mouse_action = None
                self._pending_mouse_position = None
                self._mouse_gesture = None
                self.end_window_drag()
                self.view._window_dragging = False
                self._mouse_swallow_release = button
                self._run_mouse_action(action)
                return True
        if kind == QEvent.Type.MouseButtonPress and gesture:
            action = self._matching_mouse_action(event, gesture)
            region = self._matching_mouse_action(event, "drag" + gesture) == "region"
            defer_menu = gesture == "right" and self._matching_mouse_action(event, "doubleright") is not None
            if action or (region and not self._thumbnail_mode) or defer_menu:
                self._mouse_gesture = (button, event.globalPosition().toPoint(), action, region)
                return True
        state = self._mouse_gesture
        if state is None:
            return False
        drag_button, start, action, region = state
        current = event.globalPosition().toPoint()
        dragged = (current - start).manhattanLength() >= QApplication.startDragDistance()
        if kind == QEvent.Type.MouseMove:
            if region and dragged:
                if self._mouse_band is None:
                    self._mouse_band = QRubberBand(QRubberBand.Shape.Rectangle, self.view.viewport())
                rect = QRect(self.view.viewport().mapFromGlobal(start),
                             self.view.viewport().mapFromGlobal(current)).normalized()
                self._mouse_band.setGeometry(rect.intersected(self.view.viewport().rect()))
                self._mouse_band.show()
                self._mouse_band.raise_()
            return True
        if kind == QEvent.Type.MouseButtonRelease and button == drag_button:
            self._mouse_gesture = None
            if self._mouse_band is not None:
                self._mouse_band.hide()
            if region and dragged:
                rect = QRect(self.view.viewport().mapFromGlobal(start),
                             self.view.viewport().mapFromGlobal(current)).normalized()
                rect = rect.intersected(self.view.viewport().rect())
                if rect.width() > 2 and rect.height() > 2:
                    if self._lock_blocks("move"):
                        self.show_locked_hint()
                    else:
                        self._thumbnail.enter_region(self.view.mapToScene(rect).boundingRect())
            elif not dragged:
                if action is None and button == Qt.MouseButton.RightButton:
                    action = "context_menu"
                if action:
                    if self._matching_mouse_action(event, "double" + names[button]):
                        self._pending_mouse_action = action
                        self._pending_mouse_position = current
                        self._mouse_click_timer.start(QApplication.doubleClickInterval())
                    else:
                        self._run_mouse_action(action, current)
            return True
        return False

    @safe_event
    def mouseDoubleClickEvent(self, event):
        if self._handle_mouse_gesture(event):
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def _apply_smooth_scaling(self):
        if self._is_closed:
            return
        self._is_scaling = False
        self._refresh_background_for_scale()
        self.update()

    def _show_zoom_percent(self):
        """在左上角显示当前缩放百分比"""
        # 使用逻辑缩放比例，避免窗口像素取整掩盖真实比例。
        percent = int(round(self.scale_factor * 100))
        self._show_hint_label(f"{percent}%")

    def _show_hint_label(self, text: str):
        """在左上角显示提示 label（缩放% 和透明度% 共用）。"""
        self._zoom_label.setText(text)
        self._zoom_label.adjustSize()
        self._zoom_label.move(8, 8)
        self._zoom_label.raise_()
        self._zoom_label.show()
        self._zoom_hide_timer.start()

    @safe_event
    def enterEvent(self, event):
        super().enterEvent(event)
        self.hover_controls.set_pin_hovered(True)

    @safe_event
    def leaveEvent(self, event):
        super().leaveEvent(event)
        self.hover_controls.recheck_pin_hovered()

    @safe_event
    def keyPressEvent(self, event: QKeyEvent):
        # 钉图快捷键已由 ShortcutManager 统一分发给 PinEdit/PinNormal Handler
        # 这里只做兜底，防止焦点偶尔在 PinWindow 上时按键无反应
        super().keyPressEvent(event)

    @safe_event
    def eventFilter(self, obj, event):
        if self.view and obj in (self.view.viewport(), self.ocr_text_layer):
            if self._handle_border_resize(obj, event):
                return True
            if self._handle_mouse_gesture(event):
                return True
        if self.view and obj == self.view.viewport():
            if event.type() in (QEvent.Type.Enter, QEvent.Type.HoverEnter, QEvent.Type.MouseMove):
                self.hover_controls.set_pin_hovered(True)
            elif event.type() in (QEvent.Type.Leave, QEvent.Type.HoverLeave):
                self.hover_controls.recheck_pin_hovered()
        return super().eventFilter(obj, event)

    # ==================================================================
    # 缩略图模式（委托给 PinThumbnailMode）
    # ==================================================================

    def toggle_thumbnail_mode(self):
        self._thumbnail.toggle()

    # ==================================================================
    # 工具栏管理
    # ==================================================================

    def toggle_toolbar(self):
        self.hover_controls.toggle_toolbar()

    # 下面两个只由 PinHoverControls.sync() 调用
    def _show_toolbar(self):
        if not self.toolbar:
            from .pin_toolbar import PinToolbar
            self.toolbar = PinToolbar(parent_pin_window=self, config_manager=self.config_manager)
            if self.canvas:
                self.canvas.connect_toolbar(self.toolbar, self.view)
            log_debug(T("创建工具栏，信号已由 PinCanvas 连接"), "PinWindow")
        self.toolbar.show()

    def _hide_toolbar(self):
        if not self.toolbar:
            return
        if hasattr(self.toolbar, '_hide_all_panels'):
            self.toolbar._hide_all_panels()
        # 先隐藏再退出工具：退出工具会回调 sync()，那时工具栏得已经是隐藏状态
        self.toolbar.hide()
        if getattr(self.toolbar, 'current_tool', None):
            for btn in self.toolbar.tool_buttons.values():
                btn.setChecked(False)
            self.toolbar.current_tool = None
            self.toolbar.tool_changed.emit("cursor")

    # ==================================================================
    # 翻译
    # ==================================================================

    def _on_translate_clicked(self):
        if not hasattr(self, '_translation_helper'):
            return
        if self._ocr_has_result:
            if self.ocr_text_layer:
                self._translation_helper.translate(self.ocr_text_layer)
        else:
            if not self._translation_helper.begin_ocr_translation():
                return
            # 先把控制权交还事件循环，让翻译窗口真正绘制出来，再做可能需要
            # 初始化模型的 OCR；这与截图翻译“先出面板、后识别”的体感一致。
            QTimer.singleShot(0, self._start_ocr_for_translation)

    def _start_ocr_for_translation(self):
        if self._is_closed:
            return
        if self._ocr_mgr.recognize_then(self._on_ocr_translation_finished):
            log_info(T("OCR 识别中，翻译将在识别完成后自动执行"), "Translate")
        else:
            log_warning(T("无法启动钉图 OCR 识别"), "Translate")
            self._translation_helper.complete_ocr_translation(
                False, self.tr("OCR recognition could not be started")
            )

    def _on_ocr_translation_finished(self, success: bool, result: str):
        """接收钉图文字层 OCR 结果，交给统一翻译界面。"""
        if hasattr(self, '_translation_helper'):
            self._translation_helper.complete_ocr_translation(success, result)

    # ==================================================================
    # 右键菜单
    # ==================================================================

    def show_context_menu(self, global_pos: QPoint):
        if not hasattr(self, '_context_menu'):
            return
        state = {
            'toolbar_visible': self.toolbar and self.toolbar.isVisible(),
            'stay_on_top': bool(self.windowFlags() & Qt.WindowType.WindowStaysOnTopHint),
            'click_through': self._click_through,
            'locked': self._locked,
            'border_enabled': self.border_enabled,
            'text_selection_enabled': self._text_selection_enabled,
            'thumbnail_mode': self._thumbnail_mode,
            'close_shortcut_enabled': (
                not (self.canvas and self.canvas.is_editing)
                and not self._lock_blocks("key_close")
                and not self._click_through
            ),
        }
        # 菜单开着算悬停，工具栏不会在背后被收起，菜单上的开关状态就一直准确
        self.hover_controls.set_menu_open(True)
        try:
            self._context_menu.show(global_pos, state)
        finally:
            self.hover_controls.set_menu_open(False)

    def toggle_stay_on_top(self):
        flags = self.windowFlags()
        if flags & Qt.WindowType.WindowStaysOnTopHint:
            new_flags = flags & ~Qt.WindowType.WindowStaysOnTopHint
        else:
            new_flags = flags | Qt.WindowType.WindowStaysOnTopHint
        geo = self.geometry()
        self.setWindowFlags(new_flags)
        self.setGeometry(geo)
        self.show()

    # ==================================================================
    # 锁定
    # ==================================================================

    def toggle_lock(self):
        self.set_locked(not self._locked)

    def set_locked(self, locked: bool):
        """锁定后按设置禁止移动缩放、鼠标关闭、按键关闭；右键菜单照常可用，也从那里解锁。"""
        if self._is_closed or locked == self._locked:
            return
        self._locked = locked
        self.end_window_drag()
        self._end_border_resize()
        self._image_transform._refresh_border(self)
        if self._pin_setting("pin_lock_hint"):
            self._show_hint_label(self.tr("Locked, right-click to unlock") if locked else self.tr("Unlocked"))

    def _lock_blocks(self, kind) -> bool:
        """锁定时这类操作是否被禁止：move（含缩放、调大小）、click_close、key_close。"""
        return self._locked and bool(PinWindow._pin_setting(self, f"pin_lock_block_{kind}"))

    def show_locked_hint(self):
        if PinWindow._pin_setting(self, "pin_lock_hint"):
            self._show_hint_label(self.tr("Locked, right-click to unlock"))

    def close_unless_locked(self, kind="key_close"):
        """快捷键、鼠标手势和悬停关闭按钮走这里；右键菜单的「关闭」是明确操作，锁定也照关。"""
        if self._lock_blocks(kind):
            self.show_locked_hint()
            return
        self.close_window()

    # ==================================================================
    # 鼠标穿透
    # ==================================================================

    def toggle_click_through(self):
        self.set_click_through(not self._click_through)

    def set_click_through(self, enabled: bool):
        """穿透时点击和滚轮都落到下层窗口，移动、调不透明度、退出改由上方的工具条完成。"""
        if self._is_closed or enabled == self._click_through:
            return
        self._click_through = enabled
        self.end_window_drag()
        if enabled:
            self._opacity_before_click_through = self._win_opacity
            self._set_window_opacity(min(self._win_opacity, self._click_through_opacity))
            if self._click_through_bar is None:
                from .pin_click_through_bar import PinClickThroughBar
                self._click_through_bar = PinClickThroughBar(self)
            self._click_through_bar.set_opacity(self._win_opacity)
            self._click_through_bar.show()
        else:
            self._click_through_bar.hide()
            self._set_window_opacity(self._opacity_before_click_through)
            self._opacity_before_click_through = None
        set_window_click_through(int(self.winId()), enabled)
        # 编辑中进入穿透会在这里收起工具栏，绘制工具随之退出
        self.hover_controls.sync()
        self._show_hint_label(self.tr("Click-through on") if enabled else self.tr("Click-through off"))

    def set_click_through_opacity(self, opacity: float):
        """穿透工具条的滑块。记在这张钉图上，下次进入穿透沿用。"""
        if not self._click_through:
            return
        self._click_through_opacity = opacity
        self._set_window_opacity(opacity)

    def _set_window_opacity(self, opacity: float):
        self._win_opacity = opacity
        self.setWindowOpacity(opacity)

    def toggle_border_effect(self):
        self.border_enabled = not self.border_enabled
        self._image_transform._refresh_border(self)
        self.update()

    def toggle_text_selection(self):
        if hasattr(self, '_ocr_mgr'):
            self._ocr_mgr.toggle_text_selection()

    def reset_to_original_size(self):
        """恢复到原图 100% 大小（保留当前旋转/翻转状态，与滚轮缩放一样固定左上角坐标）"""
        # 根据当前变换状态计算 100% 显示尺寸
        if hasattr(self, '_image_transform') and self._image_transform.has_transform:
            sz = self._image_transform.display_size(self._orig_size)
        else:
            sz = self._orig_size
        target_w = sz.width()
        target_h = sz.height()
        # 固定左上角坐标（与滚轮缩放行为一致）
        self.setGeometry(self.x(), self.y(), target_w, target_h)
        self.scale_factor = 1.0
        if self.canvas:
            self.canvas.invalidate_cache()
        self.update_button_positions()
        self._update_view_transform()
        self._refresh_background_for_scale()
        if self.toolbar and self.toolbar.isVisible():
            self.toolbar.sync_with_pin_window()
        self._image_transform._refresh_border(self)
        self.update()
        self._show_zoom_percent()

    # ==================================================================
    # 图像变换（委托给 PinImageTransform）
    # ==================================================================

    def rotate_image_cw(self):
        """顺时针旋转 90°"""
        self._image_transform.rotate_cw()
        self._image_transform.apply_to_window(self)

    def rotate_image_ccw(self):
        """逆时针旋转 90°"""
        self._image_transform.rotate_ccw()
        self._image_transform.apply_to_window(self)

    def flip_image_horizontal(self):
        """水平翻转"""
        self._image_transform.flip_horizontal()
        self._image_transform.apply_to_window(self)

    def flip_image_vertical(self):
        """垂直翻转"""
        self._image_transform.flip_vertical()
        self._image_transform.apply_to_window(self)

    def reset_image_transform(self):
        """重置所有图像变换"""
        self._image_transform.reset()
        self._image_transform.apply_to_window(self)

    # ==================================================================
    # 图像导出
    # ==================================================================

    def get_current_image(self) -> QImage:
        dpr = self.devicePixelRatioF()
        if self.canvas:
            img = self.canvas.get_current_image(dpr)
        else:
            img = QImage(
                int(self.width() * dpr), int(self.height() * dpr),
                QImage.Format.Format_ARGB32_Premultiplied,
            )
            img.fill(Qt.GlobalColor.transparent)
            img.setDevicePixelRatio(dpr)
            p = QPainter(img)
            p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
            p.drawPixmap(self.rect(), self._base_pixmap)
            p.end()
        # 应用图像变换（旋转/翻转）
        if hasattr(self, '_image_transform'):
            img = self._image_transform.transform_image(img)
        return img

    def _with_edit_paused(self, func):
        """退出编辑模式执行操作，再恢复。"""
        was_editing = self.canvas and self.canvas.is_editing
        active_tool_id = None
        if was_editing and hasattr(self.canvas, 'tool_controller'):
            ct = self.canvas.tool_controller.current_tool
            if ct:
                active_tool_id = ct.id
            self.canvas.deactivate_tool()
        try:
            func()
        finally:
            if was_editing and active_tool_id:
                self.canvas.activate_tool(active_tool_id)

    def save_image(self):
        from datetime import datetime
        from core.save import SaveService
        from ui.dialogs import get_save_file_name
        import os
        import re

        fmt = (self.config_manager.get_screenshot_format() if self.config_manager else "PNG").lower()
        default_name = f"pinned_{datetime.now().strftime('%Y%m%d_%H%M%S')}.{fmt}"
        save_service = SaveService(config_manager=self.config_manager)
        file_path, selected_filter = get_save_file_name(
            self,
            self.tr("Save Pin Image"),
            save_service.suggest_path(default_name),
            "PNG (*.png);;JPG (*.jpg);;BMP (*.bmp);;WebP (*.webp);;PDF (*.pdf)",
        )
        if not file_path:
            return

        ext = os.path.splitext(file_path)[1].lstrip(".")
        if ext:
            image_format = ext.upper()
        else:
            match = re.search(r'\*\.(\w+)', selected_filter)
            image_format = match.group(1).upper() if match else "PNG"
        if not ext:
            file_path = f"{file_path}.{image_format.lower()}"

        def _do_save():
            image = self.get_current_image()
            if save_service.save_qimage_to_path(image, file_path, image_format=image_format):
                log_info(T("保存成功: {file_path}", file_path=file_path), "PinWindow")
            else:
                log_error(T("保存失败: {file_path}", file_path=file_path), "PinWindow")

        self._with_edit_paused(_do_save)

    def copy_to_clipboard(self):
        def _do_copy():
            image = self.get_current_image()
            deliver_image_async(image)
        self._with_edit_paused(_do_copy)

    def copy_all_text(self):
        """复制钉图中识别到的全部文字；尚未识别过则先识别再复制。"""
        layer = self.ocr_text_layer
        if self._ocr_has_result and layer is not None and layer.has_text():
            self._copy_ocr_text(True, layer.get_all_text(separator="\n"))
            return
        if not self._ocr_mgr.recognize_then(self._copy_ocr_text):
            self._show_hint_label(self.tr("OCR recognition could not be started"))

    def _copy_ocr_text(self, success: bool, result: str):
        """recognize_then 的回调：成功时 result 是全部文字，失败原因已由它记日志。"""
        if not success or not result.strip():
            self._show_hint_label(self.tr("No text was recognized"))
            return
        QApplication.clipboard().setText(result)
        log_info(T("已复制钉图文字: {count} 字符", count=len(result)), "PinWindow")
        self._show_hint_label(self.tr("Text copied"))

    # ==================================================================
    # 窗口关闭 / 资源清理
    # ==================================================================

    def close_window(self):
        if self._is_closed:
            return
        log_debug(T("开始关闭"), "PinWindow")
        self._is_closed = True
        self.cleanup()
        self.closed.emit()
        self.close()

    def cleanup(self):
        log_debug(T("清理资源..."), "PinWindow")
        try:
            # 从快捷键控制器注销
            try:
                from .pin_shortcut import PinShortcutController
                PinShortcutController.instance().unregister(self)
            except Exception as e:
                log_exception(e, T("注销快捷键控制器"))

            # 定时器
            if hasattr(self, 'hover_controls'):
                self.hover_controls.stop()

            if hasattr(self, '_scale_timer') and self._scale_timer:
                try:
                    self._scale_timer.stop()
                    self._scale_timer.deleteLater()
                    self._scale_timer = None
                except Exception as e:
                    log_exception(e, T("停止缩放定时器"))

            if hasattr(self, '_zoom_hide_timer') and self._zoom_hide_timer:
                try:
                    self._zoom_hide_timer.stop()
                    self._zoom_hide_timer.deleteLater()
                    self._zoom_hide_timer = None
                except Exception as e:
                    log_exception(e, T("停止缩放百分比定时器"))

            # 鼠标穿透工具条
            if self._click_through_bar is not None:
                try:
                    self._click_through_bar.close()
                    self._click_through_bar.deleteLater()
                    self._click_through_bar = None
                except Exception as e:
                    log_exception(e, T("清理鼠标穿透工具条"))

            # 工具栏
            if hasattr(self, 'toolbar') and self.toolbar:
                try:
                    for pn in ('paint_panel', 'shape_panel', 'arrow_panel', 'number_panel', 'text_panel'):
                        panel = getattr(self.toolbar, pn, None)
                        if panel:
                            try:
                                panel.close()
                                panel.deleteLater()
                            except Exception as e:
                                log_exception(e, T("关闭工具栏面板"))
                            setattr(self.toolbar, pn, None)
                    for alias in ('paint_menu', 'text_menu'):
                        if hasattr(self.toolbar, alias):
                            setattr(self.toolbar, alias, None)
                    self.toolbar.close()
                    self.toolbar.deleteLater()
                    self.toolbar = None
                except Exception as e:
                    log_exception(e, T("清理工具栏"))

            # OCR
            if hasattr(self, '_ocr_mgr'):
                self._ocr_mgr.cleanup()

            # 视图
            if hasattr(self, 'view') and self.view:
                try:
                    if hasattr(self.view, 'viewport'):
                        try:
                            self.view.viewport().removeEventFilter(self)
                        except Exception as e:
                            log_exception(e, T("移除视图事件过滤器"))
                    self.view.deleteLater()
                    self.view = None
                except Exception as e:
                    log_exception(e, T("清理视图"))

            # 画布
            if hasattr(self, 'canvas') and self.canvas:
                try:
                    self.canvas.cleanup()
                except Exception as e:
                    log_warning(T("画布清理时出错: {e}", e=e), "PinWindow")
                finally:
                    self.canvas = None

            # 图像数据
            self._base_pixmap = None

            log_info(T("资源清理完成"), "PinWindow")
        except Exception as e:
            log_error(T("cleanup过程中发生错误: {e}", e=e), "PinWindow")
            log_exception(e, "PinWindow.cleanup")

    @safe_event
    def closeEvent(self, event):
        try:
            if not self._is_closed:
                self._is_closed = True
                try:
                    self.cleanup()
                except Exception as e:
                    log_error(T("cleanup时发生错误: {e}", e=e), "PinWindow")
                    log_exception(e, "PinWindow.cleanup")
                try:
                    self.closed.emit()
                except Exception as e:
                    log_error(T("发送closed信号时发生错误: {e}", e=e), "PinWindow")
            super().closeEvent(event)
        except Exception as e:
            log_error(T("closeEvent发生严重错误: {e}", e=e), "PinWindow")
            try:
                super().closeEvent(event)
            except Exception as e:
                log_exception(e, "PinWindow super closeEvent")
