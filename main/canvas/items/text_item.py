"""文字图元。

描边、阴影、背景色块，以及编辑/候选/闲置的三态交互框。
"""
from __future__ import annotations

from PySide6.QtWidgets import (
    QGraphicsTextItem,
    QStyle, QStyleOptionGraphicsItem,
)
from PySide6.QtGui import QPen, QPainter, QPainterPath, QColor, QFont
from PySide6.QtCore import Qt, QRectF, QPointF
from core import log_debug, safe_event
from core.logger import T

from .drawing_items import DrawingItemMixin


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
