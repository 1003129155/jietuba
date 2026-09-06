"""Pixelated background annotation item."""

from PySide6.QtCore import QPointF, QRectF, QSizeF, Qt
from PySide6.QtGui import QImage, QPainter, QPainterPath, QPainterPathStroker, QTransform
from PySide6.QtWidgets import QGraphicsItem


class MosaicItem(QGraphicsItem):
    """Paint the background's reduced image, blown back up, through a round freehand mask.

    只保存按 block_size 缩小后的小图，绘制时再按块放大回去。关闭
    SmoothPixmapTransform 后放大就是最近邻，每个小图像素铺成一个
    block_size x block_size 的方块，结果与提前算好的全分辨率像素化图逐像素相同，
    但体积只有 1/block_size^2；而且 QImage 是引用计数的隐式共享，所有笔画共用
    同一份小图，内存不随笔画数量增长。
    """

    def __init__(
        self,
        path: QPainterPath,
        brush_width: float,
        block_size: int,
        reduced_image: QImage,
        background_rect: QRectF,
        fill_mode: bool = False,
        smooth: bool = False,
    ):
        super().__init__()
        self._path = QPainterPath()
        self._stroke_shape = QPainterPath()
        self._brush_width = max(1.0, float(brush_width))
        self._block_size = max(2, int(block_size))
        self._reduced_image = QImage(reduced_image)
        self._background_rect = QRectF(background_rect)
        # 框选模式：path 本身就是要挖空的矩形区域，不再用画笔描边。
        self._fill_mode = bool(fill_mode)
        # 放大缩小图时用平滑插值而不是最近邻，插值本身把边界糊开，
        # 是"模糊"种类的全部实现——复用同一份缩小图缓存，不必再算一张全分辨率模糊图。
        self._smooth = bool(smooth)
        self._rect = QRectF()
        self.setZValue(5)

        # 跟画笔/荧光笔的自由笔画一样：可选中（需 Ctrl+点击）、可拖动。
        # 框选（矩形）模式还额外能拖角调整大小——这套交互（8 点缩放、拖动
        # 撤销）全部是 handle_editor.py 里认 rect()/setRect() 的通用逻辑，
        # 这里不需要另写一套；自由涂抹没有"矩形"可言，handle_editor 那边会
        # 跳过给它生成缩放手柄（见 _generate_handles 里的判断）。
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsSelectable, True)
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)
        self.setAcceptHoverEvents(True)

        self.set_path(path)

    def fill_mode(self) -> bool:
        return self._fill_mode

    def smooth(self) -> bool:
        return self._smooth

    def set_smooth(self, smooth: bool):
        """切换已放置图元的马赛克/模糊种类：只是换一下放大时的插值方式，不用重新取图。"""
        smooth = bool(smooth)
        if smooth == self._smooth:
            return
        self._smooth = smooth
        self.update()

    def set_block_size(self, block_size: int, reduced_image: QImage):
        """切换已放置图元的马赛克粒度：粒度变了，缩小图的像素内容也变了，
        调用方必须传入按新 block_size 重新收缩出来的那张图（见
        BackgroundItem.reduced_image），这里不负责重新计算。
        """
        block_size = max(2, int(block_size))
        if block_size == self._block_size:
            return
        self._block_size = block_size
        self._reduced_image = QImage(reduced_image)
        self.update()

    def rect(self) -> QRectF:
        """框选模式下的本地矩形；供 handle_editor 的通用 rect()/setRect() 缩放逻辑使用。"""
        return QRectF(self._rect)

    def setRect(self, rect: QRectF):
        """框选模式下按新矩形重建 path；自由涂抹模式不支持，调用无意义。"""
        if not self._fill_mode:
            return
        path = QPainterPath()
        path.addRect(QRectF(rect))
        self.set_path(path)

    def _make_shape(self, path: QPainterPath) -> QPainterPath:
        if self._fill_mode:
            return QPainterPath(path)

        if path.elementCount() == 0:
            return QPainterPath()

        stroker = QPainterPathStroker()
        stroker.setWidth(self._brush_width)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        shape = stroker.createStroke(path)
        if not shape.isEmpty():
            # 圆头端帽已经把起点包住了，不必再并一个圆点。并集是路径布尔运算，
            # 放在每次 on_move 上会随笔画长度急剧变慢。
            return shape

        # 只按下、没有拖动时 createStroke 得到空路径，退化成一个圆点。
        first = path.elementAt(0)
        radius = self._brush_width / 2.0
        stamp = QPainterPath()
        stamp.addEllipse(QPointF(first.x, first.y), radius, radius)
        return stamp

    def set_path(self, path: QPainterPath):
        self.prepareGeometryChange()
        self._path = QPainterPath(path)
        self._stroke_shape = self._make_shape(self._path)
        if self._fill_mode:
            self._rect = self._path.boundingRect()
        self.update()

    def path(self) -> QPainterPath:
        return QPainterPath(self._path)

    def shape(self) -> QPainterPath:
        return QPainterPath(self._stroke_shape)

    def boundingRect(self) -> QRectF:
        return self._stroke_shape.boundingRect()

    def reduced_image(self) -> QImage:
        return QImage(self._reduced_image)

    def background_rect(self) -> QRectF:
        return QRectF(self._background_rect)

    def translate_background_anchor(self, dx: float, dy: float):
        """把背景锚点平移 (dx, dy)。

        paint() 刻意撤销图元自身的变换，把背景钉死在 _background_rect 描述的
        **场景坐标**上（理由见 paint 里的注释）。代价是这份坐标只对它出生的
        那个场景成立：图元被搬进另一个场景时（钉图克隆），只平移 pos() 不够，
        锚点必须跟着一起搬，否则画出来的是偏移了一整个选区的那一块背景。
        """
        self._background_rect.translate(dx, dy)
        self.update()

    def brush_width(self) -> float:
        return self._brush_width

    def block_size(self) -> int:
        return self._block_size

    def get_stroke_width(self) -> float:
        return self._brush_width

    def paint(self, painter: QPainter, option, widget=None):
        if self._reduced_image.isNull() or self._stroke_shape.isEmpty():
            return
        painter.save()
        try:
            # paint() 收到的 painter 已经叠加了本图元的完整变换（拖动改
            # pos()，拖角缩放改 transform()）。裁剪窗口跟着这份变换走是
            # 对的；但铺开的那张"整背景像素化图"必须钉死在背景真实的场景
            # 位置，不能被这份变换一起拉伸/挪动——不然拖动或缩放马赛克时，
            # 显示内容会跟着搬家/变形，跟真实背景对不上。
            #
            # 做法：先把裁剪路径换算到场景坐标，再把画笔当前变换里"属于
            # 这个图元自己"的那部分撤销掉（只留视图缩放/平移），后面就相当
            # 于直接画在场景坐标系里，用 self.pos()/self.transform() 是什么
            # 都不影响画出来的内容。
            scene_shape = self.mapToScene(self._stroke_shape)
            inverse, invertible = self.sceneTransform().inverted()
            if invertible:
                painter.setTransform(inverse * QTransform(painter.transform()), False)

            painter.setClipPath(scene_shape)
            # 铺开后的尺寸是 block_size 的整数倍，末尾那块可能越过背景边缘，
            # 所以再按背景矩形收一次。
            painter.setClipRect(self._background_rect, Qt.ClipOperation.IntersectClip)
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, self._smooth)
            target = QRectF(
                self._background_rect.topLeft(),
                QSizeF(
                    self._reduced_image.width() * self._block_size,
                    self._reduced_image.height() * self._block_size,
                ),
            )
            painter.drawImage(target, self._reduced_image, QRectF(self._reduced_image.rect()))
        finally:
            painter.restore()
