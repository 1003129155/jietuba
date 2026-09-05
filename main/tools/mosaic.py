"""Freehand mosaic tool."""

import math

from PySide6.QtCore import QPointF, QRect, Qt
from PySide6.QtGui import QImage, QPainterPath

from canvas.items import MosaicItem
from canvas.undo import AddItemCommand
from core.logger import log_exception, log_warning
from .base import Tool, ToolContext


class MosaicTool(Tool):
    id = "mosaic"

    def __init__(self):
        self.drawing = False
        self.current_item = None
        self.path = None
        self.full_image = None

    def _reset(self):
        self.drawing = False
        self.current_item = None
        self.path = None
        self.full_image = None

    def _remove_live_item(self, ctx: ToolContext):
        item = self.current_item
        if item is None or item.scene() is not ctx.scene:
            return
        try:
            ctx.scene.removeItem(item)
        except Exception as exc:
            # A cleanup failure must not mask the original tool failure or leave
            # a visible/exportable provisional annotation.
            try:
                item.setVisible(False)
                item.setEnabled(False)
            except Exception:
                pass
            log_exception(exc, "清理马赛克临时图元")

    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button != Qt.MouseButton.LeftButton:
            return False
        self._remove_live_item(ctx)
        self._reset()
        try:
            block_size = 8
            if ctx.settings_manager:
                block_size = ctx.settings_manager.get_setting("mosaic", "block_size", 8)
            full_image = ctx.scene.background.pixelated_image(block_size)
            if full_image.isNull():
                return False

            path = QPainterPath(pos)
            item = MosaicItem(
                path,
                ctx.stroke_width,
                block_size,
                full_image,
                ctx.scene.scene_rect.topLeft(),
            )
            ctx.scene.addItem(item)
            self.full_image = full_image
            self.path = path
            self.current_item = item
            self.drawing = True
            return True
        except Exception as exc:
            self._remove_live_item(ctx)
            self._reset()
            log_exception(exc, "创建马赛克笔画")
            return False

    def on_move(self, pos: QPointF, ctx: ToolContext):
        if not self.drawing or self.current_item is None or self.path is None:
            return
        self.path.lineTo(pos)
        self.current_item.set_path(self.path)

    def _crop_patch(self, ctx: ToolContext):
        if self.current_item is None or self.full_image is None:
            return QImage(), QPointF()
        background_rect = ctx.scene.scene_rect
        crop_scene = self.current_item.boundingRect().intersected(background_rect)
        if crop_scene.isEmpty():
            return QImage(), QPointF()

        left = max(0, int(math.floor(crop_scene.left() - background_rect.left())))
        top = max(0, int(math.floor(crop_scene.top() - background_rect.top())))
        right = min(self.full_image.width(), int(math.ceil(crop_scene.right() - background_rect.left())))
        bottom = min(self.full_image.height(), int(math.ceil(crop_scene.bottom() - background_rect.top())))
        if right <= left or bottom <= top:
            return QImage(), QPointF()
        patch = self.full_image.copy(QRect(left, top, right - left, bottom - top))
        origin = QPointF(background_rect.left() + left, background_rect.top() + top)
        return patch, origin

    def on_release(self, pos: QPointF, ctx: ToolContext):
        if not self.drawing:
            self._reset()
            return
        commit_started = False
        before_count = ctx.undo_stack.count()
        before_index = ctx.undo_stack.index()
        had_redo = ctx.undo_stack.canRedo()
        command = None
        try:
            if self.path is not None and self.current_item is not None:
                self.current_item.set_path(self.path)
            patch, origin = self._crop_patch(ctx)
            if patch.isNull() or self.current_item is None:
                log_warning("马赛克裁剪结果为空，已丢弃临时笔画", "Mosaic")
                self._remove_live_item(ctx)
                return
            item = self.current_item
            item.set_patch(patch, origin)
            # The live item is already in the scene. AddItemCommand.redo() is a
            # no-op in that state, so pushing the command is the single commit
            # boundary and no fallible scene operation follows it.
            command = AddItemCommand(ctx.scene, item, "Add Mosaic")
            commit_started = True
            ctx.undo_stack.push_command(command)
        except Exception as exc:
            committed = (
                commit_started
                and command is not None
                and ctx.undo_stack.count() == (
                    before_index + 1 if had_redo else before_count + 1
                )
                and ctx.undo_stack.index() == before_index + 1
                and ctx.undo_stack.command(before_index) is command
                and self.current_item is not None
                and self.current_item.scene() is ctx.scene
            )
            if not committed:
                self._remove_live_item(ctx)
            log_exception(exc, "完成马赛克笔画")
        finally:
            self._reset()

    def on_deactivate(self, ctx: ToolContext):
        self._remove_live_item(ctx)
        self._reset()
        super().on_deactivate(ctx)
