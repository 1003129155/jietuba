"""
扫码模块 — 识别截图里的二维码与条形码（zxing-cpp）

- read_codes: 解码，给出每个码的内容、格式和在图上的轮廓
- show_barcode_result: 解码并弹出结果窗口，截图工具栏的「扫码」按钮走这里
"""

from .reader import DecodedCode, read_codes
from .result_window import BarcodeResultWindow, show_barcode_result

__all__ = ["DecodedCode", "read_codes", "BarcodeResultWindow", "show_barcode_result"]
