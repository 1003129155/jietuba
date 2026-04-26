"""
画布视图 - 处理用户交互
"""

from typing import Optional

from PySide6.QtWidgets import QGraphicsView, QGraphicsTextItem
from PySide6.QtCore import Qt, QPointF, QRectF, QTimer
from PySide6.QtGui import QPainter, QPen, QColor, QBrush
import shiboken6
from canvas.items import (
    StrokeItem,
    RectItem,
    EllipseItem,
    ArrowItem,
    TextItem,
    NumberItem,
)
from core import log_debug, log_info, log_warning, log_error, safe_event


class CanvasView(QGraphicsView):
    """
    画布视图
    """
    
    def __init__(self, scene, parent=None):
        super().__init__(scene, parent)
        
        self.canvas_scene = scene
        
        # 设置渲染选项 - 关闭抗锯齿以提高性能
        # self.setRenderHint(QPainter.RenderHint.Antialiasing)  # 关闭抗锯齿
        # self.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)  # 关闭平滑变换
        
        # 使用智能视口更新模式
        self.setViewportUpdateMode(QGraphicsView.ViewportUpdateMode.SmartViewportUpdate)
        
        # 禁用滚动条
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        
        # 左上角对齐：禁止 QGraphicsView 在 resizeEvent 时自动居中场景，
        # 防止内部 scroll offset 偶发偏移导致 mapFromScene 结果不稳定
        self.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
        
        # 移除边框，确保视图完全填充窗口
        self.setFrameShape(QGraphicsView.Shape.NoFrame)
        self.setContentsMargins(0, 0, 0, 0)
        self.setViewportMargins(0, 0, 0, 0)

        
        # 重要：设置视图变换，确保场景坐标和窗口坐标 1:1 对应
        # 场景使用全局屏幕坐标（可能不是从 0,0 开始），需要将场景原点映射到视图原点
        self.resetTransform()  # 重置变换
        # 将场景的 topLeft (可能是负数或正数) 映射到视图的 (0,0)
        scene_rect = scene.sceneRect()
        self.translate(-scene_rect.x(), -scene_rect.y())
        
        # 禁用视图自动改变光标（避免与 CursorManager 冲突）
        self.viewport().setMouseTracking(True)
        
        # 交互状态
        self.is_selecting = False  # 是否在选择区域
        self.is_drawing = False    # 是否在绘制
        self.is_dragging_selection = False # 是否正在拖拽选区（用于区分点击和拖拽）
        
        # 启用鼠标追踪以支持悬停检测
        self.setMouseTracking(True)
        
        self.start_pos = QPointF()
        
        # 智能选区相关
        self.smart_selection_enabled = False
        self.window_finder = None  # WindowFinder 实例（按需创建）
        self._last_smart_selection_pos = None  # 上次智能选区触发的位置（防抖）
        self._last_smart_selection_rect = QRectF()  # 缓存上次的智能选区矩形
        
        # 初始化光标管理器
        from tools.cursor_manager import CursorManager
        self.cursor_manager = CursorManager(self)
        
        # 初始化智能编辑控制器
        from canvas.smart_edit_controller import SmartEditController
        self.smart_edit_controller = SmartEditController(self.canvas_scene)
        
        # 注入 layer_editor 引用到 Scene（用于 drawForeground 渲染控制点）
        # 这是单向引用（Scene 只持有 layer_editor，不持有 view），避免循环引用
        self.canvas_scene._layer_editor = self.smart_edit_controller.layer_editor
        
        # 连接 Scene 的解耦信号（替代 scene.view = self 的循环引用）
        self.canvas_scene.cursor_color_update_requested.connect(
            self.cursor_manager.update_tool_cursor_color
        )
        self.canvas_scene.cursor_tool_update_requested.connect(
            lambda tool_id, force: self.cursor_manager.set_tool_cursor(tool_id, force=force)
        )
        self.canvas_scene.item_auto_select_requested.connect(
            lambda item: self.smart_edit_controller.select_item(item, auto_select=True)
        )
        self.canvas_scene.editing_cleanup_requested.connect(self._on_editing_cleanup)
        
        # 连接智能编辑控制器的信号
        self.smart_edit_controller.cursor_change_request.connect(self._on_edit_cursor_change)
        self.smart_edit_controller.selection_changed.connect(self._on_edit_selection_changed)
        
        # 监听工具切换，同步到智能编辑控制器
        self.canvas_scene.tool_controller.add_tool_changed_callback(self._on_tool_changed_for_edit)
        
        # 同步当前工具光标
        current_tool = self.canvas_scene.tool_controller.current_tool
        if current_tool:
            self.cursor_manager.set_tool_cursor(current_tool.id)
            self.smart_edit_controller.set_tool(current_tool.id)
        
        # Pending 单击文字进入编辑的状态
        self._pending_text_edit_item = None
        self._pending_text_edit_press_pos = None
        self._pending_text_edit_moved = False
        self._text_drag_hover_item = None
        self._text_drag_active = False
        self._text_drag_item = None
        self._text_drag_last_scene_pos = None
        self._text_drag_cursor_active = False
    
    def setCursor(self, cursor):
        """同时更新视图和 viewport，避免 Qt 只在父部件上应用光标"""
        super().setCursor(cursor)
        viewport = self.viewport()
        if viewport is not None:
            viewport.setCursor(cursor)
    
    @safe_event
    def enterEvent(self, event):
        """
        鼠标进入画布时强制应用当前光标，并刷新放大镜位置
        
        解决问题：点击工具栏按钮后，鼠标移回画布时光标可能不正确；
                  鼠标离开再回来时放大镜不跟随（enterEvent 比首个 mouseMoveEvent 早触发）
        """
        if not self.canvas_scene.selection_model.is_confirmed:
            self.setCursor(Qt.CursorShape.CrossCursor)
        elif hasattr(self, 'cursor_manager') and self.cursor_manager and self.cursor_manager.current_cursor:
            # 强制重新应用光标
            self.cursor_manager._apply_cursor(self.cursor_manager.current_cursor)
        # 鼠标重新进入时，用当前鼠标全局坐标刷新放大镜，
        # 避免等到第一个 mouseMoveEvent 才恢复显示
        overlay = self._get_magnifier_overlay()
        if overlay is not None:
            from PySide6.QtGui import QCursor
            from PySide6.QtCore import QPointF
            gpos = QCursor.pos()
            overlay.update_cursor(QPointF(gpos.x(), gpos.y()))
        super().enterEvent(event)

    @safe_event
    def showEvent(self, event):
        """窗口显示后强制刷新光标，避免沿用上一次截图的光标"""
        super().showEvent(event)
        if not self.canvas_scene.selection_model.is_confirmed:
            def _safe_set_cursor():
                if shiboken6.isValid(self):
                    self.setCursor(Qt.CursorShape.CrossCursor)
            QTimer.singleShot(0, _safe_set_cursor)
    
    # ========================================================================
    # 智能选区功能
    # ========================================================================
    
    def enable_smart_selection(self, enabled: bool):
        """
        启用/禁用智能选区功能
        
        Args:
            enabled: True=启用，False=禁用
        """
        self.smart_selection_enabled = enabled
        
        if enabled:
            # 检查依赖
            from capture.window_finder import is_smart_selection_available
            if not is_smart_selection_available():
                log_warning("win32gui 未安装，智能选区功能不可用", "SmartSelect")
                self.smart_selection_enabled = False
                return
            
            # 创建 WindowFinder 实例
            if not self.window_finder:
                from capture.window_finder import WindowFinder
                # 新架构 CanvasScene 使用全局坐标系（与屏幕物理坐标一致）
                # 因此不需要减去偏移量，直接使用全局坐标即可
                self.window_finder = WindowFinder(0, 0)
            
            # 枚举窗口
            self.window_finder.find_windows()
            log_debug(f"已启用，找到 {len(self.window_finder.windows)} 个窗口", "SmartSelect")
        else:
            log_debug("已禁用", "SmartSelect")
            if self.window_finder:
                self.window_finder.clear()
    
    def _get_smart_selection_rect(self, scene_pos: QPointF) -> QRectF:
        """
        获取智能选区矩形（鼠标位置的窗口边界）
        
        优化策略：
        1. 防抖：只在鼠标移动超过阈值时才触发查找
        2. 缓存：相同位置直接返回缓存结果
        
        Args:
            scene_pos: 鼠标在场景中的位置
        
        Returns:
            窗口矩形（场景坐标）
        """
        if not self.smart_selection_enabled or not self.window_finder:
            return QRectF()
        
        # 优化1：防抖 - 鼠标移动小于阈值不触发查找
        # 智能选区结果是窗口矩形（通常几百像素宽），小幅移动结果不会变
        if self._last_smart_selection_pos is not None:
            dist = (scene_pos - self._last_smart_selection_pos).manhattanLength()
            if dist < 8:  # 阈值：8px（约0.5mm物理距离，跨窗口边界延迟肉眼无感）
                return self._last_smart_selection_rect
        
        # 查找鼠标位置的窗口
        x = int(scene_pos.x())
        y = int(scene_pos.y())
        
        # 设置备选矩形为全场景（使用场景真实坐标，包含负坐标显示器）
        scene_rect = self.canvas_scene.scene_rect
        fallback_rect = [
            int(scene_rect.x()),
            int(scene_rect.y()),
            int(scene_rect.x() + scene_rect.width()),
            int(scene_rect.y() + scene_rect.height())
        ]
        
        window_rect = self.window_finder.find_window_at_point(x, y, fallback_rect)
        
        # 转换为 QRectF
        if window_rect:
            result = QRectF(
                float(window_rect[0]),
                float(window_rect[1]),
                float(window_rect[2] - window_rect[0]),
                float(window_rect[3] - window_rect[1])
            )
        else:
            result = QRectF()
        
        # 优化2：缓存结果
        self._last_smart_selection_pos = scene_pos
        self._last_smart_selection_rect = result
        
        return result
    
    # ========================================================================
    # 智能编辑控制器回调
    # ========================================================================
    
    def _on_editing_cleanup(self):
        """响应 editing_cleanup_requested 信号，清除编辑状态"""
        if self.smart_edit_controller.selected_item:
            log_debug("取消智能编辑选择", "CanvasView")
            self.smart_edit_controller.clear_selection(suppress_block=True)
        if hasattr(self.cursor_manager, 'hide_brush_indicator'):
            self.cursor_manager.hide_brush_indicator()

    def _on_tool_changed_for_edit(self, tool_id: str):
       self.smart_edit_controller.set_tool(tool_id)

       # 工具切换时立即更新光标
       self.cursor_manager.set_tool_cursor(tool_id)
       if self.cursor_manager.current_cursor:
        self.setCursor(self.cursor_manager.current_cursor)

    
    def _on_edit_cursor_change(self, cursor_type: str):
        """智能编辑控制器请求光标变化"""
        # 将字符串类型映射到 Qt.CursorShape
        from PySide6.QtCore import Qt
        cursor_map = {
            "cross": Qt.CursorShape.CrossCursor,
            "default": Qt.CursorShape.ArrowCursor,
            "move": Qt.CursorShape.SizeAllCursor,
            "resize": Qt.CursorShape.SizeFDiagCursor,
        }
        cursor_shape = cursor_map.get(cursor_type, Qt.CursorShape.ArrowCursor)
        self.setCursor(cursor_shape)
    
    def _on_edit_selection_changed(self, item):
        """智能编辑选择变化"""
        if item:
            log_debug(f"选中: {type(item).__name__}", "SmartEdit")
        else:
            log_debug("取消选择", "SmartEdit")
            self._sync_highlighter_panel_mode()
        self._sync_selection_style_to_toolbar(item)

    def _sync_highlighter_panel_mode(self):
        toolbar = self._get_active_toolbar()
        tool_controller = getattr(self.canvas_scene, "tool_controller", None)
        if not toolbar or not tool_controller:
            return
        current_tool = getattr(tool_controller, "current_tool", None)
        if not current_tool or current_tool.id != "highlighter":
            return

        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            settings = manager.get_tool_settings("highlighter") if manager else None
            mode = settings.get("draw_mode", "freehand") if settings else "freehand"
        except Exception as exc:
            log_warning(f"读取荧光笔模式失败: {exc}", "CanvasView")
            mode = "freehand"

        if hasattr(toolbar, "paint_panel") and hasattr(toolbar.paint_panel, "set_highlighter_mode"):
            toolbar.paint_panel.set_highlighter_mode(mode)

    def _get_active_toolbar(self):
        window = self.window()
        if window is None:
            return None
        toolbar = getattr(window, "toolbar", None)
        return toolbar if toolbar else None

    def _sync_selection_style_to_toolbar(self, item):
        toolbar = self._get_active_toolbar()
        tool_controller = getattr(self.canvas_scene, "tool_controller", None)
        if not toolbar or not tool_controller:
            return

        if not item:
            return

        is_text_item = isinstance(item, QGraphicsTextItem)
        width_value = None if is_text_item else self._extract_selection_width(item)
        opacity_value = self._extract_selection_opacity(item)

        style_kwargs = {}
        if width_value is not None:
            style_kwargs["width"] = max(1.0, float(width_value))
        if opacity_value is not None:
            style_kwargs["opacity"] = max(0.0, min(1.0, float(opacity_value)))

        if style_kwargs:
            tool_controller.update_style(**style_kwargs)
            if "width" in style_kwargs and getattr(self, "cursor_manager", None):
                ctx_width = int(max(1, round(tool_controller.ctx.stroke_width)))
                self.cursor_manager.update_tool_cursor_size(ctx_width)

        if width_value is not None:
            toolbar.set_stroke_width(int(round(width_value)))

        if opacity_value is not None:
            toolbar.set_opacity(int(round(opacity_value * 255)))

        if isinstance(item, StrokeItem) and hasattr(toolbar, "paint_panel"):
            try:
                from PySide6.QtCore import Qt
                pen = item.pen()
                pen_style = pen.style()
                dash_pattern = [round(x, 1) for x in pen.dashPattern()]
                has_dash = bool(dash_pattern)
                if has_dash and dash_pattern[:2] == [1.0, 2.0]:
                    line_style = "dashed_dense"
                elif pen_style in (Qt.PenStyle.DashLine, Qt.PenStyle.CustomDashLine) or has_dash:
                    line_style = "dashed"
                else:
                    line_style = "solid"
                toolbar.paint_panel.line_style = line_style
                from settings import get_tool_settings_manager
                manager = get_tool_settings_manager()
                settings_tool_id = "highlighter" if getattr(item, "is_highlighter", False) else "pen"
                manager.update_settings(settings_tool_id, line_style=line_style)
            except Exception as exc:
                log_warning(f"无法同步线条样式: {exc}", "CanvasView")
        elif isinstance(item, (RectItem, EllipseItem)) and hasattr(toolbar, "shape_panel"):
            try:
                from PySide6.QtCore import Qt
                pen = item.pen()
                pen_style = pen.style()
                dash_pattern = [round(x, 1) for x in pen.dashPattern()]
                has_dash = bool(dash_pattern)
                if has_dash and dash_pattern[:2] == [1.0, 2.0]:
                    line_style = "dashed_dense"
                elif pen_style in (Qt.PenStyle.DashLine, Qt.PenStyle.CustomDashLine) or has_dash:
                    line_style = "dashed"
                else:
                    line_style = "solid"
                toolbar.shape_panel.line_style = line_style
                from settings import get_tool_settings_manager
                manager = get_tool_settings_manager()
                tool_id = "rect" if isinstance(item, RectItem) else "ellipse"
                manager.update_settings(tool_id, line_style=line_style)
            except Exception as exc:
                log_warning(f"无法同步形状线条样式: {exc}", "CanvasView")

        # 根据选中的图元类型，显示对应的设置面板（二次编辑时支持修改样式）
        self._show_panel_for_selection(item, toolbar)

    def _show_panel_for_selection(self, item, toolbar):
        """根据选中的图元类型显示对应的设置面板（二次编辑支持）"""
        try:
            # 文字图元
            if isinstance(item, QGraphicsTextItem) and hasattr(toolbar, "text_panel"):
                try:
                    toolbar.text_panel.set_state_from_item(item)
                except Exception as exc:
                    log_warning(f"无法同步文字面板: {exc}", "CanvasView")
                toolbar._show_panel_for_tool("text")
            # 箭头图元
            elif isinstance(item, ArrowItem) and hasattr(toolbar, "arrow_panel"):
                # 同步箭头样式到面板
                arrow_style = getattr(item, '_arrow_style', 'single')
                toolbar.arrow_panel.arrow_style = arrow_style
                # 同步颜色
                if hasattr(item, 'color'):
                    toolbar.arrow_panel.set_color(item.color)
                toolbar._show_panel_for_tool("arrow")
            # 高亮矩形（使用画笔面板）
            elif isinstance(item, RectItem) and getattr(item, "is_highlighter_rect", False) and hasattr(toolbar, "paint_panel"):
                try:
                    toolbar.paint_panel.set_state_from_item(item)
                    toolbar.paint_panel.set_line_style_visible(False)
                except Exception as exc:
                    log_warning(f"无法同步高亮矩形面板: {exc}", "CanvasView")
                toolbar._show_panel_for_tool("highlighter")
                if hasattr(toolbar.paint_panel, "set_highlighter_mode"):
                    toolbar.paint_panel.set_highlighter_mode("rect")
            # 形状图元
            elif isinstance(item, (RectItem, EllipseItem)) and hasattr(toolbar, "shape_panel"):
                try:
                    toolbar.shape_panel.set_state_from_item(item)
                except Exception as exc:
                    log_warning(f"无法同步形状面板: {exc}", "CanvasView")
                toolbar._show_panel_for_tool("rect")
            # 序号图元
            elif isinstance(item, NumberItem) and hasattr(toolbar, "number_panel"):
                toolbar._show_panel_for_tool("number")
            # 画笔图元
            elif isinstance(item, StrokeItem) and hasattr(toolbar, "paint_panel"):
                try:
                    toolbar.paint_panel.set_state_from_item(item)
                    toolbar.paint_panel.set_line_style_visible(not getattr(item, "is_highlighter", False))
                except Exception as exc:
                    log_warning(f"无法同步画笔面板: {exc}", "CanvasView")
                toolbar._show_panel_for_tool("highlighter" if getattr(item, "is_highlighter", False) else "pen")
        except Exception as e:
            log_warning(f"显示编辑面板失败: {e}", "CanvasView")

    def _extract_selection_width(self, item):
        if hasattr(item, 'get_stroke_width'):
            return item.get_stroke_width()
        return None

    def _extract_selection_opacity(self, item):
        if not item:
            return None
        if hasattr(item, 'get_visual_opacity'):
            result = item.get_visual_opacity()
            if result is not None:
                return result
        direct = max(0.0, min(1.0, float(item.opacity())))
        return direct

    @safe_event
    def mousePressEvent(self, event):
        """
        鼠标按下
        
        优先级逻辑：
        0. 右键 → 直接退出截图（仅截图窗口）
        1. 选区未确认 → 创建选区
        2. 选区已确认：
           a. 优先检查智能编辑（选中已有图元 + 控制点拖拽）
           b. 如果未处理，再执行绘图工具逻辑
        """
        # 右键直接退出截图（复用 ESC 的清理逻辑）
        # 只在截图窗口中生效，钉图窗口不响应
        if event.button() == Qt.MouseButton.RightButton:
            # 检查父窗口类型，只对 ScreenshotWindow 生效
            parent_window = self.window()
            if parent_window and parent_window.__class__.__name__ == 'ScreenshotWindow':
                log_debug("右键退出截图", "CanvasView")
                event.accept()  # 立即接受事件
                # 复用 cleanup_and_close 方法，与 ESC 保持一致
                if hasattr(parent_window, 'cleanup_and_close'):
                    parent_window.cleanup_and_close()
                else:
                    parent_window.close()
                return
            # 钉图窗口：不处理右键，让事件继续传递（显示右键菜单）
        
        scene_pos = self.mapToScene(event.pos())
        # 新的一次点击开始前重置单击编辑状态
        self._clear_pending_text_edit()
        
        if not self.canvas_scene.selection_model.is_confirmed:
            # 选区未确认：拖拽创建选区
            self.is_selecting = True
            self.is_dragging_selection = False # 重置拖拽状态
            self.start_pos = scene_pos
            self.canvas_scene.selection_model.activate()
            # 开始拖拽，隐藏控制点（降低渲染压力）
            self.canvas_scene.selection_model.start_dragging()
            
            # 智能选区：点击时立即更新选区（防止 activate 清除选区）
            if self.smart_selection_enabled:
                smart_rect = self._get_smart_selection_rect(scene_pos)
                if not smart_rect.isEmpty():
                    self.canvas_scene.selection_model.set_rect(smart_rect)
        else:
            # 选区已确认：优先尝试智能编辑
            current_tool = self.canvas_scene.tool_controller.current_tool
            current_tool_id = current_tool.id if current_tool else "cursor"
            
            log_debug(f"选区已确认，当前工具: {current_tool_id}", "CanvasView")
            
            # 步骤0：如果正在编辑文本，点击外部只确认编辑，不创建新文本
            if self._is_text_editing():
                focus_item = self._get_active_text_item()
                if focus_item is None:
                    return
                if isinstance(focus_item, QGraphicsTextItem) and \
                        self._is_point_on_text_edge(focus_item, scene_pos):
                    self._begin_text_drag(focus_item, scene_pos)
                    return
                # 检查点击位置是否在当前编辑的文本框内
                if focus_item.contains(focus_item.mapFromScene(scene_pos)):
                    # 点击在文本框内，正常传递事件（移动光标等）
                    super().mousePressEvent(event)
                    return
                else:
                    # 点击在文本框外，清除焦点（触发 focusOutEvent 自动确认/删除）
                    log_debug("结束文本编辑", "CanvasView")
                    focus_item.clearFocus()
                    self._finalize_text_edit_state(focus_item)
                    # 阻止本次点击触发新绘图
                    return

            # 步骤1：优先检查控制点拖拽（如果已选中图元）
            edit_handled = self.smart_edit_controller.handle_edit_press(
                scene_pos, event.pos(), event.button(), event.modifiers()
            )
            
            if edit_handled:
                # 控制点拖拽被处理，不继续
                log_debug("控制点拖拽被处理", "CanvasView")
                return
            
            # 步骤2：检查是否点击了可选中的图元
            selection_handled = self.smart_edit_controller.handle_press(
                event.pos(), 
                scene_pos, 
                event.button(), 
                event.modifiers()
            )
            
            if selection_handled:
                # 选中了图元，阻止绘图
                # 传递给 Scene（让图元处理拖拽）
                log_debug("图元选择被处理，阻止绘图", "CanvasView")
                self._maybe_prepare_text_edit(event, scene_pos)
                super().mousePressEvent(event)
                return
            
            # 如果刚刚清除了选择，这次点击仅用于取消选择，不应该开始绘图
            if getattr(self.smart_edit_controller, '_just_cleared_selection', False):
                self.smart_edit_controller._just_cleared_selection = False
                log_debug("刚清除选择，跳过本次绘图", "CanvasView")
                return
            
            # 步骤3：如果是绘图工具且未选中图元，执行绘图
            is_drawing_tool = current_tool_id != "cursor"
            
            if is_drawing_tool:
                # 绘图工具激活：绘图
                log_debug("开始绘图", "CanvasView")
                self.is_drawing = True
                # 立即隐藏放大镜，避免 hide() 和首帧绘图重绘叠加导致卡顿
                self._clear_magnifier_overlay()
                self.canvas_scene.tool_controller.on_press(scene_pos, event.button())
            else:
                # cursor 工具：传递给 Scene（可能拖拽窗口/选区）
                log_debug("cursor工具，传递给Scene", "CanvasView")
                super().mousePressEvent(event)
    
    @safe_event
    def mouseMoveEvent(self, event):
        """
        鼠标移动事件 - 状态机路由
        
        状态优先级（互斥）：
        1. 创建选区 (is_selecting)
        2. 绘图中 (is_drawing)
        3. 文字拖拽 (_text_drag_active)
        4. 选区已确认 - 编辑模式
        5. 选区未确认 - 悬停预览
        """
        # 窗口关闭时 scene 可能已被清理，忽略残留的鼠标事件
        if self.canvas_scene is None or self.scene() is None:
            return
        scene_pos = self.mapToScene(event.pos())

        # ====================================================================
        # 状态1：创建选区（拖拽选框）
        # ====================================================================
        if self.is_selecting:
            self._handle_selection_drag(scene_pos)
            return
        
        # ====================================================================
        # 状态2：绘图中（使用画笔/矩形/箭头等工具）
        # ====================================================================
        if self.is_drawing:
            self._handle_drawing_move(scene_pos)
            return
        
        # ====================================================================
        # 状态3：文字拖拽（拖动文字框边缘调整大小）
        # ====================================================================
        if self._text_drag_active:
            self._handle_text_drag_move(scene_pos)
            return
        
        # ====================================================================
        # 状态4：选区已确认 - 编辑模式（智能编辑、悬停、拖拽图元）
        # ====================================================================
        if self.canvas_scene.selection_model.is_confirmed:
            self._handle_edit_mode_move(event, scene_pos)
            return
        
        # ====================================================================
        # 状态5：选区未确认 - 悬停预览（智能选区）
        # ====================================================================
        self._handle_hover_preview(scene_pos)

    # ========================================================================
    # 状态处理器：创建选区
    # ========================================================================
    
    def _handle_selection_drag(self, scene_pos: QPointF):
        """处理选区拖拽（状态1）"""
        self._update_magnifier_overlay(scene_pos)
        
        if not self.is_dragging_selection:
            dist = (scene_pos - self.start_pos).manhattanLength()
            if dist > 10:
                self.is_dragging_selection = True

        if self.is_dragging_selection:
            rect = QRectF(self.start_pos, scene_pos).normalized()
            self.canvas_scene.selection_model.set_rect(rect)
    
    # ========================================================================
    # 状态处理器：绘图模式
    # ========================================================================
    
    def _handle_drawing_move(self, scene_pos: QPointF):
        """处理绘图工具移动（状态2）
        
        绘图中放大镜已在 mousePressEvent 开始时隐藏，
        不再每帧调用 _update_magnifier_overlay 做无用判断。
        """
        self.canvas_scene.tool_controller.on_move(scene_pos)
        self._apply_tool_cursor()
    
    # ========================================================================
    # 状态处理器：文字拖拽
    # ========================================================================
    
    def _handle_text_drag_move(self, scene_pos: QPointF):
        """处理文字框边缘拖拽（状态3）"""
        self._update_magnifier_overlay(scene_pos)
        self._set_text_drag_cursor(True)
        self._perform_text_drag(scene_pos)
    
    # ========================================================================
    # 状态处理器：编辑模式（选区已确认）
    # ========================================================================
    
    def _handle_edit_mode_move(self, event, scene_pos: QPointF):
        """处理编辑模式的鼠标移动（状态4）"""
        self._track_pending_text_edit_movement(event)
        self._update_magnifier_overlay(scene_pos)
        
        # 检查是否正在编辑文字（需要检测文字框边缘拖拽）
        # 左键按住时用户可能在拖拽选文字，不应检测边缘拖拽 hover
        is_left_pressed = bool(event.buttons() & Qt.MouseButton.LeftButton)
        if self._is_text_editing() and not is_left_pressed:
            self._update_text_drag_hover(scene_pos)
        
        # 子状态1：正在拖拽控制点/手柄
        if self._handle_edit_handle_drag(scene_pos):
            return
        
        # 子状态2：悬停在控制手柄上
        if self._handle_edit_handle_hover(event, scene_pos):
            return
        
        # 子状态3：拖拽已选中的图元
        if self._handle_selected_item_drag(event, scene_pos):
            return
        
        # 子状态4：悬停检测（是否在可编辑图元上）
        self._handle_edit_hover_detection(event, scene_pos)
    
    def _handle_edit_handle_drag(self, scene_pos: QPointF) -> bool:
        """处理控制点/手柄拖拽（编辑模式子状态1）"""
        edit_move_handled = self.smart_edit_controller.handle_edit_move(scene_pos)
        if edit_move_handled:
            if self.smart_edit_controller.layer_editor.dragging_handle:
                dragging_cursor = self.smart_edit_controller.layer_editor.get_cursor(scene_pos)
                self.setCursor(dragging_cursor)
            return True
        return False
    
    def _handle_edit_handle_hover(self, event, scene_pos: QPointF) -> bool:
        """处理控制手柄悬停（编辑模式子状态2）"""
        if not self.smart_edit_controller.selected_item:
            return False
        
        if not self.smart_edit_controller.layer_editor.is_editing():
            return False
        
        is_left_button_pressed = bool(event.buttons() & Qt.MouseButton.LeftButton)
        is_moving_item = getattr(self.smart_edit_controller.layer_editor, "is_moving_item", False)
        
        if is_left_button_pressed and is_moving_item:
            return False
        
        self.smart_edit_controller.layer_editor.update_hover(scene_pos)
        if self.smart_edit_controller.layer_editor.hovered_handle:
            handle_cursor = self.smart_edit_controller.layer_editor.get_cursor(scene_pos)
            self.setCursor(handle_cursor)
            return True
        
        return False
    
    def _handle_selected_item_drag(self, event, scene_pos: QPointF) -> bool:
        """处理已选中图元的拖拽（编辑模式子状态3）"""
        if not self.smart_edit_controller.selected_item:
            return False
        
        selected_item = self.smart_edit_controller.selected_item
        is_on_selected_item = selected_item.contains(
            selected_item.mapFromScene(scene_pos)
        )
        is_left_button_pressed = bool(event.buttons() & Qt.MouseButton.LeftButton)
        
        if is_left_button_pressed:
            # 按住左键 → 正在拖拽
            # 但如果选中的是文字图元且处于编辑模式，左键拖拽是在选文字，不是移动图元
            is_text_editing = self._is_text_editing() and isinstance(selected_item, QGraphicsTextItem)
            if is_text_editing:
                # 文字编辑中拖拽 = 选中文字，保持工字光标，交由 Qt 原生处理
                self.setCursor(Qt.CursorShape.IBeamCursor)
                super().mouseMoveEvent(event)
                return True
            self.smart_edit_controller.handle_move(event.pos(), scene_pos)
            self.setCursor(Qt.CursorShape.SizeAllCursor)
            super().mouseMoveEvent(event)
            self._update_edit_handles()
            return True
        elif is_on_selected_item:
            # 悬停在选中的图元上
            if self._is_text_editing() and isinstance(selected_item, QGraphicsTextItem):
                # 文字编辑模式：光标由 TextItem.hoverMoveEvent 负责（工字/拖拽区分）
                super().mouseMoveEvent(event)
                return True
            # 非文字：十字光标
            self.setCursor(Qt.CursorShape.CrossCursor)
            super().mouseMoveEvent(event)
            return True
        
        return False
    
    def _handle_edit_hover_detection(self, event, scene_pos: QPointF):
        """处理悬停检测（编辑模式子状态4）"""
        is_hovering = self.smart_edit_controller.handle_hover(event.pos(), scene_pos)
        if is_hovering:
            self.setCursor(Qt.CursorShape.CrossCursor)
        else:
            self._apply_tool_cursor()
        
        super().mouseMoveEvent(event)
    
    # ========================================================================
    # 状态处理器：悬停预览（选区未确认）
    # ========================================================================
    
    def _handle_hover_preview(self, scene_pos: QPointF):
        """处理智能选区悬停预览（状态5）"""
        self._update_magnifier_overlay(scene_pos)
        
        if self.smart_selection_enabled:
            smart_rect = self._get_smart_selection_rect(scene_pos)
            if not smart_rect.isEmpty():
                self.canvas_scene.selection_model.activate()
                if not self.canvas_scene.selection_model.is_dragging:
                    self.canvas_scene.selection_model.start_dragging()
                self.canvas_scene.selection_model.set_rect(smart_rect)
            else:
                if self.canvas_scene.selection_model.is_dragging:
                    self.canvas_scene.selection_model.stop_dragging()
        else:
            if self.canvas_scene.selection_model.is_dragging:
                self.canvas_scene.selection_model.stop_dragging()
    
    def _apply_tool_cursor(self):
        """应用当前工具的光标（SVG 光标）"""
        if (self.cursor_manager and 
            self.cursor_manager.current_cursor and
            self.cursor_manager.current_tool_id != "cursor"):
            self.setCursor(self.cursor_manager.current_cursor)

    @safe_event
    def leaveEvent(self, event):
        """鼠标离开画布时隐藏放大镜"""
        self._clear_magnifier_overlay()
        super().leaveEvent(event)
    
    def _update_edit_handles(self):
        """更新编辑控制点位置（图元移动后调用）"""
        if self.smart_edit_controller.layer_editor.is_editing():
            item = self.smart_edit_controller.selected_item
            if item:
                # 重新生成控制点
                self.smart_edit_controller.layer_editor.handles = \
                    self.smart_edit_controller.layer_editor._generate_handles(item)
                
                # 优化：只更新受影响的区域，而不是全场景重绘
                rect = self.smart_edit_controller.layer_editor._get_scene_rect(item)
                if rect:
                    margin = 25  # 手柄大小 + 缓冲
                    update_rect = rect.adjusted(-margin, -margin, margin, margin)
                    self.canvas_scene.update(update_rect)
                else:
                    self.canvas_scene.update()
    
    @safe_event
    def mouseReleaseEvent(self, event):
        """
        鼠标释放
        
        逻辑：
        1. is_selecting=True → 完成选区创建，确认选区
        2. is_drawing=True → 完成绘图，调用工具的 on_release
        3. 智能编辑控制点拖拽 → LayerEditor 处理
        4. 其他情况 → 智能编辑 + 传递给 Scene
        """
        scene_pos = self.mapToScene(event.pos())
        
        if self._text_drag_active:
            self._end_text_drag()
            return

        if self.is_selecting:
            self.is_selecting = False
            # 结束拖拽，显示控制点
            self.canvas_scene.selection_model.stop_dragging()
            # 确认选区
            self.canvas_scene.confirm_selection()
            return
        
        if self.is_drawing:
            self.is_drawing = False
            self.canvas_scene.tool_controller.on_release(scene_pos)
            # 绘图结束，恢复放大镜跟踪（如果此时 _should_render 允许显示）
            self._update_magnifier_overlay(scene_pos)
            return
        
        # 检查是否在释放控制点拖拽
        edit_release_handled = self.smart_edit_controller.handle_edit_release(
            scene_pos, event.button()
        )
        
        if edit_release_handled:
            # 控制点拖拽释放，不传递事件
            return
        
        # 智能编辑：处理释放
        self.smart_edit_controller.handle_release(event.pos(), scene_pos, event.button())
        
        # 如果有选中的图元，更新控制点（可能刚拖拽完成）
        if self.smart_edit_controller.selected_item:
            self._update_edit_handles()
        
        self._maybe_enter_text_edit_on_release(event, scene_pos)
        # 传递给场景处理（可能是在释放图元拖拽）
        super().mouseReleaseEvent(event)
    
    @safe_event
    def wheelEvent(self, event):
        """
        鼠标滚轮事件 - 调整画笔大小或放大镜倍数
        """
        # 只在绘图工具激活时响应
        current_tool = self.canvas_scene.tool_controller.current_tool
        if not current_tool or current_tool.id == "cursor":
            # 无绘图工具激活时，尝试调整放大镜倍数
            window = self.window()
            # 确保不是钉图窗口（钉图窗口没有放大镜）
            if (hasattr(window, 'magnifier_overlay') and 
                window.magnifier_overlay and 
                window.magnifier_overlay.cursor_scene_pos is not None and 
                window.magnifier_overlay._should_render()):
                # 获取滚轮方向
                delta = event.angleDelta().y()
                if delta != 0:
                    # 向上滚动增加倍数，向下滚动减少倍数
                    zoom_delta = 1 if delta > 0 else -1
                    window.magnifier_overlay.adjust_zoom(zoom_delta)
                    event.accept()
                    return
            super().wheelEvent(event)
            return
            
        # 获取滚轮方向
        delta = event.angleDelta().y()
        modifiers = event.modifiers()

        # Ctrl + 滚轮：调整透明度（0-255）
        if modifiers & Qt.KeyboardModifier.ControlModifier:
            if delta != 0:
                ctx = self.canvas_scene.tool_controller.context
                current_opacity = ctx.opacity if ctx.opacity is not None else 1.0
                step = 0.05
                new_opacity = current_opacity + (step if delta > 0 else -step)
                new_opacity = max(0.0, min(1.0, float(new_opacity)))

                self.canvas_scene.tool_controller.update_style(opacity=new_opacity)
                self._apply_opacity_change_to_selection(new_opacity)

                toolbar = getattr(self.window(), 'toolbar', None)
                if toolbar and hasattr(toolbar, 'set_opacity'):
                    toolbar.set_opacity(int(round(new_opacity * 255)))

            event.accept()
            return

        # Shift + 滚轮：调整序号工具的下一次数字
        if (
            current_tool.id == "number"
            and modifiers & Qt.KeyboardModifier.ShiftModifier
        ):
            if delta != 0:
                step = 1 if delta > 0 else -1
                ctx = self.canvas_scene.tool_controller.context
                next_value = current_tool.adjust_next_number(ctx.scene, step)
                if getattr(self, "cursor_manager", None):
                    self.cursor_manager.set_tool_cursor("number", force=True)
                toolbar = getattr(self.window(), "toolbar", None)
                if toolbar and hasattr(toolbar, "set_number_next_value"):
                    toolbar.set_number_next_value(next_value)
            event.accept()
            return
        
        # 特殊处理文字工具
        if current_tool.id == "text":
            toolbar = self.window().toolbar if hasattr(self.window(), "toolbar") else None
            controller = getattr(self, "smart_edit_controller", None)
            active_text = self._get_active_text_item()
            selected_text = None
            if controller and isinstance(controller.selected_item, (TextItem, QGraphicsTextItem)):
                selected_text = controller.selected_item

            # 获取当前字号：优先取正在编辑或选中的文字，若没有则退回面板，最后用默认值
            current_size = None
            if active_text:
                current_size = self._get_text_point_size(active_text)
            elif selected_text:
                current_size = self._get_text_point_size(selected_text)
            elif toolbar and hasattr(toolbar, "text_menu"):
                current_size = toolbar.text_menu.size_spin.value()
            if current_size is None:
                current_size = 16
            
            # 调整字号（每次滚动 ±2）
            step = 2
            if delta > 0:
                new_size = min(current_size + step, 144)
            else:
                new_size = max(current_size - step, 8)

            self._apply_text_point_size(active_text, selected_text, new_size)
            if toolbar and hasattr(toolbar, "text_menu"):
                toolbar.text_menu.size_spin.setValue(int(new_size))
                
            event.accept()
            return
        
        # 获取当前笔触宽度
        ctx = self.canvas_scene.tool_controller.context
        current_width = max(1.0, float(ctx.stroke_width))
        
        # 调整宽度（每次滚动 ±1，范围 1-50）
        if delta > 0:
            new_width = min(current_width + 1, 50)
        else:
            new_width = max(current_width - 1, 1)
        
        # 更新笔触宽度
        self.canvas_scene.tool_controller.update_style(width=int(new_width))
        
        # 更新光标上的虚线圈大小
        self.cursor_manager.update_tool_cursor_size(int(new_width))

        # 同步到当前选中的图元（若有）
        scale = new_width / current_width if current_width > 0 else 1.0
        if abs(scale - 1.0) > 1e-6:
            self._apply_size_change_to_selection(scale)
        
        # 同步到 toolbar 的滑块
        toolbar = getattr(self.window(), 'toolbar', None)
        if toolbar and hasattr(toolbar, 'set_stroke_width'):
            toolbar.set_stroke_width(int(new_width))
        
        event.accept()
    
    @safe_event
    def keyPressEvent(self, event):
        """
        键盘事件
        
        View 只处理文字编辑模式下的特殊按键（回车换行）。
        所有窗口级快捷键（ESC、Enter确认、Ctrl+Z/Y、Ctrl+C/D 等）
        统一由 ScreenshotWindow.keyPressEvent 处理，避免重复和冲突。
        """
        is_text_editing = self._is_text_editing()

        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            # 文字编辑模式下允许换行，不传递给父窗口
            if is_text_editing:
                super().keyPressEvent(event)
                return

        if is_text_editing:
            # 文字编辑模式下，Ctrl+Z/Y 交给 QGraphicsTextItem 处理内部撤销
            if (event.key() in (Qt.Key.Key_Z, Qt.Key.Key_Y)
                    and event.modifiers() == Qt.KeyboardModifier.ControlModifier):
                super().keyPressEvent(event)
                return
            # 文字编辑模式下，所有普通按键交给 QGraphicsTextItem 处理
            super().keyPressEvent(event)
            return

        # 非文字编辑模式：其余按键一律 ignore，让事件冒泡到父窗口统一处理
        event.ignore()

    def _is_text_editing(self) -> bool:
        """判断当前是否在编辑文字图元"""
        return self._get_active_text_item() is not None

    def _get_active_text_item(self):
        focus_item = self.canvas_scene.focusItem() if hasattr(self.canvas_scene, 'focusItem') else None
        if isinstance(focus_item, QGraphicsTextItem) and focus_item.hasFocus():
            flags = focus_item.textInteractionFlags()
            if bool(flags & Qt.TextInteractionFlag.TextEditorInteraction):
                return focus_item
        return None

    def _is_point_on_text_edge(self, item: QGraphicsTextItem, scene_pos: QPointF, margin: float = None) -> bool:
        if not item:
            return False
        # 使用 TextItem 的 document margin 作为边缘判定区域
        if margin is None:
            margin = getattr(item, 'TEXT_PADDING', 12)
        rect = item.mapToScene(item.boundingRect()).boundingRect()
        if not rect.contains(scene_pos):
            return False
        inner = rect.adjusted(margin, margin, -margin, -margin)
        if inner.width() <= 0 or inner.height() <= 0:
            return True
        return not inner.contains(scene_pos)

    def _set_text_drag_cursor(self, active: bool):
        if active:
            self._text_drag_cursor_active = True
            self.setCursor(Qt.CursorShape.SizeAllCursor)
        else:
            if not self._text_drag_cursor_active:
                return
            self._text_drag_cursor_active = False
            if self._is_text_editing():
                self.viewport().unsetCursor()
            elif (
                self.cursor_manager
                and self.cursor_manager.current_cursor
                and self.cursor_manager.current_tool_id != "cursor"
            ):
                self.setCursor(self.cursor_manager.current_cursor)
            else:
                self.setCursor(Qt.CursorShape.ArrowCursor)

    def _update_text_drag_hover(self, scene_pos: QPointF):
        if self._text_drag_active:
            return
        if not self._is_text_editing():
            if self._text_drag_hover_item is not None:
                self._text_drag_hover_item = None
                self._set_text_drag_cursor(False)
            return
        item = self._get_active_text_item()
        if item and self._is_point_on_text_edge(item, scene_pos):
            self._text_drag_hover_item = item
            self._set_text_drag_cursor(True)
        else:
            self._text_drag_hover_item = None
            self._set_text_drag_cursor(False)

    def _begin_text_drag(self, item: QGraphicsTextItem, scene_pos: QPointF):
        self._clear_pending_text_edit()
        self._text_drag_active = True
        self._text_drag_item = item
        self._text_drag_last_scene_pos = scene_pos
        self._set_text_drag_cursor(True)
        if self.smart_edit_controller:
            self.smart_edit_controller.select_item(item, auto_select=False)

    def _perform_text_drag(self, scene_pos: QPointF):
        if not self._text_drag_active or not self._text_drag_item:
            return
        if not self._text_drag_last_scene_pos:
            self._text_drag_last_scene_pos = scene_pos
            return
        delta = scene_pos - self._text_drag_last_scene_pos
        if abs(delta.x()) < 1e-3 and abs(delta.y()) < 1e-3:
            return
        self._text_drag_item.moveBy(delta.x(), delta.y())
        self._text_drag_last_scene_pos = scene_pos
        self.canvas_scene.update()

    def _end_text_drag(self):
        self._text_drag_active = False
        self._text_drag_item = None
        self._text_drag_last_scene_pos = None
        self._set_text_drag_cursor(False)

    def _reset_text_drag_state(self):
        self._text_drag_hover_item = None
        self._text_drag_item = None
        self._text_drag_last_scene_pos = None
        self._text_drag_active = False
        self._set_text_drag_cursor(False)

    def _apply_size_change_to_selection(self, scale: float):
        controller = getattr(self, "smart_edit_controller", None)
        if not controller or scale <= 0:
            return

        handled_text = False
        active_text = self._get_active_text_item()
        if active_text:
            self._scale_text_item(active_text, scale)
            handled_text = True

        selected_item = getattr(controller, "selected_item", None)
        if selected_item:
            if handled_text and selected_item is active_text:
                return
            if self._scale_item_size(selected_item, scale):
                editor = controller.layer_editor
                if (
                    editor
                    and editor.is_editing()
                    and controller.selected_item is selected_item
                    and not isinstance(selected_item, TextItem)
                ):
                    editor.start_edit(selected_item)
                self.canvas_scene.update()
            else:
                pass

    def _scale_item_size(self, item, scale: float) -> bool:
        # 优先使用统一接口
        if hasattr(item, 'scale_stroke_width'):
            return item.scale_stroke_width(scale)
        # 兜底：QGraphicsTextItem（非 TextItem 子类）
        if isinstance(item, QGraphicsTextItem):
            self._scale_text_item(item, scale)
            return True
        return False

    def _scale_text_item(self, item: QGraphicsTextItem, scale: float):
        point_size = self._get_text_point_size(item)
        new_size = max(6.0, point_size * scale)
        self._set_text_item_point_size(item, new_size)
    
    def _get_text_point_size(self, item: QGraphicsTextItem) -> float:
        font = item.font()
        point_size = font.pointSizeF()
        if point_size <= 0:
            point_size = float(font.pointSize() or 12)
        return point_size

    def _set_text_item_point_size(self, item: QGraphicsTextItem, point_size: float):
        if not item:
            return
        font = item.font()
        font.setPointSizeF(max(6.0, float(point_size)))
        item.setFont(font)
        item.update()

    def _apply_text_point_size(
        self,
        active_item: Optional[QGraphicsTextItem],
        selected_item: Optional[QGraphicsTextItem],
        point_size: float,
    ):
        applied = False
        if active_item:
            self._set_text_item_point_size(active_item, point_size)
            applied = True
        if selected_item and selected_item is not active_item:
            self._set_text_item_point_size(selected_item, point_size)
            applied = True
        if applied:
            self.canvas_scene.update()

    def _apply_opacity_change_to_selection(self, opacity: float):
        controller = getattr(self, "smart_edit_controller", None)
        if not controller:
            return

        opacity = max(0.0, min(1.0, float(opacity)))

        updated = False
        active_text = self._get_active_text_item()
        if active_text:
            if self._update_item_visual_opacity(active_text, opacity):
                updated = True

        selected_item = getattr(controller, "selected_item", None)
        if selected_item and selected_item is not active_text:
            if self._update_item_visual_opacity(selected_item, opacity):
                updated = True

        if updated:
            self.canvas_scene.update()

    def _apply_line_style_change_to_selection(self, style: str):
        controller = getattr(self, "smart_edit_controller", None)
        if not controller:
            return

        selected_item = getattr(controller, "selected_item", None)
        if isinstance(selected_item, StrokeItem):
            if getattr(selected_item, "is_highlighter", False):
                return
        elif not isinstance(selected_item, (RectItem, EllipseItem)):
            return

        from PySide6.QtCore import Qt
        pen = selected_item.pen()
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        if style == "dashed":
            pen.setStyle(Qt.PenStyle.DashLine)
            pen.setDashPattern([3, 2])
        elif style == "dashed_dense":
            pen.setStyle(Qt.PenStyle.DashLine)
            pen.setDashPattern([1, 2])
        else:
            pen.setStyle(Qt.PenStyle.SolidLine)
            pen.setDashPattern([])
        selected_item.setPen(pen)
        selected_item.update()
        self.canvas_scene.update()

    def _update_item_visual_opacity(self, item, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))

        # 优先使用统一接口
        if hasattr(item, 'set_visual_opacity'):
            return item.set_visual_opacity(opacity)

        # 兜底：通用 QGraphicsItem
        if hasattr(item, "setOpacity"):
            item.setOpacity(opacity)
            item.update()
            return True

        return False
    
    def _clear_pending_text_edit(self):
        self._pending_text_edit_item = None
        self._pending_text_edit_press_pos = None
        self._pending_text_edit_moved = False

    def _update_magnifier_overlay(self, scene_pos: QPointF):
        overlay = self._get_magnifier_overlay()
        if overlay:
            overlay.update_cursor(scene_pos)

    def _clear_magnifier_overlay(self):
        overlay = self._get_magnifier_overlay()
        if overlay:
            overlay.clear_cursor()

    def _get_magnifier_overlay(self):
        window = self.window()
        if window and hasattr(window, "magnifier_overlay"):
            return window.magnifier_overlay
        return None
    
    def _maybe_prepare_text_edit(self, event, scene_pos: QPointF):
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self._is_text_editing():
            return
        item = getattr(self.smart_edit_controller, "selected_item", None)
        if not isinstance(item, TextItem):
            return
        # 只在点击位置仍在文字上时才进入待编辑状态
        if not item.contains(item.mapFromScene(scene_pos)):
            return
        self._pending_text_edit_item = item
        self._pending_text_edit_press_pos = event.pos()
        self._pending_text_edit_moved = False
    
    def _track_pending_text_edit_movement(self, event):
        if (self._pending_text_edit_item is None or
                self._pending_text_edit_press_pos is None):
            return
        if not (event.buttons() & Qt.MouseButton.LeftButton):
            return
        if (event.pos() - self._pending_text_edit_press_pos).manhattanLength() > 5:
            self._pending_text_edit_moved = True
    
    def _maybe_enter_text_edit_on_release(self, event, scene_pos: QPointF):
        if self._pending_text_edit_item is None:
            return
        if event.button() != Qt.MouseButton.LeftButton:
            self._clear_pending_text_edit()
            return
        if self._pending_text_edit_moved:
            self._clear_pending_text_edit()
            return
        item = self._pending_text_edit_item
        if not isinstance(item, TextItem):
            self._clear_pending_text_edit()
            return
        if not item.contains(item.mapFromScene(scene_pos)):
            self._clear_pending_text_edit()
            return
        self._clear_pending_text_edit()
        self._enter_text_edit_mode(item)
    
    def _enter_text_edit_mode(self, item: TextItem):
        item.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
        item.setFocus(Qt.FocusReason.MouseFocusReason)
        cursor = item.textCursor()
        cursor.movePosition(cursor.MoveOperation.End)  # 光标移到末尾，不全选
        item.setTextCursor(cursor)
        if hasattr(self.smart_edit_controller, "select_item"):
            self.smart_edit_controller.select_item(item, auto_select=False)
    
    def _finalize_text_edit_state(self, text_item: QGraphicsTextItem):
        controller = getattr(self, "smart_edit_controller", None)
        if controller and controller.selected_item is text_item:
            controller.clear_selection(suppress_block=True)
        elif text_item and text_item.isSelected():
            text_item.setSelected(False)
        self._reset_text_drag_state()
        self._clear_pending_text_edit()
    
    def export_and_close(self):
        """
        导出并关闭
        """
        from core.export import ExportService
        
        # 创建导出服务（传入整个scene）
        exporter = ExportService(self.canvas_scene)
        
        # 导出选区图像
        selection_rect = self.canvas_scene.selection_model.rect()
        log_debug(f"准备导出选区: {selection_rect}", module="CanvasView")
        
        result = exporter.export(selection_rect)
        
        if result:
            log_info(f"导出成功，图像大小: {result.width()}x{result.height()}", module="CanvasView")
            from core.clipboard_utils import deliver_image_async
            deliver_image_async(result)
            log_info("已提交异步复制任务", module="CanvasView")
            self.window().close()
        else:
            log_error("导出失败！", module="CanvasView")
 