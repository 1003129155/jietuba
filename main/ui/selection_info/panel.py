# -*- coding: utf-8 -*-
"""
选区信息面板 —— 纯 UI 层

独立小 QWidget 浮层，跟随选区上方。
职责：按钮布局 + 信号发射，不包含业务逻辑。

架构与 MagnifierOverlay / Toolbar 相同：
  - parent = ScreenshotWindow
  - move() 跟随选区
  - update() 仅重绘自身
  - 不参与 QGraphicsScene → 不污染截图导出
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QPoint, QRectF, QSize, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QBrush, QCursor
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QLabel, QPushButton, QSpacerItem, QSizePolicy,
)

from core.resource_manager import ResourceManager
from core.ui_scale import get_ui_scale, scaled
from core import safe_event
from core.logger import log_exception, T
from core.ui_theme import set_own_style


class SelectionInfoPanel(QWidget):
    """选区上方的信息 + 快捷操作面板（纯 UI）"""

    # ── 信号：按钮被点击/切换，由 controller 连接 ──
    border_toggled   = Signal(bool)    # 边框/阴影开关
    rounded_toggled  = Signal(bool)    # 圆角截图开关
    lock_ratio_toggled = Signal(bool)  # 锁定纵横比开关
    refresh_pressed  = Signal()        # 刷新按钮按下（单击 / 长按开始）
    refresh_released = Signal()        # 刷新按钮松开（长按结束）

    _BG = QColor(30, 30, 30, 210)
    _BORDER_COLOR = QColor(255, 255, 255, 140)

    # 基准尺寸（100% 下的实际像素）
    BASE_RADIUS = 7
    BASE_HEIGHT = 34
    BASE_MARGIN_ABOVE = 7   # 面板底部到选区顶部的间距
    BASE_BTN = 26
    BASE_ICON = 19
    BASE_SEP_HEIGHT = 19
    BASE_FONT = 14

    def __init__(self, parent: QWidget, view):
        super().__init__(parent)
        self._view = view
        self._last_rect = None   # 改比例后要按新尺寸重新贴回选区上方
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self._init_ui()
        self.hide()
        # 改比例后自行重算尺寸（连接随本部件销毁自动断开）
        get_ui_scale().scale_changed.connect(self.apply_scale)

    # ------------------------------------------------------------------
    # UI
    # ------------------------------------------------------------------
    def _init_ui(self):
        lay = QHBoxLayout(self)
        self._row_layout = lay

        # 坐标 + 尺寸
        self._info_label = QLabel()
        lay.addWidget(self._info_label)
        # 留引用，改比例时连这段间隔一起重算
        self._label_spacer = QSpacerItem(scaled(5), 0,
                                         QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Minimum)
        lay.addItem(self._label_spacer)

        # 按钮（顺序：圆角 → 纵横比 → 描边阴影）
        self._buttons = []
        self.btn_rounded = self._make_btn("圆角.svg", checkable=True,
                                           tip="Rounded Corners")
        self.btn_lock = self._make_btn("保持纵横比.svg", checkable=True,
                                        tip="Lock Aspect Ratio")
        self.btn_border = self._make_btn("阴影描边.svg", checkable=True,
                                          tip="Border / Shadow")

        for btn in (self.btn_rounded, self.btn_lock, self.btn_border):
            lay.addWidget(btn)

        # 分隔线
        self._sep = QLabel()
        set_own_style(self._sep, "background: rgba(255,255,255,30);")
        lay.addWidget(self._sep)

        # 刷新背景按钮（最右）
        self.btn_refresh = self._make_btn("刷新背景.svg", checkable=False,
                                           tip="Refresh Background")
        self.btn_refresh.pressed.connect(self.refresh_pressed)
        self.btn_refresh.released.connect(self.refresh_released)
        lay.addWidget(self.btn_refresh)

        # 转发信号
        self.btn_border.toggled.connect(self.border_toggled)
        self.btn_rounded.toggled.connect(self.rounded_toggled)
        self.btn_lock.toggled.connect(self.lock_ratio_toggled)

        # 右侧按钮默认隐藏，选区确认后才显示
        self._action_widgets = [
            self.btn_rounded, self.btn_lock, self.btn_border,
            self._sep, self.btn_refresh,
        ]
        for w in self._action_widgets:
            w.hide()

        self.apply_scale()

    def _make_btn(self, svg: str, checkable=False, tip="") -> QPushButton:
        btn = QPushButton()
        btn.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setCheckable(checkable)
        btn.setToolTip(self.tr(tip))
        try:
            path = ResourceManager.get_icon_path(svg)
            btn.setIcon(ResourceManager.get_icon(path))
        except Exception as e:
            log_exception(e, T("加载按钮图标"))
            btn.setText(tip[:2])  # 找不到图标时用文字兜底
        self._buttons.append(btn)
        return btn

    # ------------------------------------------------------------------
    # 缩放
    # ------------------------------------------------------------------
    def apply_scale(self):
        """按当前比例重算面板尺寸。开关状态和尺寸文字都不动，只改显示大小。"""
        self.setFixedHeight(scaled(self.BASE_HEIGHT))
        self._row_layout.setContentsMargins(scaled(10), 0, scaled(5), 0)
        self._row_layout.setSpacing(scaled(7))
        set_own_style(self._info_label,
            f"color: #D0D0D0; font-size: {scaled(self.BASE_FONT)}px; background: transparent;"
        )
        btn_sz = scaled(self.BASE_BTN)
        icon_sz = scaled(self.BASE_ICON)
        radius = scaled(4)
        padding = scaled(2)
        for btn in self._buttons:
            btn.setFixedSize(btn_sz, btn_sz)
            btn.setIconSize(QSize(icon_sz, icon_sz))
            btn.setStyleSheet(f"""
                QPushButton {{ background: transparent; border: none;
                               border-radius: {radius}px; padding: {padding}px; }}
                QPushButton:hover {{ background: rgba(255,255,255,40); }}
                QPushButton:checked {{ background: rgba(64,224,208,80); }}
            """)
        self._sep.setFixedSize(1, scaled(self.BASE_SEP_HEIGHT))
        self._label_spacer.changeSize(scaled(5), 0,
                                      QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Minimum)
        self._row_layout.invalidate()
        self.adjustSize()
        self.update()
        if self.isVisible() and self._view is not None and self._last_rect is not None:
            self.follow_rect(self._last_rect)

    # ------------------------------------------------------------------
    # 位置跟随（由 controller 调用）
    # ------------------------------------------------------------------
    def follow_rect(self, scene_rect: QRectF):
        """
        根据选区场景坐标，把自己定位到不遮挡选区的位置。
        优先级：
          1. 选区正上方靠左对齐
          2. 选区左侧上方（垂直排列）
          3. 选区右侧上方（垂直排列）
        """
        self._last_rect = QRectF(scene_rect)
        if scene_rect.isEmpty():
            self.hide()
            return

        vp = self._view.viewport()
        vw, vh = vp.width(), vp.height()
        pw, ph = self.width(), self.height()
        gap = scaled(self.BASE_MARGIN_ABOVE)

        # 选区在 view 坐标系中的边界
        view_tl = self._view.mapFromScene(scene_rect.topLeft())
        view_br = self._view.mapFromScene(scene_rect.bottomRight())
        sel_left = int(view_tl.x())
        sel_top = int(view_tl.y())
        sel_right = int(view_br.x())

        # ── 策略 1：选区正上方，靠左对齐 ──
        x = sel_left
        y = sel_top - ph - gap
        # 右侧超出 → 左移
        if x + pw > vw:
            x = vw - pw
        x = max(0, x)

        if y >= 0:
            self.move(QPoint(x, y))
            return

        # ── 策略 2：选区左侧，顶部对齐 ──
        x = sel_left - pw - gap
        y = sel_top
        if x >= 0 and y + ph <= vh:
            self.move(QPoint(x, y))
            return

        # ── 策略 3：选区右侧，顶部对齐 ──
        x = sel_right + gap
        y = sel_top
        if x + pw <= vw and y + ph <= vh:
            self.move(QPoint(x, y))
            return

        # ── 兜底：选区内部左上角 ──
        inset = scaled(4)
        x = max(0, sel_left + inset)
        y = sel_top + inset
        self.move(QPoint(x, y))

    def set_confirmed(self, confirmed: bool):
        """选区确认状态变化时，控制右侧功能按钮的显隐"""
        for w in self._action_widgets:
            w.setVisible(confirmed)
        self.adjustSize()

    def update_info_text(self, rect: QRectF):
        """更新坐标 + 尺寸文字"""
        from core.theme import get_theme
        theme_hex = get_theme().theme_color_hex
        x, y = int(rect.x()), int(rect.y())
        w, h = int(rect.width()), int(rect.height())
        self._info_label.setText(
            f"<span style='color:{theme_hex}'>{x},{y}</span>"
            f"&nbsp;&nbsp;{w} × {h} px"
        )
        self.adjustSize()

    # ------------------------------------------------------------------
    # 绘制背景
    # ------------------------------------------------------------------
    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        # 白色描边
        p.setPen(QPen(self._BORDER_COLOR, 1))
        p.setBrush(QBrush(self._BG))
        # 往内缩 0.5px，避免描边被裁切
        r = self.rect().adjusted(1, 1, -1, -1)
        radius = scaled(self.BASE_RADIUS)
        p.drawRoundedRect(r, radius, radius)
        p.end()
        super().paintEvent(event)
 