"""
智能窗口选择器 - 基于 Windows API 的窗口检测

功能：
1. 枚举所有可见窗口
2. 根据鼠标位置查找最顶层窗口
3. 支持多屏幕坐标转换
4. 过滤无效窗口（工具窗口、透明窗口等）

使用方式：
    finder = WindowFinder()
    finder.find_windows()  # 枚举所有窗口
    rect = finder.find_window_at_point(x, y)  # 查找鼠标位置的窗口
"""

import sys
import ctypes
from ctypes import wintypes
from typing import List, Tuple, Optional
from core import log_debug, log_info, log_warning, log_error
from core.logger import log_exception

try:
    import win32gui
    import win32con
    WINDOWS_API_AVAILABLE = True
except ImportError:
    WINDOWS_API_AVAILABLE = False
    log_warning("win32gui 未安装，智能选区功能不可用", module="智能选区")

# DPI 感知已在 main_app.py 中设置，此处不再重复调用
# 避免 "访问被拒绝" 警告（DPI 设置只能调用一次）

def get_window_rect_no_shadow(hwnd):
    """
    获取去除阴影的真实窗口矩形 (物理像素)
    解决 Windows 10/11 窗口自带大阴影导致选区虚空的问题
    """
    try:
        rect = ctypes.wintypes.RECT()
        # DWMWA_EXTENDED_FRAME_BOUNDS = 9
        ctypes.windll.dwmapi.DwmGetWindowAttribute(
            ctypes.wintypes.HWND(hwnd), 
            ctypes.c_int(9), 
            ctypes.byref(rect), 
            ctypes.sizeof(rect)
        )
        return [rect.left, rect.top, rect.right, rect.bottom]
    except Exception:
        # 降级回退
        return win32gui.GetWindowRect(hwnd)



class WindowFinder:
    """
    智能窗口选择器
    
    基于 Windows API 的窗口检测，用于实现"智能选区"功能
    可以在截图时自动识别鼠标下的窗口，并自动框选
    """
    
    def __init__(self, screen_offset_x: int = 0, screen_offset_y: int = 0):
        """
        初始化窗口查找器
        
        Args:
            screen_offset_x: 屏幕X偏移（多屏幕时使用）
            screen_offset_y: 屏幕Y偏移（多屏幕时使用）
        """
        if not WINDOWS_API_AVAILABLE:
            raise RuntimeError("win32gui 库未安装，无法使用智能选区功能")
        
        self.windows: List[Tuple[int, List[int], str]] = []  # [(hwnd, [x1,y1,x2,y2], title), ...]
        self.screen_offset_x = screen_offset_x
        self.screen_offset_y = screen_offset_y
        self.debug = False
    
    def set_screen_offset(self, offset_x: int, offset_y: int):
        """
        设置屏幕偏移（用于多屏幕坐标转换）
        
        Args:
            offset_x: X轴偏移
            offset_y: Y轴偏移
        """
        self.screen_offset_x = offset_x
        self.screen_offset_y = offset_y
        if self.debug:
            log_debug(f"使用偏移: ({self.screen_offset_x}, {self.screen_offset_y})", module="智能选区")
    
    def find_windows(self):
        """
        枚举所有可见窗口
        
        遍历所有窗口并过滤出有效的应用窗口：
        - 必须可见
        - 必须有标题栏
        - 不能是工具窗口
        - 不能是透明窗口
        - 必须有合理的尺寸
        """
        if not WINDOWS_API_AVAILABLE:
            self.windows = []
            return
        
        self.windows = []
        
        def enum_windows_callback(hwnd, _):
            """枚举窗口回调函数"""
            try:
                # 1. 只处理可见窗口
                if not win32gui.IsWindowVisible(hwnd):
                    return True
                
                # 2. 检查窗口样式（排除工具窗口、消息窗口等）
                style = win32gui.GetWindowLong(hwnd, win32con.GWL_STYLE)
                ex_style = win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE)
                
                # 跳过没有标题栏的窗口（通常是弹出窗口或工具栏）
                if not (style & win32con.WS_CAPTION):
                    return True
                
                # 跳过工具窗口
                if ex_style & win32con.WS_EX_TOOLWINDOW:
                    return True
                
                # 3. 必须有窗口标题
                title = win32gui.GetWindowText(hwnd)
                if not title or len(title.strip()) == 0:
                    return True
                
                # 4. 检查窗口是否真的可以接收输入（不是透明遮罩）
                if ex_style & win32con.WS_EX_TRANSPARENT:
                    return True
                
                # 5. 获取窗口矩形 (使用 DWM API 去除阴影)
                rect = get_window_rect_no_shadow(hwnd)
                x1, y1, x2, y2 = rect
                
                # 6. 窗口必须有合理的大小（排除太小的窗口）
                width = x2 - x1
                height = y2 - y1
                if width < 30 or height < 30:  # 最小尺寸阈值
                    return True
                
                # 7. 窗口必须在屏幕可见区域内（至少部分可见）
                # 排除完全在屏幕外的窗口
                if x2 < -1000 or y2 < -1000 or x1 > 10000 or y1 > 10000:
                    return True
                
                # 8. 检查窗口类名，排除一些特殊的系统窗口
                try:
                    class_name = win32gui.GetClassName(hwnd)
                    # 排除一些已知的不需要选择的窗口类
                    excluded_classes = [
                        'Windows.UI.Core.CoreWindow',  # UWP内容窗口（会和ApplicationFrameWindow重复）
                        'WorkerW',                     # 桌面工作窗口
                        'Progman',                     # 程序管理器
                    ]
                    # 注意：ApplicationFrameWindow 是 UWP 应用的主框架窗口（如系统设置），不要排除
                    if class_name in excluded_classes:
                        return True
                except Exception as e:
                    log_exception(e, f"获取窗口类名 hwnd={hwnd}")
                
                # 9. 转换为相对于截图区域的坐标
                x1 -= self.screen_offset_x
                y1 -= self.screen_offset_y
                x2 -= self.screen_offset_x
                y2 -= self.screen_offset_y
                
                self.windows.append((hwnd, [x1, y1, x2, y2], title))
                
            except Exception as e:
                # 静默处理异常，继续枚举下一个窗口
                if self.debug:
                    log_warning(f"处理窗口时出错: {e}", module="智能选区")
            
            return True
        
        try:
            win32gui.EnumWindows(enum_windows_callback, None)
            
            if self.debug:
                log_debug(f"找到 {len(self.windows)} 个有效窗口", module="智能选区")
                if self.windows:
                    log_debug("检测到的窗口列表（前5个）:", module="智能选区")
                    for i, (hwnd, rect, title) in enumerate(self.windows[:5]):
                        log_debug(f"{i+1}. 标题: {title[:30]}, 大小: {rect[2]-rect[0]}x{rect[3]-rect[1]}, 位置: ({rect[0]}, {rect[1]})", module="智能选区")
                    
        except Exception as e:
            log_error(f"枚举窗口失败: {e}", module="智能选区")
            self.windows = []
    
    def find_window_at_point(self, x: int, y: int, fallback_rect: Optional[List[int]] = None) -> List[int]:
        """
        根据鼠标位置查找最顶层的包含窗口（基于 Z-order）
        
        Args:
            x: 鼠标X坐标
            y: 鼠标Y坐标
            fallback_rect: 如果未找到窗口，返回此矩形（默认None表示返回虚拟桌面尺寸）
        
        Returns:
            窗口矩形 [x1, y1, x2, y2]
        """
        target_rect = None
        found_window_title = None
        
        # 查找所有包含该点的窗口
        matching_windows = []
        for idx, (hwnd, rect, title) in enumerate(self.windows):
            x1, y1, x2, y2 = rect
            # 检查点是否在窗口内
            if x1 <= x <= x2 and y1 <= y <= y2:
                area = (x2 - x1) * (y2 - y1)
                # idx 就是 Z-order（EnumWindows 按从顶到底的顺序枚举）
                matching_windows.append((idx, area, hwnd, rect, title))
        
        # 如果找到多个重叠窗口
        if matching_windows:
            # 排序策略：优先选择 Z-order 最小的（最顶层），其次选择面积最小的（最精确）
            matching_windows.sort(key=lambda w: (w[0], w[1]))  # (z_order, area)
            z_order, area, hwnd, target_rect, found_window_title = matching_windows[0]
            
            # 调试信息
            if self.debug:
                log_debug(f"鼠标({x}, {y})处找到窗口: '{found_window_title[:30]}', 大小: {target_rect[2]-target_rect[0]}x{target_rect[3]-target_rect[1]}, Z-order: {z_order}", module="智能选区")
                if len(matching_windows) > 1:
                    log_debug(f"共有 {len(matching_windows)} 个重叠窗口，已选择最顶层的", module="智能选区")
                    # 输出其他候选窗口
                    for i, (z, a, h, r, t) in enumerate(matching_windows[1:3], 1):
                        log_debug(f"候选{i}: '{t[:20]}', Z-order: {z}, 面积: {a}", module="智能选区")
        
        # 如果没找到窗口，返回备选矩形
        if target_rect is None:
            if self.debug:
                log_debug(f"在鼠标位置({x}, {y})未找到有效窗口，返回备选矩形", module="智能选区")
            
            if fallback_rect:
                target_rect = fallback_rect
            else:
                # 默认返回虚拟桌面尺寸（包含所有显示器）
                target_rect = self._get_virtual_desktop_rect()
        
        return target_rect
    
    def _get_virtual_desktop_rect(self) -> List[int]:
        """获取虚拟桌面尺寸（包含所有显示器）"""
        try:
            # SM_XVIRTUALSCREEN = 76, SM_YVIRTUALSCREEN = 77
            # SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79
            user32 = ctypes.windll.user32
            x = user32.GetSystemMetrics(76)
            y = user32.GetSystemMetrics(77)
            width = user32.GetSystemMetrics(78)
            height = user32.GetSystemMetrics(79)
            return [x, y, x + width, y + height]
        except Exception:
            # 降级回退到主显示器
            try:
                from PyQt6.QtGui import QGuiApplication
                screen = QGuiApplication.primaryScreen()
                if screen:
                    geom = screen.geometry()
                    return [geom.x(), geom.y(), geom.x() + geom.width(), geom.y() + geom.height()]
            except Exception:
                pass
            return [0, 0, 1920, 1080]  # 最后降级
    
    def clear(self):
        """清除窗口列表"""
        self.windows = []


def is_smart_selection_available() -> bool:
    """
    检查智能选区功能是否可用
    
    Returns:
        bool: True=可用，False=不可用（缺少依赖）
    """
    return WINDOWS_API_AVAILABLE


# ============================================================================
#  便捷接口
# ============================================================================

def find_window_at_cursor(screen_offset_x: int = 0, screen_offset_y: int = 0) -> Optional[List[int]]:
    """
    快捷方式：查找当前鼠标位置的窗口
    
    Args:
        screen_offset_x: 屏幕X偏移
        screen_offset_y: 屏幕Y偏移
    
    Returns:
        窗口矩形 [x1, y1, x2, y2]，如果功能不可用则返回 None
    """
    if not WINDOWS_API_AVAILABLE:
        return None
    
    try:
        from PyQt6.QtGui import QCursor
        
        finder = WindowFinder(screen_offset_x, screen_offset_y)
        finder.find_windows()
        
        # 获取当前鼠标位置
        cursor_pos = QCursor.pos()
        x = cursor_pos.x() - screen_offset_x
        y = cursor_pos.y() - screen_offset_y
        
        return finder.find_window_at_point(x, y)
    except Exception as e:
        log_error(f"查找窗口失败: {e}", module="智能选区")
        return None
