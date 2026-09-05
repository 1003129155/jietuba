"""
钉图管理器 - 单例模式管理所有钉图窗口实例
"""

from pathlib import Path
from typing import List, Optional, Tuple
from PySide6.QtCore import Qt, QObject, Signal, QPoint
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication
from core import log_debug, log_info, log_error
from core.logger import T

import ctypes
_user32 = ctypes.windll.user32
_SWP_NOMOVE = 0x0002
_SWP_NOSIZE = 0x0001
_SWP_NOACTIVATE = 0x0010
_SWP_FLAGS = _SWP_NOMOVE | _SWP_NOSIZE | _SWP_NOACTIVATE
_HWND_TOPMOST = -1
_HWND_NOTOPMOST = -2


class PinManager(QObject):
    """
    钉图管理器 - 单例模式（仅主线程调用）
    
    职责:
    - 创建和跟踪所有钉图窗口
    - 批量操作（关闭所有、显示所有）
    - 内存管理和清理
    
    注意: 此单例仅在 Qt 主线程中使用，不保证线程安全。
    """
    
    _instance = None
    
    # 信号
    pin_created = Signal(object)  # 钉图创建信号 (PinWindow)
    pin_closed = Signal(object)   # 钉图关闭信号 (PinWindow)
    all_pins_closed = Signal()    # 所有钉图关闭信号
    
    def __new__(cls, *args, **kwargs):
        """确保单例：通过 __new__ 控制实例创建，静默返回已有实例"""
        if cls._instance is not None:
            return cls._instance
        return super().__new__(cls)
    
    @classmethod
    def instance(cls):
        """获取单例实例（仅主线程调用）"""
        if cls._instance is None:
            cls._instance = PinManager()
        return cls._instance
    
    # 保留旧方法名作为别名，确保向后兼容
    @classmethod
    def get_instance(cls):
        """获取单例实例（已弃用，请使用 instance()）"""
        return cls.instance()
    
    def __init__(self):
        # 避免 QObject.__init__ 被重复调用
        # 注意：不能用 hasattr()，因为 QObject 未初始化时会抛 RuntimeError
        if '_initialized' in self.__dict__:
            return
        super().__init__()
        self._initialized = True
        self.pin_windows: List = []  # 所有钉图窗口列表
        self._topmost_suppressed = False    # 是否已压制置顶
        self._suppressed_pins: List = []    # 被压制的 pin 窗口列表（用于精确恢复）
        
        log_info(T("钉图管理器已初始化"), "PinManager")
    
    def create_pin(
        self,
        image: QImage,
        position: QPoint,
        config_manager,
        drawing_items: Optional[List] = None,
        selection_offset: Optional[QPoint] = None,
        number_next: Optional[int] = None,
    ):
        """
        创建新钉图窗口
        
        Args:
            image: 选区底图（只包含选区的纯净背景，不含绘制）
            position: 初始位置（全局坐标）
            config_manager: 配置管理器
            drawing_items: 绘制项目列表（从截图窗口继承的向量图形）
            selection_offset: 选区在原场景中的偏移量（用于转换绘制项目坐标）
            number_next: 源场景的下一个序号值（用于同步计数器）
            
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
            selection_offset=selection_offset,
            number_next=number_next,
        )
        
        # 连接关闭信号
        pin_window.closed.connect(lambda: self._on_pin_closed(pin_window))
        
        # 添加到列表
        self.pin_windows.append(pin_window)
        
        # 发送创建信号
        self.pin_created.emit(pin_window)
        
        log_debug(T("钉图已创建 (共 {count} 个)", count=len(self.pin_windows)), "PinManager")
        
        return pin_window
    
    def _on_pin_closed(self, pin_window):
        """钉图窗口关闭回调"""
        if pin_window in self.pin_windows:
            self.pin_windows.remove(pin_window)
            self.pin_closed.emit(pin_window)
            
            log_debug(T("钉图已关闭 (剩余 {count} 个)", count=len(self.pin_windows)), "PinManager")

            # 如果所有钉图都关闭了，发送信号
            if len(self.pin_windows) == 0:
                self.all_pins_closed.emit()
                log_debug(T("所有钉图已关闭"), "PinManager")
    
    def remove_pin(self, pin_window):
        """
        手动移除钉图窗口（不关闭窗口）
        
        Args:
            pin_window: 要移除的钉图窗口
        """
        if pin_window in self.pin_windows:
            self.pin_windows.remove(pin_window)
            log_debug(T("钉图已移除 (剩余 {count} 个)", count=len(self.pin_windows)), "PinManager")
    
    def close_all(self):
        """关闭所有钉图窗口"""
        if len(self.pin_windows) == 0:
            log_debug(T("没有钉图窗口需要关闭"), "PinManager")
            return

        log_debug(T("开始关闭 {count} 个钉图窗口...", count=len(self.pin_windows)), "PinManager")

        # 复制列表，避免在迭代时修改
        pins_to_close = self.pin_windows.copy()

        for pin_window in pins_to_close:
            try:
                pin_window.close_window()
            except Exception as e:
                log_error(T("关闭钉图窗口失败: {e}", e=e), "PinManager")

        # 清空列表
        self.pin_windows.clear()

        log_debug(T("所有钉图窗口已关闭"), "PinManager")
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
        
        log_debug(T("显示了 {count} 个钉图窗口", count=len(self.pin_windows)), "PinManager")
    
    def hide_all(self):
        """隐藏所有钉图窗口"""
        for pin_window in self.pin_windows:
            pin_window.hide()
        
        log_debug(T("隐藏了 {count} 个钉图窗口", count=len(self.pin_windows)), "PinManager")

    def move_all_to_screen_center(self):
        """移动所有钉图窗口到各自所在屏幕的中心。"""
        for pin_window in self.get_all_pins():
            try:
                screen = QApplication.screenAt(pin_window.geometry().center())
                if screen is None:
                    screen = QApplication.primaryScreen()
                if screen is None:
                    continue

                screen_rect = screen.availableGeometry()
                x = screen_rect.x() + (screen_rect.width() - pin_window.width()) // 2
                y = screen_rect.y() + (screen_rect.height() - pin_window.height()) // 2
                pin_window.move(x, y)
            except Exception as e:
                log_error(T("移动钉图到屏幕中心失败: {e}", e=e), "PinManager")

        log_debug(T("已移动 {count} 个钉图到屏幕中心", count=len(self.pin_windows)), "PinManager")

    def set_all_thumbnail_mode(self, active: bool):
        """批量进入或退出缩略图模式。"""
        changed = 0
        for pin_window in self.get_all_pins():
            try:
                if bool(pin_window._thumbnail_mode) != active:
                    pin_window.toggle_thumbnail_mode()
                    changed += 1
            except Exception as e:
                log_error(T("切换钉图缩略图模式失败: {e}", e=e), "PinManager")

        if active:
            log_debug(T("{changed} 个钉图已进入缩略图模式", changed=changed), "PinManager")
        else:
            log_debug(T("{changed} 个钉图已退出缩略图模式", changed=changed), "PinManager")

    def save_all_to_directory(self, directory: str, prefix: str = "pins") -> Tuple[int, int]:
        """
        保存所有钉图到指定目录。

        Returns:
            Tuple[int, int]: (保存成功数量, 保存失败数量)
        """
        output_dir = Path(directory)
        output_dir.mkdir(parents=True, exist_ok=True)

        saved = 0
        failed = 0
        for index, pin_window in enumerate(self.get_all_pins(), start=1):
            file_path = output_dir / f"{prefix}_{index:03d}.png"
            try:
                ok = False

                def _do_save():
                    nonlocal ok
                    ok = pin_window.get_current_image().save(str(file_path))

                if hasattr(pin_window, "_with_edit_paused"):
                    pin_window._with_edit_paused(_do_save)
                else:
                    _do_save()

                if ok:
                    saved += 1
                else:
                    failed += 1
                    log_error(T("保存钉图失败: {file_path}", file_path=file_path), "PinManager")
            except Exception as e:
                failed += 1
                log_error(T("保存钉图失败: {e}", e=e), "PinManager")

        log_info(T("批量保存钉图完成: 成功 {saved}, 失败 {failed}", saved=saved, failed=failed), "PinManager")
        return saved, failed
    
    # ------------------------------------------------------------------
    # 使用 Win32 SetWindowPos 直接切换 TOPMOST/NOTOPMOST，
    # ------------------------------------------------------------------
    def suppress_topmost(self):
        """将所有置顶 pin 窗口降级为普通窗口（NOTOPMOST）。"""
        if self._topmost_suppressed:
            return
        self._topmost_suppressed = True
        self._suppressed_pins.clear()
        for pin in self.pin_windows:
            if pin.windowFlags() & Qt.WindowType.WindowStaysOnTopHint:
                hwnd = int(pin.winId())
                _user32.SetWindowPos(hwnd, _HWND_NOTOPMOST, 0, 0, 0, 0, _SWP_FLAGS)
                self._suppressed_pins.append(pin)
        if self._suppressed_pins:
            log_debug(T("已压制 {count} 个钉图的置顶状态", count=len(self._suppressed_pins)), "PinManager")

    def restore_topmost(self):
        """恢复被压制的 pin 窗口为 TOPMOST。"""
        if not self._topmost_suppressed:
            return
        self._topmost_suppressed = False

        for pin in self._suppressed_pins:
            if pin not in self.pin_windows:
                continue
            hwnd = int(pin.winId())
            _user32.SetWindowPos(hwnd, _HWND_TOPMOST, 0, 0, 0, 0, _SWP_FLAGS)
        self._suppressed_pins.clear()

    def cleanup(self):
        """清理管理器（应用退出时调用）"""
        log_debug(T("清理管理器..."), "PinManager")
        self.close_all()
        PinManager._instance = None
        log_info(T("管理器已清理"), "PinManager")


# 便捷函数
def get_pin_manager():
    """获取钉图管理器单例"""
    return PinManager.instance()
