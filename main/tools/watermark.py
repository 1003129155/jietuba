"""
水印工具
"""

from datetime import datetime

from canvas.items import WatermarkItem, WatermarkStyle
from canvas.items.watermark_item import apply_watermark, expand_text
from canvas.undo import AddItemCommand, RemoveItemCommand

from .base import Tool, ToolContext


class WatermarkTool(Tool):
    """水印开关：工具本身不在画布上画东西，选中它只是为了打开水印的设置面板。

    铺上和去掉水印由工具栏按钮触发（add / remove），都进撤销栈。
    """

    id = "watermark"

    # 水印的颜色、字号不写进 ToolContext：下一个工具不该继承它们，样式另存在 watermark 设置下
    def load_settings(self, ctx: ToolContext):
        pass

    def save_settings(self, ctx: ToolContext):
        pass

    @staticmethod
    def load_style(settings_manager) -> WatermarkStyle:
        values = settings_manager.get_tool_settings("watermark").to_dict() if settings_manager else {}
        return WatermarkStyle(**{key: values[key] for key in WatermarkStyle.__dataclass_fields__ if key in values}).normalized()

    @staticmethod
    def current_template(settings_manager) -> str:
        return settings_manager.get_screenshot_watermark_text() if settings_manager else ""

    @classmethod
    def add(cls, scene, *, undoable: bool = True):
        """给场景铺上水印；已经有了就不再加。返回场景里的水印。"""
        existing = WatermarkItem.find(scene)
        if existing is not None:
            return existing
        manager = scene.tool_settings_manager
        item = WatermarkItem(cls.current_template(manager), cls.load_style(manager))
        if undoable:
            scene.undo_stack.push(AddItemCommand(scene, item, "Add Watermark"))
        else:
            scene.addItem(item)
        return item

    @staticmethod
    def remove(scene) -> bool:
        item = WatermarkItem.find(scene)
        if item is None:
            return False
        scene.undo_stack.push(RemoveItemCommand(scene, item, "Remove Watermark"))
        return True

    @classmethod
    def change_style(cls, scene, **changes) -> WatermarkStyle:
        """改水印样式：存成之后的默认值，场景里已有的水印跟着变。不进撤销栈，和调画笔粗细一样。"""
        manager = scene.tool_settings_manager
        style = cls.load_style(manager).with_changes(**changes)
        manager.update_settings("watermark", **vars(style))
        item = WatermarkItem.find(scene)
        if item is not None:
            item.set_style(style)
        return style

    @staticmethod
    def change_text(scene, template: str):
        """改水印文字：存成之后的默认文字，场景里已有的水印跟着换，时间仍按它加上的那一刻展开。"""
        scene.tool_settings_manager.set_screenshot_watermark_text(template)
        item = WatermarkItem.find(scene)
        if item is not None:
            item.set_template(template)

    @classmethod
    def apply_to_image(cls, image, settings_manager, when: datetime = None):
        """不经过画布的截图（全局鼠标快捷键）按「自动加水印」直接画到图上。"""
        if settings_manager is None or not settings_manager.get_screenshot_watermark_auto():
            return image
        text = expand_text(cls.current_template(settings_manager), when)
        return apply_watermark(image, text, cls.load_style(settings_manager))
