"""Pixelated background annotation item."""

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QImage, QPainter, QPainterPath, QPainterPathStroker
from PySide6.QtWidgets import QGraphicsItem


class MosaicItem(QGraphicsItem):
    """Paint a cached pixelated image through a round freehand mask."""

    def __init__(
        self,
        path: QPainterPath,
        brush_width: float,
        block_size: int,
        patch_image: QImage,
        patch_origin: QPointF,
    ):
        super().__init__()
        self._path = QPainterPath()
        self._stroke_shape = QPainterPath()
        self._brush_width = max(1.0, float(brush_width))
        self._block_size = max(2, int(block_size))
        self._patch_image = QImage(patch_image)
        self._patch_origin = QPointF(patch_origin)
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)
        self.setZValue(5)
        self.set_path(path)

    def _make_shape(self, path: QPainterPath) -> QPainterPath:
        shape = QPainterPath()
        if path.elementCount() == 0:
            return shape

        stroker = QPainterPathStroker()
        stroker.setWidth(self._brush_width)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        shape = stroker.createStroke(path)

        first = path.elementAt(0)
        radius = self._brush_width / 2.0
        stamp = QPainterPath()
        stamp.addEllipse(QPointF(first.x, first.y), radius, radius)
        return shape.united(stamp)

    def set_path(self, path: QPainterPath):
        self.prepareGeometryChange()
        self._path = QPainterPath(path)
        self._stroke_shape = self._make_shape(self._path)
        self.update()

    def path(self) -> QPainterPath:
        return QPainterPath(self._path)

    def shape(self) -> QPainterPath:
        return QPainterPath(self._stroke_shape)

    def boundingRect(self) -> QRectF:
        return self._stroke_shape.boundingRect()

    def set_patch(self, image: QImage, origin: QPointF):
        self._patch_image = QImage(image)
        self._patch_origin = QPointF(origin)
        self.update()

    def patch_image(self) -> QImage:
        return QImage(self._patch_image)

    def patch_origin(self) -> QPointF:
        return QPointF(self._patch_origin)

    def brush_width(self) -> float:
        return self._brush_width

    def block_size(self) -> int:
        return self._block_size

    def get_stroke_width(self) -> float:
        return self._brush_width

    def paint(self, painter: QPainter, option, widget=None):
        if self._patch_image.isNull() or self._stroke_shape.isEmpty():
            return
        painter.save()
        try:
            painter.setClipPath(self._stroke_shape)
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, False)
            target = QRectF(
                self._patch_origin.x(),
                self._patch_origin.y(),
                self._patch_image.width(),
                self._patch_image.height(),
            )
            painter.drawImage(target, self._patch_image, QRectF(self._patch_image.rect()))
        finally:
            painter.restore()
