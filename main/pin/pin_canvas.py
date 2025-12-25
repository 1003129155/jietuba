"""
钉图画布 - 核心类
🔥 新架构：完整复用截图窗口的 CanvasScene

特点：
- ✅ 完整的撤销/重做功能 (QUndoStack + Ctrl+Z/Shift+Z)
- ✅ 完整的工具系统 (7种绘图工具，直接复用)
- ✅ 完整的命令管理 (CommandUndoStack)
- ✅ 完整的图层系统 (QGraphicsScene + Z-Order)
- ✅ 完整的样式管理 (颜色、线宽、透明度)
"""

from PyQt6.QtCore import Qt, QObject, pyqtSignal, QRectF, QPointF
from PyQt6.QtGui import QPainter, QImage, QColor, QPixmap, QTransform

from canvas import CanvasScene


class PinCanvas(QObject):
    """
    钉图画布
    
    🔥 完整复用 CanvasScene 架构，无需自己实现工具系统
    """
    
    # 信号
    commands_changed = pyqtSignal()  # 命令列表变更（兼容旧代码）
    
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
        self.background_image = background_image
        
        # 🔥 创建 CanvasScene（完整复用截图窗口的架构）
        scene_rect = QRectF(0, 0, base_size.width(), base_size.height())
        self.scene = CanvasScene(background_image, scene_rect)
        
        # 预置选区（钉图画布默认全图可编辑）
        self._initialize_selection()
        
        # 🔥 快捷访问（用于工具栏连接）
        self.undo_stack = self.scene.undo_stack           # 撤销栈
        self.tool_controller = self.scene.tool_controller # 工具控制器
        
        # 编辑状态（兼容旧代码）
        self.is_editing = False
        self._is_drawing = False
        
        print(f"🎨 [钉图画布] 创建成功，基准尺寸 {base_size.width()}×{base_size.height()}，使用完整 CanvasScene 架构")
        
        # 🔥 连接场景信号，监听图层变化
        self.scene.changed.connect(self._on_scene_changed)
    
    def initialize_from_items(self, drawing_items, selection_offset):
        """
        从截图窗口继承绘制项目（向量数据）
        
        Args:
            drawing_items: 绘制项目列表（QGraphicsItem）
            selection_offset: 选区在原场景中的偏移量（QPoint，用于坐标转换）
        """
        if not drawing_items:
            print("📊 [钉图画布] 没有绘制项目需要继承")
            return
        
        print(f"📊 [钉图画布] 开始继承 {len(drawing_items)} 个绘制项目...")
        
        # 计算偏移量（将截图场景坐标转换为钉图场景坐标）
        offset_x = -selection_offset.x()
        offset_y = -selection_offset.y()
        
        inherited_count = 0
        for item in drawing_items:
            try:
                # 获取原始项目信息
                item_type = type(item).__name__
                item_pos = item.pos()
                print(f"    🔄 克隆项目: {item_type}, 原始位置: ({item_pos.x():.1f}, {item_pos.y():.1f})")
                
                # 🔥 克隆图形项（深拷贝）
                cloned_item = self._clone_graphics_item(item)
                
                if cloned_item:
                    self._apply_static_item_state(item, cloned_item, offset_x, offset_y)
                    base_state = self._capture_item_state(cloned_item)

                    print(f"    ✅ 克隆成功: {item_type}, 新位置: ({cloned_item.pos().x():.1f}, {cloned_item.pos().y():.1f}), Z值: {cloned_item.zValue()}")

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
                    print(f"    ❌ 克隆失败: {item_type} - 返回None")
                    
            except Exception as e:
                print(f"⚠️ [钉图画布] 继承项目失败: {e}")
                import traceback
                traceback.print_exc()
        
        print(f"✅ [钉图画布] 成功继承 {inherited_count}/{len(drawing_items)} 个绘制项目")
        
        # 🔥 打印撤销栈状态
        self.undo_stack.print_stack_status()
    
    def _clone_graphics_item(self, item):
        """
        克隆 QGraphicsItem（深拷贝）
        
        Args:
            item: 原始图形项
            
        Returns:
            克隆的图形项，如果失败返回 None
        """
        from PyQt6.QtGui import QPen
        from PyQt6.QtCore import QPointF, QRectF
        
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
                print(f"⚠️ [钉图画布] 不支持的item类型: {item_type}")
                return None
                
        except Exception as e:
            print(f"⚠️ [钉图画布] 克隆item失败 ({item_type}): {e}")
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
        except Exception:
            pass

        try:
            if hasattr(source_item, "zValue") and hasattr(cloned_item, "setZValue"):
                cloned_item.setZValue(source_item.zValue())
        except Exception:
            pass

        try:
            if hasattr(source_item, "opacity") and hasattr(cloned_item, "setOpacity"):
                cloned_item.setOpacity(float(source_item.opacity()))
        except Exception:
            pass

    def _capture_item_state(self, item):
        state = {}
        if hasattr(item, "pos"):
            pos = item.pos()
            state["pos"] = QPointF(pos.x(), pos.y())
        if hasattr(item, "transform"):
            try:
                state["transform"] = QTransform(item.transform())
            except Exception:
                pass
        if hasattr(item, "rotation"):
            try:
                state["rotation"] = float(item.rotation())
            except Exception:
                pass
        if hasattr(item, "transformOriginPoint"):
            try:
                origin = item.transformOriginPoint()
                state["transformOriginPoint"] = QPointF(origin.x(), origin.y())
            except Exception:
                pass
        if hasattr(item, "opacity"):
            try:
                state["opacity"] = float(item.opacity())
            except Exception:
                pass
        if hasattr(item, "rect") and callable(getattr(item, "rect")):
            try:
                rect = QRectF(item.rect())
                state["rect"] = rect
            except Exception:
                pass
        if hasattr(item, "start_pos"):
            try:
                state["start"] = QPointF(item.start_pos)
            except Exception:
                pass
        if hasattr(item, "end_pos"):
            try:
                state["end"] = QPointF(item.end_pos)
            except Exception:
                pass
        return state

    def _build_final_state(self, source_item, base_state):
        final_state = dict(base_state)
        try:
            if hasattr(source_item, "transformOriginPoint"):
                origin = source_item.transformOriginPoint()
                final_state["transformOriginPoint"] = QPointF(origin.x(), origin.y())
        except Exception:
            pass

        try:
            if hasattr(source_item, "rotation"):
                final_state["rotation"] = float(source_item.rotation())
        except Exception:
            pass

        try:
            if hasattr(source_item, "transform"):
                final_state["transform"] = QTransform(source_item.transform())
        except Exception:
            pass

        try:
            if hasattr(source_item, "opacity"):
                final_state["opacity"] = float(source_item.opacity())
        except Exception:
            pass

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
        from PyQt6.QtGui import QPen
        
        # 复制路径和画笔
        path = item.path()
        pen = QPen(item.pen())
        
        # 创建克隆
        cloned = StrokeItem(path, pen, item.is_highlighter)
        return cloned
    
    def _clone_rect_item(self, item):
        """克隆矩形项目"""
        from canvas.items.drawing_items import RectItem
        from PyQt6.QtGui import QPen
        from PyQt6.QtCore import QRectF
        
        # 复制矩形和画笔
        rect = QRectF(item.rect())
        pen = QPen(item.pen())
        
        cloned = RectItem(rect, pen)
        return cloned
    
    def _clone_ellipse_item(self, item):
        """克隆椭圆项目"""
        from canvas.items.drawing_items import EllipseItem
        from PyQt6.QtGui import QPen
        from PyQt6.QtCore import QRectF
        
        # 复制椭圆和画笔
        rect = QRectF(item.rect())
        pen = QPen(item.pen())
        
        cloned = EllipseItem(rect, pen)
        return cloned
    
    def _clone_arrow_item(self, item):
        """克隆箭头项目"""
        from canvas.items.drawing_items import ArrowItem
        from PyQt6.QtGui import QPen, QColor
        from PyQt6.QtCore import QPointF
        
        # 创建画笔（从箭头的颜色和宽度）
        pen = QPen(QColor(item.color), item.base_width)
        
        cloned = ArrowItem(QPointF(item.start_pos), QPointF(item.end_pos), pen)
        return cloned
    
    def _clone_text_item(self, item):
        """克隆文本项目"""
        from canvas.items.drawing_items import TextItem
        from PyQt6.QtGui import QFont, QColor
        from PyQt6.QtCore import QPointF
        
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
        from PyQt6.QtGui import QColor
        from PyQt6.QtCore import QPointF
        
        cloned = NumberItem(item.number, QPointF(item.pos()), item.radius, QColor(item.color))
        return cloned
    
    # ==================== 内部回调方法 ====================
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
        if hasattr(self.scene, "overlay_mask"):
            self.scene.overlay_mask.hide()
        if hasattr(self.scene, "selection_item"):
            self.scene.selection_item.setEnabled(False)
    
    def _on_scene_changed(self, region):
        """
        场景变化时的回调（绘图、撤销、重做时触发）
        
        Args:
            region: 变化区域（QList[QRectF]）
        """
        # 触发窗口重绘
        self.parent_window.update()
        
        # 发出信号（用于外部监听）
        self.commands_changed.emit()
    
    # ==================== 渲染方法 ====================
    
    def render_to_painter(self, painter: QPainter, target_rect):
        """
        渲染场景到 painter
        
        Args:
            painter: QPainter 对象（来自 paintEvent）
            target_rect: 目标矩形（窗口坐标，可以是 QRect 或 QRectF）
        
        🔥 直接使用 QGraphicsScene.render()，自动处理所有图层
        """
        # 保存 painter 状态
        painter.save()
        
        # 🔥 转换为 QRectF（scene.render() 需要 QRectF）
        if not isinstance(target_rect, QRectF):
            target_rect = QRectF(target_rect)
        
        # 🔥 场景渲染：QGraphicsScene 自动渲染所有图层（背景+蒙版+选区+绘图图元）
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
        
        🔥 直接使用 tool_controller.activate_tool()
        """
        print(f"🔧 [钉图画布] 激活工具: {tool_name}")
        
        # 映射工具名（兼容性处理）
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
            # 🔥 直接调用 tool_controller
            self.tool_controller.activate_tool(mapped_tool)
            editing_mode = mapped_tool != "cursor"
            self.is_editing = editing_mode
            self.parent_window._is_editing = editing_mode
            self._is_drawing = False
            if getattr(self.parent_window, 'toolbar', None):
                self.parent_window.toolbar.on_parent_editing_state_changed(editing_mode)
            print(f"✅ [钉图画布] 工具激活成功: {mapped_tool}")
        except Exception as e:
            print(f"❌ [钉图画布] 工具激活失败: {e}")
            import traceback
            traceback.print_exc()
            self.is_editing = False
            self.parent_window._is_editing = False
            if getattr(self.parent_window, 'toolbar', None):
                self.parent_window.toolbar.on_parent_editing_state_changed(False)
    
    def deactivate_tool(self):
        """退出编辑模式"""
        print("🔧 [钉图画布] 退出编辑模式")
        
        # 🔥 切换到 cursor 工具（默认工具）
        # 在清理阶段，如果scene已经被清理，跳过工具切换
        if self.scene and not self.scene.items():
            # Scene已被清理，直接重置状态
            print("⚠️ [钉图画布] Scene已清理，跳过工具切换")
            self.is_editing = False
            self._is_drawing = False
            self.parent_window._is_editing = False
            return
        
        try:
            self.tool_controller.activate_tool("cursor")
        except RuntimeError as e:
            print(f"⚠️ [钉图画布] 工具切换失败（可能正在清理）: {e}")
        
        self.is_editing = False
        self._is_drawing = False
        self.parent_window._is_editing = False
        if getattr(self.parent_window, 'toolbar', None):
            self.parent_window.toolbar.on_parent_editing_state_changed(False)

    def _map_window_pos_to_scene(self, pos: QPointF) -> QPointF:
        """将窗口坐标转换为场景坐标"""
        window = self.parent_window
        if window is None:
            return QPointF(pos)
        width = max(1, window.width())
        height = max(1, window.height())
        x_ratio = pos.x() / width
        y_ratio = pos.y() / height
        scene_x = max(0.0, min(self.base_size.width(), x_ratio * self.base_size.width()))
        scene_y = max(0.0, min(self.base_size.height(), y_ratio * self.base_size.height()))
        return QPointF(scene_x, scene_y)
    
    def handle_mouse_press(self, event):
        """
        处理鼠标按下事件（编辑模式）
        
        🔥 CanvasView 自动处理鼠标事件，这里不需要实现
        但为了兼容 PinWindow 的调用，返回 True 阻止拖动
        """
        if not self.is_editing:
            return False  # 非编辑模式，允许拖动
        
        scene_pos = self._map_window_pos_to_scene(event.position())
        self.tool_controller.on_press(scene_pos, event.button())
        if event.button() == Qt.MouseButton.LeftButton:
            self._is_drawing = True
        print(f"🖱️ [钉图画布] 鼠标按下（编辑模式）")
        return True  # 阻止拖动
    
    def handle_mouse_move(self, event):
        """处理鼠标移动事件（编辑模式）"""
        if not self.is_editing:
            return False  # 非编辑模式，允许拖动
        
        if self._is_drawing:
            scene_pos = self._map_window_pos_to_scene(event.position())
            self.tool_controller.on_move(scene_pos)
        
        return True  # 阻止拖动
    
    def handle_mouse_release(self, event):
        """处理鼠标释放事件（编辑模式）"""
        if not self.is_editing:
            return False  # 非编辑模式，允许拖动
        
        scene_pos = self._map_window_pos_to_scene(event.position())
        self.tool_controller.on_release(scene_pos)
        self._is_drawing = False
        print(f"🖱️ [钉图画布] 鼠标释放（编辑模式）")
        return True  # 阻止拖动
    
    # ==================== 样式管理方法 ====================
    
    def set_color(self, color: QColor):
        """设置当前颜色"""
        self.tool_controller.set_color(color)
    
    def set_stroke_width(self, width: int):
        """设置当前线宽"""
        self.tool_controller.set_stroke_width(width)
    
    def set_opacity(self, opacity: float):
        """设置当前透明度"""
        self.tool_controller.set_opacity(opacity)
    
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
        
        # 🔥 渲染场景
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
        print("🧹 [钉图画布] 清理资源...")
        
        # 先退出编辑模式（此时scene还存在）
        self.deactivate_tool()
        
        # 清理场景（这会删除所有items）
        if self.scene:
            self.scene.clear()
            self.scene.deleteLater()
            self.scene = None
        
        print("✅ [钉图画布] 资源清理完成")

    def invalidate_cache(self):
        """兼容 PinWindow 调用，强制场景重绘"""
        if self.scene:
            self.scene.invalidate(self.scene.sceneRect())
