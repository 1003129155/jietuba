"""
钉图管理器 - 单例模式管理所有钉图窗口实例
"""

from typing import List, Optional
from PyQt6.QtCore import QObject, pyqtSignal
from PyQt6.QtGui import QImage
from PyQt6.QtCore import QPoint


class PinManager(QObject):
    """
    钉图管理器 - 单例模式
    
    职责:
    - 创建和跟踪所有钉图窗口
    - 批量操作（关闭所有、显示所有）
    - 内存管理和清理
    """
    
    _instance = None
    
    # 信号
    pin_created = pyqtSignal(object)  # 钉图创建信号 (PinWindow)
    pin_closed = pyqtSignal(object)   # 钉图关闭信号 (PinWindow)
    all_pins_closed = pyqtSignal()    # 所有钉图关闭信号
    
    @classmethod
    def get_instance(cls):
        """获取单例实例"""
        if cls._instance is None:
            cls._instance = PinManager()
        return cls._instance
    
    def __init__(self):
        """私有构造函数（使用 get_instance() 获取实例）"""
        if PinManager._instance is not None:
            raise RuntimeError("PinManager 是单例类，请使用 get_instance() 获取实例")
        
        super().__init__()
        self.pin_windows: List = []  # 所有钉图窗口列表
        
        print("📌 [PinManager] 钉图管理器已初始化")
    
    def create_pin(
        self,
        image: QImage,
        position: QPoint,
        config_manager,
        drawing_items: Optional[List] = None,
        selection_offset: Optional[QPoint] = None
    ):
        """
        创建新钉图窗口
        
        Args:
            image: 选区底图（只包含选区的纯净背景，不含绘制）
            position: 初始位置（全局坐标）
            config_manager: 配置管理器
            drawing_items: 绘制项目列表（从截图窗口继承的向量图形）
            selection_offset: 选区在原场景中的偏移量（用于转换绘制项目坐标）
            
        Returns:
            PinWindow: 创建的钉图窗口实例
        """
        from pin.pin_window import PinWindow
        
        # 创建钉图窗口
        pin_window = PinWindow(
            image=image,
            position=position,
            config_manager=config_manager,
            drawing_items=drawing_items,
            selection_offset=selection_offset
        )
        
        # 连接关闭信号
        pin_window.closed.connect(lambda: self._on_pin_closed(pin_window))
        
        # 添加到列表
        self.pin_windows.append(pin_window)
        
        # 发送创建信号
        self.pin_created.emit(pin_window)
        
        print(f"📌 [PinManager] 钉图已创建 (共 {len(self.pin_windows)} 个)")
        
        return pin_window
    
    def _on_pin_closed(self, pin_window):
        """钉图窗口关闭回调"""
        if pin_window in self.pin_windows:
            self.pin_windows.remove(pin_window)
            self.pin_closed.emit(pin_window)
            
            print(f"🗑️ [PinManager] 钉图已关闭 (剩余 {len(self.pin_windows)} 个)")
            
            # 如果所有钉图都关闭了，发送信号
            if len(self.pin_windows) == 0:
                self.all_pins_closed.emit()
                print("✅ [PinManager] 所有钉图已关闭")
    
    def remove_pin(self, pin_window):
        """
        手动移除钉图窗口（不关闭窗口）
        
        Args:
            pin_window: 要移除的钉图窗口
        """
        if pin_window in self.pin_windows:
            self.pin_windows.remove(pin_window)
            print(f"🗑️ [PinManager] 钉图已移除 (剩余 {len(self.pin_windows)} 个)")
    
    def close_all(self):
        """关闭所有钉图窗口"""
        if len(self.pin_windows) == 0:
            print("ℹ️ [PinManager] 没有钉图窗口需要关闭")
            return
        
        print(f"🧹 [PinManager] 开始关闭 {len(self.pin_windows)} 个钉图窗口...")
        
        # 复制列表，避免在迭代时修改
        pins_to_close = self.pin_windows.copy()
        
        for pin_window in pins_to_close:
            try:
                pin_window.close_window()
            except Exception as e:
                print(f"❌ [PinManager] 关闭钉图窗口失败: {e}")
        
        # 清空列表
        self.pin_windows.clear()
        
        print("✅ [PinManager] 所有钉图窗口已关闭")
        self.all_pins_closed.emit()
    
    def get_all_pins(self) -> List:
        """
        获取所有钉图窗口
        
        Returns:
            List[PinWindow]: 钉图窗口列表
        """
        return self.pin_windows.copy()
    
    def count(self) -> int:
        """
        获取钉图数量
        
        Returns:
            int: 当前钉图数量
        """
        return len(self.pin_windows)
    
    def has_pins(self) -> bool:
        """
        是否存在钉图
        
        Returns:
            bool: 是否有钉图窗口
        """
        return len(self.pin_windows) > 0
    
    def show_all(self):
        """显示所有钉图窗口"""
        for pin_window in self.pin_windows:
            pin_window.show()
        
        print(f"👁️ [PinManager] 显示了 {len(self.pin_windows)} 个钉图窗口")
    
    def hide_all(self):
        """隐藏所有钉图窗口"""
        for pin_window in self.pin_windows:
            pin_window.hide()
        
        print(f"🙈 [PinManager] 隐藏了 {len(self.pin_windows)} 个钉图窗口")
    
    def cleanup(self):
        """清理管理器（应用退出时调用）"""
        print("🧹 [PinManager] 清理管理器...")
        self.close_all()
        PinManager._instance = None
        print("✅ [PinManager] 管理器已清理")


# 便捷函数
def get_pin_manager():
    """获取钉图管理器单例"""
    return PinManager.get_instance()
