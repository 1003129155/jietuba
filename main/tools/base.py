"""
工具基类和上下文
"""

import math
from dataclasses import dataclass
from typing import Optional

from PySide6.QtGui import QColor, QPainterPath
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtWidgets import QApplication

from settings.tool_settings import ToolSettingsManager


@dataclass
class ToolContext:
    """
    工具上下文 - 包含工具所需的所有依赖
    """
    scene: object          # CanvasScene
    selection: object      # SelectionModel
    undo_stack: object     # CommandUndoStack (原 undo)
    color: QColor          # 当前颜色
    stroke_width: int      # 笔触宽度
    opacity: float         # 透明度 (0.0-1.0)
    settings_manager: object = None  # ToolSettingsManager（新增）



class Tool:
    """工具基类 - 所有绘图工具的父类"""

    # 笔触宽度的合法范围。工具栏把同一个宽度广播给所有工具，但各工具能接受的
    # 范围不同（画笔可以细到 1，序号圈小于某个尺寸就看不清数字）。每个工具用
    # 自己的范围在入口处收一次，ctx.stroke_width 因此永远是当前工具可用的值，
    # 下游（绘制、光标预览、面板显示）直接取用即可，不必各自再判断一遍。
    MIN_WIDTH = 1
    MAX_WIDTH = 99

    @classmethod
    def clamp_width(cls, width) -> float:
        """把任意来源的宽度收进本工具的合法范围。"""
        try:
            value = float(width)
        except (TypeError, ValueError):
            value = float(cls.MIN_WIDTH)
        return max(float(cls.MIN_WIDTH), min(float(cls.MAX_WIDTH), value))

    # 自由笔迹相邻两点的最小间距（场景坐标，与物理像素 1:1）。高刷鼠标每秒能报
    # 上千个点，其中大量点与前一个点相距不足一个像素：它们对笔迹外观没有贡献，
    # 却让 on_move 每次重描的路径线性变长，整笔的重绘成本因此是 O(N²)——画得越
    # 久越卡。在追加点的入口处按间距筛一次，形状不变而点数大幅下降。
    MIN_POINT_SPACING = 1.5

    @classmethod
    def should_append_point(cls, last: Optional[QPointF], pos: QPointF) -> bool:
        """pos 是否值得追加进笔迹。

        比较对象必须是**上一个被采纳的点**，不能是上一个鼠标事件的位置：慢速描边
        时每个事件都只挪不到一个像素，按事件比会把它们全部丢掉、整笔画不出来；
        按采纳点比，距离会一路累积，挪够 MIN_POINT_SPACING 时自然被采纳。
        """
        return last is None or (pos - last).manhattanLength() >= cls.MIN_POINT_SPACING

    id = "base"  # 工具ID（子类必须重写）
    
    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        """
        鼠标按下事件
        
        Args:
            pos: 鼠标位置（场景坐标）
            button: 鼠标按钮
            ctx: 工具上下文
        """
        pass
    
    def on_move(self, pos: QPointF, ctx: ToolContext):
        """
        鼠标移动事件
        
        Args:
            pos: 鼠标位置（场景坐标）
            ctx: 工具上下文
        """
        pass
    
    def on_release(self, pos: QPointF, ctx: ToolContext):
        """
        鼠标释放事件
        
        Args:
            pos: 鼠标位置（场景坐标）
            ctx: 工具上下文
        """
        pass
    
    def on_activate(self, ctx: ToolContext):
        """工具激活时调用 - 先加载设置，再设置光标"""
        # 先加载工具的设置到上下文中
        if ctx.settings_manager:
            self.load_settings(ctx)
        
        # 然后设置工具光标
        if hasattr(ctx.scene, 'cursor_manager') and ctx.scene.cursor_manager:
            ctx.scene.cursor_manager.set_tool_cursor(self.id)
        # 尝试从 canvas_widget 获取
        elif hasattr(ctx, 'canvas_widget') and hasattr(ctx.canvas_widget, 'cursor_manager'):
            ctx.canvas_widget.cursor_manager.set_tool_cursor(self.id)
    
    def on_deactivate(self, ctx: ToolContext):
        """
        工具停用时调用
        
        Args:
            ctx: 工具上下文
        """
        # 如果有设置管理器，保存当前工具的设置
        if ctx.settings_manager:
            self.save_settings(ctx)
    
    def load_settings(self, ctx: ToolContext):
        """
        从设置管理器加载工具设置到上下文
        
        Args:
            ctx: 工具上下文
        """
        if not ctx.settings_manager:
            return
        
        # 加载颜色
        color = ctx.settings_manager.get_color(self.id)
        if color.isValid():
            ctx.color = color
        
        # 加载笔触宽度
        stroke_width = ctx.settings_manager.get_stroke_width(self.id)
        if stroke_width:
            ctx.stroke_width = self.clamp_width(stroke_width)
        
        # 加载透明度
        opacity = ctx.settings_manager.get_opacity(self.id)
        if opacity is not None:
            ctx.opacity = opacity
    
    def save_settings(self, ctx: ToolContext):
        """
        将当前上下文的设置保存到设置管理器
        
        Args:
            ctx: 工具上下文
        """
        if not ctx.settings_manager:
            return
        
        # 批量保存设置
        ctx.settings_manager.update_settings(
            self.id,
            save_immediately=True,
            color=ctx.color.name(),
            stroke_width=ctx.stroke_width,
            opacity=ctx.opacity
        )


def color_with_opacity(source: QColor, opacity: Optional[float]) -> QColor:
    """返回应用透明度后的颜色副本"""
    color = QColor(source)
    if opacity is None:
        opacity = 1.0
    opacity = max(0.0, min(1.0, float(opacity)))
    color.setAlphaF(opacity)
    return color


def drag_rect(start: QPointF, end: QPointF, square: bool = False) -> QRectF:
    """从 start 拖到 end 围出的矩形。

    square 时是正方形：边长取两个方向里较长的一边，朝鼠标所在的方向展开。
    """
    if square:
        dx, dy = end.x() - start.x(), end.y() - start.y()
        side = max(abs(dx), abs(dy))
        end = QPointF(start.x() + math.copysign(side, dx), start.y() + math.copysign(side, dy))
    return QRectF(start, end).normalized()


def shift_held() -> bool:
    """正在处理的这次输入事件是否按着 Shift。"""
    return bool(QApplication.keyboardModifiers() & Qt.KeyboardModifier.ShiftModifier)


def shift_line_angle(ctx: ToolContext) -> int:
    """按住 Shift 拉直线时吸附的角度间隔（度），0 是任意角度。"""
    getter = getattr(ctx.settings_manager, "get_shift_line_angle", None)
    if getter is None:
        return ToolSettingsManager.APP_DEFAULT_SETTINGS["shift_line_angle"]
    return getter()


def snap_line_end(anchor: QPointF, pos: QPointF, step_degrees: int) -> QPointF:
    """从 anchor 朝 pos 拉直线的终点：方向取最接近的 step_degrees 整数倍，终点是 pos 在
    这个方向上的投影。step_degrees 为 0 时不吸附，终点就是 pos。"""
    dx, dy = pos.x() - anchor.x(), pos.y() - anchor.y()
    if not step_degrees or (dx == 0 and dy == 0):
        return QPointF(pos)
    step = math.radians(step_degrees)
    angle = round(math.atan2(dy, dx) / step) * step
    # 横竖方向上 cos/sin 会剩 1e-17 级的尾数，不抹掉的话横线终点的 y 不严格等于起点
    ux, uy = round(math.cos(angle), 12), round(math.sin(angle), 12)
    length = dx * ux + dy * uy
    return QPointF(anchor.x() + ux * length, anchor.y() + uy * length)


# 平滑只作用于密集的采样点：相邻间距不超过 FULL 的点完全参与平均，达到 NONE 的点不动。
# 慢画时采样点挤在一起，整数坐标的台阶就是它们之间的抖动；快画时点稀疏，平均会把弯折拉缩，
# 这时点本身就是形状，只用曲线穿过去。
SMOOTH_FULL_GAP = 3.0
SMOOTH_NONE_GAP = 8.0

# 没移动过的一笔补的线段长度。连到当前点的 lineTo 会被 Qt 丢掉，这么短的一段仍会画出两端的圆头。
DOT_SEGMENT = 0.01


def _relax(prev, cur, nxt):
    """按间距加权，把 cur 往 (prev + 2·cur + nxt) / 4 拉。"""
    gap = max(math.hypot(cur[0] - prev[0], cur[1] - prev[1]),
              math.hypot(nxt[0] - cur[0], nxt[1] - cur[1]))
    weight = (SMOOTH_NONE_GAP - gap) / (SMOOTH_NONE_GAP - SMOOTH_FULL_GAP)
    if weight <= 0:
        return cur
    weight = min(weight, 1.0) * 0.25
    return (cur[0] + (prev[0] - 2 * cur[0] + nxt[0]) * weight,
            cur[1] + (prev[1] - 2 * cur[1] + nxt[1]) * weight)


def _catmull_rom(p0, p1, p2, p3):
    """向心 Catmull-Rom 中 p1→p2 这一段的三次贝塞尔控制点。"""
    d1 = max(math.hypot(p1[0] - p0[0], p1[1] - p0[1]), 1e-6) ** 0.5
    d2 = max(math.hypot(p2[0] - p1[0], p2[1] - p1[1]), 1e-6) ** 0.5
    d3 = max(math.hypot(p3[0] - p2[0], p3[1] - p2[1]), 1e-6) ** 0.5
    c1, c2 = [], []
    for k in (0, 1):
        m1 = ((p1[k] - p0[k]) / d1 - (p2[k] - p0[k]) / (d1 + d2) + (p2[k] - p1[k]) / d2) * d2
        m2 = ((p2[k] - p1[k]) / d2 - (p3[k] - p1[k]) / (d2 + d3) + (p3[k] - p2[k]) / d3) * d2
        c1.append(p1[k] + m1 / 3)
        c2.append(p2[k] - m2 / 3)
    return tuple(c1), tuple(c2)


class FreehandStroke:
    """自由笔迹，按住 Shift 的那一段画成直线。

    smooth 时采样点先按间距平滑两遍，再用穿过各点的 Catmull-Rom 曲线连起来。来一个新点只有
    末尾四段会变，这几段就地改写，前面的部分不再计算。起点、终点和直线的两端始终不动。

    直线从按下 Shift 时笔迹的最后一点拉出，始终是路径的最后一个元素，鼠标移动时就地改它的
    终点，所以拖着直线来回晃不会增加点数；松开 Shift 后这一段就此定下，接着自由画。
    """

    def __init__(self, start: QPointF, angle_step: int, smooth: bool = False):
        self.path = QPainterPath(start)
        self.angle_step = angle_step
        self.smooth = smooth
        self.last_point = QPointF(start)
        self._line_anchor: Optional[QPointF] = None
        self._line_open = False
        self._dirty_from: Optional[int] = None
        self._begin_run()

    def take_dirty_from(self) -> int:
        """上次取过之后，路径从第几个元素起有改动；没有改动时是元素总数。"""
        count = self.path.elementCount()
        dirty, self._dirty_from = self._dirty_from, None
        return count if dirty is None else dirty

    def _mark_dirty(self, index: int):
        if self._dirty_from is None or index < self._dirty_from:
            self._dirty_from = index

    def finish(self) -> bool:
        """松手时调用：没移动过的一笔画成按下处的一个点。返回路径是否有变化。"""
        if self.path.elementCount() > 1:
            return False
        start = self.path.elementAt(0)
        self._mark_dirty(1)
        self.path.lineTo(start.x + DOT_SEGMENT, start.y)
        return True

    def extend(self, pos: QPointF, straight: bool) -> bool:
        """把笔迹延伸到 pos，straight 时画成直线。返回路径是否有变化。"""
        if not straight:
            if self._line_anchor is not None:
                self._line_anchor = None
                self._line_open = False
                self._begin_run()
            if not Tool.should_append_point(self.last_point, pos):
                return False
            self.last_point = QPointF(pos)
            if self.smooth:
                self._append_smooth((pos.x(), pos.y()))
            else:
                self._mark_dirty(self.path.elementCount())
                self.path.lineTo(pos)
            return True

        if self._line_anchor is None:
            self._line_anchor = QPointF(self.last_point)
        end = snap_line_end(self._line_anchor, pos, self.angle_step)
        if end == self.last_point:
            return False
        if self._line_open:
            self._mark_dirty(self.path.elementCount() - 1)
            self.path.setElementPositionAt(self.path.elementCount() - 1, end.x(), end.y())
        else:
            self._mark_dirty(self.path.elementCount())
            self.path.lineTo(end)
            self._line_open = True
        self.last_point = end
        return True

    def _begin_run(self):
        """从路径当前的最后一点开始一段新的自由笔迹，这一点之后不再移动。"""
        last = self.path.elementCount() - 1
        element = self.path.elementAt(last)
        self._run_base = last
        self._raw = [(element.x, element.y)]
        self._once = [self._raw[0]]
        self._twice = [self._raw[0]]

    def _append_smooth(self, point):
        raw, once, twice = self._raw, self._once, self._twice
        raw.append(point)
        n = len(raw) - 1
        # 末点平滑不了（没有下一个点），先原样放着；来了新点它才有邻居，往前牵动两层
        if n >= 2:
            once[n - 1] = _relax(raw[n - 2], raw[n - 1], raw[n])
        once.append(point)
        for i in range(max(1, n - 2), n):
            twice[i] = _relax(once[i - 1], once[i], once[i + 1])
        twice.append(point)

        # 第 i 段连 twice[i] 和 twice[i + 1]，要用到 twice[i - 1..i + 2]
        self._mark_dirty(self._run_base + 1 + 3 * max(0, n - 4))
        for i in range(max(0, n - 4), n):
            c1, c2 = _catmull_rom(twice[max(i - 1, 0)], twice[i], twice[i + 1], twice[min(i + 2, n)])
            end = twice[i + 1]
            if i == n - 1:
                self.path.cubicTo(c1[0], c1[1], c2[0], c2[1], end[0], end[1])
            else:
                index = self._run_base + 1 + 3 * i
                self.path.setElementPositionAt(index, c1[0], c1[1])
                self.path.setElementPositionAt(index + 1, c2[0], c2[1])
                self.path.setElementPositionAt(index + 2, end[0], end[1])

 