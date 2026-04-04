"""
截图捕获服务 - 负责获取屏幕截图
"""

import mss
from PySide6.QtGui import QImage
from PySide6.QtCore import QRectF
from core.logger import log_debug

class CaptureService:
    """
    截图捕获服务
    负责使用 mss 获取多显示器截图
    """
    
    def capture_all_screens(self):
        """
        捕获所有屏幕
        
        Returns:
            tuple: (QImage, QRectF) 
            - QImage: 包含所有屏幕的完整截图
            - QRectF: 虚拟桌面的几何信息 (x, y, width, height)
        """
        with mss.mss() as sct:
            # monitors[0] 是所有显示器的合并区域 (虚拟桌面)
            monitors = sct.monitors
            all_monitors = monitors[0]
            
            # 截取整个虚拟桌面
            screenshot = sct.grab(all_monitors)
            
            # mss 的 screenshot.bgra 是 bytes 对象，可以直接传给 QImage
            # 使用 Format_RGB32：内存布局与 ARGB32 相同（小端 BGRA），
            # 但告知 Qt alpha 通道无意义，渲染时跳过 alpha 混合，性能更优
            img_width = screenshot.width
            img_height = screenshot.height
            bytes_per_line = img_width * 4
            
            qimage = QImage(screenshot.bgra, img_width, img_height, bytes_per_line, QImage.Format.Format_RGB32)
            
            # 拷贝一份，因为 screenshot.bgra 的生命周期依赖于 mss
            original_image = qimage.copy()
            
            # 虚拟桌面几何信息
            virtual_x = all_monitors['left']
            virtual_y = all_monitors['top']
            virtual_width = all_monitors['width']
            virtual_height = all_monitors['height']
            
            rect = QRectF(virtual_x, virtual_y, virtual_width, virtual_height)
            
            return original_image, rect
 