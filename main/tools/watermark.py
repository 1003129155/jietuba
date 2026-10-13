"""
水印工具
"""

from datetime import datetime

from canvas.items import WatermarkItem, WatermarkStyle
from canvas.items.watermark_item import apply_watermark, expand_text
from canvas.undo import AddItemCommand

from .base import Tool, ToolContext


class WatermarkTool(Tool):
    """水印开关：工具本身不在画布上画东西，选中它只是为了打开水印的设置面板。

    铺上水印由工具栏按钮触发（add），进撤销栈；去掉水印是在面板上清空文字。
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
        """改水印文字：存成之后的默认文字。场景里已有的水印换上新文字，时间取应用的这一刻，
        和输入框里预览的一致。返回场景里的水印。"""
        scene.tool_settings_manager.set_screenshot_watermark_text(template)
        item = WatermarkItem.find(scene)
        if item is not None:
            item.set_template(template, datetime.now())
        return item

    @classmethod
    def auto_template(cls, settings_manager) -> str:
        """截图时自动加的水印文字：开着「自动加上上次的水印」、且上次截图结束时带着水印才有，否则为空。"""
        if (settings_manager is None or not settings_manager.get_screenshot_watermark_auto()
                or not settings_manager.get_screenshot_watermark_last_on()):
            return ""
        return cls.current_template(settings_manager)

    @staticmethod
    def remember(scene):
        """截图结束时记下有没有水印（文字清空了也算没有），下次截图据此决定自动加不加。"""
        item = WatermarkItem.find(scene)
        scene.tool_settings_manager.set_screenshot_watermark_last_on(item is not None and bool(item.text()))

    @classmethod
    def apply_to_image(cls, image, settings_manager, when: datetime = None):
        """不经过画布的截图（全局鼠标快捷键）按「自动加上上次的水印」直接画到图上。"""
        text = expand_text(cls.auto_template(settings_manager), when)
        return apply_watermark(image, text, cls.load_style(settings_manager)) if text else image
