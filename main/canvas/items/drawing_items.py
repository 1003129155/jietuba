"""
矢量绘图图元
定义画笔、形状、荧光笔等基于 QGraphicsItem 的图元
"""
from __future__ import annotations

import math
from PySide6.QtWidgets import (
    QGraphicsPathItem, QGraphicsRectItem, QGraphicsEllipseItem, QGraphicsItem, QGraphicsTextItem,
    QStyle, QStyleOptionGraphicsItem,
)
from PySide6.QtGui import QPen, QPainter, QPainterPath, QColor, QFont, QPainterPathStroker, QBrush
from PySide6.QtCore import Qt, QRectF, QPointF
from core import log_debug, log_warning, safe_event
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


class TextItem(DrawingItemMixin, QGraphicsTextItem):
    """文字图元 - 增强版"""
    # 文字与交互框之间的内边距（document margin）
    TEXT_PADDING = 3
    MIN_POINT_SIZE = 6.0
    MAX_POINT_SIZE = 400.0
    CLICK_MARGIN = 2  # 点击/悬停旷量（像素/每侧），命中区比交互矩形略宽

    # 手柄 id：避开矩形(0-7)、圆角(10-13)、序号(200-202)
    HANDLE_ROTATE = 210
    HANDLE_DELETE = 211
    HANDLE_SCALE = 212
    SCALE_HANDLE_SIZE = 10
    NORMAL_ANNOTATION_Z_VALUE = 20
    ANNOTATION_Z_VALUE = 30
    BACKGROUND_RADIUS = 6.0

    # 边框、命中区、四角按钮至少按这个宽度摆。空文字的文档区域只有 6px 左右，
    # 而左上旋转、右上删除两个按钮各 14px（LayerEditor.FUNCTIONAL_HANDLE_SIZE），
    # 按角点摆就会叠在一起，点下去谁响应都说不准。30px 让两者之间还剩 16px 空隙。
    # 放宽只加在右边：左边始终离文字起点一段固定距离（FRAME_SIDE_GAP），跟着宽度
    # 变的话，刚建出来的框会跳一下。
    MIN_INTERACTION_WIDTH = 30.0
    # 框离文字左右各让开这么多。文档边距只有 3px，框还要再往里让 1px、线宽 2px，
    # 不留空当的话，框就和闪烁的光标粘成一条：空文字框上光标贴着左边，打字时光标
    # 又贴着右边，两条线分不开。上下不用让——那两条边离光标本来就远。
    FRAME_SIDE_GAP = 4.0
    # 框往里让 1px，cosmetic 画笔的线宽才不会画到包围盒外面去（拖动会留残影）。
    FRAME_INSET = 1.0

    # 描边粗细只有四档，而且按字号的比例算，不是固定像素：同样 3px，在 12 号字上
    # 是一圈粗框，到 72 号字上细得几乎看不见。按比例算，拖右下角手柄把字放大时
    # 描边跟着等比变粗，同一档在任何字号下都是同一种观感。
    #
    # 档位按约 1.7 倍递增而不是等差：粗细的观感差异是对数的，等差档位在粗端分不
    # 出来。最粗一档在 16 号字上字眼仍然是通的，再粗字就糊成一团。
    OUTLINE_WIDTH_LEVELS = (0.04, 0.07, 0.12, 0.2)
    DEFAULT_OUTLINE_WIDTH = 0.07
    DEFAULT_OUTLINE_COLOR = "#FFFFFF"
    # 阴影朝右下偏移的距离，同样按字号比例算，理由同上
    SHADOW_OFFSET_RATIO = 0.08
    DEFAULT_SHADOW_COLOR = "#66000000"  # #AARRGGBB：黑色，40% 不透明

    def __init__(
        self,
        text: str,
        pos: QPointF,
        font: QFont,
        color: QColor,
        always_on_top: bool = True,
        provisional: bool = False,
    ):
        super().__init__(text)
        self._init_drawing_mixin()
        # 尺寸始终跟随内容：不设换行宽度，短内容才不会撑出多余的背景色。
        self.setTextWidth(-1)
        self.setPos(pos)
        self.setFont(font)
        self.setDefaultTextColor(color)
        # 允许点击编辑
        self.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
        self.setZValue(
            self.ANNOTATION_Z_VALUE
            if always_on_top
            else self.NORMAL_ANNOTATION_Z_VALUE
        )
        
        # 增大 document margin，使虚线边框与文字之间有足够间距
        # 默认只有 4px，太小导致鼠标难以区分文字区域和边框区域
        self.document().setDocumentMargin(self.TEXT_PADDING)

        # 描边、阴影默认关闭；颜色是打开时的初始值（阴影的 alpha 即不透明度）
        self.has_outline = False
        self.outline_color = QColor(self.DEFAULT_OUTLINE_COLOR)
        self.outline_width = self.DEFAULT_OUTLINE_WIDTH
        self.has_shadow = False
        self.shadow_color = QColor(self.DEFAULT_SHADOW_COLOR)

        self.has_background = False # 默认关闭背景
        self.background_color = QColor(255, 255, 255, 255) # 白色全不透明

        # 临时文字：创建者没有推 AddItemCommand，把"是否真的创建"推迟到第一次
        # 失焦时按内容决定（见 focusOutEvent）。只有 TextTool 这样做，所以由它
        # 在创建时显式声明；其余路径（钉图克隆、测试）和其它图元一样创建即入栈，
        # 默认值对它们天然正确，不需要各自记得补一个标记。
        self._provisional = provisional
        # 每次进入编辑前的内容快照（见 focusInEvent）。清空后失焦要撤销，
        # 撤销栈上的 RemoveItemCommand 只会把图元加回场景，不知道它清空前
        # 写的是什么字——真正的文本得从这份快照里找回来。
        self._text_before_edit = text

    # ------------------------------------------------------------------
    # 字号缩放（右下角手柄驱动）
    # ------------------------------------------------------------------

    def font_point_size(self) -> float:
        """当前字号；点阵字体回退到用像素高度近似。"""
        size = self.font().pointSizeF()
        if size <= 0:
            size = float(self.font().pixelSize())
        return max(float(size), self.MIN_POINT_SIZE)

    def set_font_point_size(self, point_size: float):
        """按字号重新排版；描边粗细和阴影距离按字号比例算，跟着一起变。"""
        clamped = max(
            self.MIN_POINT_SIZE,
            min(self.MAX_POINT_SIZE, float(point_size)),
        )
        font = QFont(self.font())
        font.setPointSizeF(clamped)
        self.setFont(font)

    def get_edit_handles(self):
        """左上旋转、右上删除、右下缩放；左下角不放功能。

        锚点逐点 mapToScene 映射 local 包围盒的角，而不是取 sceneBoundingRect()
        的角：后者是轴对齐外包围盒，旋转之后它的角会甩到文字外面去（实测 45°
        偏 35px，137° 偏 239px），手柄既画错位置也点不到。

        按 interaction_rect() 摆而不是内容矩形：空文字只有 6px 宽，两个 14px 的
        按钮会叠在一起。
        """
        from canvas.handle_editor import EditHandle, HandleType, LayerEditor

        local = self.interaction_rect()
        return [
            EditHandle(
                self.HANDLE_ROTATE,
                HandleType.ROTATE,
                QPointF(self.mapToScene(local.topLeft())),
                Qt.CursorShape.SizeAllCursor,
                LayerEditor.FUNCTIONAL_HANDLE_SIZE,
            ),
            EditHandle(
                self.HANDLE_DELETE,
                HandleType.ITEM_DELETE,
                QPointF(self.mapToScene(local.topRight())),
                Qt.CursorShape.PointingHandCursor,
                LayerEditor.FUNCTIONAL_HANDLE_SIZE,
                2,
            ),
            EditHandle(
                self.HANDLE_SCALE,
                HandleType.TEXT_SCALE,
                QPointF(self.mapToScene(local.bottomRight())),
                Qt.CursorShape.SizeFDiagCursor,
                self.SCALE_HANDLE_SIZE,
                8,
            ),
        ]

    # ------------------------------------------------------------------
    # 描边与阴影
    # ------------------------------------------------------------------

    @classmethod
    def normalize_outline_width(cls, width) -> float:
        """把任意来源的描边粗细吸附到最近的档位（与 MosaicTool.clamp_block_size 同一个套路）。

        档位就是这个量的合法取值域：设置、面板、撤销记录、钉图克隆都经由这里，
        面板高亮的档和实际画出来的粗细才不会分家。正中间的平局取更粗的一档。
        """
        try:
            value = float(width)
        except (TypeError, ValueError):
            return cls.DEFAULT_OUTLINE_WIDTH
        return min(cls.OUTLINE_WIDTH_LEVELS, key=lambda level: (abs(level - value), -level))

    def outline_extent(self) -> float:
        """描边伸出字形之外的距离；没开描边为 0。"""
        return self.outline_width * self.font_point_size() if self.has_outline else 0.0

    def shadow_distance(self) -> float:
        """阴影往右、往下各挪多远；没开阴影为 0。"""
        return self.SHADOW_OFFSET_RATIO * self.font_point_size() if self.has_shadow else 0.0

    def outline_state(self) -> tuple:
        """(enabled, color, width)，与 set_outline 的参数一一对应，钉图克隆和面板回填原样取用。"""
        return (self.has_outline, QColor(self.outline_color), self.outline_width)

    def shadow_state(self) -> tuple:
        """(enabled, color)，与 set_shadow 的参数一一对应。"""
        return (self.has_shadow, QColor(self.shadow_color))

    def set_outline(self, enabled: bool, color: QColor = None, width: float = None):
        """开关描边。color / width 不传就沿用当前值；width 是档位（字号的比例）。"""
        self.prepareGeometryChange()
        self.has_outline = bool(enabled)
        if color is not None:
            self.outline_color = QColor(color)
        if width is not None:
            self.outline_width = self.normalize_outline_width(width)
        self.update()

    def set_shadow(self, enabled: bool, color: QColor = None):
        """开关阴影。color 不传就沿用当前值，它的 alpha 就是阴影的不透明度。"""
        self.prepareGeometryChange()
        self.has_shadow = bool(enabled)
        if color is not None:
            self.shadow_color = QColor(color)
        self.update()

    def content_rect(self) -> QRectF:
        """文字真正画到的地方：文档区域再往外放出描边和阴影占的地方。

        描边、阴影都画在字形外面，粗档的描边远比 3px 的文档边距宽。包围盒不包住
        它们，重绘区就漏掉这一圈（拖动留残影、导出被裁掉），背景色块和四角手柄
        也会压进描边里。

        背景色块按这个矩形画，而不是 boundingRect()：后者为了摆得下四角按钮有最小
        宽度，窄字的背景跟着变宽就成了画面上看得见的差别，导出的图也跟着变。
        """
        rect = super().boundingRect()
        outline = self.outline_extent()
        far_side = outline + self.shadow_distance()
        return rect.adjusted(-outline, -outline, far_side, far_side)

    def interaction_rect(self) -> QRectF:
        """交互用的矩形：内容矩形左右各让开 FRAME_SIDE_GAP，再至少放宽到 MIN_INTERACTION_WIDTH。

        边框、命中区、四角按钮都按它算；字画在哪里、背景画多大都不受它影响。
        """
        gap = self.FRAME_SIDE_GAP
        rect = QRectF(self.content_rect()).adjusted(-gap, 0, gap, 0)
        if rect.width() < self.MIN_INTERACTION_WIDTH:
            rect.setWidth(self.MIN_INTERACTION_WIDTH)
        return rect

    def hit_rect(self) -> QRectF:
        """命中/包围用的矩形：交互矩形再往外扩一圈点击旷量。

        文字是简单矩形几何，放大参数即可扩容差，不需要像箭头那样描边——
        旷量只用来扩点击/悬停判定，边框、四角按钮仍然按 interaction_rect()
        摆，不跟着放大。
        """
        margin = self.CLICK_MARGIN
        return self.interaction_rect().adjusted(-margin, -margin, margin, margin)

    def boundingRect(self) -> QRectF:
        """包围盒按命中矩形算：必须完整覆盖 shape()，否则命中区会漏出包围盒外。"""
        return self.hit_rect()

    def shape(self) -> QPainterPath:
        """命中区跟着命中矩形走。

        QGraphicsTextItem.shape() 取的是它自己缓存的文档矩形，不会回头调用这里
        重写的 boundingRect()。不重写它，空文字框上放宽出来的那块就点不中——
        框看得见却点不着，比不放宽更糟。
        """
        path = QPainterPath()
        path.addRect(self.hit_rect())
        return path

    def contains(self, point: QPointF) -> bool:
        """命中判定同样按命中矩形来。

        QGraphicsTextItem.contains() 也是绕开 shape() 直接量它缓存的文档矩形的，
        场景的点击命中、view 里的"点在不在这段文字上"都走它，漏掉就等于没放宽。
        """
        return self.hit_rect().contains(point)

    def _glyph_path(self) -> QPainterPath:
        """文档当前排版出来的字形轮廓，和 super().paint() 画出来的字逐像素重合。

        向排版引擎要每个字形的实际位置，而不是自己用 QPainterPath.addText 重排一遍：
        多行、中英混排时的字体回退、粘贴进来的混合字号都由排版引擎决定，自己重排
        就得另外猜一份和它对齐的边距。历史上那版描边就是卡在这里，最后留下了一个
        空循环。
        """
        path = QPainterPath()
        # 非零环绕：相邻字形的轮廓叠在一起时，奇偶填充会把重叠处挖成空洞
        path.setFillRule(Qt.FillRule.WindingFill)
        block = self.document().begin()
        while block.isValid():
            layout = block.layout()
            origin = layout.position()
            # 范围要显式传：PySide6 6.11 里不带参数的 glyphRuns() 返回空列表
            for run in layout.glyphRuns(0, block.length()):
                raw_font = run.rawFont()
                for index, position in zip(run.glyphIndexes(), run.positions()):
                    path.addPath(raw_font.pathForGlyph(index).translated(origin + position))
                self._add_decoration_lines(path, run, origin)
            block = block.next()
        return path

    @staticmethod
    def _add_decoration_lines(path: QPainterPath, run, origin: QPointF):
        """下划线、删除线不在字形轮廓里，按字形串上的标记补成矩形。

        不补的话，开了描边的下划线是光秃秃的一条，阴影里也没有它。位置照 Qt
        自己画这几种线的取法：下划线在基线下 underlinePosition，删除线、上划线
        分别在基线上 ascent 的 1/3 和整个 ascent。
        """
        positions = run.positions()
        if not positions:
            return
        raw_font = run.rawFont()
        baseline = origin.y() + positions[0].y()
        thickness = raw_font.lineThickness()
        span = run.boundingRect().translated(origin)
        for enabled, offset in (
            (run.underline(), raw_font.underlinePosition()),
            (run.strikeOut(), -raw_font.ascent() / 3),
            (run.overline(), -raw_font.ascent()),
        ):
            if enabled:
                top = baseline + offset - thickness / 2
                path.addRect(QRectF(span.left(), top, span.width(), thickness))

    def _outline_pen(self) -> QPen:
        # 路径描边骑在字形边缘上，里面那一半会被随后画的字盖住，所以笔宽取两倍外扩
        pen = QPen(self.outline_color, 2 * self.outline_extent())
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        return pen

    def _paint_shadow(self, painter, glyphs: QPainterPath, outline_pen):
        """阴影是"描完边之后整块字"的影子，整体往右下挪一段画在最底下。

        开了描边时，这块字由字形本身和字形外那一圈描边拼成。两部分直接各画一次
        会在字形边缘内侧重叠，半透明的阴影在那里叠成两层，出现一道深色细线；所以
        画描边那部分之前，先把字形从裁剪区里挖掉。

        不用 QPainterPath.united() 先把两部分合成一块：一行字实测要 17~50ms，
        拖手柄缩放时每一帧都得重算。也不靠非零环绕把两条路径拼成一次填充：CFF
        字体（如 Noto Sans SC）的轮廓走向和 TrueType 相反，环绕数会互相抵消，
        字形里面被挖出空洞。
        """
        distance = self.shadow_distance()
        painter.save()
        painter.translate(distance, distance)
        painter.fillPath(glyphs, self.shadow_color)
        if outline_pen is not None:
            # 默认的奇偶填充下，包围盒矩形叠上字形 = 矩形减去字形
            outside_glyphs = QPainterPath()
            outside_glyphs.addRect(self.boundingRect())
            outside_glyphs.addPath(glyphs)
            painter.setClipPath(outside_glyphs, Qt.ClipOperation.IntersectClip)
            shadow_pen = QPen(outline_pen)
            shadow_pen.setColor(self.shadow_color)
            painter.strokePath(glyphs, shadow_pen)
        painter.restore()

    def paint(self, painter, option, widget):
        """由下往上：背景 → 阴影 → 描边 → 文字本身（含光标、选区）→ 交互框。"""
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing)
        
        # 1. 绘制背景（如果在底层）
        if self.has_background:
            painter.save()
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(self.background_color)
            background_rect = self.content_rect()
            radius = min(
                self.BACKGROUND_RADIUS,
                max(0.0, background_rect.width() / 2.0),
                max(0.0, background_rect.height() / 2.0),
            )
            painter.drawRoundedRect(background_rect, radius, radius)
            painter.restore()

        if self.has_outline or self.has_shadow:
            glyphs = self._glyph_path()
            outline_pen = self._outline_pen() if self.has_outline else None
            if self.has_shadow:
                self._paint_shadow(painter, glyphs, outline_pen)
            if outline_pen is not None:
                painter.strokePath(glyphs, outline_pen)

        super().paint(painter, self._text_paint_option(option), widget)
        self._paint_interaction_frame(painter)

    # ------------------------------------------------------------------
    # 三态交互框
    # ------------------------------------------------------------------

    def is_editing(self) -> bool:
        """光标是否落在这段文字里（编辑态）。

        只看 textInteractionFlags 不够：文字新建出来就带着可编辑标志，钉图克隆、
        测试里造出来的文字从没获得过焦点，只凭标志会被当成"正在编辑"，平白画出
        一圈实线框。所以还要它确实是焦点图元；窗口失活时 hasFocus() 会变 False，
        这时看 scene 记的焦点图元。
        """
        if not (
            self.textInteractionFlags() & Qt.TextInteractionFlag.TextEditorInteraction
        ):
            return False
        scene = self.scene()
        return self.hasFocus() or (scene is not None and scene.focusItem() is self)

    def _text_paint_option(self, option):
        """摘掉选中/焦点状态位，再交给 Qt 画字。

        QGraphicsTextItem 自带的高亮是"选中就画一圈虚线"，分不出"正在编辑的这一段"
        和"点一下就能切过去的那一段"，还会和下面自己画的框叠成两圈。三态框统一由
        _paint_interaction_frame 负责。
        """
        if option is None:
            return option
        cleaned = QStyleOptionGraphicsItem(option)
        cleaned.state &= ~(
            QStyle.StateFlag.State_Selected | QStyle.StateFlag.State_HasFocus
        )
        return cleaned

    def is_edit_target(self) -> bool:
        """文字比别的图元多一个编辑态：光标落在这一段里，它就是当前对象。

        编辑态要单独算，光问控制器不够：TextTool 新建的那一段只 setFocus()，不走
        select_item()（见 tools/text.py），控制器那边此刻还是空的。
        """
        return super().is_edit_target() or self.is_editing()

    def _paint_interaction_frame(self, painter):
        """三态框画在交互矩形上；画不画、画成什么线型归 DrawingItemMixin。"""
        pen = self.selection_frame_pen()
        if pen is None:
            return
        inset = self.FRAME_INSET
        painter.save()
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.setPen(pen)
        painter.drawRect(self.interaction_rect().adjusted(inset, inset, -inset, -inset))
        painter.restore()

    def set_background(self, enabled: bool, color: QColor = None, opacity: int = None):
        self.has_background = enabled
        if color:
            self.background_color = QColor(color)
        if opacity is not None:
            self.background_color.setAlpha(int(max(0, min(255, opacity))))
        self.update()
        
    @safe_event
    def focusInEvent(self, event):
        """进入编辑前记一份内容快照。

        清空后失焦要撤销时，撤销栈上的命令得知道"清空前这里写的是什么字"才能
        真正找回来，而不只是把图元加回场景、留一个空壳——这份快照就是那个字的
        唯一来源，必须在还没被删之前存下来。
        """
        self._text_before_edit = self.toPlainText()
        super().focusInEvent(event)

    @safe_event
    def focusOutEvent(self, event):
        """失去焦点时的收尾：内容是否为空，决定这次退出编辑要不要在撤销栈上留痕。

        - 临时文字（刚创建、还没入栈）：空着失焦等于什么都没发生过，直接移出
          场景；有内容失焦才是它真正被创建出来的时刻，补推 AddItemCommand。
        - 已入栈的标注被编辑清空：这次清空本身是一次真实的删除，走
          ClearTextItemCommand 连清空前的文本一起记下，Ctrl+Z 才能找回内容，
          不能直接 removeItem 绕开撤销系统。
        """
        super().focusOutEvent(event)
        # 移除选中状态
        cursor = self.textCursor()
        cursor.clearSelection()
        self.setTextCursor(cursor)

        scene = self.scene()
        undo_stack = getattr(scene, "undo_stack", None)

        if not self.toPlainText().strip():
            if scene is None:
                return
            if self._provisional or undo_stack is None:
                scene.removeItem(self)
                log_debug(T("内容为空，自动删除"), "TextItem")
            else:
                from canvas.undo import ClearTextItemCommand
                undo_stack.push(ClearTextItemCommand(scene, self, self._text_before_edit))
                log_debug(T("内容被清空，推入可撤销的删除"), "TextItem")
            return

        # 否则取消编辑模式（可选）
        self.setTextInteractionFlags(Qt.TextInteractionFlag.NoTextInteraction)

        if self._provisional:
            self._provisional = False
            if undo_stack is not None:
                from canvas.undo import AddItemCommand
                undo_stack.push(AddItemCommand(scene, self))
            
    @safe_event
    def mouseDoubleClickEvent(self, event):
        """双击进入编辑模式"""
        if self.textInteractionFlags() == Qt.TextInteractionFlag.NoTextInteraction:
            self.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
            self.setFocus()
        super().mouseDoubleClickEvent(event)

    def _is_on_text_edge(self, local_pos: QPointF) -> bool:
        """
        判断局部坐标是否在边框边缘（内边距及描边/阴影占的那一圈）
        在边缘 → True（应显示拖拽光标）
        在文字内容区域 → False（应显示文字编辑光标）
        """
        if not self.boundingRect().contains(local_pos):
            return False
        margin = self.document().documentMargin()
        inner = super().boundingRect().adjusted(margin, margin, -margin, -margin)
        if inner.width() <= 0 or inner.height() <= 0:
            return True
        return not inner.contains(local_pos)

    def _apply_edit_cursor(self, local_pos: QPointF):
        """编辑态的光标：边缘那一圈是拖拽，文字区域是输入。"""
        if self._is_on_text_edge(local_pos):
            self.setCursor(Qt.CursorShape.SizeAllCursor)
        else:
            self.setCursor(Qt.CursorShape.IBeamCursor)

    def hoverEnterEvent(self, event):
        """编辑中的光标自己分（工字/拖拽），其余交给通用的候选态处理。"""
        if self.is_editing():
            self._apply_edit_cursor(event.pos())
            event.accept()
            return
        super().hoverEnterEvent(event)

    def hoverMoveEvent(self, event):
        """同上：编辑中按位置区分工字和拖拽光标，不编辑就是普通候选。"""
        if self.is_editing():
            self._apply_edit_cursor(event.pos())
            event.accept()
            return
        super().hoverMoveEvent(event)

    # -- 统一属性接口 --

    def scale_stroke_width(self, scale: float) -> bool:
        """对文字图元，缩放字号"""
        font = self.font()
        point_size = font.pointSizeF()
        if point_size <= 0:
            point_size = float(font.pointSize() or 12)
        new_size = max(6.0, point_size * scale)
        font.setPointSizeF(new_size)
        self.setFont(font)
        self.update()
        return True

    def set_visual_opacity(self, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))
        self.setOpacity(opacity)
        self.update()
        return True

    def get_visual_opacity(self) -> float | None:
        return max(0.0, min(1.0, float(self.opacity())))


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
 
