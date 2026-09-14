"""聚光灯：整个场景只有一张半透明的黑色幕布，每个聚光灯是在幕布上挖出的一个孔。"""

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QGraphicsItem

from .drawing_items import RectItem

_Change = QGraphicsItem.GraphicsItemChange


class SpotlightCurtain(QGraphicsItem):
    """盖住整个场景的黑色幕布，挖掉所有聚光灯的孔。

    不让每个聚光灯各画一层"压暗 + 挖孔"：后画的那层会把前一个孔重新压暗，重叠处
    还会暗好几倍。幕布只有一张，暗度也就只有一份——它属于幕布，不属于某个孔。暗度
    直接用图元自带的不透明度（opacity），幕布本身画的是纯黑。

    幕布不记孔的列表，每次绘制时现查场景里的聚光灯。孔被移动、缩放、撤销时不用谁来
    通知：Qt 重画孔的旧位置和新位置时，盖着整个场景的幕布自然也在这两处重画。只有
    孔的增删和显隐需要通知，见 SpotlightItem.itemChange。

    挖孔用矢量路径而不是全屏位图：一张 4K 的半透明全屏图就要几十 MB，拖动时还得每帧重建。
    """

    # 马赛克(5)之上：孔外的马赛克也要被压暗；荧光笔(10)和普通标注(20)之下：标注不受影响
    Z_VALUE = 6
    DEFAULT_DARKNESS = 0.5

    def __init__(self):
        super().__init__()
        self.setZValue(self.Z_VALUE)
        self.setOpacity(self.DEFAULT_DARKNESS)
        self.setAcceptedMouseButtons(Qt.MouseButton.NoButton)

    @staticmethod
    def find(scene):
        """场景里现有的幕布，没有就返回 None。"""
        if scene is None:
            return None
        return next((item for item in scene.items() if isinstance(item, SpotlightCurtain)), None)

    @classmethod
    def of(cls, scene):
        """场景里的幕布，第一次用到时才建。截图和钉图的场景都走这里，不必各自接线。"""
        curtain = cls.find(scene)
        if curtain is None:
            curtain = cls()
            scene.addItem(curtain)
        return curtain

    def boundingRect(self) -> QRectF:
        scene = self.scene()
        return QRectF(scene.sceneRect()) if scene is not None else QRectF()

    def paint(self, painter, option, widget=None):
        spotlights = [
            item for item in self.scene().items()
            if isinstance(item, SpotlightItem) and item.isVisible()
        ]
        # 判断的是"有没有聚光灯"而不是"孔是不是空的"：刚按下还没拖开时孔的面积为 0，
        # 幕布此刻就得出现，否则之后只有孔附近会被重画，孔外那一大片一直是亮的。
        if not spotlights:
            return
        holes = QPainterPath()
        for spotlight in spotlights:
            # 逐个求并集，不能全塞进一条路径：默认的奇偶填充会让重叠的部分重新变暗
            holes = holes.united(spotlight.hole_path())
        curtain = QPainterPath()
        curtain.addRect(self.boundingRect())
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillPath(curtain.subtracted(holes), QColor(0, 0, 0))


class SpotlightItem(RectItem):
    """聚光灯的一个孔。

    拖角缩放、拖动、撤销、橡皮擦、钉图克隆时的位置与变换，全是矩形那一套，所以直接
    继承 RectItem。它自己不画内容：画笔和画刷都是空的，RectItem.paint 只剩选中和
    悬停时的虚线框；压暗交给幕布。

    点选、悬停、橡皮擦认的是整个孔，和荧光笔矩形、框选马赛克一样：孔没有描边，只认边
    太难选中。代价也和它们一样——橡皮擦划过孔内会把孔一起擦掉，聚光灯工具在已有的孔里
    按下是选中它而不是画新孔。孔外的暗色是所有孔共用的幕布，不属于任何一个孔，不参与命中。
    """

    def __init__(self, rect: QRectF, corner_radius: float = 0.0):
        super().__init__(rect, QPen(Qt.PenStyle.NoPen), corner_radius)
        # 压在幕布上面：选中时的虚线框正好骑在孔的边上，不能被幕布压暗一半
        self.setZValue(SpotlightCurtain.Z_VALUE + 1)

    def shape(self) -> QPainterPath:
        # 圆角沿用矩形的圆角手柄；半径为 0 时 addRoundedRect 就是直角矩形
        path = QPainterPath()
        radius = self.get_corner_radius()
        path.addRoundedRect(self.rect(), radius, radius)
        return path

    def hole_path(self) -> QPainterPath:
        """孔在场景坐标里的形状；经过 mapToScene，旋转或缩放过的孔也对得上。"""
        return self.mapToScene(self.shape())

    def itemChange(self, change, value):
        # 孔的增删和显隐会改变整张幕布：一个孔都没有时幕布不画，所以第一个孔出现、
        # 最后一个孔消失时，孔外的整片区域都要重画。ItemSceneChange 时孔还在旧场景里，
        # 通知旧场景的幕布；ItemSceneHasChanged 时已进了新场景，幕布没有就顺手建出来。
        if change in (_Change.ItemSceneChange, _Change.ItemVisibleHasChanged):
            curtain = SpotlightCurtain.find(self.scene())
            if curtain is not None:
                curtain.update()
        elif change == _Change.ItemSceneHasChanged and self.scene() is not None:
            SpotlightCurtain.of(self.scene()).update()
        return super().itemChange(change, value)

    # -- 统一属性接口：透明度就是整张幕布的暗度，孔只负责转交；孔没有线宽 --

    def set_visual_opacity(self, opacity: float) -> bool:
        if self.scene() is None:
            return False
        SpotlightCurtain.of(self.scene()).setOpacity(max(0.0, min(1.0, float(opacity))))
        return True

    def get_visual_opacity(self) -> float:
        curtain = SpotlightCurtain.find(self.scene())
        return curtain.opacity() if curtain is not None else SpotlightCurtain.DEFAULT_DARKNESS

    def get_stroke_width(self):
        return None

    def set_stroke_width(self, width: float):
        pass

    def scale_stroke_width(self, scale: float) -> bool:
        return False
