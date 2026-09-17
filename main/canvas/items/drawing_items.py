"""
矢量绘图图元
定义画笔、形状、荧光笔等基于 QGraphicsItem 的图元
"""
from __future__ import annotations

from PySide6.QtWidgets import (
    QGraphicsPathItem, QGraphicsRectItem, QGraphicsEllipseItem, QGraphicsItem,
)
from PySide6.QtGui import QPen, QPainter, QPainterPath, QColor, QFont, QPainterPathStroker, QBrush
from PySide6.QtCore import Qt, QRectF, QPointF
from core import log_warning
from core.logger import T

class DrawingItemMixin:
    """绘图图元通用属性

    候选/选中框的"什么时候画、画成什么样"都归这里，子类只提供"画的是什么形状"
    （见 selection_frame_pen 的用法）。

    这套状态机曾经在矩形、椭圆、箭头、序号里各复制了一份（四份逐字节相同），
    文字和框选马赛克则整个漏掉：加一个图元类型时，候选反馈不会自动跟过来，
    抄漏了也不报错、不挂测试，只是少一圈框。收进来之后默认就是对的，要"故意
    不画"才得写一行覆盖。

    [WARN] 继承时 mixin 必须写在 Qt 基类前面（DrawingItemMixin, QGraphicsXItem）：
    QGraphicsItem 自己也定义了 hoverEnterEvent 等虚函数，写在后面会被 MRO 挡住，
    这里的实现根本轮不到执行。
    """

    # 候选/选中框：所有图元共用一套配色，只用线型区分含义（实线=当前对象）
    SELECTION_FRAME_COLOR = QColor(0, 180, 255, 230)
    SELECTION_FRAME_WIDTH = 2

    def _init_drawing_mixin(self):
        """PySide6 要求在 super().__init__() 之后显式调用，而非定义 __init__
        （防止协作式 MRO 链在 Qt C++ 初始化前调用 Qt 方法）

        刻意不设 ItemIsSelectable：选中归 SmartEditController 管，这个标志会让 Qt
        在自己的鼠标事件里也去改选中——非 Ctrl 的按下 setSelected(true)，带 Ctrl 的
        松开把它整个翻转——两个所有者迟早对不上。

        拖动不受影响：ItemIsMovable 不看选中，图元照样会被 scene 认成 mouse
        grabber（test_selection_ownership.py 里逐个图元验过）。
        """
        self.setFlag(QGraphicsItem.GraphicsItemFlag.ItemIsMovable, True)
        self.setAcceptHoverEvents(True)
        self._hovered = False

    # ====================================================================
    # 候选/选中态 — 何时显示、显示成什么样
    # ====================================================================

    def _set_hovered(self, hovered: bool):
        """候选态只在当前工具选得中它时才算数，否则鼠标经过什么也不该发生。"""
        hovered = bool(hovered) and self._can_show_hover()
        if hovered == self._hovered:
            return
        self._hovered = hovered
        self.update()

    def _update_hover_cursor(self, event=None):
        if self._can_show_hover():
            self.setCursor(Qt.CursorShape.SizeAllCursor)
        else:
            self.unsetCursor()
        if event is not None:
            event.accept()

    def hoverEnterEvent(self, event):
        self._set_hovered(True)
        self._update_hover_cursor(event)

    def hoverMoveEvent(self, event):
        self._set_hovered(True)
        self._update_hover_cursor(event)

    def hoverLeaveEvent(self, event):
        self._set_hovered(False)
        self.unsetCursor()
        if event is not None:
            event.accept()

    def shows_selection_frame(self) -> bool:
        """这类图元要不要候选/选中框。

        自由笔画（画笔、荧光笔涂抹、涂抹马赛克）不要：它们要 Ctrl 才选得中，画面上
        往往叠着几十条，框跟着鼠标此起彼伏地闪反而碍事。这是图元自身的性质，所以
        写在这里，而不是借"当前工具选不选得中它"去间接表达——那是另一个问题。
        """
        return True

    def is_edit_target(self) -> bool:
        """四角按钮此刻是不是作用在它身上——决定框画实线还是虚线。

        问控制器，而不是看 Qt 的 isSelected()：选中只有 SmartEditController 一个
        所有者。Qt 那份状态自己在事件里也会改（QGraphicsItem::mouseReleaseEvent
        撞上 Ctrl 会把选中翻转），跟着它走就会和控制器打架。
        """
        controller = self._edit_controller()
        return controller is not None and controller.selected_item is self

    def selection_frame_style(self):
        """这一帧的框画成什么线型，None 表示不画。

        实线那一段是四角按钮此刻作用的对象，虚线那一段是鼠标再点一下就会切过去
        的对象——两种含义只用线型区分，颜色和线宽是同一套。

        悬停是记在图元上的状态，而"当前工具选不选得中它"会随工具切换而变，所以
        虚线这一路要再校验一次：否则停在图元上时换个工具，框会一直留着。
        """
        if not self.shows_selection_frame():
            return None
        if self.is_edit_target():
            return Qt.PenStyle.SolidLine
        if self._hovered and self._can_show_hover():
            return Qt.PenStyle.DashLine
        return None

    def selection_frame_pen(self) -> QPen | None:
        """候选/选中框的画笔，None 表示这一帧不画。

        画不画、画成什么样全由这里说了算，子类只管把框画在自己的形状上——线型
        跟着画笔一起给出去，就没有哪个子类会漏掉实线那一档。
        """
        style = self.selection_frame_style()
        if style is None:
            return None
        pen = QPen(self.SELECTION_FRAME_COLOR, self.SELECTION_FRAME_WIDTH, style)
        pen.setCosmetic(True)
        return pen

    # ====================================================================
    # 统一属性接口 — View 层通过这些方法修改图元，不直接操作内部属性
    # ====================================================================

    def set_stroke_width(self, width: float):
        """设置线宽/大小，子类应按自身语义重写"""
        pass  # 默认无操作

    def scale_stroke_width(self, scale: float) -> bool:
        """按比例缩放线宽/大小，返回是否处理成功"""
        return False  # 默认不处理

    def set_visual_opacity(self, opacity: float) -> bool:
        """设置视觉透明度（优先修改颜色 alpha，保持 item opacity=1.0），返回是否成功"""
        return False  # 默认不处理

    def get_stroke_width(self) -> float | None:
        """获取当前线宽/大小，返回 None 表示不适用"""
        return None

    def get_visual_opacity(self) -> float | None:
        """获取当前视觉透明度"""
        if hasattr(self, 'opacity'):
            return max(0.0, min(1.0, float(self.opacity())))
        return None

    def _edit_controller(self):
        """管着选中的那个控制器；图元还没进场景、场景还没有视图时为 None。

        通过 QGraphicsScene.views() 拿（Qt 内建方法，无循环引用问题）。
        """
        scene = self.scene()
        if scene is None:
            return None
        views = scene.views()
        if not views:
            return None
        return getattr(views[0], "smart_edit_controller", None)

    def _can_show_hover(self) -> bool:
        """当前工具选不选得中它——决定鼠标经过时给不给候选反馈（框和光标）。"""
        controller = self._edit_controller()
        if controller is None:
            return False
        try:
            return controller.can_show_hover_cursor(self)
        except Exception:
            return False

class StrokeItem(DrawingItemMixin, QGraphicsPathItem):
    """画笔/荧光笔图元"""
    
    def __init__(self, path: QPainterPath, pen: QPen, is_highlighter: bool = False):
        super().__init__(path)
        self._init_drawing_mixin()
        
        # 缓存 shape，避免移动时重复计算昂贵的 createStroke
        self._shape_cache = None
        
        self.setPen(pen)
        self.is_highlighter = is_highlighter
        
        if is_highlighter:
            # 荧光笔层级较低，但在背景之上
            self.setZValue(10)
        else:
            # 普通画笔层级较高
            self.setZValue(20)
            
    def shows_selection_frame(self) -> bool:
        """画笔和荧光笔的自由笔画不画框，理由见基类。

        以前这件事是靠"paint() 里干脆没写画框那几行"表达的——看不出是有意还是漏了。
        """
        return False

    def setPen(self, pen: QPen):
        """重写 setPen 以清除 shape 缓存"""
        super().setPen(pen)
        self._shape_cache = None

    def setPath(self, path: QPainterPath):
        """重写 setPath 以清除 shape 缓存"""
        super().setPath(path)
        self._shape_cache = None
            
    def shape(self):
        """
        重写 shape 以增加点击容错范围
        使用 QPainterPathStroker 生成比视觉路径更宽的点击区域
        """
        # 如果有缓存，直接返回
        if self._shape_cache is not None:
            return self._shape_cache
            
        path = self.path()
        if path.isEmpty():
            return path
            
        # 创建路径描边器
        stroker = QPainterPathStroker()
        # 设置宽度：当前笔触宽度 + 额外旷量(20px)
        # 这样即使是细线，也有至少 20px 的点击范围
        # 同时也方便移动，因为点击范围变大了
        stroker.setWidth(self.pen().widthF() + 20)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        
        # 生成扩大的形状路径并缓存
        self._shape_cache = stroker.createStroke(path)
        return self._shape_cache

    def paint(self, painter, option, widget=None):
        try:
            if self.is_highlighter:
                # 荧光笔使用正片叠底
                painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)
            
            # 优化渲染质量
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            super().paint(painter, option, widget)
        except KeyboardInterrupt:
            # 传递键盘中断，允许用户终止程序
            raise
        except Exception as e:
            # 绘制错误记录日志，避免崩溃
            log_warning(T("DrawingItem paint 异常: {e}", e=e), "Canvas")

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        pen = self.pen()
        pen.setWidthF(max(1.0, float(width)))
        self.setPen(pen)
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        pen = self.pen()
        pen.setWidthF(max(1.0, pen.widthF() * scale))
        self.setPen(pen)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        pen = QPen(self.pen())
        color = QColor(pen.color())
        color.setAlphaF(opacity)
        pen.setColor(color)
        self.setPen(pen)
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        width = float(self.pen().widthF())
        if getattr(self, 'is_highlighter', False):
            width = width / 3.0
        return width

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.pen().color().alphaF()

class RectItem(DrawingItemMixin, QGraphicsRectItem):
    """矩形图元"""
    CLICK_MARGIN = 4  # 点击旷量（像素/每侧）
    def __init__(self, rect: QRectF, pen: QPen, corner_radius: float = 0.0):
        # 使用 QRectF 参数初始化
        super().__init__(rect)
        self._init_drawing_mixin()
        # 设置样式和属性
        self.setPen(pen)
        self.setBrush(QBrush(Qt.BrushStyle.NoBrush))
        self.setZValue(20)
        self._shape_cache = None
        self._corner_radius = max(0.0, float(corner_radius))

    def setPen(self, pen: QPen):
        super().setPen(pen)
        self._shape_cache = None

    def setRect(self, rect: QRectF):
        super().setRect(rect)
        self._shape_cache = None

    def get_corner_radius(self) -> float:
        return getattr(self, '_corner_radius', 0.0)

    def set_corner_radius(self, radius: float):
        self._corner_radius = max(0.0, float(radius))
        self._shape_cache = None
        self.update()

    def shape(self):
        # 仅使用描边路径，避免点击到内部空白区域
        if self._shape_cache is not None:
            return self._shape_cache

        path = QPainterPath()
        r = self.get_corner_radius()
        if r > 0:
            path.addRoundedRect(self.rect(), r, r)
        else:
            path.addRect(self.rect())

        # 荧光笔矩形为实心，允许点击内部区域
        if getattr(self, "is_highlighter_rect", False):
            self._shape_cache = path
            return self._shape_cache

        stroker = QPainterPathStroker()
        stroker.setWidth(self.pen().widthF() + self.CLICK_MARGIN * 2)
        stroker.setCapStyle(Qt.PenCapStyle.SquareCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.MiterJoin)

        self._shape_cache = stroker.createStroke(path)
        return self._shape_cache

    def boundingRect(self):
        rect = super().boundingRect()
        extra = max(0.0, self.pen().widthF() / 2.0) + self.CLICK_MARGIN
        return rect.adjusted(-extra, -extra, extra, extra)

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        if getattr(self, "is_highlighter", False):
            painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Multiply)

        pen = QPen(self.pen())
        if pen.style() in (Qt.PenStyle.DashLine, Qt.PenStyle.CustomDashLine):
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(self.brush())
        r = self.get_corner_radius()
        if r > 0:
            painter.drawRoundedRect(self.rect(), r, r)
        else:
            painter.drawRect(self.rect())

        selection_pen = self.selection_frame_pen()
        if selection_pen is not None:
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(selection_pen)
            if r > 0:
                painter.drawRoundedRect(self.rect(), r, r)
            else:
                painter.drawRect(self.rect())

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        pen = self.pen()
        pen.setWidthF(max(1.0, float(width)))
        self.setPen(pen)
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        pen = self.pen()
        pen.setWidthF(max(1.0, pen.widthF() * scale))
        self.setPen(pen)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        pen = QPen(self.pen())
        color = QColor(pen.color())
        color.setAlphaF(opacity)
        pen.setColor(color)
        self.setPen(pen)
        # 荧光笔矩形同时更新 brush alpha
        if getattr(self, "is_highlighter_rect", False):
            brush = QBrush(self.brush())
            brush_color = QColor(brush.color())
            brush_color.setAlphaF(opacity)
            brush.setColor(brush_color)
            self.setBrush(brush)
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        return float(self.pen().widthF())

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.pen().color().alphaF()

class EllipseItem(DrawingItemMixin, QGraphicsEllipseItem):
    """椭圆图元"""
    CLICK_MARGIN = 4  # 点击旷量（像素/每侧）
    def __init__(self, rect: QRectF, pen: QPen):
        # 使用 QRectF 参数初始化
        super().__init__(rect)
        self._init_drawing_mixin()
        # 设置样式和属性
        self.setPen(pen)
        self.setBrush(QBrush(Qt.BrushStyle.NoBrush))
        self.setZValue(20)
        self._shape_cache = None

    def setPen(self, pen: QPen):
        super().setPen(pen)
        self._shape_cache = None

    def setRect(self, rect: QRectF):
        super().setRect(rect)
        self._shape_cache = None

    def shape(self):
        # 仅使用描边路径，避免点击到内部空白区域
        if self._shape_cache is not None:
            return self._shape_cache

        path = QPainterPath()
        path.addEllipse(self.rect())

        stroker = QPainterPathStroker()
        stroker.setWidth(self.pen().widthF() + self.CLICK_MARGIN * 2)
        stroker.setCapStyle(Qt.PenCapStyle.RoundCap)
        stroker.setJoinStyle(Qt.PenJoinStyle.RoundJoin)

        self._shape_cache = stroker.createStroke(path)
        return self._shape_cache

    def boundingRect(self):
        rect = super().boundingRect()
        extra = max(0.0, self.pen().widthF() / 2.0) + self.CLICK_MARGIN
        return rect.adjusted(-extra, -extra, extra, extra)

    def paint(self, painter, option, widget=None):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        # 绘制椭圆本体（避免默认选中矩形框）
        painter.setPen(self.pen())
        painter.setBrush(self.brush())
        painter.drawEllipse(self.rect())

        # 选中或悬停时沿椭圆画一圈候选/选中框
        selection_pen = self.selection_frame_pen()
        if selection_pen is not None:
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(selection_pen)
            painter.drawEllipse(self.rect())

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        pen = self.pen()
        pen.setWidthF(max(1.0, float(width)))
        self.setPen(pen)
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        pen = self.pen()
        pen.setWidthF(max(1.0, pen.widthF() * scale))
        self.setPen(pen)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        pen = QPen(self.pen())
        color = QColor(pen.color())
        color.setAlphaF(opacity)
        pen.setColor(color)
        self.setPen(pen)
        self.setOpacity(1.0)
        self.update()
        return True

    def get_stroke_width(self) -> float | None:
        return float(self.pen().widthF())

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.pen().color().alphaF()






class NumberItem(DrawingItemMixin, QGraphicsItem):
    """序号图元"""
    FONT_SCALE = 0.95
    MIN_FONT_SIZE = 10
    CLICK_MARGIN = 6  # 点击旷量（像素/每侧）

    # 三种样式
    STYLE_SOLID = "solid"            # 实心圆 + 实心字
    STYLE_HOLLOW_BG = "hollow_bg"    # 描边圆 + 实心字
    STYLE_HOLLOW_ALL = "hollow_all"  # 描边圆 + 描边字
    STYLE_NO_CIRCLE = "no_circle"    # 不画圈，只有实心数字
    STYLES = (STYLE_SOLID, STYLE_HOLLOW_BG, STYLE_HOLLOW_ALL, STYLE_NO_CIRCLE)
    DEFAULT_STYLE = STYLE_SOLID

    RING_WIDTH_RATIO = 0.12     # 圆环线宽占半径的比例
    GLYPH_OUTLINE_RATIO = 0.09  # 数字描边线宽占半径的比例

    def __init__(
        self,
        number: int,
        pos: QPointF,
        radius: float,
        color: QColor,
        style: str = None,
    ):
        super().__init__()
        self._init_drawing_mixin()
        self.number = number
        self.number_order = None
        self.radius = radius
        self.color = color
        self.style = self.normalize_style(style)
        self.setPos(pos)
        self.setZValue(20)
        self._hovered = False
        
    def visualRect(self):
        return QRectF(-self.radius, -self.radius, self.radius*2, self.radius*2)

    def boundingRect(self):
        margin = self.CLICK_MARGIN
        return self.visualRect().adjusted(-margin, -margin, margin, margin)

    def shape(self):
        path = QPainterPath()
        margin = self.CLICK_MARGIN
        path.addEllipse(self.visualRect().adjusted(-margin, -margin, margin, margin))
        return path

    def sceneVisualRect(self):
        return self.mapToScene(self.visualRect()).boundingRect()

    @classmethod
    def normalize_style(cls, style) -> str:
        """无法识别的样式一律回退到实心，旧数据和脏配置才不会画不出东西。"""
        return style if style in cls.STYLES else cls.DEFAULT_STYLE

    def set_style(self, style: str):
        style = self.normalize_style(style)
        if style != self.style:
            self.style = style
            self.update()

    @property
    def is_hollow(self) -> bool:
        """没有实心底色——数字得用标注色，不能再按背景亮度取黑白。"""
        return self.style != self.STYLE_SOLID

    @property
    def has_ring(self) -> bool:
        return self.style in (self.STYLE_HOLLOW_BG, self.STYLE_HOLLOW_ALL)

    def _ring_width(self) -> float:
        return max(2.0, float(self.radius) * self.RING_WIDTH_RATIO)

    def _number_font(self) -> QFont:
        font_size = max(self.MIN_FONT_SIZE, int(self.radius * self.FONT_SCALE))
        # 创建字体时不使用QFont.Weight.Bold，改用setBold避免字体变体问题
        font = QFont("Arial", font_size)
        font.setBold(True)
        return font

    def _solid_text_color(self) -> QColor:
        """实心圆上按背景亮度选黑或白文字（整数权重快速判定）。"""
        try:
            bg = self.color if self.color is not None else QColor(0, 0, 0)
            # Y = (R*3 + G*6 + B*1) / 10，比较放大后的值避免浮点
            y_scaled = int(bg.red()) * 3 + int(bg.green()) * 6 + int(bg.blue()) * 1
            return QColor(0, 0, 0) if y_scaled > 128 * 10 else QColor(255, 255, 255)
        except Exception:
            # 任何异常都回退到白色文字，保证鲁棒性
            return QColor(255, 255, 255)

    def paint(self, painter, option, widget):
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        visual_rect = self.visualRect()
        color = self.color if self.color is not None else QColor(0, 0, 0)

        self._paint_circle(painter, visual_rect, color)
        self._paint_number(painter, visual_rect, color)
        self._paint_selection_frame(painter, visual_rect)

    def _paint_circle(self, painter, visual_rect, color):
        if self.style == self.STYLE_NO_CIRCLE:
            return

        if not self.is_hollow:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(color)
            painter.drawEllipse(visual_rect)
            return

        # 描边压着路径画，向内缩半个线宽，空心圈的外径才和实心圆一致
        inset = self._ring_width() / 2.0
        pen = QPen(color, self._ring_width())
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawEllipse(visual_rect.adjusted(inset, inset, -inset, -inset))

    def _paint_number(self, painter, visual_rect, color):
        font = self._number_font()
        text = str(self.number)

        if self.style != self.STYLE_HOLLOW_ALL:
            # 实心圆用黑白对比色；空心圈背后是截图本身，黑白会很脏，
            # 所以跟着圈走同一个颜色。
            painter.setPen(color if self.is_hollow else self._solid_text_color())
            painter.setFont(font)
            painter.drawText(visual_rect, Qt.AlignmentFlag.AlignCenter, text)
            return

        # 只描边数字：drawText 画不出"空心字"，得转成字形轮廓再 stroke
        path = QPainterPath()
        path.addText(QPointF(0.0, 0.0), font, text)
        # 用路径自身的包围盒居中。QFontMetricsF.boundingRect 在字体缺失时会返回
        # 离谱的原点（实测 x=y=100000），拿它算偏移会把数字挪出画面。
        ink_rect = path.boundingRect()
        centre = visual_rect.center()
        path.translate(
            centre.x() - ink_rect.center().x(),
            centre.y() - ink_rect.center().y(),
        )

        pen = QPen(color, max(1.0, float(self.radius) * self.GLYPH_OUTLINE_RATIO))
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawPath(path)

    def _paint_selection_frame(self, painter, visual_rect):
        selection_pen = self.selection_frame_pen()
        if selection_pen is not None:
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(selection_pen)
            painter.drawRect(visual_rect)

    # -- 统一属性接口 --

    def set_stroke_width(self, width: float):
        """对序号图元，width 映射为 radius"""
        from tools.number import NumberTool
        self.prepareGeometryChange()
        self.radius = max(4.0, float(width) * NumberTool.RADIUS_SCALE)
        self.update()

    def scale_stroke_width(self, scale: float) -> bool:
        self.prepareGeometryChange()
        self.radius = max(4.0, self.radius * scale)
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
        from tools.number import NumberTool
        if NumberTool.RADIUS_SCALE <= 0:
            return float(self.radius)
        return float(self.radius / NumberTool.RADIUS_SCALE)

    def get_visual_opacity(self) -> float | None:
        direct = max(0.0, min(1.0, float(self.opacity())))
        if direct < 0.999:
            return direct
        return self.color.alphaF()
 
