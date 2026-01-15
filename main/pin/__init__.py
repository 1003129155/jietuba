"""
钉图(贴图)模块

提供将截图固定在屏幕上的功能，支持编辑、移动、缩放

架构说明（重构后）：
- PinWindow：主窗口，只负责窗口管理和子控件布局
- PinShadowWindow：独立阴影窗口，只绘制阴影效果
- PinCanvasView：唯一内容渲染者，使用 Qt 的 GPU 加速渲染
- PinControlButtons：控制按钮管理器
- PinContextMenu：右键菜单管理器
- PinTranslationHelper：翻译功能助手
"""

from .pin_window import PinWindow
from .pin_manager import PinManager, get_pin_manager
from .pin_toolbar import PinToolbar
from .pin_canvas import PinCanvas
from .pin_canvas_view import PinCanvasView
from .pin_canvas_renderer import VectorCommandRenderer, get_vector_renderer
from .pin_controls import PinControlButtons
from .pin_context_menu import PinContextMenu
from .pin_translation import PinTranslationHelper
from .ocr_text_layer import OCRTextLayer, OCRTextItem

__all__ = [
    'PinWindow',
    'PinManager',
    'get_pin_manager',
    'PinToolbar',
    'PinCanvas',
    'PinCanvasView',
    'VectorCommandRenderer',
    'get_vector_renderer',
    'PinControlButtons',
    'PinContextMenu',
    'PinTranslationHelper',
    'OCRTextLayer',
    'OCRTextItem',
]
