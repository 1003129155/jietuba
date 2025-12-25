"""
钉图窗口 - 核心窗口类
"""

from PyQt6.QtWidgets import QWidget, QLabel, QPushButton, QVBoxLayout, QApplication, QMenu
from PyQt6.QtCore import Qt, QPoint, QPointF, QSize, QTimer, pyqtSignal, QRect, QRectF, QEvent
from PyQt6.QtGui import QPixmap, QImage, QPainter, QMouseEvent, QWheelEvent, QKeyEvent, QPaintEvent, QColor, QPainterPath, QPen, QAction
from pin.pin_canvas_view import PinCanvasView


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
        
        # 🔥 阴影缓存
        self._shadow_cache: QPixmap | None = None
        self._shadow_key = None
        
        # 窗口状态
        self._is_closed = False
        self._is_dragging = False
        self._is_editing = False
        self._drag_start_pos = QPoint()
        self._drag_start_window_pos = QPoint()
        self._last_hover_state = False
        
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
        
        # 设置初始大小和位置（加上padding用于阴影）
        padding = self.pad * 2 if self.halo_enabled else 0
        self.setGeometry(
            position.x() - (self.pad if self.halo_enabled else 0),
            position.y() - (self.pad if self.halo_enabled else 0),
            image.width() + padding,
            image.height() + padding
        )
        
        # 创建UI组件
        self.setup_ui()
        
        # 创建画布（传入背景图像）
        from pin.pin_canvas import PinCanvas
        # 🔥 传递基准坐标系（原始图像尺寸）和背景图像
        self.canvas = PinCanvas(self, self._orig_size, image)
        
        # 🔥 继承绘制项目（如果有）
        if self.drawing_items:
            self.canvas.initialize_from_items(self.drawing_items, self.selection_offset)

        # 🔥 创建 CanvasView（与截图窗口复用同一套交互/光标体系）
        self.view = PinCanvasView(self.canvas.scene, self, self.canvas)
        self.view.setParent(self)
        # 🔥 让 view 覆盖整个窗口（包括 padding 区域），这样所有鼠标事件都会先到 view
        self.view.setGeometry(0, 0, self.width(), self.height())
        self.view.lower()  # 确保按钮位于视图之上
        self._update_view_transform()
        self.view.viewport().installEventFilter(self)
        
        # 创建工具栏（按需创建）
        self.toolbar = None
        
        # OCR 文字层（初始为 None，异步初始化）
        self.ocr_text_layer = None
        self.ocr_thread = None
        
        # 显示窗口
        self.show()
        
        # 🔥 延迟初始化 OCR 文字层（等窗口完全显示后再开始，避免卡顿）
        # 500ms 延迟确保窗口动画流畅
        QTimer.singleShot(500, self._init_ocr_text_layer_async)
        
        print(f"📌 [钉图窗口] 创建成功: {image.width()}x{image.height()}, 位置: ({position.x()}, {position.y()})")
        if self.drawing_items:
            print(f"📌 [钉图窗口] 继承了 {len(self.drawing_items)} 个绘制项目（向量数据）")
            print(f"📌 [钉图窗口] 选区偏移: ({self.selection_offset.x()}, {self.selection_offset.y()})")
    
    def setup_ui(self):
        """设置UI布局"""
        # 不再使用 QLabel，直接在 paintEvent 中绘制
        # 这样可以更好地控制渲染质量和内存使用
        
        # 创建控制按钮
        self.setup_control_buttons()
    
    def setup_control_buttons(self):
        """设置控制按钮（关闭按钮 + 工具栏切换按钮）"""
        button_size = 20
        margin = 5
        spacing = 5
        
        # 1. 关闭按钮（右上角，总是显示）
        self.close_button = QPushButton('×', self)
        self.close_button.setFixedSize(button_size, button_size)
        self.close_button.setStyleSheet("""
            QPushButton {
                background-color: rgba(255, 0, 0, 180);
                color: white;
                border: none;
                border-radius: 10px;
                font-size: 14px;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: rgba(255, 0, 0, 220);
            }
            QPushButton:pressed {
                background-color: rgba(200, 0, 0, 220);
            }
        """)
        self.close_button.setToolTip("閉じる (ESC)")
        self.close_button.clicked.connect(self.close_window)
        self.close_button.hide()  # 初始隐藏，鼠标悬停显示
        
        # 2. 工具栏切换按钮（关闭按钮左边，仅在禁用自动工具栏时显示）
        self.toolbar_toggle_button = QPushButton('🔧', self)
        self.toolbar_toggle_button.setFixedSize(button_size, button_size)
        self.toolbar_toggle_button.setStyleSheet("""
            QPushButton {
                background-color: rgba(52, 152, 219, 180);
                color: white;
                border: none;
                border-radius: 10px;
                font-size: 12px;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: rgba(52, 152, 219, 220);
            }
            QPushButton:pressed {
                background-color: rgba(41, 128, 185, 220);
            }
        """)
        self.toolbar_toggle_button.setToolTip("ツールバーを表示")
        self.toolbar_toggle_button.clicked.connect(self.toggle_toolbar)
        self.toolbar_toggle_button.hide()  # 初始隐藏
        
        # 更新按钮位置
        self.update_button_positions()
    
    def update_button_positions(self):
        """更新按钮位置（窗口缩放时调用）"""
        button_size = 20
        margin = 5
        spacing = 5
        
        # 关闭按钮在右上角
        close_x = self.width() - button_size - margin
        close_y = margin
        self.close_button.move(close_x, close_y)
        self.close_button.raise_()
        
        # 工具栏切换按钮在关闭按钮左边
        toolbar_x = close_x - button_size - spacing
        toolbar_y = margin
        self.toolbar_toggle_button.move(toolbar_x, toolbar_y)
        self.toolbar_toggle_button.raise_()

    def _auto_toolbar_enabled(self) -> bool:
        """当前是否启用自动工具栏显示"""
        return self.config_manager.get_pin_auto_toolbar() if self.config_manager else True

    def _ensure_hover_controls_visible(self):
        """在鼠标悬停期间确保控制按钮与工具栏可见"""
        if not self.close_button.isVisible():
            self.close_button.show()

        if not self._auto_toolbar_enabled() and not self.toolbar_toggle_button.isVisible():
            self.toolbar_toggle_button.show()

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
            # 🔥 view 覆盖整个窗口（包括 padding）
            self.view.setGeometry(0, 0, self.width(), self.height())
            self._update_view_transform()
        self.update_button_positions()
        if self.toolbar and self.toolbar.isVisible():
            self.toolbar.sync_with_pin_window()
        
        # 同步 OCR 文字层大小和位置（覆盖内容区域，不包括边框）
        if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
            cr = self.content_rect()
            self.ocr_text_layer.setGeometry(cr.toRect())
        
        super().resizeEvent(event)
    
    # ==================== 🌟 光晕/阴影效果 ====================
    
    def content_rect(self) -> QRectF:
        """内容区域（显示截图的区域，不包括padding）"""
        if not self.halo_enabled:
            return QRectF(self.rect())
        return QRectF(
            self.pad, 
            self.pad,
            max(1, self.width() - self.pad * 2),
            max(1, self.height() - self.pad * 2)
        )
    
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
            # 🔥 二次方衰减曲线：外层淡，内层深
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
        绘制事件 - 高性能渲染 + 光晕效果
        
        🔥 新架构：光晕阴影 + CanvasScene渲染
        """
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, not self._is_scaling)
        
        # 🌟 1. 绘制阴影/光晕缓存
        if self.halo_enabled:
            self._ensure_shadow_cache()
            if self._shadow_cache is not None:
                painter.drawPixmap(0, 0, self._shadow_cache)
        
        # 🌟 2. 绘制内容（圆角裁剪）
        cr = self.content_rect()
        
        if hasattr(self, 'view') and self.view:
            # 使用CanvasView渲染
            if self.halo_enabled and self.corner > 0:
                # 圆角裁剪
                clip_path = self._rounded_path(cr, self.corner)
                painter.save()
                painter.setClipPath(clip_path)
            
            # 渲染view的内容到painter
            if self.canvas:
                self.canvas.render_to_painter(painter, cr)
            elif self._base_pixmap:
                painter.drawPixmap(cr, self._base_pixmap, QRectF(self._base_pixmap.rect()))
            
            if self.halo_enabled and self.corner > 0:
                painter.restore()
        else:
            # 回退：直接绘制pixmap
            target_rect = cr if self.halo_enabled else self.rect()
            if self.canvas:
                self.canvas.render_to_painter(painter, target_rect)
            elif self._base_pixmap:
                painter.drawPixmap(target_rect, self._base_pixmap)
        
        painter.end()
    
    # ==================== 鼠标事件 ====================
    # 🔥 让鼠标事件传递给 PinCanvasView，由它的智能编辑系统处理
    # 窗口层面不拦截，只在非编辑状态处理窗口拖动
    
    def mousePressEvent(self, event: QMouseEvent):
        """鼠标按下 - 非编辑状态拖动窗口，编辑状态传递给 view"""
        self._set_hover_state(True)
        
        # 🔥 非编辑模式：拖动窗口
        if event.button() == Qt.MouseButton.LeftButton and not (self.canvas and self.canvas.is_editing):
            self.start_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        
        # 🔥 编辑模式或其他按钮：传递给子控件（view 会处理）
        super().mousePressEvent(event)
    
    def mouseMoveEvent(self, event: QMouseEvent):
        """鼠标移动 - 拖动窗口或传递给 view"""
        self._set_hover_state(True)

        # 🔥 拖动模式
        if self._is_dragging:
            self.update_window_drag(event.globalPosition().toPoint())
            event.accept()
            return
        
        # 🔥 其他情况传递给子控件
        super().mouseMoveEvent(event)
    
    def mouseReleaseEvent(self, event: QMouseEvent):
        """鼠标释放 - 结束拖动或传递给 view"""
        if event.button() == Qt.MouseButton.LeftButton and self._is_dragging:
            # 🔥 结束拖动
            self.end_window_drag()
            event.accept()
            return
        elif event.button() == Qt.MouseButton.RightButton:
            # 🔥 显示右键菜单
            self.show_context_menu(event.globalPosition().toPoint())
            event.accept()
            return
        
        # 🔥 其他情况传递给子控件
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
        
        print(f"🔍 [钉图窗口] 缩放: {old_width}x{old_height} → {new_width}x{new_height}")
    
    def _apply_smooth_scaling(self):
        """
        应用平滑缩放（延迟触发）
        
        当用户停止滚轮缩放 150ms 后，使用高质量变换重新渲染
        """
        self._is_scaling = False
        self.update()  # 触发 paintEvent，使用 SmoothTransformation
        self._update_view_transform()
        print("✨ [钉图窗口] 应用高质量渲染")
    
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
            
            # 🔥 传递 config_manager，确保工具设置能够保存和读取
            self.toolbar = PinToolbar(parent_pin_window=self, config_manager=self.config_manager)
            
            # 🔥 连接信号到画布
            if self.canvas:
                # 工具切换
                self.toolbar.tool_changed.connect(self._on_tool_changed)
                
                # 🔥 撤销/重做（连接到 CanvasScene 的 undo_stack）
                self.toolbar.undo_clicked.connect(self.canvas.undo_stack.undo)
                self.toolbar.redo_clicked.connect(self.canvas.undo_stack.redo)
                
                # 🔥 样式改变（连接到 tool_controller）
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
            
            print("🔧 [钉图窗口] 创建工具栏，连接完整信号（撤销/重做/工具/样式）")
            
            # 🔥 打印撤销栈状态（调试用）
            if self.canvas:
                self.canvas.undo_stack.print_stack_status()
        
        # 🔥 每次显示时都检查并应用自动隐藏设置
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
    
    # ==================== 右键菜单 ====================
    
    def show_context_menu(self, global_pos: QPoint):
        """
        显示右键菜单
        
        Args:
            global_pos: 全局坐标位置
        """
        menu = QMenu(self)
        
        # 设置字体，确保在所有Windows系统上都能正常显示
        from PyQt6.QtGui import QFont
        menu_font = QFont("Microsoft YaHei UI", 9)  # 使用微软雅黑UI，Windows系统自带
        if not menu_font.exactMatch():
            # 如果微软雅黑UI不可用，尝试其他字体
            menu_font = QFont("Segoe UI", 9)  # Windows 10/11默认字体
        menu.setFont(menu_font)
        
        menu.setStyleSheet("""
            QMenu {
                background-color: white;
                border: 1px solid #ccc;
                border-radius: 4px;
                padding: 5px;
                font-family: "Microsoft YaHei UI", "Segoe UI", "Yu Gothic UI", sans-serif;
                font-size: 9pt;
                color: #000000;
            }
            QMenu::item {
                padding: 5px 30px 5px 30px;
                border-radius: 3px;
                color: #000000;
                background-color: transparent;
            }
            QMenu::item:selected {
                background-color: #e3f2fd;
                color: #000000;
            }
            QMenu::separator {
                height: 1px;
                background: #ddd;
                margin: 5px 0px;
            }
        """)
        
        # 📋 复制内容
        copy_action = QAction("📋 コピー", self)
        copy_action.triggered.connect(self.copy_to_clipboard)
        menu.addAction(copy_action)
        
        # 💾 保存图片
        save_action = QAction("💾 名前を付けて保存", self)
        save_action.triggered.connect(self.save_image)
        menu.addAction(save_action)
        
        menu.addSeparator()
        
        # 🔧 显示/隐藏工具栏
        toolbar_visible = self.toolbar and self.toolbar.isVisible()
        toolbar_action = QAction(f"{'✓ ' if toolbar_visible else '   '}🔧 ツールバー", self)
        toolbar_action.triggered.connect(self.toggle_toolbar)
        menu.addAction(toolbar_action)
        
        # 📌 切换置顶
        stay_on_top = bool(self.windowFlags() & Qt.WindowType.WindowStaysOnTopHint)
        toggle_top_action = QAction(f"{'✓ ' if stay_on_top else '   '}📌 常に手前に表示", self)
        toggle_top_action.triggered.connect(self.toggle_stay_on_top)
        menu.addAction(toggle_top_action)
        
        # 🌟 切换阴影效果
        shadow_action = QAction(f"{'✓ ' if self.halo_enabled else '   '}🌟 影効果", self)
        shadow_action.triggered.connect(self.toggle_shadow_effect)
        menu.addAction(shadow_action)
        
        menu.addSeparator()
        
        # ❌ 关闭钉图
        close_action = QAction("❌ 閉じる", self)
        close_action.triggered.connect(self.close_window)
        menu.addAction(close_action)
        
        # 显示菜单
        menu.exec(global_pos)
    
    def toggle_stay_on_top(self):
        """切换窗口置顶状态"""
        current_flags = self.windowFlags()
        
        if current_flags & Qt.WindowType.WindowStaysOnTopHint:
            # 取消置顶
            new_flags = current_flags & ~Qt.WindowType.WindowStaysOnTopHint
            print("📍 [钉图窗口] 取消置顶")
        else:
            # 设置置顶
            new_flags = current_flags | Qt.WindowType.WindowStaysOnTopHint
            print("📍 [钉图窗口] 设置置顶")
        
        # 保存当前位置和大小
        geometry = self.geometry()
        
        # 应用新的窗口标志
        self.setWindowFlags(new_flags)
        
        # 恢复位置和大小
        self.setGeometry(geometry)
        
        # 重新显示窗口
        self.show()
    
    def toggle_shadow_effect(self):
        """切换阴影/光晕效果"""
        self.halo_enabled = not self.halo_enabled
        
        if self.halo_enabled:
            print("🌟 [钉图窗口] 启用阴影效果")
            # 启用透明背景
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
            
            # 调整窗口大小，增加padding
            current_geo = self.geometry()
            content_width = current_geo.width()
            content_height = current_geo.height()
            
            new_x = current_geo.x() - self.pad
            new_y = current_geo.y() - self.pad
            new_width = content_width + self.pad * 2
            new_height = content_height + self.pad * 2
            
            self.setGeometry(new_x, new_y, new_width, new_height)
        else:
            print("🌑 [钉图窗口] 禁用阴影效果")
            # 禁用透明背景
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
            
            # 调整窗口大小，移除padding
            current_geo = self.geometry()
            content_width = current_geo.width() - self.pad * 2
            content_height = current_geo.height() - self.pad * 2
            
            new_x = current_geo.x() + self.pad
            new_y = current_geo.y() + self.pad
            new_width = max(50, content_width)
            new_height = max(50, content_height)
            
            self.setGeometry(new_x, new_y, new_width, new_height)
        
        # 清除阴影缓存
        self._shadow_cache = None
        self._shadow_key = None
        
        # 重新布局
        if hasattr(self, 'view') and self.view:
            cr = self.content_rect()
            self.view.setGeometry(cr.toRect())
            self._update_view_transform()
        
        self.update_button_positions()
        
        # 触发重绘
        self.update()
    
    def _on_tool_changed(self, tool_name: str):
        """
        工具切换事件处理
        
        Args:
            tool_name: 工具名称（pen, rect, arrow, text, 等）或 "cursor" 表示取消工具
        """
        if not self.canvas:
            return
        
        # 🔥 cursor 表示取消工具，退出编辑模式
        if tool_name and tool_name != "cursor":
            # 激活工具 → 进入编辑模式
            self.canvas.activate_tool(tool_name)
            print(f"🎨 [钉图窗口] 激活工具: {tool_name}，进入编辑模式")
            
            # 🔥 通知 OCR 层：工具激活，隐藏文字选择层
            if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
                self.ocr_text_layer.set_drawing_mode(True)
            
            # 🔥 同步 UI：工具激活后，其设置已从 config_manager 加载到 ToolContext
            # 现在需要同步到工具栏 UI（更新滑块、颜色显示）
            if self.toolbar and hasattr(self.canvas, 'tool_controller'):
                ctx = self.canvas.tool_controller.ctx
                
                # 临时断开信号，避免循环触发
                try:
                    self.toolbar.color_changed.disconnect(self._on_color_changed)
                    self.toolbar.stroke_width_changed.disconnect(self._on_stroke_width_changed)
                    self.toolbar.opacity_changed.disconnect(self._on_opacity_changed)
                except:
                    pass  # 如果信号未连接，忽略错误
                
                try:
                    # 更新工具栏 UI 显示当前工具的设置
                    self.toolbar.set_current_color(ctx.color)
                    self.toolbar.set_stroke_width(ctx.stroke_width)
                    self.toolbar.set_opacity(int(ctx.opacity * 255))
                    
                    print(f"🔄 [钉图-UI同步] 工具={tool_name}, 颜色={ctx.color.name()}, 宽度={ctx.stroke_width}, 透明度={ctx.opacity}")
                finally:
                    # 重新连接信号
                    self.toolbar.color_changed.connect(self._on_color_changed)
                    self.toolbar.stroke_width_changed.connect(self._on_stroke_width_changed)
                    self.toolbar.opacity_changed.connect(self._on_opacity_changed)
            
            # 🔥 切换工具后，将焦点还给 View（确保快捷键可用）
            # 使用 QTimer 延迟执行，确保工具按钮点击事件完成后再设置焦点
            from PyQt6.QtCore import QTimer
            if hasattr(self.canvas, 'view'):
                QTimer.singleShot(0, self.canvas.view.setFocus)
        else:
            # 取消工具 → 退出编辑模式
            self.canvas.deactivate_tool()
            print(f"🎨 [钉图窗口] 取消工具，退出编辑模式")
            
            # 🔥 通知 OCR 层：工具取消，重新显示文字选择层
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
        print(f"[PinWindow] slider width change -> prev={prev_width}, target={width}")
        self.canvas.set_stroke_width(width)
        new_width = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)
        self._apply_selection_width_scale(prev_width, new_width)
    
    def _on_opacity_changed(self, opacity_int):
        """透明度改变事件（0-255）"""
        if not self.canvas:
            return
        opacity = float(opacity_int) / 255.0
        print(f"[PinWindow] slider opacity change -> target={opacity:.3f}")
        self.canvas.set_opacity(opacity)
        self._apply_selection_opacity(opacity)

    def _apply_selection_width_scale(self, prev_width: float, new_width: float):
        if prev_width <= 0 or new_width <= 0:
            print(f"[PinWindow] skip width scaling: prev={prev_width}, new={new_width}")
            return
        if abs(new_width - prev_width) <= 1e-6:
            print(f"[PinWindow] width unchanged (prev={prev_width}, new={new_width})")
            return
        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_size_change_to_selection'):
            scale = new_width / prev_width
            print(f"[PinWindow] applying selection scale via view: scale={scale:.3f}")
            view._apply_size_change_to_selection(scale)
        else:
            print(f"[PinWindow] missing view for selection scaling: view={view}")

    def _apply_selection_opacity(self, opacity: float):
        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_opacity_change_to_selection'):
            view._apply_opacity_change_to_selection(opacity)
        else:
            print(f"[PinWindow] skip opacity sync: missing view helper (view={view})")
    
    # ==================== 窗口管理 ====================
    
    def close_window(self):
        """关闭窗口"""
        if self._is_closed:
            return
        
        print("🗑️ [钉图窗口] 开始关闭...")
        self._is_closed = True
        
        # 清理资源
        self.cleanup()
        
        # 发送关闭信号
        self.closed.emit()
        
        # 关闭窗口
        self.close()
    
    def cleanup(self):
        """清理资源"""
        print("🧹 [钉图窗口] 清理资源...")
        
        # 0. 停止所有定时器
        if hasattr(self, '_scale_timer') and self._scale_timer:
            self._scale_timer.stop()
            self._scale_timer.deleteLater()
            self._scale_timer = None
        if hasattr(self, '_hover_monitor') and self._hover_monitor:
            self._hover_monitor.stop()
            self._hover_monitor.deleteLater()
            self._hover_monitor = None
        
        # 1. 关闭工具栏
        if self.toolbar:
            # 🔥 关闭二级菜单（如果存在）
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
                print("⚠️ [OCR] 窗口关闭，OCR 线程仍在运行，将其分离以在后台完成...")
                try:
                    self.ocr_thread.finished.disconnect()
                except:
                    pass
                
                # 🔥 重设父对象，防止随窗口销毁
                self.ocr_thread.setParent(None)
                
                # 🔥 线程完成后自动清理（不阻塞窗口关闭）
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
        
        # 2. 清理视图
        if hasattr(self, 'view') and self.view:
            if hasattr(self.view, 'viewport'):
                try:
                    self.view.viewport().removeEventFilter(self)
                except Exception:
                    pass
            # 暂时不清理scene引用，让canvas负责清理
            self.view.deleteLater()
            self.view = None
        
        # 3. 清理画布（会自动清理scene）
        if self.canvas:
            try:
                self.canvas.cleanup()  # 这个方法内部会清理scene
            except Exception as e:
                print(f"⚠️ [钉图窗口] 画布清理时出错: {e}")
            self.canvas = None
        
        # 4. 清理图像数据
        self._base_pixmap = None
        self.vector_commands = None
        
        # 5. 🔥 强制垃圾回收
        import gc
        gc.collect()
        
        print("✅ [钉图窗口] 资源清理完成，内存已回收")
    
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
                print("ℹ️ [OCR] OCR 功能已禁用，跳过初始化")
                return
            
            # 2. 检查 OCR 是否可用
            if not is_ocr_available():
                print("⚠️ [OCR] OCR 模块不可用（无OCR版本），静默跳过")
                return
            
            # 3. 初始化 OCR 引擎
            if not initialize_ocr():
                print("⚠️ [OCR] OCR 引擎初始化失败")
                return
            
            print("✅ [OCR] OCR 引擎已就绪（支持中日韩英混合识别）")
            
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
                        # 从配置读取 OCR 参数
                        enable_grayscale = self.config_manager.get_ocr_grayscale_enabled() if self.config_manager else True
                        enable_upscale = self.config_manager.get_ocr_upscale_enabled() if self.config_manager else False
                        upscale_factor = self.config_manager.get_ocr_upscale_factor() if self.config_manager else 1.5
                        
                        # 调用 OCR 识别
                        self.result = recognize_text(
                            self.pixmap, 
                            return_format="dict",
                            enable_grayscale=enable_grayscale,
                            enable_upscale=enable_upscale,
                            upscale_factor=upscale_factor
                        )
                    except Exception as e:
                        print(f"❌ [OCR Thread] 识别失败: {e}")
                        import traceback
                        traceback.print_exc()
                        self.result = None
            
            # 8. 获取钉图图像（包含所有图层）
            pixmap = QPixmap.fromImage(self.get_current_image())
            original_width = pixmap.width()
            original_height = pixmap.height()
            
            # 9. 启动异步识别
            print("🔄 [OCR] 开始异步识别文字...")
            self.ocr_thread = OCRThread(pixmap, self.config_manager, self)
            
            def on_ocr_finished():
                try:
                    # 检查窗口是否已关闭
                    if self._is_closed:
                        print("⚠️ [OCR] 窗口已关闭，跳过结果加载")
                        return
                    
                    # 检查 OCR 文字层是否还存在
                    if not hasattr(self, 'ocr_text_layer') or self.ocr_text_layer is None:
                        print("⚠️ [OCR] OCR 文字层已被清理，跳过结果加载")
                        return
                    
                    # 检查线程是否还存在
                    if not hasattr(self, 'ocr_thread') or self.ocr_thread is None:
                        print("⚠️ [OCR] OCR 线程已被清理，跳过结果加载")
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
                            print(f"✅ [OCR] 钉图文字层已就绪，识别到 {len(self.ocr_thread.result.get('data', []))} 个文字块")
                except Exception as e:
                    print(f"❌ [OCR] 加载结果失败: {e}")
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
            # OCR 模块不存在，静默跳过
            pass
        except Exception as e:
            print(f"⚠️ [OCR] 初始化失败: {e}")
            import traceback
            traceback.print_exc()
    
    def closeEvent(self, event):
        """窗口关闭事件"""
        if not self._is_closed:
            self._is_closed = True
            self.cleanup()
            self.closed.emit()
        
        super().closeEvent(event)
        print("🗑️ [钉图窗口] 窗口已销毁")
    
    # ==================== 辅助方法 ====================
    
    def get_current_image(self) -> QImage:
        """
        获取当前图像（背景+所有图层）
        
        Returns:
            QImage: 当前渲染的图像（HiDPI 清晰版）
        
        🔥 新架构：直接使用 canvas.get_current_image()
        """
        # 获取窗口所在屏幕的 DPR
        dpr = self.devicePixelRatioF()
        
        if self.canvas:
            # 🔥 直接从画布导出（包含所有图层）
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
            # 🔥 临时退出编辑模式，隐藏选择框和手柄
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
            
            # 🔥 恢复编辑模式
            if was_editing and active_tool_id:
                self.canvas.activate_tool(active_tool_id)
            
            if success:
                print(f"💾 [钉图窗口] 保存成功: {file_path}")
            else:
                print(f"❌ [钉图窗口] 保存失败: {file_path}")
    
    def copy_to_clipboard(self):
        """复制图像到剪贴板"""
        # 🔥 临时退出编辑模式，隐藏选择框和手柄
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
        
        # 🔥 恢复编辑模式
        if was_editing and active_tool_id:
            self.canvas.activate_tool(active_tool_id)
        
        print("📋 [钉图窗口] 已复制到剪贴板")

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
