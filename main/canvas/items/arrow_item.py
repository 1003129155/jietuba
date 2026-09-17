"""箭头图元。

九种样式的几何全在这里：箭杆、端头、空心描边，都按线宽等比算出来。
"""
from __future__ import annotations

import math
from PySide6.QtWidgets import (
    QGraphicsPathItem,
)
from PySide6.QtGui import QPen, QPainter, QPainterPath, QColor, QPainterPathStroker
from PySide6.QtCore import Qt, QPointF

from .drawing_items import DrawingItemMixin


class ArrowItem(DrawingItemMixin, QGraphicsPathItem):
    """
    箭头图元 - 平滑箭头，支持弯曲

    始终是3点结构（start / control / end）：
    - control 未修改时 → 自动保持在中点，表现为直线箭头
    - control 被拖动后 → 变成弯曲箭头
    - 撤销可以恢复到直线状态

    形状不按"直线/曲线 × 样式"逐个手写：每种样式只在 STYLE_SPECS 里声明
    "箭杆长什么样 + 两端各是什么头"，轮廓由同一套代码拼出来。直线是"控制点
    恰好落在中点"的退化情形（二次贝塞尔此时就是直线），所以直线和曲线共用同
    一条代码路径，新增一种样式只要加一行声明。
    """

    # === 箭头样式 ===
    STYLE_SINGLE = "single"                    # 实心锥形箭头（默认）
    STYLE_DOUBLE = "double"                    # 实心双向箭头
    STYLE_HOLLOW = "hollow"                    # 空心（描边）箭头
    STYLE_LINE = "line"                        # 线条箭头（等宽线 + 开口箭头）
    STYLE_LINE_DOUBLE = "line_double"          # 线条双向箭头
    STYLE_TRIANGLE = "triangle"                # 细杆 + 实心三角头
    STYLE_TRIANGLE_DOUBLE = "triangle_double"  # 细杆 + 两端实心三角头
    STYLE_BAR = "bar"                          # 工字（标注线）
    STYLE_BAR_ARROW = "bar_arrow"              # 工字 + 双向箭头

    # === 端头形态 ===
    HEAD_NONE = "none"            # 没有头（锥形尾巴收成尖）
    HEAD_SOLID = "solid"          # 实心三角
    HEAD_SWEPT = "swept"          # 后掠燕尾：颈部先收窄，尾翼甩到颈部后方再收尖
    HEAD_OPEN = "open"            # 开口 V 形（描线画出来的两根翼）
    HEAD_BAR = "bar"              # 垂直短横杠
    HEAD_BAR_SOLID = "bar_solid"  # 短横杠 + 顶着横杠朝外的实心三角

    # === 箭杆形态 ===
    SHAFT_TAPER = "taper"  # 尾部尖细、到颈部渐宽
    SHAFT_EVEN = "even"    # 等宽实心杆
    SHAFT_LINE = "line"    # 等宽细线

    # 样式表：样式 -> (箭杆, 起点端头, 终点端头, 是否只描边)
    STYLE_SPECS = {
        STYLE_SINGLE:          (SHAFT_TAPER, HEAD_NONE,      HEAD_SWEPT,     False),
        STYLE_DOUBLE:          (SHAFT_EVEN,  HEAD_SWEPT,     HEAD_SWEPT,     False),
        STYLE_HOLLOW:          (SHAFT_TAPER, HEAD_NONE,      HEAD_SWEPT,     True),
        STYLE_LINE:            (SHAFT_LINE,  HEAD_NONE,      HEAD_OPEN,      False),
        STYLE_LINE_DOUBLE:     (SHAFT_LINE,  HEAD_OPEN,      HEAD_OPEN,      False),
        STYLE_TRIANGLE:        (SHAFT_LINE,  HEAD_NONE,      HEAD_SOLID,     False),
        STYLE_TRIANGLE_DOUBLE: (SHAFT_LINE,  HEAD_SOLID,     HEAD_SOLID,     False),
        STYLE_BAR:             (SHAFT_LINE,  HEAD_BAR,       HEAD_BAR,       False),
        STYLE_BAR_ARROW:       (SHAFT_LINE,  HEAD_BAR_SOLID, HEAD_BAR_SOLID, False),
    }

    # 面板列出来的顺序：先实心、再线条、最后标注类
    STYLES = (
        STYLE_SINGLE, STYLE_DOUBLE, STYLE_HOLLOW,
        STYLE_LINE, STYLE_LINE_DOUBLE,
        STYLE_TRIANGLE, STYLE_TRIANGLE_DOUBLE,
        STYLE_BAR, STYLE_BAR_ARROW,
    )

    CLICK_MARGIN = 4  # 点击旷量（像素/每侧）

    OPEN_HEAD_ANGLE = math.radians(27)  # 开口箭头单侧张角

    # 后掠燕尾头的比例，相对头半宽（head_half）：颈部比尾翼更靠近尖，尾翼往
    # 后甩出去再收尖，画出来才有"后掠"的速度感，不是平底三角形
    SWEPT_NECK_LEN_RATIO = 1.62    # 尖 -> 颈部（接杆处）的轴向距离
    SWEPT_BARB_DEPTH_RATIO = 1.96  # 尖 -> 尾翼最宽处的轴向距离（比颈部更靠后）
    SWEPT_NECK_HALF_RATIO = 0.4    # 颈部半宽 / 尾翼最宽半宽

    @classmethod
    def normalize_style(cls, value) -> str:
        """把任意输入收敛成一个合法样式（老存档里的未知值回退到默认）"""
        return value if value in cls.STYLE_SPECS else cls.STYLE_SINGLE

    def __init__(self, start_pos: QPointF, end_pos: QPointF, pen: QPen, arrow_style: str = "single"):
        super().__init__()
        self._init_drawing_mixin()
        self.setPen(QPen(Qt.PenStyle.NoPen))  # 不使用轮廓线
        self.setBrush(pen.color())  # 使用填充
        self.setZValue(20)
        self._hovered = False

        self.start_pos = start_pos
        self.end_pos = end_pos
        # 控制点初始化为中点
        self._control_pos = QPointF(
            (start_pos.x() + end_pos.x()) / 2,
            (start_pos.y() + end_pos.y()) / 2
        )
        # 标记控制点是否被用户修改过（决定是直线还是曲线）
        self._control_modified = False

        self.base_width = pen.width()
        self.color = pen.color()
        self._shape_cache = None
        # 命中区用的实心剪影：空心样式的 path() 只剩一圈描边，拿它去点会点不中肚子
        self._hit_path = None
        self._arrow_style = self.normalize_style(arrow_style)
        self.update_geometry()

    def setPath(self, path: QPainterPath):
        super().setPath(path)
        self._shape_cache = None

    def boundingRect(self):
        # 默认实现只按 path() 本身的外接矩形算，圈不住 shape() 里再往外扩的
        # 点击旷量；shape() 必须完整落在 boundingRect() 之内，否则命中区
        # 会漏出包围盒外，场景的粗筛（先按包围盒过一遍再测 shape）会先把
        # 这部分点漏掉。
        margin = self.CLICK_MARGIN
        return self.path().boundingRect().adjusted(-margin, -margin, margin, margin)

    def shape(self):
        if self._shape_cache is not None:
            return self._shape_cache

        # 空心箭头的 path() 是一圈描边，实心剪影才是用户眼里"这支箭头占的地方"
        path = self._hit_path if self._hit_path is not None else self.path()
        if path.isEmpty():
            return path

        # 箭头 paint() 是整块填色（setBrush + drawPath），轮廓本身就是闭合填色
        # 区域，不是要被描边的骨架线——直接拿它当命中区，三角形箭头/工字端头的
        # 中心才点得中，不会只有贴着轮廓线的一圈能点中。
        # 轮廓是拼出来的不规则多边形，没有"放大参数"这条近路能加点击旷量，
        # 于是并上一条沿轮廓的窄带（宽度=旷量*2），边界外侧就多出一圈容差。
        stroker = QPainterPathStroker()
        stroker.setWidth(self.CLICK_MARGIN * 2)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        band = stroker.createStroke(path)

        self._shape_cache = path.united(band)
        return self._shape_cache

    @property
    def arrow_style(self) -> str:
        """获取箭头样式"""
        return self._arrow_style

    @arrow_style.setter
    def arrow_style(self, value: str):
        """设置箭头样式"""
        if value in self.STYLE_SPECS:
            self._arrow_style = value
            self.update_geometry()

    @property
    def control_pos(self) -> QPointF:
        """获取控制点位置"""
        if self._control_modified:
            return self._control_pos
        else:
            # 未修改时，返回当前的中点（跟随 start/end）
            return QPointF(
                (self.start_pos.x() + self.end_pos.x()) / 2,
                (self.start_pos.y() + self.end_pos.y()) / 2
            )

    @control_pos.setter
    def control_pos(self, value):
        """设置控制点（用于撤销恢复等）"""
        if value is None:
            self._control_modified = False
            self._control_pos = QPointF(
                (self.start_pos.x() + self.end_pos.x()) / 2,
                (self.start_pos.y() + self.end_pos.y()) / 2
            )
        else:
            self._control_pos = QPointF(value)
            # 注意：这里不自动设置 _control_modified
            # 因为撤销恢复时可能恢复到中点位置但仍是"未修改"状态

    def set_positions(self, start_pos: QPointF, end_pos: QPointF):
        """设置起点和终点"""
        self.start_pos = start_pos
        self.end_pos = end_pos

        if not self._control_modified:
            # 控制点未修改时，自动更新到新的中点
            self._control_pos = QPointF(
                (start_pos.x() + end_pos.x()) / 2,
                (start_pos.y() + end_pos.y()) / 2
            )
        # 如果控制点已修改，保持其绝对位置不变

        self.update_geometry()

    def set_control_point(self, control_pos: QPointF):
        """用户拖动控制点时调用 - 标记为已修改"""
        self._control_pos = QPointF(control_pos)
        self._control_modified = True
        self.update_geometry()

    def reset_control_point(self):
        """重置控制点（恢复直线箭头）"""
        self._control_modified = False
        self._control_pos = QPointF(
            (self.start_pos.x() + self.end_pos.x()) / 2,
            (self.start_pos.y() + self.end_pos.y()) / 2
        )
        self.update_geometry()

    def get_control_point(self) -> QPointF:
        """获取控制点位置（始终返回有效位置）"""
        return self.control_pos

    def is_curved(self) -> bool:
        """是否是弯曲箭头"""
        return self._control_modified

    # ------------------------------------------------------------------
    # 几何构建
    # ------------------------------------------------------------------

    @staticmethod
    def _unit(vec: QPointF, fallback: QPointF | None = None) -> QPointF | None:
        """单位化；长度可忽略时退回 fallback 方向"""
        length = math.hypot(vec.x(), vec.y())
        if length > 1e-6:
            return QPointF(vec.x() / length, vec.y() / length)
        if fallback is not None:
            return ArrowItem._unit(fallback)
        return None

    @staticmethod
    def _perp(unit: QPointF) -> QPointF:
        """左法向"""
        return QPointF(-unit.y(), unit.x())

    def _frame(self):
        """箭头骨架：(起点, 终点, 曲线中点, 起点切向, 终点切向, 近似长度)

        中点是用户拖的那个点，要求曲线真的经过它：对二次贝塞尔
        B(0.5) = 0.25*P0 + 0.5*P1 + 0.25*P2，反解得 P1 = 2M - 0.5*P0 - 0.5*P2。
        控制点没被拖过时 M 就是中点，此时 P1 也落在中点上，曲线退化成直线，
        两端切向都等于 end-start —— 直线不需要单独一套代码。
        """
        start, end, mid = self.start_pos, self.end_pos, self.control_pos
        bezier = QPointF(
            2 * mid.x() - 0.5 * start.x() - 0.5 * end.x(),
            2 * mid.y() - 0.5 * start.y() - 0.5 * end.y()
        )
        chord = QPointF(end.x() - start.x(), end.y() - start.y())
        v_start = QPointF(bezier.x() - start.x(), bezier.y() - start.y())  # B'(0)/2
        v_end = QPointF(end.x() - bezier.x(), end.y() - bezier.y())        # B'(1)/2

        length = max(
            math.hypot(v_start.x(), v_start.y()) + math.hypot(v_end.x(), v_end.y()),
            math.hypot(chord.x(), chord.y())
        )
        if length < 0.1:
            return None

        u_start = self._unit(v_start, chord)
        u_end = self._unit(v_end, chord)
        if u_start is None or u_end is None:
            return None
        return start, end, mid, u_start, u_end, length

    def _metrics(self, length: float, head_start: str, head_end: str) -> dict:
        """按线宽算出各部件尺寸

        头的长宽比固定（head_len ≈ 1.75 * 半宽），所以无论线宽怎么调，箭头的
        尖锐程度都是同一个观感；只有画得太短时才整体等比缩头，否则一支短箭头
        会被自己的头吃光。
        """
        base = max(1.0, float(self.base_width))
        head_half = max(base * 2.2, 8.0)
        head_len = head_half * 1.75

        solid_heads = sum(1 for h in (head_start, head_end)
                          if h in (self.HEAD_SOLID, self.HEAD_BAR_SOLID))
        swept_heads = sum(1 for h in (head_start, head_end) if h == self.HEAD_SWEPT)
        if solid_heads or swept_heads:
            budget = length * (0.45 if (solid_heads + swept_heads) == 1 else 0.34)
            # 燕尾的尾翼比平底三角形的头更靠后地鼓出去，缩放要按那个更深的
            # 进深来算，否则短箭头上尾翼会甩到起点外面
            reach = head_len
            if swept_heads:
                reach = max(reach, head_half * self.SWEPT_BARB_DEPTH_RATIO)
            if reach > budget:
                # 等比缩，不是只压长度：只压长度会把头压成一把扁铲子
                shrink = budget / reach
                head_half *= shrink
                head_len *= shrink

        line_w = max(base * 0.9, 2.0)

        wing_len = max(base * 4.2, 15.0)
        open_heads = sum(1 for h in (head_start, head_end) if h == self.HEAD_OPEN)
        if open_heads:
            wing_len = min(wing_len, length * (0.45 if open_heads == 1 else 0.35))
        # 人字头的翼厚有上限：厚过 wing_len * tan(张角)，两翼的内边就越过轴线交叉，
        # 头会拧成一个结。顶到上限时人字自己收成一个实心三角——短粗箭头正好该这样。
        wing_w = min(line_w, wing_len * math.tan(self.OPEN_HEAD_ANGLE) * 0.9)

        sweep_neck_half = head_half * self.SWEPT_NECK_HALF_RATIO
        return {
            "line_w": line_w,
            "head_half": head_half,
            "head_len": head_len,
            "wing_len": wing_len,
            "wing_w": wing_w,
            # 人字头的内凹尖落在头尖后方这么远：两翼内边在轴线上就交在这儿
            "open_notch": wing_w / math.sin(self.OPEN_HEAD_ANGLE),
            "bar_half": max(base * 2.0, 8.0),
            "sweep_len": head_half * self.SWEPT_NECK_LEN_RATIO,
            "sweep_depth": head_half * self.SWEPT_BARB_DEPTH_RATIO,
            "sweep_neck_half": sweep_neck_half,
            # 燕尾头接杆处的宽度，也是杆本身在这一端该有的宽度
            "neck_w": sweep_neck_half * 2,
            # 尾巴收成真正的尖：流线型箭头的尾巴一钝，整支看着就像被剪掉一截
            "tail_w": 0.0,
            "outline_w": max(base * 0.34, 1.6),
        }

    def _head_trim(self, kind: str, m: dict) -> float:
        """箭杆在这一端要让出多少长度给端头

        实心头留一点重叠（0.92），接缝处才不会因为抗锯齿透出一条细缝；横杠是
        骑在端点上的，不用让。
        """
        if kind == self.HEAD_SOLID:
            return m["head_len"] * 0.92
        if kind == self.HEAD_SWEPT:
            return m["sweep_len"] * 0.92
        if kind == self.HEAD_BAR_SOLID:
            return m["line_w"] * 0.5 + m["head_len"] * 0.8 * 0.92
        if kind == self.HEAD_OPEN:
            # 杆是根长方形，怼到头尖上就会从尖的两侧支出两个角，头看着是钝的。
            # 停在内凹尖前面一点：这一段被两翼盖着，杆的平口就藏进头里了。
            return min(m["line_w"] * 1.4, m["open_notch"])
        return 0.0

    def _head_path(self, kind: str, tip: QPointF, out_dir: QPointF, m: dict) -> QPainterPath:
        """造一个端头。out_dir 是这一端"朝外"的方向"""
        if kind == self.HEAD_SOLID:
            return self._solid_head_path(tip, out_dir, m["head_len"], m["head_half"])

        if kind == self.HEAD_SWEPT:
            return self._swept_head_path(tip, out_dir, m)

        if kind == self.HEAD_OPEN:
            return self._open_head_path(tip, out_dir, m["wing_len"],
                                        m["wing_w"], m["open_notch"])

        if kind == self.HEAD_BAR:
            return self._bar_path(tip, out_dir, m["bar_half"], m["line_w"])

        if kind == self.HEAD_BAR_SOLID:
            # 三角顶着横杠内侧朝外，读起来就是"量到这条线为止"
            path = self._bar_path(tip, out_dir, m["bar_half"], m["line_w"])
            inner_tip = QPointF(tip.x() - out_dir.x() * m["line_w"] * 0.5,
                                tip.y() - out_dir.y() * m["line_w"] * 0.5)
            triangle = self._solid_head_path(
                inner_tip, out_dir, m["head_len"] * 0.8, m["head_half"] * 0.8
            )
            return path.united(triangle)

        return QPainterPath()

    def _solid_head_path(self, tip: QPointF, out_dir: QPointF,
                         head_len: float, head_half: float) -> QPainterPath:
        """实心三角头"""
        perp = self._perp(out_dir)
        neck_x = tip.x() - out_dir.x() * head_len
        neck_y = tip.y() - out_dir.y() * head_len
        path = QPainterPath()
        path.moveTo(tip)
        path.lineTo(neck_x + perp.x() * head_half, neck_y + perp.y() * head_half)
        path.lineTo(neck_x - perp.x() * head_half, neck_y - perp.y() * head_half)
        path.closeSubpath()
        return path

    def _swept_head_path(self, tip: QPointF, out_dir: QPointF, m: dict) -> QPainterPath:
        """后掠燕尾头：颈部窄，尾翼比颈部更靠后地甩宽，再扫回尖上

        跟平底三角形（_solid_head_path）不同，这里的最宽点（尾翼）不在颈部，
        而是颈部后方一截：轮廓从颈部先往外后甩到尾翼尖，再折回来收成头尖，
        画出来是"尖三角 + 两片后掠尾翼"，比平底三角更有速度感。
        """
        perp = self._perp(out_dir)
        neck = QPointF(tip.x() - out_dir.x() * m["sweep_len"],
                       tip.y() - out_dir.y() * m["sweep_len"])
        barb = QPointF(tip.x() - out_dir.x() * m["sweep_depth"],
                       tip.y() - out_dir.y() * m["sweep_depth"])
        neck_half = m["sweep_neck_half"]
        barb_half = m["head_half"]

        path = QPainterPath()
        path.moveTo(neck.x() + perp.x() * neck_half, neck.y() + perp.y() * neck_half)
        path.lineTo(barb.x() + perp.x() * barb_half, barb.y() + perp.y() * barb_half)
        path.lineTo(tip)
        path.lineTo(barb.x() - perp.x() * barb_half, barb.y() - perp.y() * barb_half)
        path.lineTo(neck.x() - perp.x() * neck_half, neck.y() - perp.y() * neck_half)
        path.closeSubpath()
        return path

    def _open_head_path(self, tip: QPointF, out_dir: QPointF,
                        wing_len: float, wing_w: float, notch: float) -> QPainterPath:
        """开口头：一个有尖的人字形

        轮廓直接按点连出来，不是把两根翼线描粗——描边器在两翼夹角处只会接出一
        个半径半线宽的圆头，线一粗，头就是圆钝的，还整整鼓出用户松手位置半个
        线宽。这里头尖就是端点本身：两翼外边从端点起后掠，末端顺着翼向平切，
        内边再交回轴线收成凹口。
        """
        cos_a = math.cos(self.OPEN_HEAD_ANGLE)
        sin_a = math.sin(self.OPEN_HEAD_ANGLE)
        back_x, back_y = -out_dir.x(), -out_dir.y()
        left = QPointF(back_x * cos_a - back_y * sin_a, back_x * sin_a + back_y * cos_a)
        right = QPointF(back_x * cos_a + back_y * sin_a, -back_x * sin_a + back_y * cos_a)
        # 两翼各自朝轴线的法向：外边整条推过去 wing_w 就是内边
        perp_left = self._perp(left)
        in_left = QPointF(-perp_left.x(), -perp_left.y())
        in_right = self._perp(right)

        def wing_end(direction: QPointF, inward: QPointF):
            outer = QPointF(tip.x() + direction.x() * wing_len,
                            tip.y() + direction.y() * wing_len)
            return outer, QPointF(outer.x() + inward.x() * wing_w,
                                  outer.y() + inward.y() * wing_w)

        out_left, inner_left = wing_end(left, in_left)
        out_right, inner_right = wing_end(right, in_right)

        path = QPainterPath()
        path.moveTo(tip)
        path.lineTo(out_left)
        path.lineTo(inner_left)
        path.lineTo(tip.x() + back_x * notch, tip.y() + back_y * notch)
        path.lineTo(inner_right)
        path.lineTo(out_right)
        path.closeSubpath()
        return path

    def _bar_path(self, at: QPointF, out_dir: QPointF,
                  bar_half: float, thickness: float) -> QPainterPath:
        """端点上的垂直短横杠"""
        perp = self._perp(out_dir)
        half_t = thickness / 2
        path = QPainterPath()
        path.moveTo(at.x() + perp.x() * bar_half + out_dir.x() * half_t,
                    at.y() + perp.y() * bar_half + out_dir.y() * half_t)
        path.lineTo(at.x() - perp.x() * bar_half + out_dir.x() * half_t,
                    at.y() - perp.y() * bar_half + out_dir.y() * half_t)
        path.lineTo(at.x() - perp.x() * bar_half - out_dir.x() * half_t,
                    at.y() - perp.y() * bar_half - out_dir.y() * half_t)
        path.lineTo(at.x() + perp.x() * bar_half - out_dir.x() * half_t,
                    at.y() + perp.y() * bar_half - out_dir.y() * half_t)
        path.closeSubpath()
        return path

    def _shaft_path(self, a: QPointF, b: QPointF, mid: QPointF,
                    u_a: QPointF, u_b: QPointF, w_a: float, w_b: float,
                    chord: QPointF) -> QPainterPath:
        """从 a 到 b 的实心箭杆，经过曲线中点 mid

        上下两条边各是一条二次贝塞尔，控制点同样按 P1 = 2M - 0.5*A - 0.5*B 反
        解，保证边线真的贴着曲线走。直线箭头时三点共线，画出来就是直边。
        """
        ab = QPointF(b.x() - a.x(), b.y() - a.y())
        ab_len2 = ab.x() * ab.x() + ab.y() * ab.y()
        if ab_len2 < 1e-6:
            return QPainterPath()

        # mid 在 a→b 上的投影决定这里该多宽：端头削掉的长度两端不一样，直接
        # 取 (w_a+w_b)/2 会让本该笔直的边鼓出一点弧
        t = ((mid.x() - a.x()) * ab.x() + (mid.y() - a.y()) * ab.y()) / ab_len2
        t = min(1.0, max(0.0, t))
        w_mid = w_a + (w_b - w_a) * t

        # mid 是原曲线 t=0.5 处的点：二次贝塞尔在自己中点的切线恒等于弦
        # (end-start)，跟两端切线各自怎么偏都无关。弯得狠一点（比如往回折）
        # u_a、u_b 会指向差很远甚至相反的方向，两者相加求平均就会退化成一个
        # 大小不定、方向说不准的向量——中点的偏移量因此被甩到犄角旮旯，上下
        # 两条边线在中间交叉，杆身画出来就是一段镂空。用弦方向就没有这个问题。
        u_mid = self._unit(chord, ab) or u_a
        perp_a, perp_b, perp_mid = self._perp(u_a), self._perp(u_b), self._perp(u_mid)

        def edge(sign):
            pa = QPointF(a.x() + sign * perp_a.x() * w_a / 2, a.y() + sign * perp_a.y() * w_a / 2)
            pb = QPointF(b.x() + sign * perp_b.x() * w_b / 2, b.y() + sign * perp_b.y() * w_b / 2)
            pm = QPointF(mid.x() + sign * perp_mid.x() * w_mid / 2,
                         mid.y() + sign * perp_mid.y() * w_mid / 2)
            ctrl = QPointF(2 * pm.x() - 0.5 * pa.x() - 0.5 * pb.x(),
                           2 * pm.y() - 0.5 * pa.y() - 0.5 * pb.y())
            return pa, ctrl, pb

        up_a, up_ctrl, up_b = edge(1)
        dn_a, dn_ctrl, dn_b = edge(-1)

        path = QPainterPath()
        path.moveTo(up_a)
        path.quadTo(up_ctrl, up_b)
        path.lineTo(dn_b)
        path.quadTo(dn_ctrl, dn_a)
        path.closeSubpath()
        return path

    def update_geometry(self):
        """更新箭头几何形状"""
        frame = self._frame()
        if frame is None:
            return
        start, end, mid, u_start, u_end, length = frame

        shaft_kind, head_start, head_end, hollow = self.STYLE_SPECS[self._arrow_style]
        m = self._metrics(length, head_start, head_end)

        # 箭杆两端各让出端头占的长度
        trim_start = self._head_trim(head_start, m)
        trim_end = self._head_trim(head_end, m)
        a = QPointF(start.x() + u_start.x() * trim_start, start.y() + u_start.y() * trim_start)
        b = QPointF(end.x() - u_end.x() * trim_end, end.y() - u_end.y() * trim_end)

        if shaft_kind == self.SHAFT_LINE:
            w_a = w_b = m["line_w"]
        elif shaft_kind == self.SHAFT_EVEN:
            w_a = w_b = m["neck_w"]
        else:  # SHAFT_TAPER：没有头的那端收成尖尾
            w_a = m["neck_w"] if head_start != self.HEAD_NONE else m["tail_w"]
            w_b = m["neck_w"] if head_end != self.HEAD_NONE else m["tail_w"]

        pieces = []
        # 两头一挤，杆有可能已经被削没了（画得很短时）——那就只剩两个头。
        # 要用总长减两端裁掉的量来判断，不能把 b-a 投影到 u_start 上：弯曲弯得
        # 狠一点，起点切线方向会偏离整条弧线的走向甚至反过来，投影会把明明还
        # 很长的杆误判成"削没了"，杆身直接消失只剩两个头。
        if length - trim_start - trim_end > 0.5:
            chord = QPointF(end.x() - start.x(), end.y() - start.y())
            pieces.append(self._shaft_path(a, b, mid, u_start, u_end, w_a, w_b, chord))
        pieces.append(self._head_path(head_start, start, QPointF(-u_start.x(), -u_start.y()), m))
        pieces.append(self._head_path(head_end, end, u_end, m))

        filled = QPainterPath()
        for piece in pieces:
            if piece.isEmpty():
                continue
            filled = piece if filled.isEmpty() else filled.united(piece)
        if filled.isEmpty():
            return

        self._hit_path = filled
        if hollow:
            # 描边往轮廓内侧收（描双倍宽再与剪影取交）：骑在轮廓线上描的话，
            # 收成尖的尾巴会被斜接拉出一根长刺——那支空心箭头就比用户拖出来的
            # 那段长出一大截。收进去之后，空心和实心占的地方分毫不差。
            stroker = QPainterPathStroker()
            stroker.setWidth(m["outline_w"] * 2)
            stroker.setJoinStyle(Qt.PenJoinStyle.MiterJoin)
            stroker.setMiterLimit(8)
            self.setPath(stroker.createStroke(filled).intersected(filled).simplified())
        else:
            self.setPath(filled)

    def paint(self, painter, option, widget=None):
        """优化渲染"""
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(self.color)
        painter.drawPath(self.path())

        selection_pen = self.selection_frame_pen()
        if selection_pen is not None:
            selection_pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            selection_pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(selection_pen)
            painter.drawPath(self.path())

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        self.base_width = max(1.0, float(width))
        self.update_geometry()
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        self.base_width = max(1.0, self.base_width * scale)
        self.update_geometry()
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        color = QColor(self.color)
        color.setAlphaF(opacity)
        self.color = color
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        return float(self.base_width)

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.color.alphaF()
