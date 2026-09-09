"""
马赛克工具设置面板

左侧：框选/画笔模式切换 + 笔刷大小；右侧：粒度滑动条 + 马赛克种类（马赛克/模糊）。
马赛克没有"颜色"这个概念（涂抹的是背景像素），所以不带颜色和透明度控件。
"""
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QPushButton, QComboBox, QFrame, QButtonGroup,
    QStyle, QStyleOptionComboBox, QStylePainter,
)
from PySide6.QtCore import Qt, Signal, QSize, QPointF
from PySide6.QtGui import QColor, QIcon, QImage, QPainter, QPixmap, QPolygonF
from core.resource_manager import ResourceManager
from core.i18n import make_tr
from tools.mosaic import MosaicTool
from .base_settings_panel import (
    StepperWidget, build_settings_panel_stylesheet, paint_rounded_panel, PANEL_SCALE,
)
from core import safe_event

# 与其它工具设置面板共用同一翻译上下文
_tr = make_tr("ArrowSettingsPanel")


class CenteredComboBox(QComboBox):
    """闭合态文本居中的下拉框。

    QSS 的 text-align 只支持 QPushButton / QProgressBar，QComboBox 的
    当前项文本由原生样式左对齐绘制；这里重写 paintEvent，框架交给样式画，
    文本自己按 SC_ComboBoxEditField 区域居中绘制。
    """

    def paintEvent(self, event):
        option = QStyleOptionComboBox()
        self.initStyleOption(option)
        option.currentText = ""  # 空文本交给样式画，避免与居中文本重叠
        painter = QStylePainter(self)
        painter.drawComplexControl(QStyle.ComplexControl.CC_ComboBox, option)
        # 下拉箭头宽度为 0，整个内容矩形都可用，直接按它居中
        painter.setPen(option.palette.text().color())
        painter.drawText(option.rect, Qt.AlignmentFlag.AlignCenter, self.currentText())


SWATCH_DARK = QColor("#7a7a7a")
SWATCH_LIGHT = QColor("#e8e8e8")

# 图标里用几格来表达这一档，与 MosaicTool.BLOCK_SIZE_LEVELS 一一对应。
#
# 不能直接按"该档 px ÷ 最粗档 px"去缩：24px 的图标那样算下来，最粗一档只剩
# 1 格，糊成一片纯色——四个图标里最该说明问题的那个反而什么都没说，而且纯色
# 上马赛克和模糊长得一模一样。所以图标是相对刻度：从 8 格递减到 2 格，四档
# 各不相同，最粗那档仍看得出是"块"。
SWATCH_GRIDS = (8, 5, 3, 2)

# 每一档 tooltip 说的是"糊到什么程度"，与 MosaicTool.BLOCK_SIZE_LEVELS 一一对应。
#
# 不报 px 数字：用户挑档位靠的是观感，"8"或"16"对他没有意义，而且这四个数是
# 实现细节（改档位值不该让提示跟着变得莫名其妙）。措辞用"强度"而不是"粒度"，
# 因为马赛克和模糊共用这一组档位，说法得对两种形态都成立。
BLOCK_SIZE_TIPS = (
    "Strength: Subtle",
    "Strength: Medium",
    "Strength: Strong",
    "Strength: Maximum",
)


def _swatch_source(side: int) -> QImage:
    """给档位图标当"内容"的那张小图。

    必须有一条明确的斜边：格子化会把它啃成阶梯，模糊会把它化开，两种形态才
    各自看得出来。用棋盘格不行——糊完只剩一片灰，四个档看着一个样。
    """
    image = QImage(side, side, QImage.Format.Format_ARGB32_Premultiplied)
    image.fill(SWATCH_LIGHT)
    painter = QPainter(image)
    try:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(SWATCH_DARK)
        painter.drawPolygon(QPolygonF([
            QPointF(0.0, float(side)),
            QPointF(float(side), 0.0),
            QPointF(float(side), float(side)),
        ]))
    finally:
        painter.end()
    return image


def render_block_size_swatch(block_size: int, side: int, smooth: bool, ratio: float = 1.0) -> QPixmap:
    """把"这一档画出来是什么样"直接画成一小片实际效果。

    走的是和 MosaicItem 完全相同的一套：按粒度把图缩小，再按块放大回去，
    马赛克与模糊的唯一区别就是放大时开不开平滑插值。所以图标不会和实际画出来
    的东西各说各话——包括切到模糊时，四个图标跟着一起变糊。

    每一档缩到几格见 SWATCH_GRIDS，四个图标并排就是一条从细到粗的梯子。

    ratio 传屏幕的 devicePixelRatio：按物理像素出图再标注回逻辑尺寸，高分屏上
    才不会被系统放大糊掉——马赛克那一档的方块边缘本来就该是硬的。格数不随
    ratio 变，所以梯子在任何缩放下都是同一条。
    """
    pixels = max(1, round(side * max(1.0, float(ratio))))
    source = _swatch_source(pixels)
    grid = SWATCH_GRIDS[MosaicTool.BLOCK_SIZE_LEVELS.index(MosaicTool.clamp_block_size(block_size))]
    reduced = source.scaled(
        grid, grid,
        Qt.AspectRatioMode.IgnoreAspectRatio,
        Qt.TransformationMode.SmoothTransformation,
    )
    mode = (Qt.TransformationMode.SmoothTransformation if smooth
            else Qt.TransformationMode.FastTransformation)
    pixmap = QPixmap.fromImage(
        reduced.scaled(pixels, pixels, Qt.AspectRatioMode.IgnoreAspectRatio, mode)
    )
    pixmap.setDevicePixelRatio(max(1.0, float(ratio)))
    return pixmap


def _cached_icon(svg_name):
    """获取缓存的 QIcon"""
    return ResourceManager.get_icon(ResourceManager.get_icon_path(svg_name))


class MosaicSettingsPanel(QWidget):
    """马赛克工具二级菜单"""

    # 模式与种类的取值以工具为准，面板不另立一套字面量——两边一旦分家，
    # 存进设置的字符串和工具认得的字符串就会对不上。
    MODE_FREEHAND_VALUE = MosaicTool.MODE_FREEHAND
    MODE_RECT_VALUE = MosaicTool.MODE_RECT
    STYLE_PIXELATE_VALUE = MosaicTool.STYLE_PIXELATE
    STYLE_BLUR_VALUE = MosaicTool.STYLE_BLUR

    draw_mode_changed = Signal(str)   # freehand / rect
    style_changed = Signal(str)       # pixelate / blur
    size_changed = Signal(int)
    block_size_changed = Signal(int)  # 马赛克/模糊粒度

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)

        self.current_draw_mode = self.MODE_FREEHAND_VALUE
        self.current_style = self.STYLE_PIXELATE_VALUE
        self.current_size = 30
        self.current_block_size = MosaicTool.DEFAULT_BLOCK_SIZE

        self._init_ui()
        self._connect_signals()

    @safe_event
    def paintEvent(self, event):
        paint_rounded_panel(self)

    def _init_ui(self):
        from tools.base import Tool

        self.setStyleSheet(build_settings_panel_stylesheet(
            combo_enabled=True,
            combo_padding="1px",
            combo_min_width=72,
            combo_max_width=72,
            combo_padding_compact=True,
        ))

        layout = QHBoxLayout(self)
        layout.setContentsMargins(round(10 * PANEL_SCALE), round(8 * PANEL_SCALE),
                                  round(10 * PANEL_SCALE), round(8 * PANEL_SCALE))
        layout.setSpacing(round(10 * PANEL_SCALE))

        # === 左侧：框选/画笔模式切换 ===
        self.mode_widget = QWidget()
        mode_layout = QHBoxLayout(self.mode_widget)
        mode_layout.setContentsMargins(0, 0, 0, 0)
        mode_layout.setSpacing(2)

        _btn_sz = round(30 * PANEL_SCALE)
        _icon_sz = round(24 * PANEL_SCALE)

        self.freehand_btn = QPushButton()
        self.freehand_btn.setCheckable(True)
        self.freehand_btn.setFixedSize(_btn_sz, _btn_sz)
        self.freehand_btn.setToolTip(_tr("Freehand Mosaic"))
        self.freehand_btn.setIcon(_cached_icon("画笔.svg"))
        self.freehand_btn.setIconSize(QSize(_icon_sz, _icon_sz))
        self.freehand_btn.setStyleSheet("QPushButton { padding: 0px; }")

        self.rect_btn = QPushButton()
        self.rect_btn.setCheckable(True)
        self.rect_btn.setFixedSize(_btn_sz, _btn_sz)
        self.rect_btn.setToolTip(_tr("Rect Mosaic"))
        self.rect_btn.setIcon(_cached_icon("方框.svg"))
        self.rect_btn.setIconSize(QSize(_icon_sz, _icon_sz))
        self.rect_btn.setStyleSheet("QPushButton { padding: 0px; }")

        self.mode_group = QButtonGroup(self)
        self.mode_group.setExclusive(True)
        self.mode_group.addButton(self.freehand_btn)
        self.mode_group.addButton(self.rect_btn)

        mode_layout.addWidget(self.freehand_btn)
        mode_layout.addWidget(self.rect_btn)
        layout.addWidget(self.mode_widget)

        # === 笔刷大小 ===
        self.size_spin = StepperWidget(self.current_size, Tool.MIN_WIDTH, Tool.MAX_WIDTH)
        self.size_spin.setFixedWidth(round(60 * PANEL_SCALE))
        self.size_spin.setToolTip(_tr("Brush Size"))
        layout.addWidget(self.size_spin)

        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        layout.addWidget(line1)

        layout.addStretch()

        # === 粒度：四个档位 ===
        #
        # 粒度是凭观感定的，不需要无级变速；倍增的四档就覆盖了从"还看得出轮廓"
        # 到"糊成一块"的全程（档位本身定义在 MosaicTool 上）。
        #
        # 做成离散档位还顺手解决了一个真问题：每换一次粒度，下游都要按新粒度把
        # 整张背景重新收缩一遍（4K 实测 28ms/档），并给撤销栈压一条各自持有一份
        # 缩小图的命令。滑块拖一次会连发三十来次——画面卡住、撤销要按三十多下才
        # 回得去、这些命令攥着的小图加起来 39MB，比背景原图还大。以前靠关掉
        # slider 的 tracking 来兜这件事，现在"一次点击 = 一次改动"是结构本身
        # 保证的，不再依赖那个容易被下一个人改掉的开关。
        self.block_size_group = QButtonGroup(self)
        self.block_size_group.setExclusive(True)
        self.block_size_buttons = {}

        block_size_widget = QWidget()
        block_size_layout = QHBoxLayout(block_size_widget)
        block_size_layout.setContentsMargins(0, 0, 0, 0)
        block_size_layout.setSpacing(2)

        # 面板通用的选中态是浅灰底（#e0e0e0），和悬停的 #f0f0f0 只差一点点；
        # 这四个按钮又被图样铺满，底色基本露不出来。所以选中态改用一圈主题色
        # 描边——否则"现在是哪一档"要凑近了才看得出来。
        level_button_qss = (
            "QPushButton { padding: 0px; border: 1px solid #ddd; }"
            "QPushButton:hover { border-color: #bbb; }"
            "QPushButton:checked { border: 2px solid #0078d7; }"
        )
        self._swatch_side = _icon_sz
        for index, level in enumerate(MosaicTool.BLOCK_SIZE_LEVELS):
            button = QPushButton()
            button.setCheckable(True)
            button.setFixedSize(_btn_sz, _btn_sz)
            button.setIconSize(QSize(_icon_sz, _icon_sz))
            button.setToolTip(_tr(BLOCK_SIZE_TIPS[index]))
            button.setStyleSheet(level_button_qss)
            self.block_size_group.addButton(button)
            self.block_size_buttons[level] = button
            block_size_layout.addWidget(button)

        layout.addWidget(block_size_widget)

        # === 右侧：马赛克种类 ===
        self.style_combo = CenteredComboBox()
        self.style_combo.addItem(_tr("Mosaic"), self.STYLE_PIXELATE_VALUE)
        self.style_combo.addItem(_tr("Blur"), self.STYLE_BLUR_VALUE)
        self.style_combo.setToolTip(_tr("Mosaic Style"))
        self.style_combo.setMaxVisibleItems(10)
        layout.addWidget(self.style_combo)

        self.set_draw_mode(self.current_draw_mode)
        self.set_style(self.current_style)
        self.set_block_size(self.current_block_size)

    def _connect_signals(self):
        self.mode_group.buttonClicked.connect(self._on_mode_clicked)
        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.style_combo.currentIndexChanged.connect(self._on_style_changed)
        self.block_size_group.buttonClicked.connect(self._on_block_size_clicked)

    def _on_mode_clicked(self):
        mode = self.MODE_RECT_VALUE if self.rect_btn.isChecked() else self.MODE_FREEHAND_VALUE
        self.current_draw_mode = mode
        self.draw_mode_changed.emit(mode)

    def _on_size_changed(self, value: int):
        self.current_size = value
        self.size_changed.emit(value)

    def _on_style_changed(self):
        style = self.style_combo.currentData() or self.STYLE_PIXELATE_VALUE
        self.current_style = style
        self._refresh_block_size_icons()
        self.style_changed.emit(style)

    def _on_block_size_clicked(self, button):
        level = next(l for l, b in self.block_size_buttons.items() if b is button)
        if level == self.current_block_size:
            # 再点一次当前档不算一次改动：下游会白重算一张缩小图、白压一条撤销
            return
        self.current_block_size = level
        self.block_size_changed.emit(level)

    def _refresh_block_size_icons(self):
        """按当前形态重画四个档位图样。

        粒度是马赛克和模糊共用的参数，图标只画其中一种就是在撒谎——所以形态一
        变，四个图标必须跟着变。
        """
        smooth = self.current_style == self.STYLE_BLUR_VALUE
        ratio = self.devicePixelRatioF()
        for level, button in self.block_size_buttons.items():
            button.setIcon(QIcon(
                render_block_size_swatch(level, self._swatch_side, smooth, ratio)
            ))

    # ------------------------------------------------------------------
    # 供 Toolbar 调用的公共接口（不触发信号）
    # ------------------------------------------------------------------

    def set_size(self, size: int):
        self.current_size = int(size)
        self.size_spin.blockSignals(True)
        self.size_spin.setValue(self.current_size)
        self.size_spin.blockSignals(False)

    def set_draw_mode(self, mode: str):
        if mode not in (self.MODE_FREEHAND_VALUE, self.MODE_RECT_VALUE):
            mode = self.MODE_FREEHAND_VALUE
        self.current_draw_mode = mode
        self.freehand_btn.blockSignals(True)
        self.rect_btn.blockSignals(True)
        self.freehand_btn.setChecked(mode == self.MODE_FREEHAND_VALUE)
        self.rect_btn.setChecked(mode == self.MODE_RECT_VALUE)
        self.freehand_btn.blockSignals(False)
        self.rect_btn.blockSignals(False)

    def set_style(self, style: str):
        if style not in (self.STYLE_PIXELATE_VALUE, self.STYLE_BLUR_VALUE):
            style = self.STYLE_PIXELATE_VALUE
        self.current_style = style
        idx = self.style_combo.findData(style)
        if idx >= 0:
            self.style_combo.blockSignals(True)
            self.style_combo.setCurrentIndex(idx)
            self.style_combo.blockSignals(False)
        self._refresh_block_size_icons()

    def set_block_size(self, value: int):
        # 吸附规则只有 MosaicTool 一份：面板不另立一套，旧配置里的非档位值
        # 才不会在"高亮哪一档"和"实际画出来多粗"之间分家。
        value = MosaicTool.clamp_block_size(value)
        self.current_block_size = value
        for level, button in self.block_size_buttons.items():
            button.setChecked(level == value)

    def retranslate(self):
        """语言切换后刷新面板上的可翻译文本（保留当前选中状态）。"""
        self.freehand_btn.setToolTip(_tr("Freehand Mosaic"))
        self.rect_btn.setToolTip(_tr("Rect Mosaic"))
        self.size_spin.setToolTip(_tr("Brush Size"))
        for index, button in enumerate(self.block_size_buttons.values()):
            button.setToolTip(_tr(BLOCK_SIZE_TIPS[index]))
        self.style_combo.setItemText(0, _tr("Mosaic"))
        self.style_combo.setItemText(1, _tr("Blur"))
        self.style_combo.setToolTip(_tr("Mosaic Style"))

    @property
    def draw_mode(self) -> str:
        return self.current_draw_mode

    @draw_mode.setter
    def draw_mode(self, value: str):
        self.set_draw_mode(value)

    @property
    def style(self) -> str:
        return self.current_style

    @style.setter
    def style(self, value: str):
        self.set_style(value)

    @property
    def block_size(self) -> int:
        return self.current_block_size

    @block_size.setter
    def block_size(self, value: int):
        self.set_block_size(value)
