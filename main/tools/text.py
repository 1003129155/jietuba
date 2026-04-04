"""
文字工具
"""

from PySide6.QtCore import QPointF, Qt
from PySide6.QtGui import QFont, QColor
from .base import Tool, ToolContext
from canvas.items import TextItem
from canvas.undo import AddItemCommand
from core import log_debug


class TextTool(Tool):
    """
    文字工具
    """
    
    id = "text"
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button == Qt.MouseButton.LeftButton:
            # 从设置中读取字体配置
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            
            # 获取保存的设置，如果获取失败则使用默认值
            font_family = manager.get_setting("text", "font_family", "Microsoft YaHei")
            font_size = manager.get_setting("text", "font_size", 16)
            font_bold = manager.get_setting("text", "font_bold", False)
            font_italic = manager.get_setting("text", "font_italic", False)
            font_underline = manager.get_setting("text", "font_underline", False)
            background_enabled = manager.get_setting("text", "background_enabled", False)
            background_color = manager.get_setting("text", "background_color", "#FFFFFF")
            background_opacity = manager.get_setting("text", "background_opacity", 255)
            
            # 创建字体对象
            font = QFont(font_family, font_size)
            font.setBold(font_bold)
            font.setItalic(font_italic)
            font.setUnderline(font_underline)
            
            item = TextItem("", pos, font, ctx.color)
            
            # 应用默认增强效果
            # 默认开启描边，颜色为白色，提升可读性
            item.set_outline(True, color=Qt.GlobalColor.white, width=3)
            # 默认开启阴影
            item.set_shadow(True)

            # 始终设置背景颜色（即使背景未启用），这样开启背景时会使用上次保存的颜色
            bg_color = QColor(background_color)
            bg_color.setAlpha(int(background_opacity))
            item.set_background(background_enabled, bg_color, int(background_opacity))
            
            ctx.scene.addItem(item)
            
            # 提交到撤销栈
            command = AddItemCommand(ctx.scene, item)
            ctx.undo_stack.push(command)
            
            # 自动进入编辑模式，光标置于末尾（新建时文本为空，效果相同）
            item.setFocus()
            cursor = item.textCursor()
            cursor.movePosition(cursor.MoveOperation.End)
            item.setTextCursor(cursor)
            
            # 通知智能编辑控制器选中该图元（以便显示二级菜单）
            ctx.scene.item_auto_select_requested.emit(item)
            
            log_debug(f"创建文字: {pos}", "TextTool")

    def on_deactivate(self, ctx: ToolContext):
        """工具停用时，清除焦点以触发自动删除空文本"""
        if ctx.scene.focusItem():
            ctx.scene.focusItem().clearFocus()

 