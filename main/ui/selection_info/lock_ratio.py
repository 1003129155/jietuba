# -*- coding: utf-8 -*-
"""
锁定纵横比 —— 独立逻辑模块

功能：
  开启后，用户拖拽 SelectionItem 的手柄调整选区时，
  保持宽高比不变（以拖拽主轴为基准，自动推算另一轴）。

实现方式：
  monkey-patch SelectionItem.mouseMoveEvent，
  在原逻辑计算出 new_rect 后，再根据锁定比例进行矫正。
"""

from __future__ import annotations

from PySide6.QtCore import QRectF, QPointF
from core.logger import log_debug


class LockRatioLogic:
    """锁定纵横比逻辑"""

    def __init__(self, selection_item):
        self._item = selection_item
        self._enabled = False
        self._ratio: float | None = None     # w / h
        self._original_mouse_move = None      # 保存原始 mouseMoveEvent

        # 安装 hook
        self._install_hook()

    # ------------------------------------------------------------------
    # 公共接口
    # ------------------------------------------------------------------
    def set_enabled(self, enabled: bool, current_rect: QRectF | None = None):
        """
        开启/关闭锁定。
        开启时根据当前选区计算 ratio；关闭时清除。
        """
        self._enabled = enabled
        if enabled and current_rect and current_rect.height() > 0:
            self._ratio = current_rect.width() / current_rect.height()
            log_debug(f"锁定纵横比: ON  ratio={self._ratio:.4f}", "LockRatio")
        else:
            self._ratio = None
            if not enabled:
                log_debug("锁定纵横比: OFF", "LockRatio")

    @property
    def enabled(self) -> bool:
        return self._enabled

    @property
    def ratio(self) -> float | None:
        return self._ratio

    # ------------------------------------------------------------------
    # Hook 安装
    # ------------------------------------------------------------------
    def _install_hook(self):
        """monkey-patch SelectionItem.mouseMoveEvent"""
        item = self._item
        self._original_mouse_move = item.mouseMoveEvent

        logic = self  # 闭包引用

        def _hooked_mouse_move(event):
            """替换后的 mouseMoveEvent"""
            if item.active_handle == item.HANDLE_NONE:
                return

            current_pos = event.scenePos()
            dx = current_pos.x() - item.start_pos.x()
            dy = current_pos.y() - item.start_pos.y()

            new_rect = QRectF(item.start_rect)

            if item.active_handle == item.HANDLE_BODY:
                # 移动整体不需要约束
                new_rect.translate(dx, dy)
            else:
                # 先按原始逻辑计算
                if item.active_handle in [item.HANDLE_LEFT, item.HANDLE_TOP_LEFT, item.HANDLE_BOTTOM_LEFT]:
                    new_rect.setLeft(item.start_rect.left() + dx)
                if item.active_handle in [item.HANDLE_RIGHT, item.HANDLE_TOP_RIGHT, item.HANDLE_BOTTOM_RIGHT]:
                    new_rect.setRight(item.start_rect.right() + dx)
                if item.active_handle in [item.HANDLE_TOP, item.HANDLE_TOP_LEFT, item.HANDLE_TOP_RIGHT]:
                    new_rect.setTop(item.start_rect.top() + dy)
                if item.active_handle in [item.HANDLE_BOTTOM, item.HANDLE_BOTTOM_LEFT, item.HANDLE_BOTTOM_RIGHT]:
                    new_rect.setBottom(item.start_rect.bottom() + dy)

                # ── 锁定比例矫正 ──
                if logic._enabled and logic._ratio and logic._ratio > 0:
                    new_rect = logic._constrain(
                        new_rect, item.start_rect, item.active_handle
                    )

            item._model.set_rect(new_rect.normalized())
            event.accept()

        item.mouseMoveEvent = _hooked_mouse_move

    # ------------------------------------------------------------------
    # 比例约束核心
    # ------------------------------------------------------------------
    def _constrain(self, rect: QRectF, start: QRectF, handle: int) -> QRectF:
        """
        根据锁定的 w/h 比例约束矩形。
        策略：以用户拖拽的"主轴"为准，推算另一轴。
        """
        ratio = self._ratio
        item = self._item
        r = QRectF(rect)

        # --- 四角手柄：以宽度为基准，推算高度 ---
        if handle in (item.HANDLE_TOP_LEFT, item.HANDLE_TOP_RIGHT,
                       item.HANDLE_BOTTOM_LEFT, item.HANDLE_BOTTOM_RIGHT):
            w = r.width()
            h = w / ratio if ratio else r.height()

            if handle in (item.HANDLE_TOP_LEFT, item.HANDLE_TOP_RIGHT):
                # 顶部手柄：固定 bottom，调整 top
                r.setTop(r.bottom() - h)
            else:
                # 底部手柄：固定 top，调整 bottom
                r.setBottom(r.top() + h)

        # --- 左/右手柄：改宽度 → 推算高度（中心不变）---
        elif handle in (item.HANDLE_LEFT, item.HANDLE_RIGHT):
            w = r.width()
            h = w / ratio if ratio else r.height()
            cy = r.center().y()
            r.setTop(cy - h / 2)
            r.setBottom(cy + h / 2)

        # --- 上/下手柄：改高度 → 推算宽度（中心不变）---
        elif handle in (item.HANDLE_TOP, item.HANDLE_BOTTOM):
            h = r.height()
            w = h * ratio if ratio else r.width()
            cx = r.center().x()
            r.setLeft(cx - w / 2)
            r.setRight(cx + w / 2)

        return r

    # ------------------------------------------------------------------
    # 卸载（用于销毁时恢复原始方法）
    # ------------------------------------------------------------------
    def uninstall(self):
        if self._original_mouse_move:
            self._item.mouseMoveEvent = self._original_mouse_move
            self._original_mouse_move = None
 