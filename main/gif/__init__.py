# -*- coding: utf-8 -*-
"""
gif 录制模块入口

对外暴露的主要接口：
    GifRecordWindow   — 主录制/回放窗口（入口类）

典型调用方：
    from gif import GifRecordWindow
    win = GifRecordWindow(capture_rect)
    win.show()
"""

from .record_window import GifRecordWindow

__all__ = ["GifRecordWindow"]
 