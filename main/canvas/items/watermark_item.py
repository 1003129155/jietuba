"""水印：盖在选区上的一层文字，平铺或排成菱形网格铺满选区，或放在中间、四角、上下边的中间。

只存参数、不存位图：每次重绘只画与重绘区相交的那几条文字（和网格线）。实测 2560×1440 整屏重画约 3 ms、
4K 约 8 ms，画笔拖动的局部重绘不到 0.2 ms。
"""

import math
import re
from dataclasses import dataclass, replace
from datetime import datetime

from PySide6.QtCore import QDateTime, QLineF, QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QFontMetricsF, QImage, QPainter, QPainterPath, QPen, QTransform
from PySide6.QtWidgets import QGraphicsItem

LAYOUT_TILE = "tile"
LAYOUT_DIAMOND = "diamond"
LAYOUT_CENTER = "center"
# 位置 → 文字块中心在选区里的相对位置（0 左/上，0.5 中，1 右/下）
ANCHORS = {
    LAYOUT_CENTER: (0.5, 0.5),
    "top_left": (0.0, 0.0),
    "top": (0.5, 0.0),
    "top_right": (1.0, 0.0),
    "bottom_left": (0.0, 1.0),
    "bottom": (0.5, 1.0),
    "bottom_right": (1.0, 1.0),
}
LAYOUTS = (LAYOUT_TILE, LAYOUT_DIAMOND) + tuple(ANCHORS)

LINE_NONE, LINE_SOLID, LINE_DASH = "none", "solid", "dash"
LINE_STYLES = (LINE_NONE, LINE_SOLID, LINE_DASH)

MIN_FONT_SIZE, MAX_FONT_SIZE = 8, 96
MIN_SPACING, MAX_SPACING = 0, 100
MIN_ANGLE, MAX_ANGLE = -90, 90

# 间距是字号的十分之几：字号变了疏密观感不变；纵向留白是横向的四分之三
SPACING_UNIT = 0.1
VERTICAL_GAP_RATIO = 0.75
EDGE_MARGIN = 0.6
GLYPH_OVERHANG = 0.25

# 菱形网格：格子是正方形转 45°，行距是格距的一半；线在文字外接框外多留 LINE_TEXT_GAP 个字号；
# 线宽随字号放大但不细于 1 像素，虚线的段长、空长以线宽为单位
DIAMOND_ROW_RATIO = 0.5
LINE_TEXT_GAP = 0.3
LINE_WIDTH_DIVISOR = 24
LINE_DASH_PATTERN = [4.0, 3.0]

# $…$ 里是日期格式，按水印创建的那一刻展开；没配对的 $ 原样保留
TIME_TOKEN = re.compile(r"\$([^$\n]*)\$")


@dataclass(frozen=True)
class WatermarkStyle:
    color: str = "#808080"
    font_size: int = 24     # 像素：导出图和屏幕上一样大，不随 DPI 变
    opacity: float = 0.35
    layout: str = LAYOUT_TILE
    spacing: int = 40
    angle: int = -30        # 只用于平铺：转的是文字，格子不转
    font_family: str = ""   # 空 = 跟随界面语言的默认字体
    # 菱形整张网格（文字和线）一起转，所以单独存：它的 0° 是菱形正立，和平铺默认的 -30° 不是一回事
    diamond_angle: int = 0
    line_style: str = LINE_SOLID  # 只用于菱形

    def normalized(self) -> "WatermarkStyle":
        def number(value, default, low, high, cast=int):
            try:
                value = cast(round(float(value))) if cast is int else cast(value)
            except (TypeError, ValueError):
                value = default
            return max(low, min(high, value))

        color = QColor(self.color)
        return WatermarkStyle(
            color=color.name() if color.isValid() else WatermarkStyle.color,
            font_size=number(self.font_size, WatermarkStyle.font_size, MIN_FONT_SIZE, MAX_FONT_SIZE),
            opacity=number(self.opacity, WatermarkStyle.opacity, 0.0, 1.0, float),
            layout=self.layout if self.layout in LAYOUTS else LAYOUT_TILE,
            spacing=number(self.spacing, WatermarkStyle.spacing, MIN_SPACING, MAX_SPACING),
            angle=number(self.angle, WatermarkStyle.angle, MIN_ANGLE, MAX_ANGLE),
            font_family=str(self.font_family or ""),
            diamond_angle=number(self.diamond_angle, WatermarkStyle.diamond_angle, MIN_ANGLE, MAX_ANGLE),
            line_style=self.line_style if self.line_style in LINE_STYLES else WatermarkStyle.line_style,
        )

    def with_changes(self, **changes) -> "WatermarkStyle":
        return replace(self, **changes).normalized()


def expand_text(template: str, when: datetime = None) -> str:
    """把 $yyyy-MM-dd HH:mm:ss$ 这样的占位换成 when 那一刻的时间，格式写法同 Qt 的日期格式。"""
    moment = QDateTime(when or datetime.now())

    def format_token(match):
        pattern = match.group(1)
        return moment.toString(pattern) if pattern else "$"

    return TIME_TOKEN.sub(format_token, str(template or "")).strip()


def watermark_font(style: WatermarkStyle) -> QFont:
    from core.constants import get_default_text_font_for_language

    font = QFont(style.font_family or get_default_text_font_for_language())
    font.setPixelSize(style.font_size)
    return font


class _TextBlock:
    """多行文字排成一块：各行水平居中，块的中心在原点。"""

    def __init__(self, text: str, font: QFont):
        metrics = QFontMetricsF(font)
        self.lines = text.split("\n")
        self.widths = [metrics.horizontalAdvance(line) for line in self.lines]
        self.line_height = metrics.lineSpacing()
        self.ascent = metrics.ascent()
        self.width = max(self.widths, default=0.0)
        self.height = metrics.height() + self.line_height * (len(self.lines) - 1)

    def baselines(self):
        top = -self.height / 2
        for index, (line, width) in enumerate(zip(self.lines, self.widths)):
            yield QPointF(-width / 2, top + self.ascent + index * self.line_height), line


def _tile_centers(rect: QRectF, area: QRectF, half_w: float, half_h: float, cell_w: float, cell_h: float):
    """平铺的文字中心点，只给外接框（中心 ± half_w/half_h）与 area 相交的那些。

    网格锚在选区左上角，奇数行错开半格；选区移动时水印跟着走，不会在内容上滑动。
    """
    top = math.ceil((area.top() - half_h - rect.top()) / cell_h - 0.5)
    bottom = math.floor((area.bottom() + half_h - rect.top()) / cell_h - 0.5)
    for row in range(top, bottom + 1):
        y = rect.top() + (row + 0.5) * cell_h
        shift = cell_w / 2 if row % 2 else 0.0
        left = math.ceil((area.left() - half_w - rect.left() - shift) / cell_w - 0.5)
        right = math.floor((area.right() + half_w - rect.left() - shift) / cell_w - 0.5)
        for col in range(left, right + 1):
            yield QPointF(rect.left() + shift + (col + 0.5) * cell_w, y)


def diamond_cell(block: _TextBlock, style: WatermarkStyle) -> float:
    """菱形的水平对角线长，也是同一行相邻两块文字的中心距。

    最紧时同一行的、隔一行的、斜对角的相邻文字都刚好不重叠，再加上间距。只看文字自己的外接框、
    不看角度：网格是整张转的，转动时格子大小不变。
    """
    tight = max(block.width, block.height, 2 * min(block.width, block.height), 1.0)
    return tight + style.font_size * style.spacing * SPACING_UNIT


def diamond_layout(rect: QRectF, block: _TextBlock, style: WatermarkStyle, exposed: QRectF = None,
                   with_lines: bool = True):
    """菱形网格里画哪几块文字、哪几段线：([文字中心], [线段])，都在选区坐标里。

    网格锚在选区左上角、绕它转 diamond_angle；奇数行错开半格，文字落在菱形的左右顶点上。
    线连相邻两块文字，到文字外接框就停，不从字上穿过。每条线归它上端的文字画，局部重绘时
    每个方向取到重绘区外最近的一圈文字，重绘区边缘的线才不会漏画。
    """
    area = rect if exposed is None else rect.intersected(exposed)
    if rect.isEmpty() or area.isEmpty():
        return [], []
    to_rect = QTransform()
    to_rect.translate(rect.left(), rect.top())
    to_rect.rotate(style.diamond_angle)
    inverse, _invertible = to_rect.inverted()
    wanted = inverse.mapRect(QRectF(area))

    cell = diamond_cell(block, style)
    row_height = cell * DIAMOND_ROW_RATIO
    half_cell = cell / 2
    gap = style.font_size * LINE_TEXT_GAP
    margin = gap
    # 线沿对角方向离开文字外接框：横竖两个方向取先出框的那个，用占整条边的比例表示；
    # 两头各让出这一段还有剩，才画线
    exit_at = min((block.width / 2 + gap) / half_cell, (block.height / 2 + gap) / row_height)
    draw_lines = with_lines and exit_at < 0.5

    centers, segments = [], []
    top = math.floor((wanted.top() - margin) / row_height - 0.5)
    bottom = math.ceil((wanted.bottom() + margin) / row_height - 0.5)
    for row in range(top, bottom + 1):
        y = (row + 0.5) * row_height
        shift = half_cell if row % 2 else 0.0
        left = math.floor((wanted.left() - margin - shift) / cell - 0.5)
        right = math.ceil((wanted.right() + margin - shift) / cell - 0.5)
        for col in range(left, right + 1):
            x = shift + (col + 0.5) * cell
            centers.append(to_rect.map(QPointF(x, y)))
            if draw_lines:
                for side in (-1, 1):
                    start = QPointF(x + side * half_cell * exit_at, y + row_height * exit_at)
                    end = QPointF(x + side * half_cell * (1 - exit_at), y + row_height * (1 - exit_at))
                    segments.append(QLineF(to_rect.map(start), to_rect.map(end)))
    return centers, segments


def watermark_placements(rect: QRectF, text: str, style: WatermarkStyle, exposed: QRectF = None,
                         font: QFont = None, block: _TextBlock = None):
    """[(文字块中心, 旋转角度)]：画哪几块、画在哪。exposed 为空表示整个选区都要画。"""
    if not text or rect.isEmpty():
        return []
    block = block or _TextBlock(text, font or watermark_font(style))
    if style.layout == LAYOUT_DIAMOND:
        centers, _segments = diamond_layout(rect, block, style, exposed, with_lines=False)
        return [(center, float(style.diamond_angle)) for center in centers]
    anchor = ANCHORS.get(style.layout)
    if anchor is not None:
        margin = style.font_size * EDGE_MARGIN
        inner = rect.adjusted(margin + block.width / 2, margin + block.height / 2,
                              -margin - block.width / 2, -margin - block.height / 2)
        # 选区比文字还窄时退回选区中心，不让文字跑到选区外面去
        x = inner.left() + inner.width() * anchor[0] if inner.width() > 0 else rect.center().x()
        y = inner.top() + inner.height() * anchor[1] if inner.height() > 0 else rect.center().y()
        return [(QPointF(x, y), 0.0)]

    area = rect if exposed is None else rect.intersected(exposed)
    if area.isEmpty():
        return []
    rad = math.radians(abs(style.angle))
    box_w = block.width * math.cos(rad) + block.height * math.sin(rad)
    box_h = block.width * math.sin(rad) + block.height * math.cos(rad)
    gap = style.font_size * style.spacing * SPACING_UNIT
    cell_w = box_w + max(gap, 1.0)
    cell_h = box_h + max(gap * VERTICAL_GAP_RATIO, 1.0)
    # 字形可能略微伸出字体度量的框（抗锯齿、字形外悬），相交判断多留一点
    pad = style.font_size * GLYPH_OVERHANG
    centers = _tile_centers(rect, area, box_w / 2 + pad, box_h / 2 + pad, cell_w, cell_h)
    return [(center, float(style.angle)) for center in centers]


def paint_watermark(painter: QPainter, rect: QRectF, text: str, style: WatermarkStyle, exposed: QRectF = None,
                    font: QFont = None):
    """在 rect 里画水印，超出 rect 的部分裁掉。画布图元和不经过画布的截图共用这一份。"""
    if not text:
        return
    font = font or watermark_font(style)
    block = _TextBlock(text, font)
    segments = []
    if style.layout == LAYOUT_DIAMOND:
        centers, segments = diamond_layout(rect, block, style, exposed, style.line_style != LINE_NONE)
        placements = [(center, float(style.diamond_angle)) for center in centers]
    else:
        placements = watermark_placements(rect, text, style, exposed, font, block)
    if not placements:
        return

    painter.save()
    try:
        painter.setClipRect(rect, Qt.ClipOperation.IntersectClip)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
        painter.setFont(font)
        # 不透明度放进画笔颜色，不用 painter.setOpacity：带整体不透明度的文字走慢路径，整屏要慢 5 倍
        color = QColor(style.color)
        color.setAlphaF(style.opacity)
        if segments:
            pen = QPen(color, max(1.0, style.font_size / LINE_WIDTH_DIVISOR))
            pen.setCapStyle(Qt.PenCapStyle.FlatCap)
            if style.line_style == LINE_DASH:
                pen.setDashPattern(LINE_DASH_PATTERN)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            painter.setPen(pen)
            painter.drawLines(segments)
        painter.setPen(color)
        base = painter.transform()
        lines = list(block.baselines())
        for center, angle in placements:
            transform = QTransform(base)
            transform.translate(center.x(), center.y())
            transform.rotate(angle)
            painter.setTransform(transform)
            for origin, line in lines:
                painter.drawText(origin, line)
    finally:
        painter.restore()


def apply_watermark(image: QImage, text: str, style: WatermarkStyle) -> QImage:
    """给一张已经截好的图加水印，返回新图，原图不动。"""
    if image.isNull() or not text:
        return image
    result = image.convertToFormat(QImage.Format.Format_ARGB32_Premultiplied)
    painter = QPainter(result)
    try:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        paint_watermark(painter, QRectF(result.rect()), text, style)
    finally:
        painter.end()
    return result


class WatermarkItem(QGraphicsItem):
    """场景里唯一的一层水印，范围始终是所在场景的选区。

    截图场景的选区移动、缩放时由场景通知它跟上；钉图场景的选区就是整张图。
    文字里的时间按 created_at 展开：加上水印、重新应用文字时定下，之后预览、导出、钉图都不再变。
    水印不参与命中：点选、悬停、橡皮擦都穿过它落到下面的标注上。
    """

    # 文字(30)之上、选区框(101)之下：水印盖住所有标注
    Z_VALUE = 50

    def __init__(self, template: str, style: WatermarkStyle, created_at: datetime = None):
        super().__init__()
        self._created_at = created_at or datetime.now()
        self._template = str(template or "")
        self._text = expand_text(self._template, self._created_at)
        self._style = style.normalized()
        # 字体在定样式时解析一次；重绘频繁，不在 paint 里查系统字体
        self._font = watermark_font(self._style)
        self._rect = QRectF()
        self.setZValue(self.Z_VALUE)
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        # 要拿到 option.exposedRect，局部重绘时才能只画相交的那几条
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemUsesExtendedStyleOption, True)

    @staticmethod
    def find(scene):
        if scene is None:
            return None
        return next((item for item in scene.items() if isinstance(item, WatermarkItem)), None)

    def template(self) -> str:
        return self._template

    def created_at(self) -> datetime:
        return self._created_at

    def text(self) -> str:
        return self._text

    def set_template(self, template: str, when: datetime = None):
        """换文字；给了 when 就把水印的时间也换成那一刻。"""
        template = str(template or "")
        if template == self._template and when is None:
            return
        self._template = template
        if when is not None:
            self._created_at = when
        self._text = expand_text(template, self._created_at)
        self.update()

    def style(self) -> WatermarkStyle:
        return self._style

    def set_style(self, style: WatermarkStyle):
        style = style.normalized()
        if style != self._style:
            self._style = style
            self._font = watermark_font(style)
            self.update()

    def set_scene_rect(self, rect: QRectF):
        """把水印范围对到场景坐标里的 rect（一般是选区）。"""
        local = self.mapRectFromScene(QRectF(rect)) if rect is not None else QRectF()
        if local != self._rect:
            self.prepareGeometryChange()
            self._rect = local

    def rect(self) -> QRectF:
        return QRectF(self._rect)

    def boundingRect(self) -> QRectF:
        return QRectF(self._rect)

    def shape(self) -> QPainterPath:
        return QPainterPath()

    def itemChange(self, change, value):
        if change == QGraphicsItem.GraphicsItemChange.ItemSceneHasChanged:
            model = getattr(self.scene(), "selection_model", None)
            if model is not None:
                self.set_scene_rect(model.rect())
        return super().itemChange(change, value)

    def paint(self, painter, option, widget=None):
        exposed = option.exposedRect if option is not None else QRectF()
        paint_watermark(painter, self._rect, self._text, self._style,
                        None if exposed.isEmpty() else exposed, self._font)
