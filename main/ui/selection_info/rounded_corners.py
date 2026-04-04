# -*- coding: utf-8 -*-
"""
圆角截图 —— 独立逻辑模块

功能：
  1. 切换按钮启用后，弹出浮层滑块调节圆角半径
  2. 预览：SelectionItem 边框变圆角 + MaskOverlay 四角补遮罩
  3. 导出：ExportService 裁剪四角为透明

UI 组件：
  RoundedSliderPopup  – 悬浮滑块面板（hover 显示/隐藏）
  RoundedCornersLogic – 业务逻辑控制器
"""

from __future__ import annotations

from PySide6.QtCore import Qt, QRectF, QSize, QTimer, QPoint, Signal, QObject, QEvent
from PySide6.QtGui import (
    QColor, QPainter, QPainterPath, QBrush, QPen, QImage, QCursor,
)
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QSlider, QLabel, QPushButton,
)

from core.logger import log_debug
from core import safe_event


# =====================================================================
# UI: 圆角半径滑块弹出面板
# =====================================================================

class RoundedSliderPopup(QWidget):
    """
    悬浮滑块面板，用于调节圆角半径。

    显示逻辑（由 RoundedCornersLogic 驱动）：
      - 鼠标进入 btn_rounded 或本面板 → show
      - 鼠标离开 btn_rounded 且不在本面板 → 延时隐藏
      - 鼠标离开本面板且不在 btn_rounded → 延时隐藏
    """

    radius_changed = Signal(int)   # 半径值改变

    _BG = QColor(30, 30, 30, 220)
    _RADIUS = 6
    _HEIGHT = 30
    _WIDTH = 180

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.setFixedSize(self._WIDTH, self._HEIGHT)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setMouseTracking(True)
        self._init_ui()
        self.hide()

        # 延时隐藏定时器
        self._hide_timer = QTimer(self)
        self._hide_timer.setSingleShot(True)
        self._hide_timer.setInterval(200)
        self._hide_timer.timeout.connect(self._do_hide)

    def _init_ui(self):
        lay = QHBoxLayout(self)
        lay.setContentsMargins(10, 0, 10, 0)
        lay.setSpacing(6)

        # 滑块
        self._slider = QSlider(Qt.Orientation.Horizontal)
        self._slider.setRange(0, 100)
        self._slider.setValue(16)
        self._slider.setFixedWidth(110)
        self._slider.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        from core.theme import get_theme
        _thx = get_theme().theme_color_hex
        self._slider.setStyleSheet(f"""
            QSlider::groove:horizontal {{
                height: 4px; background: #555; border-radius: 2px;
            }}
            QSlider::handle:horizontal {{
                width: 12px; height: 12px; margin: -4px 0;
                background: {_thx}; border-radius: 6px;
            }}
            QSlider::sub-page:horizontal {{
                background: {_thx}; border-radius: 2px;
            }}
        """)
        lay.addWidget(self._slider)

        # 数值标签
        self._label = QLabel("16")
        self._label.setFixedWidth(30)
        self._label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._label.setStyleSheet(
            "color: #D0D0D0; font-size: 12px; background: transparent;"
        )
        lay.addWidget(self._label)

        # 信号
        self._slider.valueChanged.connect(self._on_value_changed)

    def _on_value_changed(self, value: int):
        self._label.setText(str(value))
        self.radius_changed.emit(value)

    @property
    def value(self) -> int:
        return self._slider.value()

    @value.setter
    def value(self, v: int):
        self._slider.setValue(v)

    # ------------------------------------------------------------------
    # 显隐控制
    # ------------------------------------------------------------------
    def schedule_hide(self):
        """延时隐藏（给鼠标移到面板上留时间）"""
        self._hide_timer.start()

    def cancel_hide(self):
        """取消延时隐藏"""
        self._hide_timer.stop()

    def _do_hide(self):
        # 如果鼠标还在面板内，不隐藏
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

class RoundedCornersLogic(QObject):
    """
    圆角截图的完整逻辑：
      - UI 弹出 / 隐藏
      - SelectionItem 圆角边框预览
      - MaskOverlay 四角补遮罩预览
      - ExportService 导出裁切
    """

    def __init__(self, parent_widget: QWidget, btn_rounded: QPushButton,
                 selection_item, mask_overlay, export_service, selection_model,
                 config_manager=None, hook_manager=None):
        """
        Args:
            parent_widget:   ScreenshotWindow（弹出面板的 parent）
            btn_rounded:     圆角切换按钮（在 SelectionInfoPanel 上）
            selection_item:  SelectionItem
            mask_overlay:    MaskOverlayWidget
            export_service:  ExportService
            selection_model: SelectionModel
            config_manager:  ToolSettingsManager（持久化设置）
            hook_manager:    HookManager（共享的 hook 管理器）
        """
        super().__init__(parent_widget)
        self._parent = parent_widget
        self._btn = btn_rounded
        self._item = selection_item
        self._mask = mask_overlay
        self._export = export_service
        self._model = selection_model
        self._config = config_manager
        self._hook_mgr = hook_manager

        # 从设置中读取上次的状态
        if self._config:
            self._enabled = self._config.get_app_setting("screenshot_rounded_enabled", False)
            self._radius = self._config.get_app_setting("screenshot_rounded_radius", 16)
        else:
            self._enabled = False
            self._radius = 16

        # 创建弹出面板
        self._popup = RoundedSliderPopup(parent_widget)
        self._popup.value = self._radius   # 同步滑块初始值

        # 同步按钮初始选中状态（不触发信号）
        self._btn.blockSignals(True)
        self._btn.setChecked(self._enabled)
        self._btn.blockSignals(False)

        # 连接信号
        self._popup.radius_changed.connect(self._on_radius_changed)

        # 为按钮安装 hover 事件
        self._btn.setMouseTracking(True)
        self._btn.installEventFilter(self)

        # 安装绘制钩子
        self._original_paint = None      # paint 仍用传统方式（只有圆角 patch，无链式问题）
        self._install_paint_hook()
        self._install_mask_hook()        # 通过 HookManager
        self._install_export_hook()      # 通过 HookManager

    # ------------------------------------------------------------------
    # 公共接口
    # ------------------------------------------------------------------
    def set_enabled(self, enabled: bool):
        self._enabled = enabled
        log_debug(f"圆角截图: {'ON' if enabled else 'OFF'}  r={self._radius}",
                  "RoundedCorners")
        # 持久化
        if self._config:
            self._config.set_app_setting("screenshot_rounded_enabled", enabled)

        if enabled:
            # 开启时立即弹出滑块面板（此时鼠标已在按钮上，不会再触发 Enter）
            self._popup.cancel_hide()
            self._popup.show_near(self._btn)
        else:
            self._popup.hide()
        # 刷新预览
        self._item.update()
        self._mask.update()

    @property
    def enabled(self) -> bool:
        return self._enabled

    @property
    def radius(self) -> int:
        return self._radius

    # ------------------------------------------------------------------
    # 按钮 hover → 弹出/隐藏
    # ------------------------------------------------------------------
    @safe_event
    def eventFilter(self, obj, event):
        """拦截圆角按钮的 enter/leave 事件"""
        if obj is self._btn:
            if event.type() == QEvent.Type.Enter:
                if self._enabled:
                    self._popup.cancel_hide()
                    self._popup.show_near(self._btn)
            elif event.type() == QEvent.Type.Leave:
                if self._enabled:
                    self._popup.schedule_hide()
        return False   # 不拦截

    # ------------------------------------------------------------------
    # 半径变化
    # ------------------------------------------------------------------
    def _on_radius_changed(self, value: int):
        self._radius = value
        log_debug(f"圆角半径: {value}", "RoundedCorners")
        # 持久化
        if self._config:
            self._config.set_app_setting("screenshot_rounded_radius", value)
        self._item.update()
        self._mask.update()

    # ------------------------------------------------------------------
    # Hook: SelectionItem.paint() 绘制圆角边框
    # ------------------------------------------------------------------
    def _install_paint_hook(self):
        item = self._item
        self._original_paint = item.paint
        logic = self

        def _hooked_paint(painter: QPainter, option, widget=None):
            if item._model.is_empty():
                return

            rect = item._model.rect()

            if logic._enabled and logic._radius > 0:
                # ── 圆角边框 ──
                r = logic._radius
                from core.theme import get_theme
                pen = QPen(get_theme().theme_color, 4, Qt.PenStyle.SolidLine)
                painter.setPen(pen)
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.setRenderHint(QPainter.RenderHint.Antialiasing)
                painter.drawRoundedRect(rect, r, r)

                # 控制点（圆角模式下隐藏四角手柄）
                if not item._model.is_dragging and item._model.is_confirmed:
                    _draw_handles(painter, rect, item, skip_corners=True)
            else:
                # 原始绘制
                logic._original_paint(painter, option, widget)

        item.paint = _hooked_paint

    # ------------------------------------------------------------------
    # Hook: MaskOverlay.paintEvent() 四角补遮罩（通过 HookManager）
    # ------------------------------------------------------------------
    def _install_mask_hook(self):
        mask = self._mask
        logic = self

        def _rounded_mask_callback(event):
            """圆角遮罩补丁回调（在原始 paintEvent 之后执行）"""
            if not logic._enabled or logic._radius <= 0:
                return
            if logic._model.is_empty():
                return

            sel = logic._model.rect()
            local_sel = mask._scene_to_local(sel)
            r = logic._radius

            if r <= 0 or local_sel.width() < r * 2 or local_sel.height() < r * 2:
                return

            painter = QPainter(mask)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            color = mask._mask_color

            sx, sy = local_sel.x(), local_sel.y()
            sw, sh = local_sel.width(), local_sel.height()

            corners = [
                (sx, sy, r, r, 90, 90),
                (sx + sw - r, sy, r, r, 0, 90),
                (sx, sy + sh - r, r, r, 180, 90),
                (sx + sw - r, sy + sh - r, r, r, 270, 90),
            ]

            for cx, cy, cw, ch, start_angle, span in corners:
                corner_rect = QRectF(cx, cy, cw, ch)
                if start_angle == 90:
                    arc_rect = QRectF(cx, cy, cw * 2, ch * 2)
                elif start_angle == 0:
                    arc_rect = QRectF(cx - cw, cy, cw * 2, ch * 2)
                elif start_angle == 180:
                    arc_rect = QRectF(cx, cy - ch, cw * 2, ch * 2)
                elif start_angle == 270:
                    arc_rect = QRectF(cx - cw, cy - ch, cw * 2, ch * 2)

                path = QPainterPath()
                path.addRect(corner_rect)

                arc_path = QPainterPath()
                arc_path.moveTo(arc_rect.center())
                arc_path.arcTo(arc_rect, start_angle, span)
                arc_path.closeSubpath()

                mask_path = path - arc_path
                painter.setPen(Qt.PenStyle.NoPen)
                painter.setBrush(color)
                painter.drawPath(mask_path)

            painter.end()

        self._mask_callback = _rounded_mask_callback
        self._hook_mgr.register(mask, 'paintEvent', _rounded_mask_callback,
                                wrap_mode="after")

    # ------------------------------------------------------------------
    # Hook: ExportService.export() 导出时裁剪圆角（通过 HookManager）
    # ------------------------------------------------------------------
    def _install_export_hook(self):
        logic = self

        def _rounded_export_callback(img, selection_rect):
            """圆角导出回调（chain 模式：接收上游返回的 img，处理后传给下游）"""
            if not logic._enabled or logic._radius <= 0:
                return img
            if img.isNull():
                return img

            r = logic._radius
            w, h = img.width(), img.height()
            r = min(r, w // 2, h // 2)
            if r <= 0:
                return img

            result = QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
            result.fill(0)

            painter = QPainter(result)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            clip = QPainterPath()
            clip.addRoundedRect(QRectF(0, 0, w, h), r, r)
            painter.setClipPath(clip)
            painter.drawImage(0, 0, img)
            painter.end()

            log_debug(f"圆角裁剪完成: r={r}", "RoundedCorners")
            return result

        self._export_callback = _rounded_export_callback
        self._hook_mgr.register(self._export, 'export', _rounded_export_callback,
                                wrap_mode="chain")

    # ------------------------------------------------------------------
    # 卸载
    # ------------------------------------------------------------------
    def uninstall(self):
        # paint hook（传统方式，只有圆角用）
        if self._original_paint:
            self._item.paint = self._original_paint
        # mask / export hook（通过 HookManager）
        if self._hook_mgr:
            self._hook_mgr.unregister(self._mask, 'paintEvent', self._mask_callback)
            self._hook_mgr.unregister(self._export, 'export', self._export_callback)
        self._popup.hide()
        self._popup.deleteLater()


# =====================================================================
# 辅助绘制函数（从 SelectionItem.paint 中提取，供 hook 复用）
# =====================================================================

def _draw_handles(painter: QPainter, rect: QRectF, item, skip_corners: bool = False):
    """绘制控制点。skip_corners=True 时跳过四角手柄（圆角模式）"""
    _CORNER_HANDLES = {
        item.HANDLE_TOP_LEFT,
        item.HANDLE_TOP_RIGHT,
        item.HANDLE_BOTTOM_LEFT,
        item.HANDLE_BOTTOM_RIGHT,
    }
    handles = item._get_handle_positions(rect)
    for handle_id, pos in handles.items():
        if skip_corners and handle_id in _CORNER_HANDLES:
            continue
        painter.setPen(QPen(QColor(255, 255, 255), 2))
        painter.setBrush(QColor(48, 200, 192))
        painter.drawEllipse(pos, item.HANDLE_SIZE // 2 + 1, item.HANDLE_SIZE // 2 + 1)

        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(48, 200, 192))
        painter.drawEllipse(pos, item.HANDLE_SIZE // 2, item.HANDLE_SIZE // 2)
 