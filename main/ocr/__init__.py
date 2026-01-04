"""
OCR 模块 - 文字识别功能

支持多种 OCR 引擎：
- ocr_rs: 基于 PaddleOCR + MNN 的高性能 OCR (需要模型文件，~100MB)
- windows_media_ocr: Windows 系统自带 OCR API (轻量级，仅几MB)

主要功能：
- OCRManager: OCR 管理器（单例模式）
- is_ocr_available: 检查 OCR 是否可用
- get_available_engines: 获取可用的 OCR 引擎列表
- set_ocr_engine: 设置当前使用的 OCR 引擎
- get_current_engine: 获取当前使用的 OCR 引擎
- initialize_ocr: 初始化 OCR 引擎
- recognize_text: 识别图像中的文字

注意：OCRTextLayer（钉图文字选择层）已移至 pin 模块

使用示例：
    from ocr import is_ocr_available, get_available_engines, set_ocr_engine, initialize_ocr, recognize_text
    
    if is_ocr_available():
        engines = get_available_engines()
        print(f"可用引擎: {engines}")
        
        set_ocr_engine("windows_media_ocr")  # 切换到 Windows OCR
        initialize_ocr()
        result = recognize_text(pixmap, return_format="text")
        print(result)
"""

from .ocr_manager import (
    OCRManager,
    is_ocr_available,
    get_available_engines,
    set_ocr_engine,
    get_current_engine,
    initialize_ocr,
    recognize_text,
    release_ocr_engine,
    get_ocr_memory_status,
    format_ocr_result_text
)

__all__ = [
    'OCRManager',
    'is_ocr_available',
    'get_available_engines',
    'set_ocr_engine',
    'get_current_engine',
    'initialize_ocr',
    'recognize_text',
    'release_ocr_engine',
    'get_ocr_memory_status',
    'format_ocr_result_text'
]
