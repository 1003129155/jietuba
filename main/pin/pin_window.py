"""
钉图窗口 - 核心窗口类

架构说明（重构后）：
- PinWindow：主窗口，只负责窗口管理和子控件布局
- PinShadowWindow：独立阴影窗口，只绘制阴影效果
- PinCanvasView：唯一内容渲染者，使用 Qt 的 GPU 加速渲染
- PinControlButtons：控制按钮管理器
- PinContextMenu：右键菜单管理器
- PinTranslationHelper：翻译功能助手
- 不再在 paintEvent 中调用 scene.render()
"""

from PyQt6.QtWidgets import QWidget, QLabel, QPushButton, QVBoxLayout, QApplication, QMenu
from PyQt6.QtCore import Qt, QPoint, QPointF, QSize, QTimer, pyqtSignal, QRect, QRectF, QEvent
from PyQt6.QtGui import QPixmap, QImage, QPainter, QMouseEvent, QWheelEvent, QKeyEvent, QPaintEvent, QColor, QPainterPath, QPen, QAction
from pin.pin_canvas_view import PinCanvasView
from pin.pin_shadow_window import PinShadowWindow
from pin.pin_controls import PinControlButtons
from pin.pin_context_menu import PinContextMenu
from pin.pin_translation import PinTranslationHelper
from core import log_debug, log_info, log_warning, log_error
from core.logger import log_exception


class PinWindow(QWidget):
    """
    钉图窗口 - 可拖动、缩放、编辑的置顶图像窗口
    
    核心特性:
    - 无边框置顶窗口 + 光晕/阴影效果
    - 拖动移动位置
    - 滚轮缩放大小
    - 鼠标悬停显示控制按钮
    - ESC 快速关闭
    - 支持绘图编辑
    
    内存优化:
    - 不保存完整截图窗口数据
    """
    
    # 信号
    closed = pyqtSignal()  # 窗口关闭信号
    
    def __init__(self, image: QImage, position: QPoint, config_manager, drawing_items=None, selection_offset=None):
        """
        Args:
            image: 选区底图（只包含选区的纯净背景，不含绘制）
            position: 初始位置（全局坐标）
            config_manager: 配置管理器
            drawing_items: 绘制项目列表（从截图窗口继承）
            selection_offset: 选区在原场景中的偏移量（QPoint，用于转换坐标）
        """
        super().__init__()
        
        self.config_manager = config_manager
        self.drawing_items = drawing_items or []
        self.selection_offset = selection_offset or QPoint(0, 0)
        
        # ====== 🌟 光晕/阴影样式参数 ======
        self.halo_enabled = True          # 是否启用光晕效果
        self.pad = 20                     # 阴影留白（逻辑像素）
        self.corner = 8                   # 内容圆角
        self.shadow_spread = 18           # 阴影"扩散层数"（越大越柔和）
        self.shadow_max_alpha = 80        # 阴影最深处 alpha（0~255）
        self.glow_enable = True           # 外发光开关
        self.glow_spread = 6              # 外发光层数
        self.glow_color = QColor(255, 255, 255)  # 外发光颜色
        self.glow_max_alpha = 35          # 外发光最大alpha
        self.border_enable = True         # 描边开关
        self.border_color = QColor(255, 255, 255, 100)  # 描边颜色
        self.border_width = 1.0           # 描边宽度
        
        # 阴影缓存
        self._shadow_cache: QPixmap | None = None
        self._shadow_key = None
        
        # 窗口状态
        self._is_closed = False
        self._is_dragging = False
        self._is_editing = False
        self._drag_start_pos = QPoint()
        self._drag_start_window_pos = QPoint()
        self._last_hover_state = False
        
        # OCR 和翻译状态
        self._ocr_has_result = False
        
        # 设置窗口属性
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool |
            Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        # 🌟 启用透明背景（用于光晕效果）
        if self.halo_enabled:
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setMouseTracking(True)
        
        # 设置底图
        # 保存原始尺寸（用于缩放限制）
        self._orig_size = image.size()
        
        # 缓存底图 pixmap（避免重复转换）
        self._base_pixmap = QPixmap.fromImage(image)
        # DPR 会在 showEvent 中同步（窗口绑定到屏幕后才准确）
        
        # 释放 QImage 以节省内存（钉图只展示+矢量叠加，不做像素编辑）
        self.base_image = None
        
        # 缩放因子（用于高性能渲染）
        self.scale_factor = 1.0
        
        # 滚轮缩放定时器（用于延迟高质量渲染）
        self._scale_timer = QTimer(self)
        self._scale_timer.setSingleShot(True)
        self._scale_timer.setInterval(150)  # 150ms 没有新滚轮事件后触发
        self._scale_timer.timeout.connect(self._apply_smooth_scaling)
        self._is_scaling = False  # 标记是否正在缩放

        self.view = None
        
        # 🌟 新架构：内容窗口不再有 padding，阴影由独立窗口负责
        # 设置初始大小和位置（内容窗口就是图像大小）
        self.setGeometry(
            position.x(),
            position.y(),
            image.width(),
            image.height()
        )
        
        # 创建UI组件
        self.setup_ui()
        
        # 创建画布（传入背景图像）
        from pin.pin_canvas import PinCanvas
        # 传递基准坐标系（原始图像尺寸）和背景图像
        self.canvas = PinCanvas(self, self._orig_size, image)
        
        # 继承绘制项目（如果有）
        if self.drawing_items:
            self.canvas.initialize_from_items(self.drawing_items, self.selection_offset)

        # 🌟 新架构：创建 CanvasView 作为唯一内容渲染者
        self.view = PinCanvasView(self.canvas.scene, self, self.canvas)
        self.view.setParent(self)
        # View 覆盖整个窗口
        self.view.setGeometry(0, 0, self.width(), self.height())
        # 🌟 设置圆角（与阴影窗口一致）
        self.view.set_corner_radius(self.corner)
        self._update_view_transform()
        self.view.viewport().installEventFilter(self)
        # 🌟 不再 lower()，View 现在是主要显示层
        
        # 创建工具栏（按需创建）
        self.toolbar = None
        
        # OCR 文字层（初始为 None，异步初始化）
        self.ocr_text_layer = None
        self.ocr_thread = None
        
        # 🌟 新架构：创建独立阴影窗口
        self.shadow_window = None
        if self.halo_enabled:
            self.shadow_window = PinShadowWindow(self)
            # 同步阴影样式参数
            self.shadow_window.pad = self.pad
            self.shadow_window.corner = self.corner
            self.shadow_window.shadow_spread = self.shadow_spread
            self.shadow_window.shadow_max_alpha = self.shadow_max_alpha
            self.shadow_window.glow_enable = self.glow_enable
            self.shadow_window.glow_spread = self.glow_spread
            self.shadow_window.glow_color = self.glow_color
            self.shadow_window.glow_max_alpha = self.glow_max_alpha
            self.shadow_window.border_enable = self.border_enable
            self.shadow_window.border_color = self.border_color
            self.shadow_window.border_width = self.border_width
            # 同步位置
            self._sync_shadow_window()
            # 先显示阴影窗口
            self.shadow_window.show_shadow()
        
        # 显示窗口
        self.show()
        
        # 🔴 关键：确保按钮在 View 之上（View 创建后按钮被覆盖了）
        self.update_button_positions()
        
        # 延迟初始化 OCR 文字层（等窗口完全显示后再开始，避免卡顿）
        QTimer.singleShot(500, self._init_ocr_text_layer_async)
        
        log_info(f"创建成功: {image.width()}x{image.height()}, 位置: ({position.x()}, {position.y()})", "PinWindow")
        if self.drawing_items:
            log_debug(f"继承了 {len(self.drawing_items)} 个绘制项目（向量数据）", "PinWindow")
            log_debug(f"选区偏移: ({self.selection_offset.x()}, {self.selection_offset.y()})", "PinWindow")
    
    def setup_ui(self):
        """设置UI布局"""
        # 不再使用 QLabel，直接在 paintEvent 中绘制
        # 这样可以更好地控制渲染质量和内存使用
        
        # 创建控制按钮
        self.setup_control_buttons()
    
    def setup_control_buttons(self):
        """设置控制按钮（使用 PinControlButtons 管理器）"""
        # 创建控制按钮管理器
        self._control_buttons = PinControlButtons(self)
        
        # 创建属性别名（保持向后兼容）
        self.close_button = self._control_buttons.close_button
        self.toolbar_toggle_button = self._control_buttons.toolbar_toggle_button
        self.translate_button = self._control_buttons.translate_button
        
        # 连接信号
        self._control_buttons.connect_signals(
            close_handler=self.close_window,
            toggle_toolbar_handler=self.toggle_toolbar,
            translate_handler=self._on_translate_clicked
        )
        
        # 创建右键菜单管理器
        self._context_menu = PinContextMenu(self)
        
        # 创建翻译助手
        self._translation_helper = PinTranslationHelper(self, self.config_manager)
        
        # 更新按钮位置
        self.update_button_positions()
    
    def update_button_positions(self):
        """更新按钮位置（窗口缩放时调用）"""
        if hasattr(self, '_control_buttons'):
            self._control_buttons.update_positions(self.width())

    def _auto_toolbar_enabled(self) -> bool:
        """当前是否启用自动工具栏显示"""
        return self.config_manager.get_pin_auto_toolbar() if self.config_manager else True

    def _ensure_hover_controls_visible(self):
        """在鼠标悬停期间确保控制按钮与工具栏可见"""
        if not self.close_button.isVisible():
            self.close_button.show()
        # 🔴 确保按钮在 View 之上
        self.close_button.raise_()

        if not self._auto_toolbar_enabled() and not self.toolbar_toggle_button.isVisible():
            self.toolbar_toggle_button.show()
        self.toolbar_toggle_button.raise_()
        
        # 显示翻译按钮（如果 OCR 有结果）
        if hasattr(self, 'translate_button') and hasattr(self, '_ocr_has_result'):
            if self._ocr_has_result and not self.translate_button.isVisible():
                self.translate_button.show()
            self.translate_button.raise_()

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

    # ==================== 窗口拖动辅助 ====================

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
    
    def update_display(self):
        """更新图像显示（触发重绘）"""
        if hasattr(self, 'view') and self.view:
            self.view.viewport().update()
        else:
            self.update()

    def _update_view_transform(self):
        """根据窗口大小刷新 CanvasView 的缩放"""
        if not getattr(self, 'view', None) or not getattr(self, 'canvas', None):
            return
        scene_rect = self.canvas.scene.sceneRect()
        if scene_rect.width() == 0 or scene_rect.height() == 0:
            return
        self.view.resetTransform()
        # 🌟 基于content_rect计算缩放
        cr = self.content_rect()
        scale_x = cr.width() / scene_rect.width()
        scale_y = cr.height() / scene_rect.height()
        self.view.scale(scale_x, scale_y)

    def resizeEvent(self, event):
        if hasattr(self, 'view') and self.view:
            # 🌟 新架构：View 覆盖整个窗口
            self.view.setGeometry(0, 0, self.width(), self.height())
            self._update_view_transform()
            # 更新圆角遮罩
            self.view._update_viewport_mask()
        self.update_button_positions()
        if self.toolbar and self.toolbar.isVisible():
            self.toolbar.sync_with_pin_window()
        
        # 同步 OCR 文字层大小和位置（覆盖整个窗口）
        if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
            self.ocr_text_layer.setGeometry(self.rect())
        # 🌟 同步阴影窗口位置
        self._sync_shadow_window()
        
        super().resizeEvent(event)
    
    def moveEvent(self, event):
        """窗口移动事件 - 同步阴影窗口位置"""
        super().moveEvent(event)
        self._sync_shadow_window()
    
    def _sync_shadow_window(self):
        """同步阴影窗口的位置和大小"""
        if hasattr(self, 'shadow_window') and self.shadow_window:
            self.shadow_window.sync_geometry(self.geometry())
    
    # ==================== 🌟 光晕/阴影效果 ====================
    
    def content_rect(self) -> QRectF:
        """
        内容区域（显示截图的区域）
        
        🌟 新架构：内容窗口就是整个窗口，不再有 padding
        """
        return QRectF(self.rect())
    
    def _rounded_path(self, rect: QRectF, radius: float) -> QPainterPath:
        """创建圆角矩形路径"""
        path = QPainterPath()
        path.addRoundedRect(rect, radius, radius)
        return path
    
    def _ensure_shadow_cache(self):
        """确保阴影缓存是最新的"""
        if not self.halo_enabled:
            return
        
        dpr = float(self.devicePixelRatioF())
        key = (
            self.width(), self.height(), round(dpr, 6),
            self.pad, self.corner, self.shadow_spread, self.shadow_max_alpha,
            self.glow_enable, self.glow_spread, self.glow_max_alpha,
            self.glow_color.rgba(), self.border_enable, 
            self.border_color.rgba(), self.border_width
        )
        
        if self._shadow_cache is not None and self._shadow_key == key:
            return  # 缓存有效
        
        self._shadow_key = key
        self._shadow_cache = self._build_shadow_pixmap()
    
    def _build_shadow_pixmap(self) -> QPixmap:
        """构建阴影/光晕缓存 - 使用多层叠加近似高斯模糊"""
        dpr = float(self.devicePixelRatioF())
        w = max(1, self.width())
        h = max(1, self.height())
        phys_w = max(1, int(w * dpr))
        phys_h = max(1, int(h * dpr))
        
        img = QImage(phys_w, phys_h, QImage.Format.Format_ARGB32_Premultiplied)
        img.fill(Qt.GlobalColor.transparent)
        img.setDevicePixelRatio(dpr)
        
        p = QPainter(img)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        
        cr = self.content_rect()
        
        # 1) 阴影层（黑色柔和渐变）
        for i in range(self.shadow_spread, 0, -1):
            t = i / self.shadow_spread  # 1.0 → 0.0
            # 二次方衰减曲线：外层淡，内层深
            alpha = int(self.shadow_max_alpha * (1.0 - t) ** 2)
            if alpha <= 0:
                continue
            
            rect = cr.adjusted(-i, -i, i, i)
            radius = self.corner + i
            color = QColor(0, 0, 0, alpha)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(color)
            p.drawPath(self._rounded_path(rect, radius))
        
        # 2) 外发光层（白色/彩色光晕）
        if self.glow_enable:
            for i in range(self.glow_spread, 0, -1):
                t = i / self.glow_spread
                alpha = int(self.glow_max_alpha * (1.0 - t) ** 2)
                if alpha <= 0:
                    continue
                
                rect = cr.adjusted(-i, -i, i, i)
                radius = self.corner + i
                c = QColor(self.glow_color)
                c.setAlpha(alpha)
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(c)
                p.drawPath(self._rounded_path(rect, radius))
        
        # 3) 描边（让边缘清晰）
        if self.border_enable:
            pen = QPen(self.border_color)
            pen.setWidthF(self.border_width)
            p.setPen(pen)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawPath(self._rounded_path(cr, self.corner))
        
        p.end()
        return QPixmap.fromImage(img)
    
    def paintEvent(self, event):
        """
        绘制事件
        
        🌟 新架构：PinWindow 不再负责内容渲染！
        - 阴影由独立的 ShadowWindow 负责
        - 内容由 PinCanvasView（QGraphicsView）负责
        - 这里什么都不画，让 View 自己渲染
        """
        # 🌟 不再调用 scene.render()！
        # View 是唯一的内容渲染者
        pass
    
    # ==================== 鼠标事件 ====================
    # 让鼠标事件传递给 PinCanvasView，由它的智能编辑系统处理
    # 窗口层面不拦截，只在非编辑状态处理窗口拖动
    
    def mousePressEvent(self, event: QMouseEvent):
        """鼠标按下 - 非编辑状态拖动窗口，编辑状态传递给 view"""
        self._set_hover_state(True)
        
        # 非编辑模式：拖动窗口
        if event.button() == Qt.MouseButton.LeftButton and not (self.canvas and self.canvas.is_editing):
            self.start_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        
        # 编辑模式或其他按钮：传递给子控件（view 会处理）
        super().mousePressEvent(event)
    
    def mouseMoveEvent(self, event: QMouseEvent):
        """鼠标移动 - 拖动窗口或传递给 view"""
        self._set_hover_state(True)

        # 拖动模式
        if self._is_dragging:
            self.update_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        
        # 其他情况传递给子控件
        super().mouseMoveEvent(event)
    
    def mouseReleaseEvent(self, event: QMouseEvent):
        """鼠标释放 - 结束拖动或传递给 view"""
        if event.button() == Qt.MouseButton.LeftButton and self._is_dragging:
            # 结束拖动
            self.end_window_drag()
            event.accept()
            return
        elif event.button() == Qt.MouseButton.RightButton:
            # 显示右键菜单
            self.show_context_menu(event.globalPosition().toPoint())
            event.accept()
            return
        
        # 其他情况传递给子控件
        super().mouseReleaseEvent(event)
    
    def wheelEvent(self, event: QWheelEvent):
        """
        滚轮缩放窗口大小 - 优化版本
        
        优化点:
        1. 标记缩放状态，使用快速变换
        2. 延迟触发高质量渲染（150ms 后）
        3. 不创建临时 pixmap，内存稳定
        """
        # 标记正在缩放（使用快速变换）
        self._is_scaling = True
        
        # 获取滚轮方向
        delta = event.angleDelta().y()
        
        # 计算缩放比例（每次5%）
        scale_factor = 1.05 if delta > 0 else 0.95
        
        # 计算新尺寸
        new_width = int(self.width() * scale_factor)
        new_height = int(self.height() * scale_factor)
        
        # 限制最小尺寸（50x50）和最大尺寸（4倍原始大小）
        min_size = 50
        max_width = self._orig_size.width() * 4
        max_height = self._orig_size.height() * 4
        
        new_width = max(min_size, min(new_width, max_width))
        new_height = max(min_size, min(new_height, max_height))
        
        # 计算鼠标位置相对窗口的比例
        mouse_pos = event.position()
        ratio_x = mouse_pos.x() / self.width()
        ratio_y = mouse_pos.y() / self.height()
        
        # 计算新位置（保持鼠标位置在窗口中的相对位置不变）
        old_width = self.width()
        old_height = self.height()
        new_x = self.x() + int((old_width - new_width) * ratio_x)
        new_y = self.y() + int((old_height - new_height) * ratio_y)
        
        # 应用新尺寸和位置
        self.setGeometry(new_x, new_y, new_width, new_height)
        
        # 使画布缓存失效（窗口尺寸变化）
        if self.canvas:
            self.canvas.invalidate_cache()
        
        # 更新按钮位置
        self.update_button_positions()
        
        # 触发快速重绘（FastTransformation）
        self.update()
        self._update_view_transform()
        
        # 同步工具栏位置
        if self.toolbar and self.toolbar.isVisible():
            self.toolbar.sync_with_pin_window()
        
        # 重启延迟定时器（150ms 后触发高质量渲染）
        self._scale_timer.start()
        
        log_debug(f"缩放: {old_width}x{old_height} → {new_width}x{new_height}", "PinWindow")
    
    def _apply_smooth_scaling(self):
        """
        应用平滑缩放（延迟触发）
        
        当用户停止滚轮缩放 150ms 后，使用高质量变换重新渲染
        """
        self._is_scaling = False
        self.update()  # 触发 paintEvent，使用 SmoothTransformation
        self._update_view_transform()
        log_debug("应用高质量渲染", "PinWindow")
    
    def enterEvent(self, event):
        """鼠标进入窗口 - 显示控制按钮"""
        super().enterEvent(event)
        self._set_hover_state(True)
    
    def leaveEvent(self, event):
        """鼠标离开窗口 - 隐藏控制按钮"""
        super().leaveEvent(event)
        self._set_hover_state(False)
    
    def _delayed_hide_buttons(self):
        """延迟隐藏按钮"""
        # 检查鼠标是否还在窗口内
        if self._last_hover_state:
            return
        if not self.underMouse():
            self.close_button.hide()
            self.toolbar_toggle_button.hide()
            # 翻译按钮也跟随隐藏
            if hasattr(self, 'translate_button'):
                self.translate_button.hide()
    
    def keyPressEvent(self, event: QKeyEvent):
        """键盘事件 - ESC 关闭窗口"""
        if event.key() == Qt.Key.Key_Escape:
            self.close_window()
        else:
            super().keyPressEvent(event)
    
    # ==================== 工具栏管理 ====================
    
    def show_toolbar(self):
        """显示工具栏"""
        if not self.toolbar:
            # 延迟导入，避免循环依赖
            from pin.pin_toolbar import PinToolbar
            
            # 传递 config_manager，确保工具设置能够保存和读取
            self.toolbar = PinToolbar(parent_pin_window=self, config_manager=self.config_manager)
            
            # 连接信号到画布
            if self.canvas:
                # 工具切换
                self.toolbar.tool_changed.connect(self._on_tool_changed)
                
                # 撤销/重做（连接到 CanvasScene 的 undo_stack）
                self.toolbar.undo_clicked.connect(self.canvas.undo_stack.undo)
                self.toolbar.redo_clicked.connect(self.canvas.undo_stack.redo)
                
                # 样式改变（连接到 tool_controller）
                self.toolbar.color_changed.connect(self._on_color_changed)
                self.toolbar.stroke_width_changed.connect(self._on_stroke_width_changed)
                self.toolbar.opacity_changed.connect(self._on_opacity_changed)

            # 文字工具的高级样式需要直接作用于 SmartEditController
            controller = getattr(self, "view", None)
            controller = getattr(controller, "smart_edit_controller", None)
            if controller:
                self.toolbar.text_font_changed.connect(controller.on_text_font_changed)
                self.toolbar.text_outline_changed.connect(controller.on_text_outline_changed)
                self.toolbar.text_shadow_changed.connect(controller.on_text_shadow_changed)
                self.toolbar.text_background_changed.connect(controller.on_text_background_changed)
                self.toolbar.color_changed.connect(controller.on_text_color_changed)
            
            # 保存/复制
            self.toolbar.save_clicked.connect(self.save_image)
            self.toolbar.copy_clicked.connect(self.copy_to_clipboard)
            
            log_debug("创建工具栏，连接完整信号", "PinWindow")
            
            # 打印撤销栈状态（调试用）
            if self.canvas:
                self.canvas.undo_stack.print_stack_status()
        
        # 每次显示时都检查并应用自动隐藏设置
        auto_toolbar = self.config_manager.get_pin_auto_toolbar() if self.config_manager else True
        if auto_toolbar:
            self.toolbar.enable_auto_hide(True)
            self.toolbar.set_auto_hide_delay(2000)  # 2秒后自动隐藏
        else:
            self.toolbar.enable_auto_hide(False)
        
        self.toolbar.show()
    
    def hide_toolbar(self):
        """隐藏工具栏"""
        if self.toolbar:
            self.toolbar.hide()
    
    def toggle_toolbar(self):
        """切换工具栏显示/隐藏"""
        if self.toolbar and self.toolbar.isVisible():
            self.hide_toolbar()
        else:
            self.show_toolbar()
    
    # ==================== 翻译功能 ====================
    
    def _on_translate_clicked(self):
        """翻译按钮点击处理（委托给翻译助手）"""
        if hasattr(self, '_translation_helper') and hasattr(self, 'ocr_text_layer'):
            self._translation_helper.translate(self.ocr_text_layer)
    
    # ==================== 右键菜单 ====================
    
    def show_context_menu(self, global_pos: QPoint):
        """
        显示右键菜单（委托给菜单管理器）
        
        Args:
            global_pos: 全局坐标位置
        """
        if hasattr(self, '_context_menu'):
            state = {
                'toolbar_visible': self.toolbar and self.toolbar.isVisible(),
                'stay_on_top': bool(self.windowFlags() & Qt.WindowType.WindowStaysOnTopHint),
                'shadow_enabled': self.halo_enabled,
                'has_ocr_result': hasattr(self, '_ocr_has_result') and self._ocr_has_result
            }
            self._context_menu.show(global_pos, state)
    
    def toggle_stay_on_top(self):
        """切换窗口置顶状态"""
        current_flags = self.windowFlags()
        
        if current_flags & Qt.WindowType.WindowStaysOnTopHint:
            # 取消置顶
            new_flags = current_flags & ~Qt.WindowType.WindowStaysOnTopHint
            log_debug("取消置顶", "PinWindow")
        else:
            # 设置置顶
            new_flags = current_flags | Qt.WindowType.WindowStaysOnTopHint
            log_debug("设置置顶", "PinWindow")
        
        # 保存当前位置和大小
        geometry = self.geometry()
        
        # 应用新的窗口标志
        self.setWindowFlags(new_flags)
        
        # 恢复位置和大小
        self.setGeometry(geometry)
        
        # 重新显示窗口
        self.show()
    
    def toggle_shadow_effect(self):
        """
        切换阴影/光晕效果
        
        🌟 新架构：阴影由独立窗口负责，这里只控制显示/隐藏
        """
        self.halo_enabled = not self.halo_enabled
        
        if self.halo_enabled:
            log_debug("启用阴影效果", "PinWindow")
            # 创建或显示阴影窗口
            if not hasattr(self, 'shadow_window') or not self.shadow_window:
                from pin.pin_shadow_window import PinShadowWindow
                self.shadow_window = PinShadowWindow(self)
                # 🔴 同步所有阴影样式参数
                self.shadow_window.pad = self.pad
                self.shadow_window.corner = self.corner
                self.shadow_window.shadow_spread = self.shadow_spread
                self.shadow_window.shadow_max_alpha = self.shadow_max_alpha
                self.shadow_window.glow_enable = self.glow_enable
                self.shadow_window.glow_spread = self.glow_spread
                self.shadow_window.glow_color = self.glow_color
                self.shadow_window.glow_max_alpha = self.glow_max_alpha
                self.shadow_window.border_enable = self.border_enable
                self.shadow_window.border_color = self.border_color
                self.shadow_window.border_width = self.border_width
            # 🔴 先同步位置，再显示
            self._sync_shadow_window()
            self.shadow_window.show_shadow()
            # 更新 View 圆角
            if hasattr(self, 'view') and self.view:
                self.view.set_corner_radius(self.corner)
        else:
            log_debug("禁用阴影效果", "PinWindow")
            # 隐藏阴影窗口
            if hasattr(self, 'shadow_window') and self.shadow_window:
                self.shadow_window.hide_shadow()
            # 移除 View 圆角
            if hasattr(self, 'view') and self.view:
                self.view.set_corner_radius(0)
        
        self.update_button_positions()
    
    def _on_tool_changed(self, tool_name: str):
        """
        工具切换事件处理
        
        Args:
            tool_name: 工具名称（pen, rect, arrow, text, 等）或 "cursor" 表示取消工具
        """
        if not self.canvas:
            return
        
        # cursor 表示取消工具，退出编辑模式
        if tool_name and tool_name != "cursor":
            # 激活工具 → 进入编辑模式
            self.canvas.activate_tool(tool_name)
            log_debug(f"激活工具: {tool_name}，进入编辑模式", "PinWindow")
            
            # 通知 OCR 层：工具激活，隐藏文字选择层
            if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
                self.ocr_text_layer.set_drawing_mode(True)
            
            # 同步 UI：工具激活后，其设置已从 config_manager 加载到 ToolContext
            # 需要同步到工具栏 UI（更新滑块、颜色显示）
            if self.toolbar and hasattr(self.canvas, 'tool_controller'):
                ctx = self.canvas.tool_controller.ctx
                
                # 临时断开信号，避免循环触发
                try:
                    self.toolbar.color_changed.disconnect(self._on_color_changed)
                    self.toolbar.stroke_width_changed.disconnect(self._on_stroke_width_changed)
                    self.toolbar.opacity_changed.disconnect(self._on_opacity_changed)
                except Exception as e:
                    log_exception(e, "钉图工具切换时断开信号")
                
                try:
                    # 更新工具栏 UI 显示当前工具的设置
                    self.toolbar.set_current_color(ctx.color)
                    self.toolbar.set_stroke_width(ctx.stroke_width)
                    self.toolbar.set_opacity(int(ctx.opacity * 255))
                    
                    log_debug(f"UI同步: 工具={tool_name}, 颜色={ctx.color.name()}, 宽度={ctx.stroke_width}, 透明度={ctx.opacity}", "PinWindow")
                finally:
                    # 重新连接信号
                    self.toolbar.color_changed.connect(self._on_color_changed)
                    self.toolbar.stroke_width_changed.connect(self._on_stroke_width_changed)
                    self.toolbar.opacity_changed.connect(self._on_opacity_changed)
            
            # 切换工具后，将焦点还给 View（确保快捷键可用）
            from PyQt6.QtCore import QTimer
            if hasattr(self.canvas, 'view'):
                QTimer.singleShot(0, self.canvas.view.setFocus)
        else:
            # 取消工具 → 退出编辑模式
            self.canvas.deactivate_tool()
            log_debug("取消工具，退出编辑模式", "PinWindow")
            
            # 通知 OCR 层：工具取消，重新显示文字选择层
            if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
                self.ocr_text_layer.set_drawing_mode(False)
    
    def _on_color_changed(self, color):
        """颜色改变事件"""
        if self.canvas:
            self.canvas.set_color(color)
    
    def _on_stroke_width_changed(self, width):
        """线宽改变事件"""
        if not self.canvas:
            return
        ctx = getattr(self.canvas, 'tool_controller', None)
        ctx = getattr(ctx, 'context', None) if ctx else None
        prev_width = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)
        log_debug(f"slider width change -> prev={prev_width}, target={width}", "PinWindow")
        self.canvas.set_stroke_width(width)
        new_width = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)
        self._apply_selection_width_scale(prev_width, new_width)
    
    def _on_opacity_changed(self, opacity_int):
        """透明度改变事件（0-255）"""
        if not self.canvas:
            return
        opacity = float(opacity_int) / 255.0
        log_debug(f"slider opacity change -> target={opacity:.3f}", "PinWindow")
        self.canvas.set_opacity(opacity)
        self._apply_selection_opacity(opacity)

    def _apply_selection_width_scale(self, prev_width: float, new_width: float):
        if prev_width <= 0 or new_width <= 0:
            log_debug(f"skip width scaling: prev={prev_width}, new={new_width}", "PinWindow")
            return
        if abs(new_width - prev_width) <= 1e-6:
            log_debug(f"width unchanged (prev={prev_width}, new={new_width})", "PinWindow")
            return
        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_size_change_to_selection'):
            scale = new_width / prev_width
            log_debug(f"applying selection scale via view: scale={scale:.3f}", "PinWindow")
            view._apply_size_change_to_selection(scale)
        else:
            log_warning(f"missing view for selection scaling: view={view}", "PinWindow")

    def _apply_selection_opacity(self, opacity: float):
        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_opacity_change_to_selection'):
            view._apply_opacity_change_to_selection(opacity)
        else:
            log_debug(f"skip opacity sync: missing view helper (view={view})", "PinWindow")
    
    # ==================== 窗口管理 ====================
    
    def close_window(self):
        """关闭窗口"""
        if self._is_closed:
            return
        
        log_info("开始关闭", "PinWindow")
        self._is_closed = True
        
        # 清理资源
        self.cleanup()
        
        # 发送关闭信号
        self.closed.emit()
        
        # 关闭窗口
        self.close()
    
    def cleanup(self):
        """清理资源"""
        log_debug("清理资源...", "PinWindow")
        
        # 0. 停止所有定时器
        if hasattr(self, '_scale_timer') and self._scale_timer:
            self._scale_timer.stop()
            self._scale_timer.deleteLater()
            self._scale_timer = None
        if hasattr(self, '_hover_monitor') and self._hover_monitor:
            self._hover_monitor.stop()
            self._hover_monitor.deleteLater()
            self._hover_monitor = None
        
        # 🌟 关闭阴影窗口
        if hasattr(self, 'shadow_window') and self.shadow_window:
            self.shadow_window.close_shadow()
            self.shadow_window = None
        
        # 翻译窗口现在由 TranslationManager 单例管理，无需在此清理
        # 翻译窗口是全局共享的，关闭钉图不会关闭翻译窗口
        
        # 1. 关闭工具栏
        if self.toolbar:
            # 关闭二级菜单（如果存在）
            if hasattr(self.toolbar, 'paint_menu') and self.toolbar.paint_menu:
                self.toolbar.paint_menu.close()
                self.toolbar.paint_menu.deleteLater()
                self.toolbar.paint_menu = None
            
            self.toolbar.close()
            self.toolbar.deleteLater()
            self.toolbar = None
        
        # 1.5. 清理 OCR 资源
        if hasattr(self, 'ocr_thread') and self.ocr_thread is not None:
            if self.ocr_thread.isRunning():
                log_warning("窗口关闭，OCR 线程仍在运行，将其分离以在后台完成...", "OCR")
                try:
                    self.ocr_thread.finished.disconnect()
                except Exception as e:
                    log_exception(e, "断开OCR线程finished信号")
                
                # 重设父对象，防止随窗口销毁
                self.ocr_thread.setParent(None)
                
                # 线程完成后自动清理（不阻塞窗口关闭）
                self.ocr_thread.finished.connect(self.ocr_thread.deleteLater)
            else:
                # 线程已完成，立即清理
                self.ocr_thread.deleteLater()
            self.ocr_thread = None
        
        if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
            self.ocr_text_layer.set_enabled(False)
            # 调用清理方法停止定时器
            if hasattr(self.ocr_text_layer, 'cleanup'):
                self.ocr_text_layer.cleanup()
            self.ocr_text_layer.deleteLater()
            self.ocr_text_layer = None
        
        # 不再释放 OCR 引擎 - 保持常驻内存，避免下次钉图时重新初始化导致卡顿
        # OCR 引擎约占用 50-100MB 内存，但保持加载可以让钉图更流畅
        
        # 2. 清理视图
        if hasattr(self, 'view') and self.view:
            if hasattr(self.view, 'viewport'):
                try:
                    self.view.viewport().removeEventFilter(self)
                except Exception as e:
                    log_exception(e, "移除视图事件过滤器")
            # 暂时不清理scene引用，让canvas负责清理
            self.view.deleteLater()
            self.view = None
        
        # 3. 清理画布（会自动清理scene）
        if self.canvas:
            try:
                self.canvas.cleanup()  # 这个方法内部会清理scene
            except Exception as e:
                log_warning(f"画布清理时出错: {e}", "PinWindow")
            self.canvas = None
        
        # 4. 清理图像数据
        self._base_pixmap = None
        self.vector_commands = None
        
        # 5. 不强制GC，让Python自动管理（避免阻塞UI）
        # gc.collect() 可能导致卡顿，尤其是有大量QGraphicsItem时
        
        log_info("资源清理完成", "PinWindow")
    
    def _init_ocr_text_layer_async(self):
        """异步初始化 OCR 文字选择层（不阻塞主线程）"""
        try:
            from PyQt6.QtCore import QThread
            from ocr import is_ocr_available, initialize_ocr, recognize_text
            from pin import OCRTextLayer
            
            # 1. 检查 OCR 是否启用（从配置读取）
            if not self.config_manager:
                return
            
            ocr_enabled = self.config_manager.get_ocr_enabled()
            if not ocr_enabled:
                log_info("OCR 功能已禁用，跳过初始化", "OCR")
                return
            
            # 2. 检查 OCR 是否可用
            if not is_ocr_available():
                log_debug("OCR 模块不可用（无OCR版本），静默跳过", "OCR")
                return
            
            # 3. 初始化 OCR 引擎
            if not initialize_ocr():
                log_warning("OCR 引擎初始化失败", "OCR")
                return
            
            log_info("OCR 引擎已就绪（支持中日韩英混合识别）", "OCR")
            
            # 4. 创建透明文字层（覆盖内容区域，不包括边框）
            self.ocr_text_layer = OCRTextLayer(self)
            cr = self.content_rect()
            self.ocr_text_layer.setGeometry(cr.toRect())
            
            # 5. 启用文字层
            self.ocr_text_layer.set_enabled(True)
            
            # 6. 创建异步 OCR 识别线程
            class OCRThread(QThread):
                def __init__(self, pixmap, config_manager, parent=None):
                    super().__init__(parent)
                    self.pixmap = pixmap
                    self.config_manager = config_manager
                    self.result = None
                
                def run(self):
                    try:
                        # 调用 OCR 识别（已移除预处理功能）
                        self.result = recognize_text(
                            self.pixmap, 
                            return_format="dict"
                        )
                    except Exception as e:
                        log_error(f"识别失败: {e}", "OCR")
                        import traceback
                        traceback.print_exc()
                        self.result = None
            
            # 8. 获取钉图图像（包含所有图层）
            pixmap = QPixmap.fromImage(self.get_current_image())
            original_width = pixmap.width()
            original_height = pixmap.height()
            
            # 9. 启动异步识别
            log_debug("开始异步识别文字...", "OCR")
            self.ocr_thread = OCRThread(pixmap, self.config_manager, self)
            
            def on_ocr_finished():
                try:
                    # 检查窗口是否已关闭
                    if self._is_closed:
                        log_debug("窗口已关闭，跳过结果加载", "OCR")
                        return
                    
                    # 检查 OCR 文字层是否还存在
                    if not hasattr(self, 'ocr_text_layer') or self.ocr_text_layer is None:
                        log_debug("OCR 文字层已被清理，跳过结果加载", "OCR")
                        return
                    
                    # 检查线程是否还存在
                    if not hasattr(self, 'ocr_thread') or self.ocr_thread is None:
                        log_debug("OCR 线程已被清理，跳过结果加载", "OCR")
                        return
                    
                    # 检查结果是否有效
                    if self.ocr_thread.result and isinstance(self.ocr_thread.result, dict):
                        if self.ocr_thread.result.get('code') == 100:
                            # 加载 OCR 结果到文字层
                            self.ocr_text_layer.load_ocr_result(
                                self.ocr_thread.result, 
                                original_width, 
                                original_height
                            )
                            
                            # 标记 OCR 有结果，用于显示翻译按钮
                            text_count = len(self.ocr_thread.result.get('data', []))
                            if text_count > 0:
                                self._ocr_has_result = True
                            
                            log_info(f"钉图文字层已就绪，识别到 {text_count} 个文字块", "OCR")
                except Exception as e:
                    log_error(f"加载结果失败: {e}", "OCR")
                    import traceback
                    traceback.print_exc()
                finally:
                    # 清理线程
                    if hasattr(self, 'ocr_thread') and self.ocr_thread:
                        self.ocr_thread.deleteLater()
                        self.ocr_thread = None
            
            self.ocr_thread.finished.connect(on_ocr_finished)
            self.ocr_thread.start()
            
        except ImportError:
            # OCR 模块不存在（无OCR版本），静默跳过
            pass
        except Exception as e:
            log_exception(e, "OCR初始化", silent=False)
            import traceback
            traceback.print_exc()
    
    def closeEvent(self, event):
        """窗口关闭事件"""
        if not self._is_closed:
            self._is_closed = True
            self.cleanup()
            self.closed.emit()
        
        super().closeEvent(event)
        log_debug("窗口已销毁", "PinWindow")
    
    # ==================== 辅助方法 ====================
    
    def get_current_image(self) -> QImage:
        """
        获取当前图像（背景+所有图层）
        
        Returns:
            QImage: 当前渲染的图像（HiDPI 清晰版）
        
        新架构：直接使用 canvas.get_current_image()
        """
        # 获取窗口所在屏幕的 DPR
        dpr = self.devicePixelRatioF()
        
        if self.canvas:
            # 直接从画布导出（包含所有图层）
            return self.canvas.get_current_image(dpr)
        else:
            # 如果没有画布，返回底图
            result_image = QImage(
                int(self.width() * dpr),
                int(self.height() * dpr),
                QImage.Format.Format_ARGB32_Premultiplied
            )
            result_image.fill(Qt.GlobalColor.transparent)
            result_image.setDevicePixelRatio(dpr)
            
            painter = QPainter(result_image)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
            painter.drawPixmap(self.rect(), self._base_pixmap)
            painter.end()
            
            return result_image
    
    def save_image(self):
        """保存图像到文件"""
        from PyQt6.QtWidgets import QFileDialog
        
        file_path, _ = QFileDialog.getSaveFileName(
            self,
            "保存钉图",
            "pinned_image.png",
            "Images (*.png *.jpg *.bmp)"
        )
        
        if file_path:
            # 临时退出编辑模式，隐藏选择框和手柄
            was_editing = self.canvas and self.canvas.is_editing
            active_tool_id = None
            if was_editing and hasattr(self.canvas, 'tool_controller'):
                current_tool = self.canvas.tool_controller.current_tool
                if current_tool:
                    active_tool_id = current_tool.id
                self.canvas.deactivate_tool()
            
            # 获取并保存图像
            image = self.get_current_image()
            success = image.save(file_path)
            
            # 恢复编辑模式
            if was_editing and active_tool_id:
                self.canvas.activate_tool(active_tool_id)
            
            if success:
                log_info(f"保存成功: {file_path}", "PinWindow")
            else:
                log_error(f"保存失败: {file_path}", "PinWindow")
    
    def copy_to_clipboard(self):
        """复制图像到剪贴板"""
        # 临时退出编辑模式，隐藏选择框和手柄
        was_editing = self.canvas and self.canvas.is_editing
        active_tool_id = None
        if was_editing and hasattr(self.canvas, 'tool_controller'):
            current_tool = self.canvas.tool_controller.current_tool
            if current_tool:
                active_tool_id = current_tool.id
            self.canvas.deactivate_tool()
        
        # 获取并复制图像
        image = self.get_current_image()
        pixmap = QPixmap.fromImage(image)
        QApplication.clipboard().setPixmap(pixmap)
        
        # 恢复编辑模式
        if was_editing and active_tool_id:
            self.canvas.activate_tool(active_tool_id)
        
        log_info("已复制到剪贴板", "PinWindow")

    def eventFilter(self, obj, event):
        if self.view and obj == self.view.viewport():
            if event.type() in (
                QEvent.Type.Enter,
                QEvent.Type.HoverEnter,
                QEvent.Type.MouseMove,
            ):
                self._set_hover_state(True)
            elif event.type() in (
                QEvent.Type.Leave,
                QEvent.Type.HoverLeave,
            ):
                self._set_hover_state(False)
        return super().eventFilter(obj, event)


# 测试代码
if __name__ == "__main__":
    import sys
    
    app = QApplication(sys.argv)
    
    # 创建测试图像
    test_image = QImage(400, 300, QImage.Format.Format_ARGB32)
    test_image.fill(Qt.GlobalColor.lightGray)
    
    # 在图像上绘制一些内容
    painter = QPainter(test_image)
    painter.setPen(Qt.GlobalColor.red)
    painter.setFont(painter.font())
    font = painter.font()
    font.setPixelSize(30)
    painter.setFont(font)
    painter.drawText(test_image.rect(), Qt.AlignmentFlag.AlignCenter, "测试钉图窗口\n拖动移动\n滚轮缩放")
    painter.end()
    
    # 创建钉图窗口
    from settings import get_tool_settings_manager
    config_manager = get_tool_settings_manager()
    
    pin_window = PinWindow(test_image, QPoint(100, 100), config_manager)
    
    sys.exit(app.exec())
