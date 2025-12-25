"""
工具控制器 - 管理工具切换和事件分发
"""

from PyQt6.QtCore import QPointF
from .base import Tool, ToolContext


class ToolController:
    """
    工具控制器
    """
    
    def __init__(self, ctx: ToolContext):
        self.ctx = ctx
        self.context = ctx  # 添加 context 属性供外部访问
        self.tools = {}  # 工具ID -> Tool实例
        self.current_tool = None
        self.tool_changed_callbacks = []
        
        print("[ToolController] 初始化")
    
    def add_tool_changed_callback(self, callback):
        """添加工具切换回调"""
        self.tool_changed_callbacks.append(callback)

    def register(self, tool: Tool):
        """
        注册工具
        
        Args:
            tool: 工具实例
        """
        self.tools[tool.id] = tool
        print(f"[ToolController] 注册工具: {tool.id}")
    
    def get_tool(self, tool_id: str) -> Tool:
        """
        获取工具实例
        
        Args:
            tool_id: 工具ID
            
        Returns:
            Tool实例，不存在则返回None
        """
        return self.tools.get(tool_id)
    
    def activate(self, tool_id: str):
        """
        激活工具
        
        Args:
            tool_id: 工具ID
        """
        if tool_id not in self.tools:
            print(f"[ToolController] 工具不存在: {tool_id}")
            return
        
        # 停用旧工具
        if self.current_tool:
            self.current_tool.on_deactivate(self.ctx)
        
        # 激活新工具
        self.current_tool = self.tools[tool_id]
        self.current_tool.on_activate(self.ctx)
        
        # 触发回调
        for callback in self.tool_changed_callbacks:
            try:
                callback(tool_id)
            except Exception as e:
                print(f"Error in tool changed callback: {e}")
        
        print(f"[ToolController] 激活工具: {tool_id}")

    def activate_tool(self, tool_id: str):
        """兼容旧API: 转发到 activate"""
        return self.activate(tool_id)
    
    def on_press(self, pos: QPointF, button):
        """
        鼠标按下事件
        """
        if self.current_tool:
            self.current_tool.on_press(pos, button, self.ctx)
    
    def on_move(self, pos: QPointF):
        """
        鼠标移动事件
        """
        if self.current_tool:
            self.current_tool.on_move(pos, self.ctx)
    
    def on_release(self, pos: QPointF):
        """
        鼠标释放事件
        """
        if self.current_tool:
            self.current_tool.on_release(pos, self.ctx)
    
    def update_style(self, color=None, width=None, opacity=None):
        """
        更新样式参数
        
        Args:
            color: 颜色
            width: 笔触宽度
            opacity: 透明度
        """
        if color is not None:
            self.ctx.color = color
        if width is not None:
            self.ctx.stroke_width = width
        if opacity is not None:
            self.ctx.opacity = opacity
        
        # 如果有当前工具且有设置管理器，立即保存设置
        if self.current_tool and self.ctx.settings_manager:
            self.current_tool.save_settings(self.ctx)

    def set_color(self, color):
        """兼容旧API: 设置颜色"""
        self.update_style(color=color)

    def set_stroke_width(self, width):
        """兼容旧API: 设置线宽"""
        self.update_style(width=width)

    def set_opacity(self, opacity):
        """兼容旧API: 设置透明度"""
        self.update_style(opacity=opacity)
