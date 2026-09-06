"""
马赛克工具设置面板

左侧：框选/画笔模式切换 + 笔刷大小；右侧：粒度滑动条 + 马赛克种类（马赛克/模糊）。
马赛克没有"颜色"这个概念（涂抹的是背景像素），所以不带颜色和透明度控件。
"""
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QPushButton, QComboBox, QFrame, QButtonGroup,
    QStyle, QStyleOptionComboBox, QStylePainter, QSlider,
)
from PySide6.QtCore import Qt, Signal, QSize
from core.resource_manager import ResourceManager
from core.i18n import make_tr
from tools.mosaic import MosaicTool
from .base_settings_panel import StepperWidget, build_settings_panel_stylesheet, paint_rounded_panel, PANEL_SCALE
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
    MIN_BLOCK_SIZE = MosaicTool.MIN_BLOCK_SIZE
    MAX_BLOCK_SIZE = MosaicTool.MAX_BLOCK_SIZE

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

        # === 粒度滑动条：格子/模糊有多粗，越往右越糊 ===
        self.block_size_slider = QSlider(Qt.Orientation.Horizontal)
        self.block_size_slider.setRange(self.MIN_BLOCK_SIZE, self.MAX_BLOCK_SIZE)
        self.block_size_slider.setValue(self.current_block_size)
        # 拖动过程中不发 valueChanged，松手才发一次。
        #
        # 这不是性能微调，是这个控件的语义：滑块每挪一格都会让下游按新粒度把
        # 整张背景重新收缩一遍（4K 实测 28ms/档），并且给撤销栈压一条各自持有
        # 一份缩小图的命令。按住拖一次会连发三十来次——画面卡住、撤销要按三十
        # 多下才回得去、这些命令攥着的小图加起来 39MB，比背景原图本身还大，
        # 把"只留一份缩小图"省下来的内存又全赔了回去。
        #
        # 换句话说：滑块拖动中的每个中间值是"还没想好"，不是用户的决定，
        # 不该让下游看见。Qt 自带这个语义，不必再搭去抖定时器或命令合并。
        self.block_size_slider.setTracking(False)
        self.block_size_slider.setFixedWidth(round(64 * PANEL_SCALE))
        self.block_size_slider.setToolTip(_tr("Mosaic Granularity"))
        self.block_size_slider.setStyleSheet("""
            QSlider { background: transparent; }
            QSlider::groove:horizontal {
                height: 3px;
                background: #ccc;
                border-radius: 1px;
            }
            QSlider::handle:horizontal {
                width: 12px;
                height: 12px;
                margin: -5px 0;
                background: #666;
                border-radius: 6px;
            }
            QSlider::handle:horizontal:hover {
                background: #333;
            }
        """)
        layout.addWidget(self.block_size_slider)

        # === 右侧：马赛克种类 ===
        self.style_combo = CenteredComboBox()
        self.style_combo.addItem(_tr("Mosaic"), self.STYLE_PIXELATE_VALUE)
        self.style_combo.addItem(_tr("Blur"), self.STYLE_BLUR_VALUE)
        self.style_combo.setToolTip(_tr("Mosaic Style"))
        self.style_combo.setMaxVisibleItems(10)
        layout.addWidget(self.style_combo)

        self.set_draw_mode(self.current_draw_mode)
        self.set_style(self.current_style)

    def _connect_signals(self):
        self.mode_group.buttonClicked.connect(self._on_mode_clicked)
        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.style_combo.currentIndexChanged.connect(self._on_style_changed)
        self.block_size_slider.valueChanged.connect(self._on_block_size_changed)

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
        self.style_changed.emit(style)

    def _on_block_size_changed(self, value: int):
        self.current_block_size = value
        self.block_size_changed.emit(value)

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

    def set_block_size(self, value: int):
        value = max(self.MIN_BLOCK_SIZE, min(self.MAX_BLOCK_SIZE, int(value)))
        self.current_block_size = value
        self.block_size_slider.blockSignals(True)
        self.block_size_slider.setValue(value)
        self.block_size_slider.blockSignals(False)

    def retranslate(self):
        """语言切换后刷新面板上的可翻译文本（保留当前选中状态）。"""
        self.freehand_btn.setToolTip(_tr("Freehand Mosaic"))
        self.rect_btn.setToolTip(_tr("Rect Mosaic"))
        self.size_spin.setToolTip(_tr("Brush Size"))
        self.block_size_slider.setToolTip(_tr("Mosaic Granularity"))
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
