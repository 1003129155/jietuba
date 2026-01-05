"""
工具栏适配器 - 将 toolbar_full.py 适配到新架构
"""

from PyQt6.QtCore import QObject, pyqtSignal
from PyQt6.QtGui import QColor

from ui.toolbar import Toolbar
from tools.controller import ToolController


class ToolbarAdapter(QObject):
    """
    工具栏适配器 - 连接专业工具栏和新架构
    
    职责:
    1. 将工具栏信号转发到 ToolController
    2. 同步样式变化到 ToolContext
    3. 处理撤销/重做/保存等操作
    """
    
    # 对外信号
    save_requested = pyqtSignal()
    copy_requested = pyqtSignal()
    confirm_requested = pyqtSignal()
    
    def __init__(self, toolbar: Toolbar, tool_controller: ToolController, undo_stack):
        super().__init__()
        
        self.toolbar = toolbar
        self.tool_controller = tool_controller
        self.undo_stack = undo_stack
        
        # 工具映射(工具栏ID → 新架构ID)
        self.tool_map = {
            "pen": "pen",
            "highlighter": "highlighter",
            "arrow": "arrow",
            "number": "number",
            "rect": "rect",
            "ellipse": "ellipse",
            "text": "text",
            "eraser": "eraser",
            "mosaic": "mosaic",  # 工具栏暂无,但架构支持
        }
        
        # 连接信号
        self._connect_signals()
        
        # 连接工具控制器的工具切换回调，用于更新UI显示
        self.tool_controller.add_tool_changed_callback(self._sync_ui_on_tool_change)
        
        print("[OK] [ToolbarAdapter] 工具栏适配器初始化")
    
    def _connect_signals(self):
        """连接工具栏信号"""
        
        # 1. 工具切换
        self.toolbar.tool_changed.connect(self._on_tool_changed)
        
        # 2. 样式变化
        self.toolbar.color_changed.connect(self._on_color_changed)
        self.toolbar.stroke_width_changed.connect(self._on_stroke_width_changed)
        self.toolbar.opacity_changed.connect(self._on_opacity_changed)
        
        # 2.1 文字样式变化
        self.toolbar.text_font_changed.connect(self._on_text_font_changed)
        self.toolbar.text_color_changed.connect(self._on_text_color_changed)
        
        # 3. 撤销/重做
        self.toolbar.undo_clicked.connect(self._on_undo)
        self.toolbar.redo_clicked.connect(self._on_redo)
        
        # 4. 保存/复制/确认
        self.toolbar.save_clicked.connect(self.save_requested.emit)
        self.toolbar.copy_clicked.connect(self.copy_requested.emit)
        self.toolbar.confirm_clicked.connect(self.confirm_requested.emit)
    
    # ========================================================================
    #  信号处理
    # ========================================================================
    
    def _on_tool_changed(self, tool_id: str):
        """工具切换"""
        # 映射工具ID
        new_tool_id = self.tool_map.get(tool_id, tool_id)
        
        # 激活工具（工具的 on_activate 会自动设置光标并加载设置）
        self.tool_controller.activate(new_tool_id)
        
        print(f"[FIX] [工具切换] {tool_id} → {new_tool_id}")
    
    def _sync_ui_on_tool_change(self, tool_id: str):
        """
        工具切换后同步UI显示
        当工具切换时，工具的设置已经加载到 ToolContext 中
        这里需要将 ToolContext 的值同步到工具栏UI
        """
        ctx = self.tool_controller.ctx
        
        # 临时断开信号，避免循环触发
        self.toolbar.color_changed.disconnect(self._on_color_changed)
        self.toolbar.stroke_width_changed.disconnect(self._on_stroke_width_changed)
        self.toolbar.opacity_changed.disconnect(self._on_opacity_changed)
        
        try:
            # 更新工具栏UI显示当前工具的设置
            self.toolbar.set_current_color(ctx.color)
            self.toolbar.set_stroke_width(ctx.stroke_width)
            self.toolbar.set_opacity(int(ctx.opacity * 255))
            
            print(f"🔄 [UI同步] 工具={tool_id}, 颜色={ctx.color.name()}, 宽度={ctx.stroke_width}, 透明度={ctx.opacity}")
        finally:
            # 重新连接信号
            self.toolbar.color_changed.connect(self._on_color_changed)
            self.toolbar.stroke_width_changed.connect(self._on_stroke_width_changed)
            self.toolbar.opacity_changed.connect(self._on_opacity_changed)
    
    def _on_color_changed(self, color: QColor):
        """颜色变化"""
        self.tool_controller.update_style(color=color)
        
        # 更新光标颜色
        if hasattr(self.tool_controller.ctx, 'canvas_widget') and \
           hasattr(self.tool_controller.ctx.canvas_widget, 'cursor_manager'):
            self.tool_controller.ctx.canvas_widget.cursor_manager.update_tool_cursor_color(color)
            
        print(f"🎨 [颜色] {color.name()}")
    
    def _on_stroke_width_changed(self, width: int):
        """线宽变化"""
        ctx = self.tool_controller.ctx
        prev_width = max(1.0, float(getattr(ctx, "stroke_width", width)))
        print(f"[ToolbarAdapter] slider width change request -> prev={prev_width}, target={width}")
        self.tool_controller.update_style(width=width)
        new_width = max(1.0, float(getattr(ctx, "stroke_width", width)))
        self._apply_width_change_to_selection(prev_width, new_width)
        
        # 更新光标大小圈
        if hasattr(self.tool_controller.ctx, 'canvas_widget') and \
           hasattr(self.tool_controller.ctx.canvas_widget, 'cursor_manager'):
            self.tool_controller.ctx.canvas_widget.cursor_manager.update_tool_cursor_size(width)
        
        print(f"📏 [线宽] {width}")
    
    def _on_opacity_changed(self, opacity_255: int):
        """透明度变化(0-255)"""
        # 转换为0.0-1.0
        opacity = opacity_255 / 255.0
        print(f"[ToolbarAdapter] slider opacity change request -> target={opacity:.3f}")
        self.tool_controller.update_style(opacity=opacity)
        self._apply_opacity_change_to_selection(opacity)
        print(f"✨ [透明度] {opacity:.2f}")
        
    def _on_text_font_changed(self, font):
        """文字字体/大小变化"""
        # 更新当前选中的文字图元
        self._update_selected_text_item(font=font)
        print(f"🔤 [字体] {font.family()} {font.pointSize()}pt")

    def _on_text_color_changed(self, color):
        """文字颜色变化"""
        # 更新当前选中的文字图元
        self._update_selected_text_item(color=color)
        print(f"🎨 [文字颜色] {color.name()}")
        
    def _update_selected_text_item(self, font=None, color=None):
        """更新选中的文字图元"""
        # 获取 SmartEditController
        if not hasattr(self.tool_controller.ctx, 'scene'): return
        scene = self.tool_controller.ctx.scene
        if not hasattr(scene, 'view'): return
        view = scene.view
        if not hasattr(view, 'smart_edit_controller'): return
        
        controller = view.smart_edit_controller
        item = controller.selected_item
        
        # 检查是否是文字图元
        from canvas.items import TextItem
        if isinstance(item, TextItem):
            if font:
                item.setFont(font)
            if color:
                item.setDefaultTextColor(color)
            item.update()

    def _apply_width_change_to_selection(self, prev_width: float, new_width: float):
        if prev_width <= 0 or new_width <= 0:
            print(f"[ToolbarAdapter] skip width apply: prev={prev_width}, new={new_width}")
            return
        if abs(new_width - prev_width) <= 1e-6:
            print(f"[ToolbarAdapter] width unchanged (prev={prev_width}, new={new_width}), skip selection scale")
            return
        scene = getattr(self.tool_controller.ctx, 'scene', None)
        view = getattr(scene, 'view', None) if scene else None
        if view and hasattr(view, '_apply_size_change_to_selection'):
            scale = new_width / prev_width
            print(f"[ToolbarAdapter] applying selection scale via view: scale={scale:.3f}")
            view._apply_size_change_to_selection(scale)
        else:
            print(f"[ToolbarAdapter] missing view or method for selection scaling: view={view}")

    def _apply_opacity_change_to_selection(self, opacity: float):
        scene = getattr(self.tool_controller.ctx, 'scene', None)
        view = getattr(scene, 'view', None) if scene else None
        if view and hasattr(view, '_apply_opacity_change_to_selection'):
            view._apply_opacity_change_to_selection(opacity)
        else:
            print(f"[ToolbarAdapter] skip opacity apply: view missing helper (view={view})")
    
    def _on_undo(self):
        """撤销"""
        if self.undo_stack.canUndo():
            self.undo_stack.undo()
            print(f"↩️ [撤销] 剩余: {self.undo_stack.count()}")
    
    def _on_redo(self):
        """重做"""
        if self.undo_stack.canRedo():
            self.undo_stack.redo()
            print(f"↪️ [重做] 剩余: {self.undo_stack.count()}")
    
    # ========================================================================
    #  工具栏控制
    # ========================================================================
    
    def show_at(self, x: int, y: int):
        """显示工具栏"""
        self.toolbar.move(x, y)
        self.toolbar.show()
    
    def hide(self):
        """隐藏工具栏"""
        self.toolbar.hide()
    
    def set_tool(self, tool_id: str):
        """设置当前工具(同步到工具栏UI)"""
        # 工具栏内部会处理按钮状态
        pass
