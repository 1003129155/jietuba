"""
钉图(贴图)模块

提供将截图固定在屏幕上的功能，支持编辑、移动、缩放
"""

from .pin_window import PinWindow
from .pin_manager import PinManager, get_pin_manager
from .pin_toolbar import PinToolbar
from .pin_canvas import PinCanvas
from .pin_canvas_renderer import VectorCommandRenderer, get_vector_renderer
from .ocr_text_layer import OCRTextLayer, OCRTextItem

__all__ = [
    'PinWindow',
    'PinManager',
    'get_pin_manager',
    'PinToolbar',
    'PinCanvas',
    'VectorCommandRenderer',
    'get_vector_renderer',
    'OCRTextLayer',
    'OCRTextItem',
]
