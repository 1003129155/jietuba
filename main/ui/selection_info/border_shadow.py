# -*- coding: utf-8 -*-
"""
描边/阴影 —— 独立逻辑模块

功能：
  1. 切换按钮启用后弹出二级菜单：阴影/描边二选一 + 滑块调大小 + 持久开关
  2. 预览：二级菜单可见时，在 MaskOverlay 上绘制描边或阴影效果
  3. 导出：ExportService 导出后在图像外围添加描边或阴影

UI 组件：
  BorderShadowPopup  – 二级菜单浮层
  BorderShadowLogic  – 业务逻辑控制器
"""

from __future__ import annotations

from PySide6.QtCore import (
    Qt, QRectF, QRect, QSize, QTimer, QPoint,
    Signal, QObject, QEvent,
)
from PySide6.QtGui import (
    QColor, QPainter, QPainterPath, QBrush, QPen, QImage, QCursor,
    QLinearGradient, QRadialGradient,
)
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QSlider, QLabel,
    QPushButton, QCheckBox, QFrame,
)

from core.logger import log_debug
from core import safe_event
from ..color_picker_button import ColorPickerButton


# =====================================================================
# UI: 描边/阴影 二级菜单浮层
# =====================================================================

class BorderShadowPopup(QWidget):
    """
    描边/阴影二级菜单。

    布局：
      ┌─────────────────────────┐
      │  [■色 阴影]  [■色 描边] │   ← 颜色块+文字为一组，选中整组高亮
      │  大小 ─────────●── 21   │   ← 滑块 + 数值
      │  □ 每次截图都保持开启     │   ← 持久开关
      └─────────────────────────┘
      左键点击组：切换模式  |  点击颜色块：弹出颜色选择器
    """

    mode_changed = Signal(str)              # "shadow" / "border"
    size_changed = Signal(int)              # 大小值
    shadow_color_changed = Signal(str)      # 阴影颜色 hex
    border_color_changed = Signal(str)      # 描边颜色 hex
    persist_changed = Signal(bool)          # 持久开关
    visibility_changed = Signal(bool)       # 面板显示/隐藏

    _BG = QColor(30, 30, 30, 230)
    _RADIUS = 6

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.setFixedWidth(200)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setMouseTracking(True)
        self._current_mode = "shadow"
        self._shadow_color = "#0078FF"
        self._border_color = "#FF0000"
        self._init_ui()
        # 延时隐藏定时器
        self._hide_timer = QTimer(self)
        self._hide_timer.setSingleShot(True)
        self._hide_timer.setInterval(200)
        self._hide_timer.timeout.connect(self._do_hide)
        self.hide()

    @staticmethod
    def _get_theme_hex() -> str:
        from core.theme import get_theme
        return get_theme().theme_color_hex

    # ------------------------------------------------------------------
    # UI 构建
    # ------------------------------------------------------------------
    def _init_ui(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(10, 8, 10, 8)
        root.setSpacing(6)

        # ── 第一行：[■色 阴影]  [■色 描边] 均分宽度 ──
        row_mode = QHBoxLayout()
        row_mode.setSpacing(0)

        # 阴影组（颜色块 + 文字，整体可点击切换模式）
        self._shadow_group = QWidget()
        self._shadow_group.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        shadow_lay = QHBoxLayout(self._shadow_group)
        shadow_lay.setContentsMargins(6, 4, 6, 4)
        shadow_lay.setSpacing(5)
        self._shadow_color_block = ColorPickerButton(
            QColor(self._shadow_color), show_alpha=True, size=18
        )
        self._shadow_color_block.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self._shadow_color_block.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._shadow_color_block.setToolTip(self.tr("Select shadow color"))
        self._shadow_label = QLabel(self.tr("Shadow"))
        self._shadow_label.setStyleSheet("color: #D0D0D0; font-size: 12px; background: transparent;")
        shadow_lay.addWidget(self._shadow_color_block)
        shadow_lay.addWidget(self._shadow_label)
        shadow_lay.addStretch()
        row_mode.addWidget(self._shadow_group, 1)

        row_mode.addSpacing(6)

        # 描边组（颜色块 + 文字，整体可点击切换模式）
        self._border_group = QWidget()
        self._border_group.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        border_lay = QHBoxLayout(self._border_group)
        border_lay.setContentsMargins(6, 4, 6, 4)
        border_lay.setSpacing(5)
        self._border_color_block = ColorPickerButton(
            QColor(self._border_color), show_alpha=True, size=18
        )
        self._border_color_block.setCursor(QCursor(Qt.CursorShape.PointingHandCursor))
        self._border_color_block.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._border_color_block.setToolTip(self.tr("Select border color"))
        self._border_label = QLabel(self.tr("Border"))
        self._border_label.setStyleSheet("color: #D0D0D0; font-size: 12px; background: transparent;")
        border_lay.addWidget(self._border_color_block)
        border_lay.addWidget(self._border_label)
        border_lay.addStretch()
        row_mode.addWidget(self._border_group, 1)

        root.addLayout(row_mode)

        # ── 第二行：颜色块 + 滑块 + 数值 ──
        row_slider = QHBoxLayout()
        row_slider.setSpacing(6)

        lbl_size = QLabel(self.tr("Size"))
        lbl_size.setStyleSheet("color: #AAAAAA; font-size: 12px; background: transparent;")
        row_slider.addWidget(lbl_size)

        self._slider = QSlider(Qt.Orientation.Horizontal)
        self._slider.setRange(1, 50)
        self._slider.setValue(21)
        self._slider.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._slider.setStyleSheet("""
            QSlider::groove:horizontal {
                height: 4px; background: #555; border-radius: 2px;
            }
            QSlider::handle:horizontal {
                width: 12px; height: 12px; margin: -4px 0;
                background: """ + self._get_theme_hex() + """; border-radius: 6px;
            }
            QSlider::sub-page:horizontal {
                background: """ + self._get_theme_hex() + """; border-radius: 2px;
            }
        """)
        row_slider.addWidget(self._slider, 1)

        self._label_val = QLabel("21")
        self._label_val.setFixedWidth(28)
        self._label_val.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._label_val.setStyleSheet("color: #D0D0D0; font-size: 12px; background: transparent;")
        row_slider.addWidget(self._label_val)
        root.addLayout(row_slider)

        # ── 第三行：持久开关 ──
        self._chk_persist = QCheckBox(self.tr("Keep enabled for every screenshot"))
        self._chk_persist.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._chk_persist.setStyleSheet("""
            QCheckBox { color: #AAAAAA; font-size: 11px; background: transparent; spacing: 4px; }
            QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #666; border-radius: 3px; background: transparent; }
            QCheckBox::indicator:checked { background: """ + self._get_theme_hex() + """; border-color: """ + self._get_theme_hex() + """; }
        """)
        root.addWidget(self._chk_persist)

        self.adjustSize()

        # ── 信号 ──
        self._shadow_group.mousePressEvent = lambda e: (
            self._shadow_color_block.click() if e.button() == Qt.MouseButton.RightButton
            else self._set_mode("shadow")
        )
        self._border_group.mousePressEvent = lambda e: (
            self._border_color_block.click() if e.button() == Qt.MouseButton.RightButton
            else self._set_mode("border")
        )
        self._shadow_color_block.color_changed.connect(self._on_shadow_color_picked)
        self._border_color_block.color_changed.connect(self._on_border_color_picked)
        self._slider.valueChanged.connect(self._on_slider_changed)
        self._chk_persist.toggled.connect(self.persist_changed)

        # 初始高亮
        self._refresh_mode_buttons()

    def _on_shadow_color_picked(self, color: QColor):
        """阴影颜色选择器回调"""
        self._shadow_color = color.name()
        self.shadow_color_changed.emit(self._shadow_color)

    def _on_border_color_picked(self, color: QColor):
        """描边颜色选择器回调"""
        self._border_color = color.name()
        self.border_color_changed.emit(self._border_color)

    def _refresh_mode_buttons(self):
        """刷新组容器的高亮状态（选中整组出现背景）"""
        _ACTIVE = """
            background: rgba(64,224,208,50);
            border: 1px solid rgba(64,224,208,120);
            border-radius: 4px;
        """
        _INACTIVE = """
            background: transparent;
            border: none;
            border-radius: 4px;
        """
        for group, label, mode in [
            (self._shadow_group, self._shadow_label, "shadow"),
            (self._border_group, self._border_label, "border"),
        ]:
            if mode == self._current_mode:
                group.setStyleSheet(_ACTIVE)
                label.setStyleSheet("color: #FFFFFF; font-size: 12px; background: transparent;")
            else:
                group.setStyleSheet(_INACTIVE)
                label.setStyleSheet("color: #999999; font-size: 12px; background: transparent;")

    # ------------------------------------------------------------------
    # 状态访问
    # ------------------------------------------------------------------
    @property
    def mode(self) -> str:
        return self._current_mode

    @mode.setter
    def mode(self, m: str):
        self._current_mode = m
        self._refresh_mode_buttons()

    @property
    def size_value(self) -> int:
        return self._slider.value()

    @size_value.setter
    def size_value(self, v: int):
        self._slider.setValue(v)

    @property
    def persist(self) -> bool:
        return self._chk_persist.isChecked()

    @persist.setter
    def persist(self, v: bool):
        self._chk_persist.setChecked(v)

    @property
    def shadow_color(self) -> str:
        return self._shadow_color

    @shadow_color.setter
    def shadow_color(self, c: str):
        self._shadow_color = c
        self._shadow_color_block.set_color(QColor(c))

    @property
    def border_color(self) -> str:
        return self._border_color

    @border_color.setter
    def border_color(self, c: str):
        self._border_color = c
        self._border_color_block.set_color(QColor(c))

    # ------------------------------------------------------------------
    # 内部回调
    # ------------------------------------------------------------------
    def _set_mode(self, mode: str):
        if self._current_mode == mode:
            return
        self._current_mode = mode
        self._refresh_mode_buttons()
        self.mode_changed.emit(mode)

    def _on_slider_changed(self, value: int):
        self._label_val.setText(str(value))
        self.size_changed.emit(value)

    # ------------------------------------------------------------------
    # 显隐控制
    # ------------------------------------------------------------------
    def schedule_hide(self):
        self._hide_timer.start()

    def cancel_hide(self):
        self._hide_timer.stop()

    def _do_hide(self):
        if self.underMouse():
            return
        self.hide()

    def show_near(self, anchor: QWidget):
        """优先显示在 anchor 按钮的正上方，空间不够则显示在下方"""
        parent = self.parentWidget()
        pw = parent.width()

        # ── 水平居中 ──
        global_ref = anchor.mapToGlobal(QPoint(0, 0))
        local_ref = parent.mapFromGlobal(global_ref)
        x = local_ref.x() - (self.width() - anchor.width()) // 2
        if x + self.width() > pw:
            x = pw - self.width()
        x = max(0, x)

        # ── 垂直方向：优先上方 ──
        gap = 4
        above_global = anchor.mapToGlobal(QPoint(0, -self.height() - gap))
        above_local = parent.mapFromGlobal(above_global)
        if above_local.y() >= 0:
            y = above_local.y()
        else:
            below_global = anchor.mapToGlobal(QPoint(0, anchor.height() + gap))
            below_local = parent.mapFromGlobal(below_global)
            y = below_local.y()

        self.move(x, y)
        self.show()
        self.raise_()

    @safe_event
    def showEvent(self, event):
        super().showEvent(event)
        self.visibility_changed.emit(True)

    @safe_event
    def hideEvent(self, event):
        super().hideEvent(event)
        self.visibility_changed.emit(False)

    # ------------------------------------------------------------------
    # 鼠标事件
    # ------------------------------------------------------------------
    @safe_event
    def enterEvent(self, event):
        self.cancel_hide()
        super().enterEvent(event)

    @safe_event
    def leaveEvent(self, event):
        self.schedule_hide()
        super().leaveEvent(event)

    # ------------------------------------------------------------------
    # 绘制背景
    # ------------------------------------------------------------------
    @safe_event
    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QBrush(self._BG))
        p.drawRoundedRect(self.rect(), self._RADIUS, self._RADIUS)
        p.end()
        super().paintEvent(event)


# =====================================================================
# 逻辑控制器
# =====================================================================

class BorderShadowLogic(QObject):
    """
    描边/阴影的完整逻辑：
      - UI 弹出/隐藏
      - 预览：二级菜单可见时在 MaskOverlay 上绘制描边或阴影
      - 导出：ExportService 导出后在图像外围添加效果
      - 跟随圆角：通过 rounded_corners_logic 获取当前圆角半径
    """

    def __init__(self, parent_widget: QWidget, btn_border: QPushButton,
                 selection_item, mask_overlay, export_service, selection_model,
                 config_manager=None, rounded_corners_logic=None,
                 hook_manager=None):
        super().__init__(parent_widget)
        self._parent = parent_widget
        self._btn = btn_border
        self._item = selection_item
        self._mask = mask_overlay
        self._export = export_service
        self._model = selection_model
        self._config = config_manager
        self._rounded_logic = rounded_corners_logic
        self._hook_mgr = hook_manager

        # 从设置中读取
        if self._config:
            self._enabled = self._config.get_app_setting("screenshot_border_enabled", False)
            self._mode = self._config.get_app_setting("screenshot_border_mode", "border")
            self._size = self._config.get_app_setting("screenshot_border_size", 21)
            self._shadow_color = self._config.get_app_setting("screenshot_shadow_color", "#0078FF")
            self._border_color = self._config.get_app_setting("screenshot_border_color", "#FF0000")
            self._persist = self._config.get_app_setting("screenshot_border_persist", False)
        else:
            self._enabled = False
            self._mode = "border"
            self._size = 21
            self._shadow_color = "#0078FF"
            self._border_color = "#FF0000"
            self._persist = False

        # 如果不持久，则每次默认关闭
        if not self._persist:
            self._enabled = False

        # 预览状态：只有二级菜单显示时才预览
        self._preview_visible = False

        # 创建弹出面板
        self._popup = BorderShadowPopup(parent_widget)
        self._popup.mode = self._mode
        self._popup.size_value = self._size
        self._popup.shadow_color = self._shadow_color
        self._popup.border_color = self._border_color
        self._popup.persist = self._persist

        # 同步按钮初始选中状态
        self._btn.blockSignals(True)
        self._btn.setChecked(self._enabled)
        self._btn.blockSignals(False)

        # 连接弹出面板信号
        self._popup.mode_changed.connect(self._on_mode_changed)
        self._popup.size_changed.connect(self._on_size_changed)
        self._popup.shadow_color_changed.connect(self._on_shadow_color_changed)
        self._popup.border_color_changed.connect(self._on_border_color_changed)
        self._popup.persist_changed.connect(self._on_persist_changed)
        self._popup.visibility_changed.connect(self._on_popup_visibility_changed)

        # 为按钮安装 hover 事件
        self._btn.setMouseTracking(True)
        self._btn.installEventFilter(self)

        # 安装钩子（通过 HookManager）
        self._install_mask_hook()
        self._install_export_hook()

    # ------------------------------------------------------------------
    # 圆角半径获取
    # ------------------------------------------------------------------
    def _get_corner_radius(self) -> int:
        """从 RoundedCornersLogic 获取当前圆角半径，未启用则返回 0"""
        if self._rounded_logic and self._rounded_logic.enabled:
            return self._rounded_logic.radius
        return 0

    def _get_active_color(self) -> str:
        """根据当前模式返回对应颜色"""
        if self._mode == "border":
            return self._border_color
        return self._shadow_color

    # ------------------------------------------------------------------
    # 公共接口
    # ------------------------------------------------------------------
    def set_rounded_corners_logic(self, logic):
        """延迟设置圆角逻辑引用（解决创建顺序依赖）"""
        self._rounded_logic = logic

    def set_enabled(self, enabled: bool):
        self._enabled = enabled
        log_debug(f"描边/阴影: {'ON' if enabled else 'OFF'}  mode={self._mode} size={self._size}",
                  "BorderShadow")
        if self._config:
            self._config.set_app_setting("screenshot_border_enabled", enabled)

        if enabled:
            self._popup.cancel_hide()
            self._popup.show_near(self._btn)
        else:
            self._popup.hide()
            self._preview_visible = False
            self._mask.update()

    @property
    def enabled(self) -> bool:
        return self._enabled

    # ------------------------------------------------------------------
    # 按钮 hover → 弹出/隐藏
    # ------------------------------------------------------------------
    @safe_event
    def eventFilter(self, obj, event):
        if obj is self._btn:
            if event.type() == QEvent.Type.Enter:
                if self._enabled:
                    self._popup.cancel_hide()
                    self._popup.show_near(self._btn)
            elif event.type() == QEvent.Type.Leave:
                if self._enabled:
                    self._popup.schedule_hide()
        return False

    # ------------------------------------------------------------------
    # 弹出面板回调
    # ------------------------------------------------------------------
    def _on_mode_changed(self, mode: str):
        self._mode = mode
        log_debug(f"模式切换: {mode}", "BorderShadow")
        if self._config:
            self._config.set_app_setting("screenshot_border_mode", mode)
        if self._preview_visible:
            self._mask.update()

    def _on_size_changed(self, value: int):
        self._size = value
        if self._config:
            self._config.set_app_setting("screenshot_border_size", value)
        if self._preview_visible:
            self._mask.update()

    def _on_shadow_color_changed(self, color_hex: str):
        self._shadow_color = color_hex
        if self._config:
            self._config.set_app_setting("screenshot_shadow_color", color_hex)
        if self._preview_visible and self._mode == "shadow":
            self._mask.update()

    def _on_border_color_changed(self, color_hex: str):
        self._border_color = color_hex
        if self._config:
            self._config.set_app_setting("screenshot_border_color", color_hex)
        if self._preview_visible and self._mode == "border":
            self._mask.update()

    def _on_persist_changed(self, checked: bool):
        self._persist = checked
        if self._config:
            self._config.set_app_setting("screenshot_border_persist", checked)

    def _on_popup_visibility_changed(self, visible: bool):
        """二级菜单显示/隐藏 → 控制预览"""
        self._preview_visible = visible and self._enabled
        self._mask.update()

    # ------------------------------------------------------------------
    # Hook: MaskOverlay.paintEvent() 预览描边/阴影（通过 HookManager）
    # ------------------------------------------------------------------
    def _install_mask_hook(self):
        mask = self._mask
        logic = self

        def _border_shadow_mask_callback(event):
            """描边/阴影遮罩预览回调（在原始 paintEvent + 圆角补丁之后执行）"""
            if not logic._preview_visible:
                return
            if logic._model.is_empty():
                return

            sel = logic._model.rect()
            local_sel = mask._scene_to_local(sel)
            size = logic._size
            corner_r = logic._get_corner_radius()
            color = QColor(logic._get_active_color())

            if local_sel.width() < 2 or local_sel.height() < 2:
                return

            painter = QPainter(mask)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)

            if logic._mode == "border":
                _draw_border_preview(painter, local_sel, size, color, corner_r)
            else:
                _draw_shadow_preview(painter, local_sel, size, color, corner_r)

            painter.end()

        self._mask_callback = _border_shadow_mask_callback
        self._hook_mgr.register(mask, 'paintEvent', _border_shadow_mask_callback,
                                wrap_mode="after")

    # ------------------------------------------------------------------
    # Hook: ExportService.export() 导出时添加描边/阴影（通过 HookManager）
    # ------------------------------------------------------------------
    def _install_export_hook(self):
        logic = self

        def _border_shadow_export_callback(img, selection_rect):
            """描边/阴影导出回调（chain 模式：接收上游 img，处理后传给下游）"""
            if not logic._enabled:
                return img
            if img.isNull():
                return img

            size = logic._size
            color = QColor(logic._get_active_color())
            corner_r = logic._get_corner_radius()

            if logic._mode == "border":
                return _apply_border_export(img, size, color, corner_r)
            else:
                return _apply_shadow_export(img, size, color, corner_r)

        self._export_callback = _border_shadow_export_callback
        self._hook_mgr.register(self._export, 'export', _border_shadow_export_callback,
                                wrap_mode="chain")

    # ------------------------------------------------------------------
    # 卸载
    # ------------------------------------------------------------------
    def uninstall(self):
        if self._hook_mgr:
            self._hook_mgr.unregister(self._mask, 'paintEvent', self._mask_callback)
            self._hook_mgr.unregister(self._export, 'export', self._export_callback)
        self._popup.hide()
        self._popup.deleteLater()


# =====================================================================
# 预览绘制辅助函数
# =====================================================================

def _draw_border_preview(painter: QPainter, local_sel: QRect, size: int,
                         color: QColor, corner_radius: int = 0):
    """在选区边缘绘制描边预览（跟随圆角）"""
    pen = QPen(color, size)
    pen.setJoinStyle(Qt.PenJoinStyle.MiterJoin)
    painter.setPen(pen)
    painter.setBrush(Qt.BrushStyle.NoBrush)
    # 偏移半个线宽使描边画在选区外侧
    half = size / 2
    r = QRectF(
        local_sel.x() - half,
        local_sel.y() - half,
        local_sel.width() + size,
        local_sel.height() + size,
    )
    if corner_radius > 0:
        # 描边圆角要比选区圆角大 half（因为描边在外侧）
        cr = corner_radius + half
        painter.drawRoundedRect(r, cr, cr)
    else:
        painter.drawRect(r)


def _draw_shadow_preview(painter: QPainter, local_sel: QRect, size: int,
                         color: QColor, corner_radius: int = 0):
    """在选区外围绘制阴影预览（多层半透明模拟高斯阴影，跟随圆角）"""
    layers = min(size, 20)
    if layers <= 0:
        return

    base_alpha = min(color.alpha(), 120)

    for i in range(layers):
        t = (i + 1) / layers  # 0→1，越外越淡
        alpha = int(base_alpha * (1.0 - t) * 0.6)
        if alpha <= 0:
            continue
        c = QColor(color.red(), color.green(), color.blue(), alpha)
        spread = int(size * t)
        r = QRectF(
            local_sel.x() - spread,
            local_sel.y() - spread,
            local_sel.width() + spread * 2,
            local_sel.height() + spread * 2,
        )
        painter.setPen(QPen(c, 1.5))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        if corner_radius > 0:
            cr = corner_radius + spread
            painter.drawRoundedRect(r, cr, cr)
        else:
            painter.drawRect(r)


# =====================================================================
# 导出后处理辅助函数
# =====================================================================

def _apply_border_export(img: QImage, size: int, color: QColor,
                         corner_radius: int = 0) -> QImage:
    """在导出图像外围添加描边（跟随圆角）"""
    w, h = img.width(), img.height()
    new_w = w + size * 2
    new_h = h + size * 2

    result = QImage(new_w, new_h, QImage.Format.Format_ARGB32_Premultiplied)
    result.fill(0)  # 透明

    painter = QPainter(result)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)

    # 绘制描边
    pen = QPen(color, size)
    pen.setJoinStyle(Qt.PenJoinStyle.MiterJoin)
    painter.setPen(pen)
    painter.setBrush(Qt.BrushStyle.NoBrush)
    half = size / 2
    stroke_rect = QRectF(half, half, w + size, h + size)

    if corner_radius > 0:
        cr = corner_radius + half
        painter.drawRoundedRect(stroke_rect, cr, cr)
    else:
        painter.drawRect(stroke_rect)

    # 绘制原图
    painter.drawImage(size, size, img)
    painter.end()

    log_debug(f"描边导出完成: size={size} corner_r={corner_radius}", "BorderShadow")
    return result


def _apply_shadow_export(img: QImage, size: int, color: QColor,
                         corner_radius: int = 0) -> QImage:
    """在导出图像外围添加阴影（跟随圆角）"""
    w, h = img.width(), img.height()
    margin = size
    new_w = w + margin * 2
    new_h = h + margin * 2

    result = QImage(new_w, new_h, QImage.Format.Format_ARGB32_Premultiplied)
    result.fill(0)  # 透明

    painter = QPainter(result)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)

    # 多层模拟阴影
    layers = min(size, 25)
    base_alpha = min(color.alpha(), 180)

    for i in range(layers):
        t = (i + 1) / layers
        alpha = int(base_alpha * (1.0 - t) * 0.5)
        if alpha <= 0:
            continue
        c = QColor(color.red(), color.green(), color.blue(), alpha)
        spread = int(margin * t)
        r = QRectF(
            margin - spread,
            margin - spread,
            w + spread * 2,
            h + spread * 2,
        )
        painter.setPen(QPen(c, 1.5))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        if corner_radius > 0:
            cr = corner_radius + spread
            painter.drawRoundedRect(r, cr, cr)
        else:
            painter.drawRect(r)

    # 绘制原图
    painter.drawImage(margin, margin, img)
    painter.end()

    log_debug(f"阴影导出完成: size={size} corner_r={corner_radius}", "BorderShadow")
    return result
 