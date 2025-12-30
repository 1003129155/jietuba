"""
选区吸附系统 - Snap System
功能:
1. 屏幕边缘吸附
2. 窗口边缘吸附
3. 像素网格吸附
4. 吸附引导线渲染
"""

from PyQt6.QtCore import QRectF, QPointF, QLineF, Qt
from PyQt6.QtGui import QPen, QColor
from typing import List, Tuple, Optional
import sys

# 判断是否在Windows环境
try:
    import win32gui
    import win32con
    HAS_WIN32 = True
except ImportError:
    HAS_WIN32 = False


class SnapGuide:
    """吸附引导线"""
    def __init__(self, line: QLineF, guide_type: str):
        self.line = line  # 引导线(垂直或水平)
        self.type = guide_type  # "screen" | "window" | "grid"


class SnapSystem:
    """
    选区吸附系统
    
    职责:
    - 检测吸附目标(屏幕边缘/窗口边缘/像素网格)
    - 计算吸附后的矩形
    - 生成吸附引导线
    
    使用方法:
    ```python
    snap = SnapSystem(screen_rect, threshold=5)
    snap.update_windows(window_list)  # 更新窗口列表
    
    # 拖拽选区时
    snapped_rect = snap.snap_rect(user_rect)
    guides = snap.get_snap_guides()  # 用于渲染红色虚线
    ```
    """
    
    def __init__(self, screen_rect: QRectF, threshold: int = 5):
        """
        Args:
            screen_rect: 屏幕矩形(截图区域)
            threshold: 吸附阈值(像素),默认5px
        """
        self.screen_rect = screen_rect
        self.threshold = threshold
        
        # 窗口边界列表 [(x1, y1, x2, y2), ...]
        self.window_rects: List[Tuple[float, float, float, float]] = []
        
        # 当前吸附的引导线
        self._active_guides: List[SnapGuide] = []
        
        # 像素网格吸附开关
        self.enable_pixel_snap = True
        self.enable_screen_snap = True
        self.enable_window_snap = True
    
    def update_windows(self, windows: List[Tuple[int, List[float], str]]):
        """
        更新窗口列表(从 Finder 获取)
        
        Args:
            windows: [(hwnd, [x1, y1, x2, y2], title), ...]
        """
        self.window_rects = []
        for hwnd, rect, title in windows:
            # rect 已经是相对坐标,直接使用
            self.window_rects.append(tuple(rect))
        
        print(f"📌 [SnapSystem] 加载了 {len(self.window_rects)} 个窗口边界")
    
    def snap_rect(self, rect: QRectF) -> QRectF:
        """
        对矩形进行吸附计算
        
        Args:
            rect: 用户拖拽的原始矩形
            
        Returns:
            吸附后的矩形
        """
        self._active_guides.clear()
        
        # 如果矩形太小,不吸附(避免干扰初始拖拽)
        if rect.width() < 10 or rect.height() < 10:
            return rect
        
        snapped = QRectF(rect)
        
        # 1. 屏幕边缘吸附
        if self.enable_screen_snap:
            snapped = self._snap_to_screen(snapped)
        
        # 2. 窗口边缘吸附
        if self.enable_window_snap:
            snapped = self._snap_to_windows(snapped)
        
        # 3. 像素网格吸附
        if self.enable_pixel_snap:
            snapped = self._snap_to_pixel_grid(snapped)
        
        return snapped
    
    def _snap_to_screen(self, rect: QRectF) -> QRectF:
        """吸附到屏幕边缘"""
        snapped = QRectF(rect)
        
        # 左边缘
        if abs(rect.left() - self.screen_rect.left()) < self.threshold:
            snapped.setLeft(self.screen_rect.left())
            self._add_guide_vertical(self.screen_rect.left(), "screen")
        
        # 右边缘
        if abs(rect.right() - self.screen_rect.right()) < self.threshold:
            snapped.setRight(self.screen_rect.right())
            self._add_guide_vertical(self.screen_rect.right(), "screen")
        
        # 上边缘
        if abs(rect.top() - self.screen_rect.top()) < self.threshold:
            snapped.setTop(self.screen_rect.top())
            self._add_guide_horizontal(self.screen_rect.top(), "screen")
        
        # 下边缘
        if abs(rect.bottom() - self.screen_rect.bottom()) < self.threshold:
            snapped.setBottom(self.screen_rect.bottom())
            self._add_guide_horizontal(self.screen_rect.bottom(), "screen")
        
        return snapped
    
    def _snap_to_windows(self, rect: QRectF) -> QRectF:
        """吸附到窗口边缘"""
        if not self.window_rects:
            return rect
        
        snapped = QRectF(rect)
        
        # 收集所有候选边缘
        candidates = {
            'left': [],    # (edge_x, distance)
            'right': [],
            'top': [],
            'bottom': []
        }
        
        for wx1, wy1, wx2, wy2 in self.window_rects:
            # 检查垂直方向是否有重叠(才可能吸附)
            if self._ranges_overlap(wy1, wy2, rect.top(), rect.bottom()):
                # 左边缘吸附
                dist_left_to_left = abs(rect.left() - wx1)
                dist_left_to_right = abs(rect.left() - wx2)
                if dist_left_to_left < self.threshold:
                    candidates['left'].append((wx1, dist_left_to_left))
                if dist_left_to_right < self.threshold:
                    candidates['left'].append((wx2, dist_left_to_right))
                
                # 右边缘吸附
                dist_right_to_left = abs(rect.right() - wx1)
                dist_right_to_right = abs(rect.right() - wx2)
                if dist_right_to_left < self.threshold:
                    candidates['right'].append((wx1, dist_right_to_left))
                if dist_right_to_right < self.threshold:
                    candidates['right'].append((wx2, dist_right_to_right))
            
            # 检查水平方向是否有重叠
            if self._ranges_overlap(wx1, wx2, rect.left(), rect.right()):
                # 上边缘吸附
                dist_top_to_top = abs(rect.top() - wy1)
                dist_top_to_bottom = abs(rect.top() - wy2)
                if dist_top_to_top < self.threshold:
                    candidates['top'].append((wy1, dist_top_to_top))
                if dist_top_to_bottom < self.threshold:
                    candidates['top'].append((wy2, dist_top_to_bottom))
                
                # 下边缘吸附
                dist_bottom_to_top = abs(rect.bottom() - wy1)
                dist_bottom_to_bottom = abs(rect.bottom() - wy2)
                if dist_bottom_to_top < self.threshold:
                    candidates['bottom'].append((wy1, dist_bottom_to_top))
                if dist_bottom_to_bottom < self.threshold:
                    candidates['bottom'].append((wy2, dist_bottom_to_bottom))
        
        # 选择最近的边缘吸附
        if candidates['left']:
            edge, _ = min(candidates['left'], key=lambda x: x[1])
            snapped.setLeft(edge)
            self._add_guide_vertical(edge, "window")
        
        if candidates['right']:
            edge, _ = min(candidates['right'], key=lambda x: x[1])
            snapped.setRight(edge)
            self._add_guide_vertical(edge, "window")
        
        if candidates['top']:
            edge, _ = min(candidates['top'], key=lambda x: x[1])
            snapped.setTop(edge)
            self._add_guide_horizontal(edge, "window")
        
        if candidates['bottom']:
            edge, _ = min(candidates['bottom'], key=lambda x: x[1])
            snapped.setBottom(edge)
            self._add_guide_horizontal(edge, "window")
        
        return snapped
    
    def _snap_to_pixel_grid(self, rect: QRectF) -> QRectF:
        """吸附到像素网格(整数坐标)"""
        snapped = QRectF(
            round(rect.left()),
            round(rect.top()),
            round(rect.width()),
            round(rect.height())
        )
        return snapped
    
    def _ranges_overlap(self, a1: float, a2: float, b1: float, b2: float) -> bool:
        """检查两个范围是否有重叠"""
        return not (a2 < b1 or a1 > b2)
    
    def _add_guide_vertical(self, x: float, guide_type: str):
        """添加垂直引导线"""
        line = QLineF(
            x, self.screen_rect.top(),
            x, self.screen_rect.bottom()
        )
        self._active_guides.append(SnapGuide(line, guide_type))
    
    def _add_guide_horizontal(self, y: float, guide_type: str):
        """添加水平引导线"""
        line = QLineF(
            self.screen_rect.left(), y,
            self.screen_rect.right(), y
        )
        self._active_guides.append(SnapGuide(line, guide_type))
    
    def get_snap_guides(self) -> List[SnapGuide]:
        """获取当前激活的吸附引导线(用于渲染)"""
        return self._active_guides
    
    def render_guides(self, painter):
        """
        渲染吸附引导线
        
        Args:
            painter: QPainter 实例
        """
        if not self._active_guides:
            return
        
        # 保存原始状态
        painter.save()
        
        # 设置引导线样式
        pen = QPen(QColor(255, 0, 0, 180))  # 红色,半透明
        pen.setWidth(1)
        pen.setStyle(Qt.PenStyle.DashLine)  # 虚线
        painter.setPen(pen)
        
        # 绘制所有引导线
        for guide in self._active_guides:
            painter.drawLine(guide.line)
        
        # 恢复状态
        painter.restore()


class SnapSystemFactory:
    """
    SnapSystem 工厂类
    用于从已有的 Finder 实例创建 SnapSystem
    """
    
    @staticmethod
    def from_finder(finder, screen_rect: QRectF, threshold: int = 5) -> SnapSystem:
        """
        从 Finder 实例创建 SnapSystem
        
        Args:
            finder: jietuba_ui_components.Finder 实例
            screen_rect: 屏幕矩形
            threshold: 吸附阈值
            
        Returns:
            配置好的 SnapSystem 实例
        """
        snap_system = SnapSystem(screen_rect, threshold)
        
        # 如果 Finder 已经加载了窗口,直接使用
        if hasattr(finder, 'windows') and finder.windows:
            snap_system.update_windows(finder.windows)
        
        return snap_system


# 调试函数
def test_snap_system():
    """测试函数"""
    print("🧪 测试 SnapSystem")
    
    # 创建测试环境
    screen = QRectF(0, 0, 1920, 1080)
    snap = SnapSystem(screen, threshold=5)
    
    # 模拟窗口
    fake_windows = [
        (0, [100, 100, 500, 400], "窗口1"),
        (0, [600, 200, 1000, 600], "窗口2"),
    ]
    snap.update_windows(fake_windows)
    
    # 测试吸附
    test_cases = [
        QRectF(3, 50, 200, 150),      # 应该吸附到屏幕左边缘
        QRectF(97, 100, 200, 150),    # 应该吸附到窗口1左边缘
        QRectF(598, 200, 200, 150),   # 应该吸附到窗口2左边缘
    ]
    
    for i, rect in enumerate(test_cases, 1):
        print(f"\n测试 {i}: 原始矩形 = {rect}")
        snapped = snap.snap_rect(rect)
        print(f"  吸附后 = {snapped}")
        print(f"  引导线数量 = {len(snap.get_snap_guides())}")


if __name__ == "__main__":
    test_snap_system()
