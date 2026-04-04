"""
钉图画布 - 核心类
新架构：完整复用截图窗口的 CanvasScene

特点：
- 完整的撤销/重做功能 (QUndoStack + Ctrl+Z/Shift+Z)
- 完整的工具系统 (7种绘图工具，直接复用)
- 完整的命令管理 (CommandUndoStack)
- 完整的图层系统 (QGraphicsScene + Z-Order)
- 完整的样式管理 (颜色、线宽、透明度)
"""

from PySide6.QtCore import Qt, QObject, Signal, QRectF, QPointF
from PySide6.QtGui import QPainter, QImage, QPixmap, QTransform

from canvas import CanvasScene
from core import log_info, log_warning, log_error
from core.logger import log_exception


class PinCanvas(QObject):
    """
    钉图画布
    
    完整复用 CanvasScene 架构，无需自己实现工具系统
    """
    

    
    def __init__(self, parent_window, base_size, background_image):
        """
        Args:
            parent_window: 父窗口（PinWindow）
            base_size: 基准坐标系尺寸（QSize，画布原始尺寸）
            background_image: QImage - 背景图像（钉图的截图图像）
        """
        super().__init__(parent=parent_window)
        
        self.parent_window = parent_window
        self.base_size = base_size
        # 内存优化：不保存 background_image 引用，CanvasScene 内部会处理
        
        # 创建 CanvasScene（完整复用截图窗口的架构）
        # 注意：CanvasScene 的 BackgroundItem 已优化，只保存 QPixmap，不保存 QImage 副本
        scene_rect = QRectF(0, 0, base_size.width(), base_size.height())
        self.scene = CanvasScene(background_image, scene_rect)
        
        # 预置选区（钉图画布默认全图可编辑）
        self._initialize_selection()
        
        # 快捷访问（用于工具栏连接）
        self.undo_stack = self.scene.undo_stack           # 撤销栈
        self.tool_controller = self.scene.tool_controller # 工具控制器
        
        self.is_editing = False
        self._is_drawing = False
        

    
    def initialize_from_items(self, drawing_items, selection_offset):
        """
        从截图窗口继承绘制项目（向量数据）
        
        Args:
            drawing_items: 绘制项目列表（QGraphicsItem）
            selection_offset: 选区在原场景中的偏移量（QPoint，用于坐标转换）
        """
        if not drawing_items:
            return
        
        # 计算偏移量（将截图场景坐标转换为钉图场景坐标）
        offset_x = -selection_offset.x()
        offset_y = -selection_offset.y()
        
        inherited_count = 0
        for item in drawing_items:
            try:
                # 获取原始项目信息
                item_type = type(item).__name__
                
                # 克隆图形项（深拷贝）
                cloned_item = self._clone_graphics_item(item)
                
                if cloned_item:
                    self._apply_static_item_state(item, cloned_item, offset_x, offset_y)
                    base_state = self._capture_item_state(cloned_item)

                    # 先推入添加命令（基础绘制状态）
                    from canvas.undo import AddItemCommand, EditItemCommand
                    add_command = AddItemCommand(self.scene, cloned_item)
                    self.undo_stack.push_command(add_command)

                    # 组装最终状态（包含旋转/缩放）
                    final_state = self._build_final_state(item, base_state)
                    if not self._states_equal(base_state, final_state):
                        edit_command = EditItemCommand(cloned_item, base_state, final_state)
                        self.undo_stack.push_command(edit_command)

                    inherited_count += 1
                else:
                    log_warning(f"克隆失败: {item_type} - 返回None", "PinCanvas")
                    
            except Exception as e:
                log_warning(f"继承项目失败: {e}", "PinCanvas")
                import traceback
                traceback.print_exc()
        
        log_info(f"成功继承 {inherited_count}/{len(drawing_items)} 个绘制项目", "PinCanvas")
        
        # 打印撤销栈状态
        self.undo_stack.print_stack_status()
    
    def _clone_graphics_item(self, item):
        """
        克隆 QGraphicsItem（深拷贝）
        
        Args:
            item: 原始图形项
            
        Returns:
            克隆的图形项，如果失败返回 None
        """
        from PySide6.QtGui import QPen
        from PySide6.QtCore import QPointF, QRectF
        
        # 获取item的类型
        item_type = type(item).__name__
        
        try:
            # 从canvas.items模块导入具体的item类
            from canvas.items.drawing_items import (
                StrokeItem, RectItem, EllipseItem, ArrowItem, 
                TextItem, NumberItem
            )
            
            # 根据类型进行克隆
            if isinstance(item, StrokeItem):
                return self._clone_stroke_item(item)
            elif isinstance(item, RectItem):
                return self._clone_rect_item(item)
            elif isinstance(item, EllipseItem):
                return self._clone_ellipse_item(item)
            elif isinstance(item, ArrowItem):
                return self._clone_arrow_item(item)
            elif isinstance(item, TextItem):
                return self._clone_text_item(item)
            elif isinstance(item, NumberItem):
                return self._clone_number_item(item)
            else:
                log_warning(f"不支持的item类型: {item_type}", "PinCanvas")
                return None
                
        except Exception as e:
            log_warning(f"克隆item失败 ({item_type}): {e}", "PinCanvas")
            import traceback
            traceback.print_exc()
            return None
    
    def _apply_static_item_state(self, source_item, cloned_item, offset_x, offset_y):
        """将绘制阶段的静态状态（位置/Z值）应用到克隆项"""
        try:
            if hasattr(source_item, "pos") and hasattr(cloned_item, "setPos"):
                src_pos = source_item.pos()
                new_pos = QPointF(src_pos.x() + offset_x, src_pos.y() + offset_y)
                cloned_item.setPos(new_pos)
        except Exception as e:
            log_exception(e, "克隆项设置位置")

        try:
            if hasattr(source_item, "zValue") and hasattr(cloned_item, "setZValue"):
                cloned_item.setZValue(source_item.zValue())
        except Exception as e:
            log_exception(e, "克隆项设置Z值")

        try:
            if hasattr(source_item, "opacity") and hasattr(cloned_item, "setOpacity"):
                cloned_item.setOpacity(float(source_item.opacity()))
        except Exception as e:
            log_exception(e, "克隆项设置透明度")

    def _capture_item_state(self, item):
        state = {}
        if hasattr(item, "pos"):
            pos = item.pos()
            state["pos"] = QPointF(pos.x(), pos.y())
        if hasattr(item, "transform"):
            try:
                state["transform"] = QTransform(item.transform())
            except Exception as e:
                log_exception(e, "捕获item transform")
        if hasattr(item, "rotation"):
            try:
                state["rotation"] = float(item.rotation())
            except Exception as e:
                log_exception(e, "捕获item rotation")
        if hasattr(item, "transformOriginPoint"):
            try:
                origin = item.transformOriginPoint()
                state["transformOriginPoint"] = QPointF(origin.x(), origin.y())
            except Exception as e:
                log_exception(e, "捕获item transformOriginPoint")
        if hasattr(item, "opacity"):
            try:
                state["opacity"] = float(item.opacity())
            except Exception as e:
                log_exception(e, "捕获item opacity")
        if hasattr(item, "rect") and callable(getattr(item, "rect")):
            try:
                rect = QRectF(item.rect())
                state["rect"] = rect
            except Exception as e:
                log_exception(e, "捕获item rect")
        if hasattr(item, "start_pos"):
            try:
                state["start"] = QPointF(item.start_pos)
            except Exception as e:
                log_exception(e, "捕获item start_pos")
        if hasattr(item, "end_pos"):
            try:
                state["end"] = QPointF(item.end_pos)
            except Exception as e:
                log_exception(e, "捕获item end_pos")
        return state

    def _build_final_state(self, source_item, base_state):
        final_state = dict(base_state)
        try:
            if hasattr(source_item, "transformOriginPoint"):
                origin = source_item.transformOriginPoint()
                final_state["transformOriginPoint"] = QPointF(origin.x(), origin.y())
        except Exception as e:
            log_exception(e, "构建final state transformOriginPoint")

        try:
            if hasattr(source_item, "rotation"):
                final_state["rotation"] = float(source_item.rotation())
        except Exception as e:
            log_exception(e, "构建final state rotation")

        try:
            if hasattr(source_item, "transform"):
                final_state["transform"] = QTransform(source_item.transform())
        except Exception as e:
            log_exception(e, "构建final state transform")

        try:
            if hasattr(source_item, "opacity"):
                final_state["opacity"] = float(source_item.opacity())
        except Exception as e:
            log_exception(e, "构建final state opacity")

        return final_state

    def _states_equal(self, state_a, state_b):
        if state_a.keys() != state_b.keys():
            return False
        for key in state_a.keys():
            if state_a[key] != state_b[key]:
                return False
        return True

    def _clone_stroke_item(self, item):
        """克隆画笔/荧光笔项目"""
        from canvas.items.drawing_items import StrokeItem
        from PySide6.QtGui import QPen
        
        # 复制路径和画笔
        path = item.path()
        pen = QPen(item.pen())
        
        # 创建克隆
        cloned = StrokeItem(path, pen, item.is_highlighter)
        return cloned
    
    def _clone_rect_item(self, item):
        """克隆矩形项目"""
        from canvas.items.drawing_items import RectItem
        from PySide6.QtGui import QPen
        from PySide6.QtCore import QRectF
        
        # 复制矩形和画笔
        rect = QRectF(item.rect())
        pen = QPen(item.pen())
        corner_radius = item.get_corner_radius() if hasattr(item, "get_corner_radius") else 0.0
        
        cloned = RectItem(rect, pen, corner_radius)
        try:
            if hasattr(item, "brush"):
                cloned.setBrush(item.brush())
        except Exception as e:
            log_exception(e, "克隆矩形brush")

        # 保留荧光笔矩形标记
        if getattr(item, "is_highlighter_rect", False):
            cloned.is_highlighter = True
            cloned.is_highlighter_rect = True
        return cloned
    
    def _clone_ellipse_item(self, item):
        """克隆椭圆项目"""
        from canvas.items.drawing_items import EllipseItem
        from PySide6.QtGui import QPen
        from PySide6.QtCore import QRectF
        
        # 复制椭圆和画笔
        rect = QRectF(item.rect())
        pen = QPen(item.pen())
        
        cloned = EllipseItem(rect, pen)
        return cloned
    
    def _clone_arrow_item(self, item):
        """克隆箭头项目"""
        from canvas.items.drawing_items import ArrowItem
        from PySide6.QtGui import QPen, QColor
        from PySide6.QtCore import QPointF
        
        # 创建画笔（从箭头的颜色和宽度）
        pen = QPen(QColor(item.color), item.base_width)
        
        # 获取箭头样式
        arrow_style = getattr(item, '_arrow_style', 'single')
        
        cloned = ArrowItem(QPointF(item.start_pos), QPointF(item.end_pos), pen, arrow_style)
        
        # 复制弯曲状态
        if hasattr(item, '_control_modified') and item._control_modified:
            # 只有当控制点被修改过时才复制弯曲
            cloned.set_control_point(QPointF(item.control_pos))
        
        return cloned
    
    def _clone_text_item(self, item):
        """克隆文本项目"""
        from canvas.items.drawing_items import TextItem
        from PySide6.QtGui import QFont, QColor
        from PySide6.QtCore import QPointF
        
        # 获取文本属性
        text = item.toPlainText()
        pos = QPointF(item.pos())
        font = QFont(item.font())
        color = QColor(item.defaultTextColor())
        
        cloned = TextItem(text, pos, font, color)
        
        # 复制增强属性
        if hasattr(item, 'has_outline'):
            cloned.has_outline = item.has_outline
            cloned.outline_color = QColor(item.outline_color)
            cloned.outline_width = item.outline_width
        if hasattr(item, 'has_shadow'):
            cloned.has_shadow = item.has_shadow
            cloned.shadow_color = QColor(item.shadow_color)
        if hasattr(item, 'has_background'):
            cloned.has_background = item.has_background
            if hasattr(item, 'background_color'):
                cloned.background_color = QColor(item.background_color)
        
        return cloned
    
    def _clone_number_item(self, item):
        """克隆序号项目"""
        from canvas.items.drawing_items import NumberItem
        from PySide6.QtGui import QColor
        from PySide6.QtCore import QPointF
        
        cloned = NumberItem(item.number, QPointF(item.pos()), item.radius, QColor(item.color))
        return cloned
    
    # ==================== 内部方法 ====================
    def _initialize_selection(self):
        full_rect = QRectF(0, 0, self.base_size.width(), self.base_size.height())
        selection_model = self.scene.selection_model
        if hasattr(selection_model, "initialize_confirmed_rect"):
            selection_model.initialize_confirmed_rect(full_rect)
        else:
            selection_model.activate()
            selection_model.set_rect(full_rect)
            selection_model.confirm()
        if hasattr(self.scene, "selection_item"):
            self.scene.selection_item.hide()
        if hasattr(self.scene, "selection_item"):
            self.scene.selection_item.setEnabled(False)
    
    # ==================== 渲染方法 ====================
    
    def render_to_painter(self, painter: QPainter, target_rect):
        """
        渲染场景到 painter
        
        Args:
            painter: QPainter 对象（来自 paintEvent）
            target_rect: 目标矩形（窗口坐标，可以是 QRect 或 QRectF）
        
        直接使用 QGraphicsScene.render()，自动处理所有图层
        """
        # 保存 painter 状态
        painter.save()
        
        # 转换为 QRectF（scene.render() 需要 QRectF）
        if not isinstance(target_rect, QRectF):
            target_rect = QRectF(target_rect)
        
        # 场景渲染：QGraphicsScene 自动渲染所有图层（背景+蒙版+选区+绘图图元）
        source_rect = QRectF(0, 0, self.base_size.width(), self.base_size.height())
        self.scene.render(painter, target_rect, source_rect)
        
        # 恢复 painter 状态
        painter.restore()
    
    # ==================== 工具管理方法 ====================
    
    def activate_tool(self, tool_name: str):
        """
        激活绘图工具（进入编辑模式）
        
        Args:
            tool_name: 工具名称（pen, rect, arrow, text, highlighter, number, ellipse, cursor）
        
        直接使用 tool_controller.activate_tool()
        """
        # 映射工具名
        tool_map = {
            "pen": "pen",
            "highlighter": "highlighter",
            "arrow": "arrow",
            "number": "number",
            "rect": "rect",
            "ellipse": "ellipse",
            "text": "text",
            "cursor": "cursor"
        }
        
        mapped_tool = tool_map.get(tool_name, tool_name)
        try:
            # 直接调用 tool_controller
            self.tool_controller.activate(mapped_tool)
            editing_mode = mapped_tool != "cursor"
            self.is_editing = editing_mode
            self.parent_window._is_editing = editing_mode
            self._is_drawing = False
            if getattr(self.parent_window, 'toolbar', None):
                self.parent_window.toolbar.on_parent_editing_state_changed(editing_mode)
        except Exception as e:
            log_error(f"工具激活失败: {e}", "PinCanvas")
            import traceback
            traceback.print_exc()
            self.is_editing = False
            self.parent_window._is_editing = False
            if getattr(self.parent_window, 'toolbar', None):
                self.parent_window.toolbar.on_parent_editing_state_changed(False)
    
    def deactivate_tool(self):
        """退出编辑模式"""
        # 切换到 cursor 工具（默认工具）
        # 在清理阶段，如果scene已经被清理，跳过工具切换
        if self.scene and not self.scene.items():
            # Scene已被清理，直接重置状态
            self.is_editing = False
            self._is_drawing = False
            self.parent_window._is_editing = False
            return
        
        try:
            self.tool_controller.activate("cursor")
        except RuntimeError as e:
            pass  # Scene可能已被销毁，忽略错误

        self.is_editing = False
        self._is_drawing = False
        self.parent_window._is_editing = False
        if getattr(self.parent_window, 'toolbar', None):
            self.parent_window.toolbar.on_parent_editing_state_changed(False)

        # 确保非编辑状态下光标恢复为箭头
        # （ESC 路径不会触发 enterEvent，所以需要主动设置）
        view = getattr(self.parent_window, 'view', None)
        if view:
            view.setCursor(Qt.CursorShape.ArrowCursor)
            vp = view.viewport() if hasattr(view, 'viewport') else None
            if vp and vp is not view:
                vp.setCursor(Qt.CursorShape.ArrowCursor)


    
    # ==================== 工具栏信号路由 ====================

    def connect_toolbar(self, toolbar, view):
        """
        将工具栏信号连接到画布/视图（工具信号路由内聚到 PinCanvas）

        Args:
            toolbar: PinToolbar 实例
            view: PinCanvasView 实例
        """
        from core.logger import log_exception

        # 工具切换
        toolbar.tool_changed.connect(lambda name: self._on_tool_changed(name, toolbar, view))

        # 撤销/重做
        toolbar.undo_clicked.connect(self.undo_stack.undo)
        toolbar.redo_clicked.connect(self.undo_stack.redo)

        # 样式改变
        toolbar.color_changed.connect(self._on_color_changed)
        toolbar.stroke_width_changed.connect(lambda w: self._on_stroke_width_changed(w, view))
        toolbar.opacity_changed.connect(lambda o: self._on_opacity_changed(o, view))

        # 箭头样式
        toolbar.arrow_style_changed.connect(lambda s: self._on_arrow_style_changed(s, view))

        # 画笔线条样式
        toolbar.line_style_changed.connect(lambda s: self._on_line_style_changed(s, view))

        # 荧光笔模式切换 → 立即刷新光标
        if hasattr(toolbar, "paint_panel") and hasattr(toolbar.paint_panel, "highlighter_mode_changed"):
            toolbar.paint_panel.highlighter_mode_changed.connect(
                lambda mode: self._on_highlighter_mode_changed(mode, view)
            )

        # 序号工具下一数字
        if hasattr(toolbar, "number_next_changed"):
            toolbar.number_next_changed.connect(
                lambda v: self._on_number_next_changed(v, toolbar, view)
            )

        # 文字工具高级样式 → SmartEditController
        controller = getattr(view, "smart_edit_controller", None)
        if controller:
            toolbar.text_font_changed.connect(controller.on_text_font_changed)
            toolbar.text_outline_changed.connect(controller.on_text_outline_changed)
            toolbar.text_shadow_changed.connect(controller.on_text_shadow_changed)
            toolbar.text_background_changed.connect(controller.on_text_background_changed)
            toolbar.color_changed.connect(controller.on_text_color_changed)

        # 保存/复制（仍由 PinWindow 处理）
        toolbar.save_clicked.connect(self.parent_window.save_image)
        toolbar.copy_clicked.connect(self.parent_window.copy_to_clipboard)

        # 翻译按钮
        if hasattr(toolbar, 'screenshot_translate_clicked'):
            toolbar.screenshot_translate_clicked.connect(self.parent_window._on_translate_clicked)

        self.undo_stack.print_stack_status()

    # ------------------------------------------------------------------
    # 工具信号处理
    # ------------------------------------------------------------------

    def _on_tool_changed(self, tool_name: str, toolbar, view):
        """工具切换事件（内聚在 PinCanvas）"""
        from core.logger import log_exception

        if tool_name and tool_name != "cursor":
            self.activate_tool(tool_name)

            # 通知 OCR 层
            if hasattr(self.parent_window, '_ocr_mgr'):
                self.parent_window._ocr_mgr.set_drawing_mode(True)

            # 同步工具栏 UI（blockSignals 避免 set_xxx 触发回调循环）
            if toolbar and self.tool_controller:
                ctx = self.tool_controller.ctx
                toolbar.blockSignals(True)
                try:
                    toolbar.set_current_color(ctx.color)
                    toolbar.set_stroke_width(ctx.stroke_width)
                    toolbar.set_opacity(int(ctx.opacity * 255))
                    if tool_name == "number" and hasattr(toolbar, "set_number_next_value"):
                        from tools.number import NumberTool
                        toolbar.set_number_next_value(NumberTool.get_next_number(self.scene))
                finally:
                    toolbar.blockSignals(False)

            # 焦点还给 View
            from PySide6.QtCore import QTimer
            QTimer.singleShot(0, view.setFocus)
        else:
            self.deactivate_tool()
            if hasattr(self.parent_window, '_ocr_mgr'):
                self.parent_window._ocr_mgr.set_drawing_mode(False)

    def _on_color_changed(self, color):
        """颜色改变"""
        self.set_color(color)

    def _on_stroke_width_changed(self, width, view):
        """线宽改变"""
        tc = getattr(self, 'tool_controller', None)
        ctx = getattr(tc, 'context', None) if tc else None
        prev = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)
        self.set_stroke_width(width)
        new = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)
        if abs(new - prev) > 1e-6 and prev > 0 and new > 0:
            if view and hasattr(view, '_apply_size_change_to_selection'):
                view._apply_size_change_to_selection(new / prev)

    def _on_opacity_changed(self, opacity_int, view):
        """透明度改变（0-255）"""
        opacity = float(opacity_int) / 255.0
        self.set_opacity(opacity)
        if view and hasattr(view, '_apply_opacity_change_to_selection'):
            view._apply_opacity_change_to_selection(opacity)

    def _on_arrow_style_changed(self, style: str, view):
        """箭头样式改变"""
        if not view or not hasattr(view, 'smart_edit_controller'):
            return
        controller = view.smart_edit_controller
        item = controller.selected_item

        from canvas.items import ArrowItem
        if isinstance(item, ArrowItem) and hasattr(item, 'arrow_style'):
            old_state = self._capture_arrow_state(item)
            item.arrow_style = style
            new_state = self._capture_arrow_state(item)

            from canvas.undo import EditItemCommand
            if self.undo_stack:
                cmd = EditItemCommand(item, old_state, new_state, "修改箭头样式")
                self.undo_stack.push(cmd)
            item.update()

    def _on_line_style_changed(self, style: str, view):
        """线条样式改变"""
        if view and hasattr(view, '_apply_line_style_change_to_selection'):
            view._apply_line_style_change_to_selection(style)

    def _on_highlighter_mode_changed(self, mode: str, view):
        """荧光笔模式切换 → 立即刷新光标"""
        if view and hasattr(view, "cursor_manager"):
            view.cursor_manager.set_tool_cursor("highlighter", force=True)

    def _on_number_next_changed(self, next_value: int, toolbar, view):
        """序号工具下一数字变化"""
        from tools.number import NumberTool
        actual = NumberTool.set_next_number(self.scene, next_value)
        if toolbar and hasattr(toolbar, "set_number_next_value"):
            toolbar.set_number_next_value(actual)
        if view and hasattr(view, "cursor_manager"):
            view.cursor_manager.set_tool_cursor("number", force=True)

    @staticmethod
    def _capture_arrow_state(item) -> dict:
        """捕获箭头图元的状态"""
        from PySide6.QtCore import QPointF
        state = {}
        if hasattr(item, 'start_pos'):
            state['start'] = QPointF(item.start_pos)
        if hasattr(item, 'end_pos'):
            state['end'] = QPointF(item.end_pos)
        if hasattr(item, '_control_pos'):
            state['control'] = QPointF(item._control_pos)
        if hasattr(item, '_control_modified'):
            state['control_modified'] = item._control_modified
        if hasattr(item, '_arrow_style'):
            state['arrow_style'] = item._arrow_style
        return state

    # ==================== 样式管理方法 ====================

    def set_color(self, color):
        """设置当前颜色"""
        self.tool_controller.update_style(color=color)
    
    def set_stroke_width(self, width: int):
        """设置当前线宽"""
        self.tool_controller.update_style(width=width)
    
    def set_opacity(self, opacity: float):
        """设置当前透明度"""
        self.tool_controller.update_style(opacity=opacity)
    
    # ==================== 导出方法 ====================
    
    def export_to_image(self, size, dpr=1.0) -> QImage:
        """
        导出场景为 QImage
        
        Args:
            size: 图像大小（QSize）
            dpr: 设备像素比
        
        Returns:
            QImage: 渲染后的图像
        """
        # 创建图像
        image = QImage(
            int(size.width() * dpr),
            int(size.height() * dpr),
            QImage.Format.Format_ARGB32_Premultiplied
        )
        image.fill(Qt.GlobalColor.transparent)
        image.setDevicePixelRatio(dpr)
        
        # 渲染场景
        painter = QPainter(image)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
        
        target_rect = QRectF(0, 0, size.width(), size.height())
        self.render_to_painter(painter, target_rect)
        
        painter.end()
        
        return image
    
    def get_current_image(self, dpr=1.0) -> QImage:
        """
        获取当前钉图图像（背景+矢量图形）
        
        Args:
            dpr: 设备像素比
        
        Returns:
            QImage: 包含所有图层的图像
        """
        return self.export_to_image(self.base_size, dpr)
    
    # ==================== 资源清理 ====================
    
    def cleanup(self):
        """清理资源"""
        try:
            # 先退出编辑模式（此时scene还存在）
            self.deactivate_tool()
        except Exception as e:
            log_warning(f"退出编辑模式时出错: {e}", "PinCanvas")
        
        # 清理撤销栈（打破循环引用：commands → items → scene）
        if hasattr(self, 'undo_stack') and self.undo_stack:
            try:
                self.undo_stack.clear()  # 清空所有命令，释放对items的引用
            except Exception as e:
                log_warning(f"清空撤销栈时出错: {e}", "PinCanvas")
        
        # 清理工具控制器（释放当前工具对items的引用）
        if hasattr(self, 'tool_controller') and self.tool_controller:
            try:
                # 停用当前工具（如果有）
                if self.tool_controller.current_tool:
                    self.tool_controller.current_tool.on_deactivate(self.tool_controller.ctx)
                    self.tool_controller.current_tool = None
            except Exception as e:
                log_warning(f"停用工具控制器时出错: {e}", "PinCanvas")
        
        # 清理场景
        if hasattr(self, 'scene') and self.scene:
            try:
                self.scene.deleteLater()
            except Exception as e:
                log_warning(f"标记场景删除时出错: {e}", "PinCanvas")
            finally:
                self.scene = None
        

    def invalidate_cache(self):
        """兼容 PinWindow 调用，强制场景重绘"""
        if self.scene:
            self.scene.invalidate(self.scene.sceneRect())
 