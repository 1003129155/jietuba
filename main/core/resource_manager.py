"""
资源管理器 - 统一管理应用资源路径和图标缓存
"""
import os
import sys

from core.logger import log_exception

class ResourceManager:
    # 应用级 QIcon 缓存：SVG 路径 → QIcon 对象
    # QIcon 内部持有光栅化后的位图，跨 ScreenshotWindow 生命周期复用
    _icon_cache: dict = {}
    
    @staticmethod
    def get_resource_path(relative_path):
        """
        获取资源文件的绝对路径
        支持 PyInstaller 打包后的路径处理
        """
        try:
            # PyInstaller 创建临时文件夹,将路径存储在 _MEIPASS 中
            base_path = sys._MEIPASS
        except Exception as e:
            log_exception(e, "获取 _MEIPASS")
            # 开发环境：从新架构文件目录返回到根目录
            current_dir = os.path.dirname(os.path.abspath(__file__))
            # 当前在 新架构文件/core，需要向上两级到根目录
            base_path = os.path.dirname(os.path.dirname(current_dir))
            
        return os.path.join(base_path, relative_path)

    @staticmethod
    def get_icon_path(icon_name):
        """获取图标路径 - 从根目录的 svg 文件夹"""
        return ResourceManager.get_resource_path(os.path.join("svg", icon_name))

    @staticmethod
    def get_icon(svg_path: str, size: int = 0):
        """获取 QIcon（带缓存）。
        
        Args:
            svg_path: SVG 文件的绝对路径
            size: 若 > 0，则用 QSvgRenderer 渲染到 size×size 的 QPixmap，
                  确保不同 SVG 图标大小一致。若为 0（默认），直接由 Qt 处理。
            
        Returns:
            QIcon 对象
        """
        cache_key = (svg_path, size)
        icon = ResourceManager._icon_cache.get(cache_key)
        if icon is not None:
            return icon

        from PySide6.QtGui import QIcon

        if size > 0:
            try:
                from PySide6.QtCore import Qt
                from PySide6.QtGui import QPixmap, QPainter
                from PySide6.QtSvg import QSvgRenderer
                renderer = QSvgRenderer(svg_path)
                if renderer.isValid():
                    pm = QPixmap(size, size)
                    pm.fill(Qt.GlobalColor.transparent)
                    painter = QPainter(pm)
                    renderer.render(painter)
                    painter.end()
                    icon = QIcon(pm)
                else:
                    icon = QIcon()
            except Exception as e:
                log_exception(e, "SVG 渲染图标")
                icon = QIcon()
        else:
            icon = QIcon(svg_path)

        ResourceManager._icon_cache[cache_key] = icon
        return icon
 