"""
资源管理器 - 统一管理应用资源路径
"""
import os
import sys

class ResourceManager:
    @staticmethod
    def get_resource_path(relative_path):
        """
        获取资源文件的绝对路径
        支持 PyInstaller 打包后的路径处理
        """
        try:
            # PyInstaller 创建临时文件夹,将路径存储在 _MEIPASS 中
            base_path = sys._MEIPASS
        except Exception:
            # 开发环境：从新架构文件目录返回到根目录
            current_dir = os.path.dirname(os.path.abspath(__file__))
            # 当前在 新架构文件/core，需要向上两级到根目录
            base_path = os.path.dirname(os.path.dirname(current_dir))
            
        return os.path.join(base_path, relative_path)

    @staticmethod
    def get_icon_path(icon_name):
        """获取图标路径 - 从根目录的 svg 文件夹"""
        return ResourceManager.get_resource_path(os.path.join("svg", icon_name))
