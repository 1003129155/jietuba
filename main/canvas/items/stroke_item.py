"""
画笔/荧光笔的笔迹图元
"""
from __future__ import annotations

import math

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPainterPathStroker, QPen
from PySide6.QtWidgets import QGraphicsItem

from core import log_warning
from core.logger import T
from .drawing_items import DrawingItemMixin


def _append_segments(target: QPainterPath, source: QPainterPath, start: int, limit: int) -> tuple[int, int]:
    """把 source 第 start 个元素起的线段接到 target 后面，最多 limit 段；返回停在第几个元素、接了几段。"""
    count = source.elementCount()
    i, segments = start, 0
    while i < count and segments < limit:
        element = source.elementAt(i)
        if element.isCurveTo():
            if i + 2 >= count:
                break
            c2, end = source.elementAt(i + 1), source.elementAt(i + 2)
            target.cubicTo(element.x, element.y, c2.x, c2.y, end.x, end.y)
            i += 3
        elif element.isMoveTo():
            target.moveTo(element.x, element.y)
            i += 1
        else:
            target.lineTo(element.x, element.y)
            i += 1
        segments += 1
    return i, segments


class _Chunk:
    """路径里第 [first, end) 号元素，单独存成一条子路径。"""

    __slots__ = ("first", "end", "segments", "path", "rect", "travelled", "length", "outline")

    def __init__(self, first: int, end: int, segments: int, path: QPainterPath, reach: float, travelled: float):
        self.first = first
        self.end = end
        self.segments = segments
        self.path = path
        self.rect = path.controlPointRect().adjusted(-reach, -reach, reach, reach)
        self.travelled = travelled  # 这一块之前的路径长度，虚线从这里接上节奏
        self.length = path.length()
        self.outline = None  # 虚线笔画出来的轮廓，用到时才算


class StrokeItem(DrawingItemMixin, QGraphicsItem):
    """画笔/荧光笔的自由笔迹。

    路径按 CHUNK_SEGMENTS 段分块，重绘时只把和重绘区相交的块连成一条路径一次画完，半透明的笔
    在块的接头处不会叠深。画的过程中 extend_path 只把变了的块标脏，笔迹再长，每帧也只重绘笔尖
    附近；旁边的图元重绘时，这条笔迹也只连带画相交的那几块。

    虚线笔按块各自算出轮廓再填充，每块按前面的总长接上虚线节奏，局部重绘和整条画出来的一致。
    Qt 直接画虚线时节奏随重绘区变，局部重绘会在重绘区边上错开。
    """

    CHUNK_SEGMENTS = 8
    GROW_MARGIN = 256.0
    HIT_MARGIN = 10.0  # 点击热区比笔迹两侧各宽这么多，细线也好点中

    def __init__(self, path: QPainterPath, pen: QPen, is_highlighter: bool = False):
        super().__init__()
        self._init_drawing_mixin()
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemUsesExtendedStyleOption, True)
        self.is_highlighter = is_highlighter
        # 荧光笔压在画笔下面
        self.setZValue(10 if is_highlighter else 20)
        self._path = QPainterPath(path)
        self._pen = QPen(pen)
        self._chunks = None  # 第一次局部重绘时才切
        self._shape_cache = None  # createStroke 很贵，移动时会反复要
        self._bounds = self._exact_bounds()

    def shows_selection_frame(self) -> bool:
        """自由笔画不画候选/选中框，理由见基类。"""
        return False

    # -- 路径与画笔 --

    def path(self) -> QPainterPath:
        return QPainterPath(self._path)

    def setPath(self, path: QPainterPath):
        self.prepareGeometryChange()
        self._path = QPainterPath(path)
        self._chunks = None
        self._shape_cache = None
        self._bounds = self._exact_bounds()
        self.update()

    def extend_path(self, source: QPainterPath, dirty_from: int):
        """画的过程中跟上 source：它第 dirty_from 个元素以前和上次一样，往后只改了坐标或追加了元素。

        只抄改过的这一截，不和 source 共用一个对象：画笔接着还会就地改写它。外接矩形每次多扩
        GROW_MARGIN，扩的时候才整块重绘一次；画完调 finish_path 收紧。
        """
        path = self._path
        count = path.elementCount()
        for i in range(min(dirty_from, count), count):
            element = source.elementAt(i)
            path.setElementPositionAt(i, element.x, element.y)
        _append_segments(path, source, count, source.elementCount())
        self._shape_cache = None

        chunks = self._chunks if self._chunks is not None else []
        # 改动都在末尾，从后往前找
        keep = len(chunks)
        while keep and chunks[keep - 1].end > dirty_from:
            keep -= 1
        # 没满的末块后面又接了元素：并进它重切，不另起一小块
        if keep and chunks[keep - 1].segments < self.CHUNK_SEGMENTS and chunks[keep - 1].end < path.elementCount():
            keep -= 1
        stale = chunks[keep:]
        del chunks[keep:]
        if chunks:
            start, travelled = chunks[-1].end, chunks[-1].travelled + chunks[-1].length
        else:
            start, travelled = 1, 0.0
        fresh = self._build_chunks(start, travelled)
        chunks.extend(fresh)
        self._chunks = chunks

        dirty = QRectF()
        for chunk in stale:
            dirty = dirty.united(chunk.rect)
        for chunk in fresh:
            dirty = dirty.united(chunk.rect)
        if dirty.isEmpty():
            return
        if not self._bounds.contains(dirty):
            self.prepareGeometryChange()
            grow = self.GROW_MARGIN
            self._bounds = self._bounds.united(dirty).adjusted(-grow, -grow, grow, grow)
        self.update(dirty)

    def finish_path(self):
        """画完了：收紧画的过程中多留的外接矩形，选中手柄挂在它的角上。"""
        bounds = self._exact_bounds()
        if bounds != self._bounds:
            self.prepareGeometryChange()
            self._bounds = bounds

    def pen(self) -> QPen:
        return QPen(self._pen)

    def setPen(self, pen: QPen):
        pen = QPen(pen)
        reshaped = self._outline_key(pen) != self._outline_key(self._pen)
        self._pen = pen
        if reshaped:
            self._chunks = None
            self._shape_cache = None
            bounds = self._exact_bounds()
            if bounds != self._bounds:
                self.prepareGeometryChange()
                self._bounds = bounds
        self.update()

    @staticmethod
    def _outline_key(pen: QPen):
        """画出来的形状只取决于这些；只换颜色时分块和虚线轮廓都还能用。"""
        return (pen.widthF(), pen.style(), tuple(pen.dashPattern()), pen.dashOffset(),
                pen.capStyle(), pen.joinStyle(), pen.miterLimit(), pen.isCosmetic())

    # -- 几何 --

    def _paint_reach(self) -> float:
        """画出来的像素离路径控制点最远多少：半个笔宽，方头和尖角另算，再留 2px 给抗锯齿。"""
        pen = self._pen
        reach = pen.widthF() / 2
        if pen.capStyle() == Qt.PenCapStyle.SquareCap:
            reach *= math.sqrt(2)
        if pen.joinStyle() in (Qt.PenJoinStyle.MiterJoin, Qt.PenJoinStyle.SvgMiterJoin):
            reach = max(reach, pen.widthF() * pen.miterLimit())
        return reach + 2

    def _exact_bounds(self) -> QRectF:
        """贴着曲线本身算，选中手柄挂在它的角上；画出来的像素和点击热区都要落在里面。

        热区的圆头和偏移曲线是近似出来的，会比理论半径略鼓出一点，多留 1px。
        """
        if self._path.isEmpty():
            return QRectF()
        reach = max(self._paint_reach(), self._pen.widthF() / 2 + self.HIT_MARGIN + 1)
        return self._path.boundingRect().adjusted(-reach, -reach, reach, reach)

    def _build_chunks(self, start: int, travelled: float) -> list:
        """从第 start 个元素切到末尾；travelled 是 start 之前的路径长度。"""
        path = self._path
        reach = self._paint_reach()
        chunks = []
        i = start
        while i < path.elementCount():
            origin = path.elementAt(i - 1)
            sub = QPainterPath(QPointF(origin.x, origin.y))
            end, segments = _append_segments(sub, path, i, self.CHUNK_SEGMENTS)
            if end == i:
                break
            chunk = _Chunk(i, end, segments, sub, reach, travelled)
            travelled += chunk.length
            chunks.append(chunk)
            i = end
        return chunks

    def _all_chunks(self) -> list:
        if self._chunks is None:
            self._chunks = self._build_chunks(1, 0.0)
        return self._chunks

    def boundingRect(self) -> QRectF:
        return QRectF(self._bounds)

    def shape(self) -> QPainterPath:
        if self._shape_cache is None:
            if self._path.isEmpty():
                return QPainterPath()
            stroker = QPainterPathStroker()
            stroker.setWidth(self._pen.widthF() + 2 * self.HIT_MARGIN)
            stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
            stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            self._shape_cache = stroker.createStroke(self._path)
        return self._shape_cache

    # -- 绘制 --

    def paint(self, painter, option, widget=None):
        if self._path.elementCount() < 2:
            return
        try:
            if self.is_highlighter:
                # 荧光笔使用正片叠底
                painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            exposed = option.exposedRect
            if exposed.isNull():
                # 直接拿新建的 QStyleOptionGraphicsItem 调 paint（画预览图）时没填重绘区
                exposed = self._bounds
            if self._dashed():
                painter.setPen(Qt.PenStyle.NoPen)
                painter.setBrush(self._pen.brush())
                painter.drawPath(self._dash_outline(exposed))
            else:
                painter.setPen(self._pen)
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawPath(self._visible_path(exposed))
        except Exception as e:
            log_warning(T("DrawingItem paint 异常: {e}", e=e), "Canvas")

    def _dashed(self) -> bool:
        return self._pen.style() not in (Qt.PenStyle.SolidLine, Qt.PenStyle.NoPen)

    def _visible_path(self, exposed: QRectF) -> QPainterPath:
        """和 exposed 相交的块连成的路径；整条都露出来时就是路径本身。"""
        if exposed.contains(self._bounds):
            return self._path
        merged = QPainterPath()
        joined_at = None
        for chunk in self._all_chunks():
            if not chunk.rect.intersects(exposed):
                continue
            if chunk.first == joined_at:
                merged.connectPath(chunk.path)
            else:
                merged.addPath(chunk.path)
            joined_at = chunk.end
        return merged

    def _dash_outline(self, exposed: QRectF) -> QPainterPath:
        """和 exposed 相交的块画成虚线后的轮廓，合成一条按非零规则填充，块的接头处不叠深。"""
        whole = exposed.contains(self._bounds)
        width = self._pen.widthF() or 1.0
        merged = QPainterPath()
        merged.setFillRule(Qt.FillRule.WindingFill)
        for chunk in self._all_chunks():
            if not whole and not chunk.rect.intersects(exposed):
                continue
            if chunk.outline is None:
                stroker = QPainterPathStroker(self._pen)
                # 虚线偏移以笔宽为单位
                stroker.setDashOffset(self._pen.dashOffset() + chunk.travelled / width)
                chunk.outline = stroker.createStroke(chunk.path)
            merged.addPath(chunk.outline)
        return merged

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        pen = self.pen()
        pen.setWidthF(max(1.0, float(width)))
        self.setPen(pen)

    def scale_stroke_width(self, scale: float) -> bool:
        pen = self.pen()
        pen.setWidthF(max(1.0, pen.widthF() * scale))
        self.setPen(pen)
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        pen = self.pen()
        color = QColor(pen.color())
        color.setAlphaF(opacity)
        pen.setColor(color)
        self.setPen(pen)
        self.setOpacity(1.0)
        return True

    def get_stroke_width(self) -> float | None:
        width = float(self._pen.widthF())
        # 荧光笔的笔按设置宽度的 3 倍建
        return width / 3.0 if self.is_highlighter else width

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.pen().color().alphaF()
