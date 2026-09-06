"""Freehand mosaic tool."""

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QPainterPath

from canvas.items import MosaicItem
from canvas.undo import AddItemCommand
from core.i18n import make_tr
from core.logger import log_exception
from .base import Tool, ToolContext

_undo_tr = make_tr("UndoCommands")


class MosaicTool(Tool):
    id = "mosaic"

    DEFAULT_BLOCK_SIZE = 8
    MIN_BLOCK_SIZE = 2
    MAX_BLOCK_SIZE = 32

    MODE_FREEHAND = "freehand"
    MODE_RECT = "rect"

    STYLE_PIXELATE = "pixelate"
    STYLE_BLUR = "blur"

    # 框选模式下，拖拽尺寸小于这个值视为误触，不落笔（与荧光笔框选一致）。
    MIN_SIZE = 10

    def __init__(self):
        self.drawing = False
        self.current_item = None
        self.path = None
        self.start_pos = None
        self.draw_mode = self.MODE_FREEHAND
        self.last_point = None  # 上一个被采纳的点（见 Tool.should_append_point）

    def _reset(self):
        self.drawing = False
        self.current_item = None
        self.path = None
        self.start_pos = None
        self.last_point = None

    # 每个设定的缺省值只在这里写一次。工具之外还有别人要读同样的设定
    # （光标预览就得知道现在是框选还是自由涂抹），下面三个读法因此都是公开的
    # classmethod —— 谁都从这里读，就不会出现第二条读取路径和第二份默认值。
    SETTING_DEFAULTS = {
        "block_size": DEFAULT_BLOCK_SIZE,
        "draw_mode": MODE_FREEHAND,
        "style": STYLE_PIXELATE,
    }

    @classmethod
    def _read_setting(cls, ctx, key):
        default = cls.SETTING_DEFAULTS[key]
        manager = getattr(ctx, "settings_manager", None)
        if manager is None:
            return default
        return manager.get_setting(cls.id, key, default)

    @classmethod
    def clamp_block_size(cls, block_size) -> int:
        """把任意来源的粒度收进合法范围（与 Tool.clamp_width 同一个套路）。"""
        try:
            value = int(block_size)
        except (TypeError, ValueError):
            value = cls.DEFAULT_BLOCK_SIZE
        return max(cls.MIN_BLOCK_SIZE, min(cls.MAX_BLOCK_SIZE, value))

    @classmethod
    def get_block_size(cls, ctx) -> int:
        return cls.clamp_block_size(cls._read_setting(ctx, "block_size"))

    # ------------------------------------------------------------------
    # 把设置落到"当前选中的那一块马赛克"上（可撤销）
    # ------------------------------------------------------------------
    # 截图窗口和钉图窗口都要做这件事，而且必须做得一模一样，所以策略只写在
    # 这里一份，两个窗口的槽函数只负责把 view 和 undo_stack 递进来
    # （与 NumberTool.apply_style_change 同一个范式）。

    @classmethod
    def _selected_mosaic(cls, view):
        """当前选中的马赛克图元；没选中或选中的不是马赛克都返回 None。"""
        controller = getattr(view, "smart_edit_controller", None)
        item = getattr(controller, "selected_item", None) if controller else None
        return item if isinstance(item, MosaicItem) else None

    @staticmethod
    def _push_edit(item, old_state, new_state, text, undo_stack):
        if undo_stack is None:
            return
        from canvas.undo import EditItemCommand

        undo_stack.push(EditItemCommand(item, old_state, new_state, text))

    @classmethod
    def apply_style_change(cls, style: str, view, undo_stack) -> bool:
        """把马赛克/模糊落到选中的那一块上。返回是否真的改了。"""
        item = cls._selected_mosaic(view)
        if item is None:
            return False

        smooth = style == cls.STYLE_BLUR
        if smooth == item.smooth():
            return False

        old_state = {"smooth": item.smooth()}
        item.set_smooth(smooth)
        cls._push_edit(
            item, old_state, {"smooth": smooth},
            _undo_tr("Change Mosaic Style"), undo_stack,
        )
        return True

    @classmethod
    def apply_block_size_change(cls, block_size, view, undo_stack) -> bool:
        """把粒度落到选中的那一块上。返回是否真的改了。

        新的缩小图向图元**自己所在**的场景要，而不是让调用方递一个进来：
        钉图和截图各有各的背景，认图元的场景就不会有拿错那张的机会。
        """
        item = cls._selected_mosaic(view)
        if item is None:
            return False

        block_size = cls.clamp_block_size(block_size)
        if block_size == item.block_size():
            return False

        background = getattr(item.scene(), "background", None)
        if background is None:
            return False
        reduced = background.reduced_image(block_size)
        if reduced.isNull():
            return False

        old_state = {
            "block_size": item.block_size(),
            "reduced_image": item.reduced_image(),
        }
        item.set_block_size(block_size, reduced)
        cls._push_edit(
            item, old_state,
            {"block_size": block_size, "reduced_image": reduced},
            _undo_tr("Change Mosaic Size"), undo_stack,
        )
        return True

    @classmethod
    def get_draw_mode(cls, ctx) -> str:
        return cls._read_setting(ctx, "draw_mode")

    @classmethod
    def get_style(cls, ctx) -> str:
        return cls._read_setting(ctx, "style")

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

    def on_activate(self, ctx: ToolContext):
        super().on_activate(ctx)
        # 先把缩小图算出来。它只算一次并缓存在背景图元上，把这几十毫秒花在
        # 点工具栏按钮的那一刻，而不是等用户按下鼠标要开始涂抹时才卡一下。
        try:
            ctx.scene.background.reduced_image(self.get_block_size(ctx))
        except Exception as exc:
            log_exception(exc, "预热马赛克缩小图")

    def on_press(self, pos: QPointF, button, ctx: ToolContext):
        if button != Qt.MouseButton.LeftButton:
            return False
        self._remove_live_item(ctx)
        self._reset()
        self.draw_mode = self.get_draw_mode(ctx)
        try:
            block_size = self.get_block_size(ctx)
            reduced = ctx.scene.background.reduced_image(block_size)
            if reduced.isNull():
                return False

            fill_mode = self.draw_mode == self.MODE_RECT
            if fill_mode:
                path = QPainterPath()
                path.addRect(QRectF(pos, pos))
            else:
                path = QPainterPath(pos)

            # 小图是背景图元缓存的那一份，QImage 隐式共享，每一笔只多一个引用。
            item = MosaicItem(
                path,
                ctx.stroke_width,
                block_size,
                reduced,
                ctx.scene.scene_rect,
                fill_mode=fill_mode,
                smooth=self.get_style(ctx) == self.STYLE_BLUR,
            )
            ctx.scene.addItem(item)
            self.path = path
            self.start_pos = pos
            self.last_point = pos
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
        if self.draw_mode == self.MODE_RECT:
            path = QPainterPath()
            path.addRect(QRectF(self.start_pos, pos).normalized())
            self.path = path
        else:
            # 只有自由涂抹需要按间距筛点：它的路径会一直变长，每次 on_move 都要
            # 重描一遍。框选走上面那条分支，每次只是重建一个矩形，成本与拖了
            # 多久无关。
            if not self.should_append_point(self.last_point, pos):
                return
            self.path.lineTo(pos)
            self.last_point = pos
        self.current_item.set_path(self.path)

    def on_release(self, pos: QPointF, ctx: ToolContext):
        if not self.drawing:
            self._reset()
            return

        if self.draw_mode == self.MODE_RECT:
            rect = QRectF(self.start_pos, pos).normalized()
            if rect.width() < self.MIN_SIZE or rect.height() < self.MIN_SIZE:
                self._remove_live_item(ctx)
                self._reset()
                return

        before_index = ctx.undo_stack.index()
        command = None
        item_to_select = None
        try:
            if self.path is not None and self.current_item is not None:
                self.current_item.set_path(self.path)
            if self.current_item is None:
                return
            # The live item is already in the scene. AddItemCommand.redo() is a
            # no-op in that state, so pushing the command is the single commit
            # boundary and no fallible scene operation follows it.
            command = AddItemCommand(ctx.scene, self.current_item, _undo_tr("Add Mosaic"))
            ctx.undo_stack.push_command(command)
            # 框选马赛克跟荧光笔矩形一样，画完直接自选中方便调整；自由涂抹
            # 沿用原有行为，不自动选中（要编辑得走 Ctrl+点选）。
            if self.draw_mode == self.MODE_RECT:
                item_to_select = self.current_item
        except Exception as exc:
            # 命令要么落在了栈上（此时图元归撤销栈管，不能删），要么没落上。
            committed = (
                command is not None
                and ctx.undo_stack.command(before_index) is command
            )
            if not committed:
                self._remove_live_item(ctx)
            log_exception(exc, "完成马赛克笔画")
        finally:
            self._reset()

        if item_to_select is not None:
            ctx.scene.item_auto_select_requested.emit(item_to_select)

    def on_deactivate(self, ctx: ToolContext):
        self._remove_live_item(ctx)
        self._reset()
        super().on_deactivate(ctx)
