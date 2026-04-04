# -*- coding: utf-8 -*-
"""
选区信息面板 —— 逻辑控制器

职责：
  1. 监听 SelectionModel 信号，驱动 panel 跟随 / 显示隐藏
  2. 将按钮操作委托给独立逻辑模块：
     - 锁定纵横比 → lock_ratio.LockRatioLogic
     - 圆角截图   → rounded_corners.RoundedCornersLogic
     - 描边/阴影  → border_shadow.BorderShadowLogic
"""

from __future__ import annotations

from PySide6.QtCore import QRectF, QTimer
from core import log_debug
from capture.capture_service import CaptureService

from .hook_manager import HookManager
from .lock_ratio import LockRatioLogic
from .rounded_corners import RoundedCornersLogic
from .border_shadow import BorderShadowLogic


class SelectionInfoController:
    """选区信息面板的逻辑控制器"""

    def __init__(self, panel, selection_model, selection_item,
                 export_service, mask_overlay, parent_widget,
                 config_manager=None):
        """
        Args:
            panel:           SelectionInfoPanel  (UI)
            selection_model:  SelectionModel
            selection_item:   SelectionItem       (手柄 + 拖拽约束)
            export_service:   ExportService       (导出后处理)
            mask_overlay:     MaskOverlayWidget   (圆角预览)
            parent_widget:    ScreenshotWindow    (弹出面板的父窗口)
            config_manager:   ToolSettingsManager (持久化设置)
        """
        self.panel = panel
        self.model = selection_model
        self.selection_item = selection_item
        self.export_service = export_service
        self.mask_overlay = mask_overlay
        self._config = config_manager

        # ── 共享 Hook 管理器（解决链式 monkey-patch 卸载顺序问题）──
        self._hook_mgr = HookManager()

        # ── 子模块 ──
        self._lock_ratio = LockRatioLogic(selection_item)
        self._rounded_corners = RoundedCornersLogic(
            parent_widget=parent_widget,
            btn_rounded=panel.btn_rounded,
            selection_item=selection_item,
            mask_overlay=mask_overlay,
            export_service=export_service,
            selection_model=selection_model,
            config_manager=config_manager,
            hook_manager=self._hook_mgr,
        )
        self._border_shadow = BorderShadowLogic(
            parent_widget=parent_widget,
            btn_border=panel.btn_border,
            selection_item=selection_item,
            mask_overlay=mask_overlay,
            export_service=export_service,
            selection_model=selection_model,
            config_manager=config_manager,
            rounded_corners_logic=self._rounded_corners,
            hook_manager=self._hook_mgr,
        )

        # ── 刷新背景 ──
        self._parent_window = parent_widget      # ScreenshotWindow
        self._refresh_timer = QTimer()
        self._refresh_timer.setInterval(16)       # ~60fps (16ms)
        self._refresh_timer.timeout.connect(self._do_refresh_background)
        self._long_press_timer = QTimer()
        self._long_press_timer.setSingleShot(True)
        self._long_press_timer.setInterval(300)   # 按住 300ms 进入连续模式
        self._long_press_timer.timeout.connect(self._start_continuous_refresh)
        self._refresh_pending = False             # 单击标记

        self._connect()

    # ------------------------------------------------------------------
    # 信号连接
    # ------------------------------------------------------------------
    def _connect(self):
        # model → 面板跟随
        self.model.rectChanged.connect(self._on_rect_changed)
        self.model.draggingChanged.connect(self._on_dragging_changed)
        self.model.confirmed.connect(self._on_confirmed)

        # panel 按钮 → 子模块
        self.panel.border_toggled.connect(self._on_border_toggled)
        self.panel.rounded_toggled.connect(self._on_rounded_toggled)
        self.panel.lock_ratio_toggled.connect(self._on_lock_ratio_toggled)

        # 刷新背景
        self.panel.refresh_pressed.connect(self._on_refresh_pressed)
        self.panel.refresh_released.connect(self._on_refresh_released)

    # ------------------------------------------------------------------
    # 选区信号回调
    # ------------------------------------------------------------------
    def _on_rect_changed(self, rect: QRectF):
        if rect.isEmpty():
            self.panel.hide()
            return
        self.panel.update_info_text(rect)
        self.panel.follow_rect(rect)
        # 未确认阶段（智能选区悬停）：只显示坐标文字，隐藏功能按钮
        if not self.model.is_confirmed:
            self.panel.set_confirmed(False)
        if not self.panel.isVisible() and not self.model.is_dragging:
            self.panel.show()
            self.panel.raise_()

    def _on_dragging_changed(self, is_dragging: bool):
        if is_dragging:
            # 根据设置决定是否在拖拽时隐藏面板
            hide_on_drag = self._config.get_app_setting(
                "screenshot_info_hide_on_drag", False
            )
            if hide_on_drag:
                self.panel.hide()
            else:
                # 不隐藏，但跟随选区更新位置
                if not self.model.rect().isEmpty():
                    self.panel.update_info_text(self.model.rect())
                    self.panel.follow_rect(self.model.rect())
        else:
            # 拖拽结束，恢复显示
            if not self.model.rect().isEmpty():
                self.panel.update_info_text(self.model.rect())
                self.panel.follow_rect(self.model.rect())
                self.panel.show()
                self.panel.raise_()

    def _on_confirmed(self, rect: QRectF):
        # 确认后显示面板及所有功能按钮
        if not rect.isEmpty():
            self.panel.update_info_text(rect)
            self.panel.follow_rect(rect)
            self.panel.set_confirmed(True)
            self.panel.show()
            self.panel.raise_()

    # ------------------------------------------------------------------
    # 按钮回调 → 委托给子模块
    # ------------------------------------------------------------------
    def _on_border_toggled(self, checked: bool):
        """描边/阴影开关 → BorderShadowLogic"""
        self._border_shadow.set_enabled(checked)

    def _on_rounded_toggled(self, checked: bool):
        """圆角截图开关 → RoundedCornersLogic"""
        self._rounded_corners.set_enabled(checked)

    def _on_lock_ratio_toggled(self, checked: bool):
        """锁定纵横比开关 → LockRatioLogic"""
        rect = self.model.rect() if checked else None
        self._lock_ratio.set_enabled(checked, rect)

    # ------------------------------------------------------------------
    # 刷新背景回调
    # ------------------------------------------------------------------
    def _on_refresh_pressed(self):
        """鼠标按下刷新按钮：立即刷新一次，同时启动长按检测"""
        self._refresh_pending = True
        self._do_refresh_background()          # 立即刷新一帧
        self._long_press_timer.start()          # 开始检测长按

    def _on_refresh_released(self):
        """鼠标释放刷新按钮：停止所有定时器"""
        self._long_press_timer.stop()
        self._refresh_timer.stop()
        self._refresh_pending = False

    def _start_continuous_refresh(self):
        """长按 300ms 后进入连续刷新模式 (~60fps)"""
        self._refresh_pending = False
        self._refresh_timer.start()

    def _do_refresh_background(self):
        """执行一次背景刷新：首次调用时设置隐身，然后截屏更新背景"""
        try:
            win = self._parent_window
            # 窗口已关闭或正在关闭时不再刷新
            if win is None or getattr(win, '_is_closing', False):
                self._refresh_timer.stop()
                self._long_press_timer.stop()
                return
            # 首次刷新时让截图窗口对截屏 API 不可见，之后保持
            if not getattr(win, '_exclude_from_capture_set', False):
                win._set_exclude_from_capture(True)
                win._exclude_from_capture_set = True
            new_image, _ = CaptureService().capture_all_screens()
            win.original_image = new_image
            win.scene.background.update_image(new_image)
        except Exception as e:
            log_debug(f"刷新背景失败: {e}")

    # ------------------------------------------------------------------
    # 清理
    # ------------------------------------------------------------------
    def cleanup(self):
        """卸载所有 hook，停止定时器，恢复原始行为"""
        # 停止定时器（必须最先，防止回调访问已销毁对象）
        self._long_press_timer.stop()
        self._refresh_timer.stop()

        # 断开信号（防止延迟排队的信号触发已销毁对象的回调）
        from core.qt_utils import safe_disconnect
        safe_disconnect(self.model.rectChanged, self._on_rect_changed)
        safe_disconnect(self.model.draggingChanged, self._on_dragging_changed)
        safe_disconnect(self.model.confirmed, self._on_confirmed)

        # 卸载子模块 hook
        self._lock_ratio.uninstall()
        self._rounded_corners.uninstall()
        self._border_shadow.uninstall()

        # 兜底：确保所有 hook 都被恢复（即使子模块漏掉）
        self._hook_mgr.unregister_all()

        # 置空引用，帮助 GC
        self._parent_window = None
 