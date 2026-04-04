"""
钉图窗口 - 核心窗口类

架构说明：
- PinWindow：主窗口，只负责窗口管理和子控件布局
- PinCanvasView：唯一内容渲染者，使用 Qt 的 GPU 加速渲染
- PinCanvas：画布核心，包含工具信号路由
- PinOCRManager：OCR 初始化和线程管理
- PinThumbnailMode：缩略图模式逻辑
- PinControlButtons：控制按钮管理器
- PinContextMenu：右键菜单管理器
- PinTranslationHelper：翻译功能助手
"""

from PySide6.QtWidgets import QWidget, QApplication, QLabel
from PySide6.QtCore import Qt, QPoint, QPointF, QSize, QTimer, Signal, QRect, QRectF, QEvent
from PySide6.QtGui import (
    QPixmap, QImage, QPainter, QMouseEvent, QWheelEvent, QKeyEvent,
    QColor, QPainterPath, QTransform,
)
from .pin_canvas_view import PinCanvasView
from .pin_controls import PinControlButtons
from .pin_context_menu import PinContextMenu
from .pin_translation import PinTranslationHelper
from .pin_border_overlay import PinBorderOverlay
from .pin_ocr_manager import PinOCRManager
from .pin_thumbnail import PinThumbnailMode
from .pin_image_transform import PinImageTransform
from core import log_debug, log_info, log_warning, log_error, safe_event
from core.theme import get_theme
from core.logger import log_exception
from core.clipboard_utils import copy_image_to_clipboard


class PinWindow(QWidget):
    """
    钉图窗口 - 可拖动、缩放、编辑的置顶图像窗口

    核心特性:
    - 无边框置顶窗口 + 描边/阴影效果
    - 拖动移动 / 滚轮缩放
    - 鼠标悬停显示控制按钮
    - ESC 关闭 / R 缩略图模式
    - 支持绘图编辑（委托给 PinCanvas）
    - OCR 文字选择（委托给 PinOCRManager）
    """

    # 信号
    closed = Signal()  # 窗口关闭信号

    def __init__(self, image: QImage, position: QPoint, config_manager,
                 drawing_items=None, selection_offset=None):
        """
        Args:
            image: 选区底图（只包含选区的纯净背景，不含绘制）
            position: 初始位置（全局坐标）
            config_manager: 配置管理器
            drawing_items: 绘制项目列表（从截图窗口继承）
            selection_offset: 选区在原场景中的偏移量
        """
        super().__init__()

        self.config_manager = config_manager
        self.drawing_items = drawing_items or []
        self.selection_offset = selection_offset or QPoint(0, 0)

        # ====== 光晕/阴影样式参数 ======
        self.halo_enabled = True
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
        self._last_hover_state = False

        # ====== 设置窗口属性 ======
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool |
            Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        if self.halo_enabled:
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
        self._win_opacity = 1.0   # 范围 [0.15, 1.0]

        # ====== 缩放百分比提示 ======
        self._zoom_label = QLabel(self)
        self._zoom_label.setStyleSheet(
            "QLabel {"
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
        self.setGeometry(position.x(), position.y(), image.width(), image.height())

        # ====== UI 组件 ======
        self.setup_ui()

        # ====== 画布 ======
        from .pin_canvas import PinCanvas
        self.canvas = PinCanvas(self, self._orig_size, image)
        if self.drawing_items:
            self.canvas.initialize_from_items(self.drawing_items, self.selection_offset)

        # ====== CanvasView ======
        self.view = PinCanvasView(self.canvas.scene, self, self.canvas)
        self.view.setParent(self)
        self.view.setGeometry(0, 0, self.width(), self.height())
        self.view.set_corner_radius(self.corner)
        self._update_view_transform()
        self.view.viewport().installEventFilter(self)

        # ====== 工具栏（按需创建） ======
        self.toolbar = None

        # ====== OCR 管理器 ======
        self._ocr_mgr = PinOCRManager(self, config_manager)

        # ====== 缩略图模式 ======
        self._thumbnail = PinThumbnailMode(self)

        # ====== 图像变换管理器 ======
        self._image_transform = PinImageTransform()

        # ====== 描边 Overlay（单圈主题色，无阴影）======
        self.border_overlay = None
        if self.halo_enabled:
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
            f"创建成功: {image.width()}x{image.height()}, 位置: ({position.x()}, {position.y()})",
            "PinWindow",
        )
        if self.drawing_items:
            log_debug(f"继承了 {len(self.drawing_items)} 个绘制项目（向量数据）", "PinWindow")

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
            close_handler=self.close_window,
            toggle_toolbar_handler=self.toggle_toolbar,
        )
        self._context_menu = PinContextMenu(self)
        self._translation_helper = PinTranslationHelper(self, self.config_manager)
        self.update_button_positions()

    def update_button_positions(self):
        """更新按钮位置"""
        if hasattr(self, '_control_buttons'):
            self._control_buttons.update_positions(self.width())

    # ==================================================================
    # 悬停 / 控制按钮
    # ==================================================================

    def _auto_toolbar_enabled(self) -> bool:
        return self.config_manager.get_pin_auto_toolbar() if self.config_manager else True

    def _ensure_hover_controls_visible(self):
        if not self.close_button.isVisible():
            self.close_button.show()
        self.close_button.raise_()

        if self._thumbnail_mode:
            return

        if not self._auto_toolbar_enabled() and not self.toolbar_toggle_button.isVisible():
            self.toolbar_toggle_button.show()
        self.toolbar_toggle_button.raise_()

        if self._auto_toolbar_enabled():
            toolbar_hidden = not self.toolbar or not self.toolbar.isVisible()
            if toolbar_hidden and not self._is_closed:
                self.show_toolbar()

    def _set_hover_state(self, hovering: bool):
        if hovering:
            self._ensure_hover_controls_visible()
            if self.toolbar:
                self.toolbar.on_parent_hover(True)
            self._last_hover_state = True
            return
        if self.toolbar:
            self.toolbar.on_parent_hover(False)
        self._last_hover_state = False
        QTimer.singleShot(300, self._delayed_hide_buttons)

    def _delayed_hide_buttons(self):
        if self._is_closed:
            return
        if self._last_hover_state:
            return
        if not self.underMouse():
            self.close_button.hide()
            self.toolbar_toggle_button.hide()

    def _set_control_buttons_visible(self, visible: bool):
        if hasattr(self, 'close_button') and self.close_button:
            self.close_button.setVisible(visible)
        if hasattr(self, 'toolbar_toggle_button') and self.toolbar_toggle_button:
            self.toolbar_toggle_button.setVisible(visible)

    # ==================================================================
    # 窗口拖动
    # ==================================================================

    def start_window_drag(self, global_pos: QPoint):
        self._is_dragging = True
        self._drag_start_pos = global_pos
        self._drag_start_window_pos = self.pos()
        self.setCursor(Qt.CursorShape.ClosedHandCursor)

    def update_window_drag(self, global_pos: QPoint):
        if not self._is_dragging:
            return
        delta = global_pos - self._drag_start_pos
        self.move(self._drag_start_window_pos + delta)
        if self.toolbar and self.toolbar.isVisible():
            self.toolbar.sync_with_pin_window()

    def end_window_drag(self):
        if self._is_dragging:
            self._is_dragging = False
            self.setCursor(Qt.CursorShape.ArrowCursor)

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

        # 通知光标管理器更新缩放（光标大小跟随视觉缩放）
        cursor_mgr = getattr(self.view, 'cursor_manager', None)
        if cursor_mgr:
            cursor_mgr.update_view_scale(float(scale_x))

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
        background_item.update_image(scaled.toImage())
        background_item.setTransform(
            QTransform.fromScale(1.0 / bg_scale_x, 1.0 / bg_scale_y)
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

        super().resizeEvent(event)

    @safe_event
    def moveEvent(self, event):
        super().moveEvent(event)

    @safe_event
    def paintEvent(self, event):
        # View 是唯一的内容渲染者，这里不画任何东西
        pass

    @safe_event
    def mousePressEvent(self, event: QMouseEvent):
        self._set_hover_state(True)
        if event.button() == Qt.MouseButton.LeftButton and not (self.canvas and self.canvas.is_editing):
            self.start_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        super().mousePressEvent(event)

    @safe_event
    def mouseMoveEvent(self, event: QMouseEvent):
        self._set_hover_state(True)
        if self._is_dragging:
            self.update_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        super().mouseMoveEvent(event)

    @safe_event
    def mouseReleaseEvent(self, event: QMouseEvent):
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
        delta = event.angleDelta().y()
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            # Ctrl + 滚轮：调整窗口透明度，每格 ±5%
            step = 0.05 if delta > 0 else -0.05
            self._win_opacity = max(0.15, min(1.0, self._win_opacity + step))
            self.setWindowOpacity(self._win_opacity)
            self._show_hint_label(f"α {int(self._win_opacity * 100)}%")
        else:
            # 普通滚轮：调整窗口大小
            self._is_scaling = True
            sf = 1.05 if delta > 0 else 0.95
            ow, oh = self.width(), self.height()
            nw = max(50, min(int(ow * sf), self._orig_size.width() * 4))
            nh = max(50, min(int(oh * sf), self._orig_size.height() * 4))

            self.setGeometry(self.x(), self.y(), nw, nh)
            if self.canvas:
                self.canvas.invalidate_cache()
            self.update()
            self._scale_timer.start()
            self._show_zoom_percent()

    def _apply_smooth_scaling(self):
        if self._is_closed:
            return
        self._is_scaling = False
        self._refresh_background_for_scale()
        self.update()

    def _show_zoom_percent(self):
        """在左上角显示当前缩放百分比"""
        # 计算百分比：当前窗口宽度 / 变换后原始宽度
        if hasattr(self, '_image_transform') and self._image_transform.is_rotated_90_or_270:
            ref_w = self._orig_size.height()
        else:
            ref_w = self._orig_size.width()
        # 四舍五入到最近的 5% 整数倍，避免像素取整导致 101% 等奇怪数字
        raw = self.width() / ref_w * 100
        percent = round(raw / 5) * 5
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
        self._set_hover_state(True)

    @safe_event
    def leaveEvent(self, event):
        super().leaveEvent(event)
        self._set_hover_state(False)

    @safe_event
    def keyPressEvent(self, event: QKeyEvent):
        # 钉图快捷键已由 ShortcutManager 统一分发给 PinEdit/PinNormal Handler
        # 这里只做兜底，防止焦点偶尔在 PinWindow 上时按键无反应
        super().keyPressEvent(event)

    @safe_event
    def eventFilter(self, obj, event):
        if self.view and obj == self.view.viewport():
            if event.type() in (QEvent.Type.Enter, QEvent.Type.HoverEnter, QEvent.Type.MouseMove):
                self._set_hover_state(True)
            elif event.type() in (QEvent.Type.Leave, QEvent.Type.HoverLeave):
                self._set_hover_state(False)
        return super().eventFilter(obj, event)

    # ==================================================================
    # 缩略图模式（委托给 PinThumbnailMode）
    # ==================================================================

    def toggle_thumbnail_mode(self):
        self._thumbnail.toggle()

    # ==================================================================
    # 工具栏管理
    # ==================================================================

    def show_toolbar(self):
        if not self.toolbar:
            from .pin_toolbar import PinToolbar
            self.toolbar = PinToolbar(parent_pin_window=self, config_manager=self.config_manager)
            if self.canvas:
                self.canvas.connect_toolbar(self.toolbar, self.view)
            log_debug("创建工具栏，信号已由 PinCanvas 连接", "PinWindow")

        auto = self.config_manager.get_pin_auto_toolbar() if self.config_manager else True
        if auto:
            self.toolbar.enable_auto_hide(True)
            self.toolbar.set_auto_hide_delay(2000)
        else:
            self.toolbar.enable_auto_hide(False)
        self.toolbar.show()

    def hide_toolbar(self):
        if self.toolbar:
            if hasattr(self.toolbar, '_hide_all_panels'):
                self.toolbar._hide_all_panels()
            if hasattr(self.toolbar, 'current_tool') and self.toolbar.current_tool:
                for btn in self.toolbar.tool_buttons.values():
                    btn.setChecked(False)
                self.toolbar.current_tool = None
                self.toolbar.tool_changed.emit("cursor")
            self.toolbar.hide()

    def toggle_toolbar(self):
        if self.toolbar and self.toolbar.isVisible():
            self.hide_toolbar()
        else:
            self.show_toolbar()

    # ==================================================================
    # 翻译
    # ==================================================================

    def _on_translate_clicked(self):
        if not hasattr(self, '_translation_helper'):
            return
        if self._ocr_has_result:
            if self.ocr_text_layer:
                self._translation_helper.translate(self.ocr_text_layer)
        elif self._ocr_mgr.is_running:
            self._ocr_mgr.translate_pending = True
            log_info("OCR 识别中，翻译将在识别完成后自动执行", "Translate")
        else:
            log_warning("没有 OCR 结果也没有正在进行的 OCR", "Translate")

    # ==================================================================
    # 右键菜单
    # ==================================================================

    def show_context_menu(self, global_pos: QPoint):
        if hasattr(self, '_context_menu'):
            state = {
                'toolbar_visible': self.toolbar and self.toolbar.isVisible(),
                'stay_on_top': bool(self.windowFlags() & Qt.WindowType.WindowStaysOnTopHint),
                'shadow_enabled': self.halo_enabled,
                'has_ocr_result': self._ocr_has_result,
                'thumbnail_mode': self._thumbnail_mode,
            }
            self._context_menu.show(global_pos, state)

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

    def toggle_border_effect(self):
        self.halo_enabled = not self.halo_enabled
        self._image_transform._refresh_border(self)
        self.update()

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
        from PySide6.QtWidgets import QFileDialog

        file_path, _ = QFileDialog.getSaveFileName(
            self, self.tr("Save Pin Image"), "pinned_image.png", "Images (*.png *.jpg *.bmp)",
        )
        if not file_path:
            return

        def _do_save():
            image = self.get_current_image()
            if image.save(file_path):
                log_info(f"保存成功: {file_path}", "PinWindow")
            else:
                log_error(f"保存失败: {file_path}", "PinWindow")

        self._with_edit_paused(_do_save)

    def copy_to_clipboard(self):
        def _do_copy():
            image = self.get_current_image()
            copy_image_to_clipboard(image)
        self._with_edit_paused(_do_copy)

    # ==================================================================
    # 窗口关闭 / 资源清理
    # ==================================================================

    def close_window(self):
        if self._is_closed:
            return
        log_debug("开始关闭", "PinWindow")
        self._is_closed = True
        self.cleanup()
        self.closed.emit()
        self.close()

    def cleanup(self):
        log_debug("清理资源...", "PinWindow")
        try:
            # 从快捷键控制器注销
            try:
                from .pin_shortcut import PinShortcutController
                PinShortcutController.instance().unregister(self)
            except Exception as e:
                log_exception(e, "注销快捷键控制器")

            # 定时器
            if hasattr(self, '_scale_timer') and self._scale_timer:
                try:
                    self._scale_timer.stop()
                    self._scale_timer.deleteLater()
                    self._scale_timer = None
                except Exception as e:
                    log_exception(e, "停止缩放定时器")

            if hasattr(self, '_zoom_hide_timer') and self._zoom_hide_timer:
                try:
                    self._zoom_hide_timer.stop()
                    self._zoom_hide_timer.deleteLater()
                    self._zoom_hide_timer = None
                except Exception as e:
                    log_exception(e, "停止缩放百分比定时器")

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
                                log_exception(e, "关闭工具栏面板")
                            setattr(self.toolbar, pn, None)
                    for alias in ('paint_menu', 'text_menu'):
                        if hasattr(self.toolbar, alias):
                            setattr(self.toolbar, alias, None)
                    self.toolbar.close()
                    self.toolbar.deleteLater()
                    self.toolbar = None
                except Exception as e:
                    log_exception(e, "清理工具栏")

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
                            log_exception(e, "移除视图事件过滤器")
                    self.view.deleteLater()
                    self.view = None
                except Exception as e:
                    log_exception(e, "清理视图")

            # 画布
            if hasattr(self, 'canvas') and self.canvas:
                try:
                    self.canvas.cleanup()
                except Exception as e:
                    log_warning(f"画布清理时出错: {e}", "PinWindow")
                finally:
                    self.canvas = None

            # 图像数据
            self._base_pixmap = None

            log_info("资源清理完成", "PinWindow")
        except Exception as e:
            log_error(f"cleanup过程中发生错误: {e}", "PinWindow")
            log_exception(e, "PinWindow.cleanup")

    @safe_event
    def closeEvent(self, event):
        try:
            if not self._is_closed:
                self._is_closed = True
                try:
                    self.cleanup()
                except Exception as e:
                    log_error(f"cleanup时发生错误: {e}", "PinWindow")
                    log_exception(e, "PinWindow.cleanup")
                try:
                    self.closed.emit()
                except Exception as e:
                    log_error(f"发送closed信号时发生错误: {e}", "PinWindow")
            super().closeEvent(event)
        except Exception as e:
            log_error(f"closeEvent发生严重错误: {e}", "PinWindow")
            try:
                super().closeEvent(event)
            except Exception as e:
                log_exception(e, "PinWindow super closeEvent")


# 测试代码
# if __name__ == "__main__":
#  import sys
# 
#  app = QApplication(sys.argv)
# 
#  test_image = QImage(400, 300, QImage.Format.Format_ARGB32)
#  test_image.fill(Qt.GlobalColor.lightGray)
# 
#  painter = QPainter(test_image)
#  painter.setPen(Qt.GlobalColor.red)
#  font = painter.font()
#  font.setPixelSize(30)
#  painter.setFont(font)
#  painter.drawText(test_image.rect(), Qt.AlignmentFlag.AlignCenter, "测试钉图窗口\n拖动移动\n滚轮缩放")
#  painter.end()
# 
#  from settings import get_tool_settings_manager
#  config_manager = get_tool_settings_manager()
#  pin_window = PinWindow(test_image, QPoint(100, 100), config_manager)
# 
#  sys.exit(app.exec())
 