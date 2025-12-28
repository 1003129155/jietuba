"""
OCR 模块 - 文字识别功能

提供基于 RapidOCR 的离线文字识别功能
支持中英日混合识别，完全离线运行

主要功能：
- OCRManager: OCR 管理器（单例模式）
- is_ocr_available: 检查 OCR 是否可用
- initialize_ocr: 初始化 OCR 引擎
- recognize_text: 识别图像中的文字

注意：OCRTextLayer（钉图文字选择层）已移至 pin 模块

使用示例：
    from ocr import is_ocr_available, initialize_ocr, recognize_text
    
    if is_ocr_available():
        initialize_ocr()
        result = recognize_text(pixmap, return_format="text")
        print(result)
"""

from .ocr_manager import (
    OCRManager,
    is_ocr_available,
    initialize_ocr,
    recognize_text,
    format_ocr_result_text
)

__all__ = [
    'OCRManager',
    'is_ocr_available',
    'initialize_ocr',
    'recognize_text',
    'format_ocr_result_text'
]
