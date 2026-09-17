"""
箭头工具设置面板
基于文字面板布局，仅用于箭头工具
"""
from PySide6.QtWidgets import (
    QApplication, QWidget, QHBoxLayout, QPushButton, QComboBox, QFrame, QStyle,
    QStyledItemDelegate, QStyleOptionGraphicsItem, QStyleOptionViewItem
)
from PySide6.QtCore import Qt, Signal, QPoint, QRect, QSize, QPointF
from PySide6.QtGui import QColor, QIcon, QPainter, QPen, QPixmap
from canvas.items import ArrowItem
from core.i18n import make_tr
from tools.base import Tool
from .base_settings_panel import StepperWidget, build_settings_panel_stylesheet, paint_rounded_panel, PANEL_SCALE
from .color_picker_button import ColorPickerButton
from core import safe_event

_tr = make_tr("ArrowSettingsPanel")

# 下拉里每种样式叫什么：图标只画形状，名字得靠 tooltip 说
ARROW_STYLE_NAMES = {
    ArrowItem.STYLE_SINGLE: "Solid Arrow",
    ArrowItem.STYLE_DOUBLE: "Solid Double Arrow",
    ArrowItem.STYLE_HOLLOW: "Outlined Arrow",
    ArrowItem.STYLE_LINE: "Line Arrow",
    ArrowItem.STYLE_LINE_DOUBLE: "Line Double Arrow",
    ArrowItem.STYLE_TRIANGLE: "Thin Arrow",
    ArrowItem.STYLE_TRIANGLE_DOUBLE: "Thin Double Arrow",
    ArrowItem.STYLE_BAR: "Dimension Line",
    ArrowItem.STYLE_BAR_ARROW: "Dimension Arrow",
}

# 预览图尺寸：够装下按 PREVIEW_STROKE_WIDTH 算出来的箭头头部，再大就只是把下拉
# 每一行白白撑高
PREVIEW_SIZE = QSize(80, 22)
# 下拉一行的大小。比预览图宽出来的部分就是行两侧的余白：不留的话，箭头尖直接
# 顶在行的边线上，九行糊成一片
PREVIEW_ROW_SIZE = QSize(96, 26)
# 画预览用的线宽。箭头各部件的尺寸都是按线宽算的，这里调大调小 = 整支预览一起缩放
PREVIEW_STROKE_WIDTH = 3
# 图标固定用中性墨色：这是样式选择器不是颜色选择器，跟着当前颜色走的话，
# 选浅色标注时白底上的图标自己就看不见了（和序号样式条同一个理由）。
# 下拉选中行是蓝底，深墨色会糊在上面，所以另备一张白的挂到 Selected 模式。
PREVIEW_INK = QColor("#444444")
PREVIEW_INK_SELECTED = QColor("#FFFFFF")

_icon_cache = {}


def render_arrow_style_preview(style: str, ink: QColor, size: QSize, ratio: float = 1.0) -> QPixmap:
    """把一个真的 ArrowItem 画进 pixmap 当预览图。

    直接复用图元自己的 paint，图标和真正画出来的箭头才不会各长各的——加一种
    样式时也不必再配一张手绘 SVG，配了就迟早和真实形状对不上。
    """
    # 按 ratio 多画一些像素再声明 DPR，下拉里放大显示时才不糊；QPainter 会
    # 自己按 DPR 缩放，所以下面一律用逻辑坐标，不要再手动 scale 一次
    pixmap = QPixmap(round(size.width() * ratio), round(size.height() * ratio))
    pixmap.setDevicePixelRatio(ratio)
    pixmap.fill(Qt.GlobalColor.transparent)

    # 预览图自己也留一圈余白：箭头画到贴边的话，收起来的那个框里它就顶着边框
    padding = 6.0
    middle = size.height() / 2.0
    item = ArrowItem(
        QPointF(padding, middle),
        QPointF(size.width() - padding, middle),
        QPen(ink, PREVIEW_STROKE_WIDTH),
        style,
    )

    painter = QPainter(pixmap)
    try:
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        item.paint(painter, QStyleOptionGraphicsItem(), None)
    finally:
        painter.end()
    return pixmap


def arrow_style_icon(style: str, ratio: float = 2.0) -> QIcon:
    """样式下拉用的图标（进程内缓存，一种样式只画一次）"""
    cached = _icon_cache.get((style, ratio))
    if cached is not None:
        return cached

    icon = QIcon()
    icon.addPixmap(
        render_arrow_style_preview(style, PREVIEW_INK, PREVIEW_SIZE, ratio),
        QIcon.Mode.Normal
    )
    icon.addPixmap(
        render_arrow_style_preview(style, PREVIEW_INK_SELECTED, PREVIEW_SIZE, ratio),
        QIcon.Mode.Selected
    )
    _icon_cache[(style, ratio)] = icon
    return icon


class StylePreviewDelegate(QStyledItemDelegate):
    """把预览图画在下拉行的正中

    行里只有图标没有文字，Qt 默认把图标顶到行首、把剩下的宽度全留给那串空文
    字——行一加宽，九张预览就齐刷刷贴在左边。这里自己量一次居中画；行宽也一并
    定死，弹出列表才有个准数照着撑开。
    """

    def sizeHint(self, option, index):
        return QSize(PREVIEW_ROW_SIZE)

    def paint(self, painter, option, index):
        opt = QStyleOptionViewItem(option)
        self.initStyleOption(opt, index)

        # 底色、选中态、悬停态照旧由样式表画，只把图标接管过来自己摆。
        # icon 必须拷一份：opt.icon 取到的是那个字段本身，清空字段会把它一起清掉
        icon = QIcon(opt.icon)
        opt.icon = QIcon()
        opt.text = ""
        widget = opt.widget
        style = widget.style() if widget is not None else QApplication.style()
        style.drawControl(QStyle.ControlElement.CE_ItemViewItem, opt, painter, widget)

        mode = (QIcon.Mode.Selected
                if opt.state & QStyle.StateFlag.State_Selected
                else QIcon.Mode.Normal)
        # 按预览图自己的尺寸摆，不按行宽拉满：拉满就等于把 2 倍图放大回来，
        # 既糊又重新贴到行的两条边上
        target = QRect(QPoint(0, 0), PREVIEW_SIZE.boundedTo(opt.rect.size()))
        target.moveCenter(opt.rect.center())
        icon.paint(painter, target, Qt.AlignmentFlag.AlignCenter, mode, QIcon.State.Off)


class ArrowSettingsPanel(QWidget):
    """箭头工具二级菜单"""

    arrow_style_changed = Signal(str)
    size_changed = Signal(int)
    color_changed = Signal(QColor)
    opacity_changed = Signal(int)  # 兼容旧接口（无控件）

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.current_arrow_style = ArrowItem.STYLE_SINGLE
        self.current_size = 3
        self.current_color = QColor(Qt.GlobalColor.red)
        self._cached_opacity = 255

        self._init_ui()
        self._connect_signals()

    @safe_event
    def paintEvent(self, event):
        paint_rounded_panel(self)

    def _init_ui(self):
        """初始化UI布局"""
        self.setStyleSheet(build_settings_panel_stylesheet(
            combo_enabled=True,
            combo_padding="1px",
            combo_min_width=88,
            combo_max_width=88,
            combo_padding_compact=True
        ))

        layout = QHBoxLayout(self)
        layout.setContentsMargins(round(10 * PANEL_SCALE), round(8 * PANEL_SCALE),
                                  round(10 * PANEL_SCALE), round(8 * PANEL_SCALE))
        layout.setSpacing(round(10 * PANEL_SCALE))

        # === 1. 基础样式区 ===
        self.arrow_style_combo = QComboBox()
        self.arrow_style_combo.setIconSize(PREVIEW_SIZE)
        for style in ArrowItem.STYLES:
            self.arrow_style_combo.addItem(arrow_style_icon(style), "", style)
            name = ARROW_STYLE_NAMES.get(style)
            if name:
                self.arrow_style_combo.setItemData(
                    self.arrow_style_combo.count() - 1, _tr(name), Qt.ItemDataRole.ToolTipRole
                )
        self.arrow_style_combo.setToolTip(self.tr("Arrow Style"))
        # 一次列完，别让用户滚：样式是靠形状认的，滚起来就得来回比
        self.arrow_style_combo.setMaxVisibleItems(len(ArrowItem.STYLES))
        self.arrow_style_combo.setItemDelegate(
            StylePreviewDelegate(self.arrow_style_combo)
        )
        # 弹出列表默认只有收起来那个框那么宽，80px 的预览塞进去要被缩一道还贴边；
        # 按行宽把它撑开，预览才能按原尺寸摆在正中
        self.arrow_style_combo.view().setMinimumWidth(PREVIEW_ROW_SIZE.width())
        layout.addWidget(self.arrow_style_combo)

        # 线宽选择：范围跟 ArrowTool（未覆写 MIN/MAX_WIDTH）实际允许的宽度一致，
        # 否则滚轮等入口能把宽度调到面板显示范围之外，图元继续变大但面板数字卡住不动
        self.size_spin = StepperWidget(self.current_size, Tool.MIN_WIDTH, Tool.MAX_WIDTH)
        self.size_spin.setFixedWidth(round(60 * PANEL_SCALE))
        self.size_spin.setToolTip(self.tr("Line Width"))
        layout.addWidget(self.size_spin)

        # 透明度选择（百分比）
        self.opacity_spin = StepperWidget(self._opacity_to_percent(self._cached_opacity), 0, 100, "%")
        self.opacity_spin.setFixedWidth(round(72 * PANEL_SCALE))
        self.opacity_spin.setToolTip(self.tr("Opacity (%)"))
        layout.addWidget(self.opacity_spin)

        # 分隔线
        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        layout.addWidget(line1)

        # === 2. 颜色预设区 ===
        self.color_btn = ColorPickerButton(
            self.current_color, size=round(28 * PANEL_SCALE), show_alpha=True
        )
        self.color_btn.setToolTip(self.tr("Custom Color"))
        layout.addWidget(self.color_btn)

        preset_colors = [
            "#FF0000",
            "#FFFF00",
            "#00FF00",
            "#0000FF",
            "#000000",
            "#FFFFFF",
        ]

        for color_str in preset_colors:
            btn = QPushButton()
            btn.setFixedSize(round(24 * PANEL_SCALE), round(24 * PANEL_SCALE))
            btn.setToolTip(color_str)
            border_color = "#888888" if color_str == "#FFFFFF" else "#333333"
            btn.setStyleSheet(f"""
                QPushButton {{
                    background-color: {color_str};
                    border: 1px solid {border_color};
                    border-radius: 6px;
                }}
                QPushButton:hover {{
                    border: 2px solid #000;
                }}
            """)
            btn.clicked.connect(lambda checked, c=color_str: self._on_preset_color_clicked(c))
            layout.addWidget(btn)

        layout.addStretch()

    def _connect_signals(self):
        """连接内部信号"""
        self.arrow_style_combo.currentIndexChanged.connect(self._on_arrow_style_changed)
        self.size_spin.valueChanged.connect(self._on_size_changed)
        self.opacity_spin.valueChanged.connect(self._on_opacity_changed)
        self.color_btn.color_changed.connect(self._on_color_picked)

    def _on_color_picked(self, color: QColor):
        """颜色选择器回调"""
        self.current_color = color
        # 同步 alpha 到 opacity_spin
        alpha = color.alpha()
        self._cached_opacity = alpha
        if hasattr(self, 'opacity_spin'):
            self.opacity_spin.blockSignals(True)
            self.opacity_spin.setValue(self._opacity_to_percent(alpha))
            self.opacity_spin.blockSignals(False)
        self.color_changed.emit(color)

    def _on_preset_color_clicked(self, color_str: str):
        """点击预设颜色"""
        color = QColor(color_str)
        self.current_color = color
        self.color_btn.set_color(color)
        self.color_changed.emit(color)

    def _on_arrow_style_changed(self):
        """箭头样式改变"""
        style = self.arrow_style_combo.currentData()
        if not style:
            style = self.arrow_style_combo.currentText()
        self.current_arrow_style = style
        self.arrow_style_changed.emit(style)

    def _on_size_changed(self, value: int):
        """线宽改变"""
        self.current_size = value
        self.size_changed.emit(value)

    def _on_opacity_changed(self, value: int):
        """透明度改变"""
        self._cached_opacity = self._percent_to_opacity(value)
        self.opacity_changed.emit(self._cached_opacity)

    def _opacity_to_percent(self, opacity: int) -> int:
        """0-255 透明度转百分比"""
        return max(0, min(100, int(round(opacity / 255 * 100))))

    def _percent_to_opacity(self, percent: int) -> int:
        """百分比转 0-255 透明度"""
        return max(0, min(255, int(round(percent / 100 * 255))))

    # ========================================================================
    # 公共接口方法（供 Toolbar 调用）
    # ========================================================================

    def set_color(self, color: QColor):
        """设置颜色（不触发信号）"""
        self.current_color = color
        self.color_btn.set_color(color)

    def set_size(self, size: int):
        """设置线宽（不触发信号）"""
        self.current_size = int(size)
        self.size_spin.blockSignals(True)
        self.size_spin.setValue(self.current_size)
        self.size_spin.blockSignals(False)

    def set_opacity(self, opacity: int):
        """兼容接口：箭头面板无透明度控件"""
        self._cached_opacity = int(opacity)
        self.opacity_spin.blockSignals(True)
        self.opacity_spin.setValue(self._opacity_to_percent(self._cached_opacity))
        self.opacity_spin.blockSignals(False)

    @property
    def arrow_style(self) -> str:
        """获取当前箭头样式"""
        return self.current_arrow_style

    @arrow_style.setter
    def arrow_style(self, value: str):
        """设置当前箭头样式（不触发信号）"""
        if value in ArrowItem.STYLE_SPECS:
            self.current_arrow_style = value
            idx = self.arrow_style_combo.findData(value)
            if idx >= 0:
                self.arrow_style_combo.blockSignals(True)
                self.arrow_style_combo.setCurrentIndex(idx)
                self.arrow_style_combo.blockSignals(False)

    def set_state_from_item(self, item):
        """根据选中的 ArrowItem 更新面板状态"""
        if not item:
            return

        self.blockSignals(True)

        arrow_style = getattr(item, "arrow_style", None) or getattr(item, "_arrow_style", None)
        if arrow_style:
            idx = self.arrow_style_combo.findData(arrow_style)
            if idx >= 0:
                self.arrow_style_combo.setCurrentIndex(idx)

        if hasattr(item, "base_width"):
            self.size_spin.setValue(int(round(float(item.base_width))))

        color = getattr(item, "color", None)
        if color is None and hasattr(item, "pen"):
            color = item.pen().color()
        if color is not None:
            self.current_color = color
        self._update_color_btn(self.current_color)

        opacity = getattr(item, "opacity", None) or getattr(item, "_opacity", None)
        if opacity is None and color is not None:
            opacity = color.alpha()
        if opacity is not None:
            self._cached_opacity = int(opacity)
            self.opacity_spin.setValue(self._opacity_to_percent(self._cached_opacity))

        self.blockSignals(False)
 