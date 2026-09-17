"""文字识别模块 — 识别截图选区里的文字（OCR），只显示、不翻译

- recognize_async: 后台识别一张图，结果报成 (文字, 失败原因键)
- shutdown_recognition: 退出前等还在跑的识别线程结束
- show_text_recognition: 识别并弹出结果窗口，截图工具栏的「文字识别」按钮走这里
"""

from .recognizer import RecognizeThread, recognize_async, shutdown_recognition
from .result_window import TextRecognitionWindow, show_text_recognition

__all__ = [
    "RecognizeThread", "recognize_async", "shutdown_recognition",
    "TextRecognitionWindow", "show_text_recognition",
]
