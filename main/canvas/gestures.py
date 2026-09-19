"""由 View 自己接管的鼠标手势。

这些手势的状态原先是 CanvasView 上十个零散的字段（_text_drag_active、
_manual_item_drag_last_scene_pos、_pending_text_edit_moved……），跨十来个方法读写。
光看 view 上那一排字段，看不出谁和谁是一伙的，更看不出一次手势该怎么开始、怎么收尾。
收进来之后，每种手势的几个阶段在同一个类里排成一列。

- TextEdgeDrag：正在编辑的文字，内部是输入光标，只有边缘那一圈能拖着走整段；
- PendingTextEdit：单击已选中的文字进入编辑，按下先挂起、松手才算数；
- ManualItemDrag：控制器有意越过顶层图元往下选中当前工具兼容的目标时（拿矩形工具
  点一段压在矩形边框上的文字就是如此），Qt 会把 move 派给顶层那个，所以这次手势
  必须由 View 全程拥有。

不是所有拖动都归这里——Qt 自己的 ItemIsMovable 能处理的仍然交给 Qt（图元照样会被
scene 认成 mouse grabber），这里只收它做不到的那些。

两种拖动都得自己把撤销那一份接过来：进入时抓一次快照，松手时交给
SmartEditController._finalize_move_edit 比较前后状态、推一条 EditItemCommand。
快照只在进入时抓一次，所以拖多远都还是一条；原地按一下又放开则一条都不推。历史上
TextEdgeDrag 漏过这一步——位置变了、撤销栈里却什么都没有，见 test_undo_granularity.py。
"""
from __future__ import annotations

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtWidgets import QGraphicsTextItem


class SelectionDrag:
    """拉出截图选区——选区确认之前的那一段。

    按下就进入，但要挪过一道距离阈值才算"在拉框"：不过这一关的话，随手点一下会把
    选区缩成一个点。松手即确认，之后整块画布转入编辑。
    """

    # 按下之后挪过这么多像素才算在拉框，而不是单纯点了一下
    DRAG_THRESHOLD = 10

    def __init__(self, view):
        self._view = view
        self.active = False
        self.dragging = False
        self.start_pos = QPointF()

    def begin(self, scene_pos: QPointF):
        view = self._view
        self.active = True
        self.dragging = False
        self.start_pos = scene_pos
        model = view.canvas_scene.selection_model
        model.activate()
        # 开始拖拽，隐藏控制点（降低渲染压力）
        model.start_dragging()
        # 智能选区：点击时立即更新选区（防止 activate 清除选区）。
        # 跳过补间——按下这一刻选区必须是真实窗口矩形，否则截到的是插值中间态。
        if view.smart_selection_enabled:
            view._apply_smart_selection_rect(
                view._get_smart_selection_rect(scene_pos), animate=False
            )

    def perform(self, scene_pos: QPointF):
        view = self._view
        view._update_magnifier_overlay(scene_pos)
        if not self.dragging:
            if (scene_pos - self.start_pos).manhattanLength() > self.DRAG_THRESHOLD:
                self.dragging = True
        if self.dragging:
            rect = QRectF(self.start_pos, scene_pos).normalized()
            view.canvas_scene.selection_model.set_rect(rect)

    def end(self):
        view = self._view
        self.active = False
        self.dragging = False
        # 结束拖拽，显示控制点
        view.canvas_scene.selection_model.stop_dragging()
        view.canvas_scene.confirm_selection()


class DrawingStroke:
    """用当前工具画一笔：按下起头、移动喂点、松手收尾。

    这一笔画成什么、要不要入撤销栈，全归工具自己（tools/ 下那些 Tool 子类）；这里
    只管"现在是不是正画着"，以及把三个阶段转给工具。
    """

    def __init__(self, view):
        self._view = view
        self.active = False

    def begin(self, scene_pos: QPointF, button):
        view = self._view
        self.active = True
        # 立即隐藏放大镜，避免 hide() 和首帧绘图重绘叠加导致卡顿
        view._clear_magnifier_overlay()
        started = view.canvas_scene.tool_controller.on_press(scene_pos, button)
        # 工具可以拒绝起笔（比如序号工具够不到位置），那就当这一下没画
        if started is False:
            self.active = False

    def perform(self, scene_pos: QPointF):
        """绘图中放大镜已在按下时隐藏，这里不必每帧再判断一次。"""
        view = self._view
        view.canvas_scene.tool_controller.on_move(scene_pos)
        view._apply_tool_cursor()

    def end(self, scene_pos: QPointF):
        view = self._view
        self.active = False
        view.canvas_scene.tool_controller.on_release(scene_pos)
        # 绘图结束，恢复放大镜跟踪（如果此时 _should_render 允许显示）
        view._update_magnifier_overlay(scene_pos)


class TextEdgeDrag:
    """编辑中的文字，抓边缘拖着走。"""

    def __init__(self, view):
        self._view = view
        self.active = False
        self.item = None
        self.hover_item = None
        self._last_scene_pos = None
        self._cursor_active = False

    @staticmethod
    def is_point_on_edge(item: QGraphicsTextItem, scene_pos: QPointF, margin: float = None) -> bool:
        """这个点落在文字框的边缘一圈上吗——那里才是拖动区，里面归输入光标管。"""
        if not item:
            return False
        # 使用 TextItem 的 document margin 作为边缘判定区域
        if margin is None:
            margin = getattr(item, 'TEXT_PADDING', 12)
        rect = item.mapToScene(item.boundingRect()).boundingRect()
        if not rect.contains(scene_pos):
            return False
        inner = rect.adjusted(margin, margin, -margin, -margin)
        if inner.width() <= 0 or inner.height() <= 0:
            return True
        return not inner.contains(scene_pos)

    def set_cursor(self, active: bool):
        view = self._view
        if active:
            self._cursor_active = True
            view.setCursor(Qt.CursorShape.SizeAllCursor)
            return
        if not self._cursor_active:
            return
        self._cursor_active = False
        if view._is_text_editing():
            view.viewport().unsetCursor()
        elif (
            view.cursor_manager
            and view.cursor_manager.current_cursor
            and view.cursor_manager.current_tool_id != "cursor"
        ):
            view.setCursor(view.cursor_manager.current_cursor)
        else:
            view.setCursor(Qt.CursorShape.ArrowCursor)

    def update_hover(self, scene_pos: QPointF):
        """鼠标在编辑中的文字边缘上经过时，把光标换成四向箭头。"""
        view = self._view
        if self.active:
            return
        if not view._is_text_editing():
            if self.hover_item is not None:
                self.hover_item = None
                self.set_cursor(False)
            return
        item = view._get_active_text_item()
        if item and self.is_point_on_edge(item, scene_pos):
            self.hover_item = item
            self.set_cursor(True)
        else:
            self.hover_item = None
            self.set_cursor(False)

    def begin(self, item: QGraphicsTextItem, scene_pos: QPointF):
        """接管这次手势，同时抓一份进入拖动前的状态留给撤销。"""
        view = self._view
        view.pending_text_edit.clear()
        self.active = True
        self.item = item
        self._last_scene_pos = scene_pos
        self.set_cursor(True)
        controller = view.smart_edit_controller
        if controller:
            controller.select_item(item, auto_select=False)
            # 快照只在这里抓一次，拖动中间移动多少下都还是一条撤销记录
            controller._move_initial_state = controller._capture_layer_state(item)

    def perform(self, scene_pos: QPointF):
        if not self.active or not self.item:
            return
        if not self._last_scene_pos:
            self._last_scene_pos = scene_pos
            return
        delta = scene_pos - self._last_scene_pos
        if abs(delta.x()) < 1e-3 and abs(delta.y()) < 1e-3:
            return
        self.item.moveBy(delta.x(), delta.y())
        self._last_scene_pos = scene_pos

    def end(self):
        """松手：先结算撤销记录，再清状态。

        _finalize_move_edit 自己会比较前后状态，原地按一下再放开不会留下空记录。
        """
        controller = self._view.smart_edit_controller
        if self.active and controller is not None and controller.selected_item is self.item:
            controller._finalize_move_edit()
        self._clear()

    def reset(self):
        """取消这次手势（编辑结束、场景清理），不结算撤销。"""
        self.hover_item = None
        self._clear()

    def _clear(self):
        self.active = False
        self.item = None
        self._last_scene_pos = None
        self.set_cursor(False)


class PendingTextEdit:
    """单击已选中的文字进入编辑——按下时先挂起，松手时才算数。

    不能在按下那一刻就进编辑：同样这一下也可能是拖动的开头。所以按下只记住"点的是
    哪一段、从哪儿按下的"，中途移动超过容差就作废，松手时手指还在原地才真的进去。
    """

    # 按下到松开之间挪过这么多像素，就当成拖动而不是单击
    MOVE_TOLERANCE = 5

    def __init__(self, view):
        self._view = view
        self.item = None
        self._press_pos = None
        self._moved = False

    def arm(self, event, scene_pos: QPointF):
        view = self._view
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if view._is_text_editing():
            return
        item = getattr(view.smart_edit_controller, "selected_item", None)
        if not isinstance(item, QGraphicsTextItem):
            return
        # 只在点击位置仍在文字上时才进入待编辑状态
        if not item.contains(item.mapFromScene(scene_pos)):
            return
        self.item = item
        self._press_pos = event.pos()
        self._moved = False

    def track(self, event):
        if self.item is None or self._press_pos is None:
            return
        if not (event.buttons() & Qt.MouseButton.LeftButton):
            return
        if (event.pos() - self._press_pos).manhattanLength() > self.MOVE_TOLERANCE:
            self._moved = True

    def settle(self, event, scene_pos: QPointF):
        """松手：这一下还算单击、且仍落在那段文字上，才进编辑。"""
        item = self.item
        if item is None:
            return
        should_enter = (
            event.button() == Qt.MouseButton.LeftButton
            and not self._moved
            and isinstance(item, QGraphicsTextItem)
            and item.contains(item.mapFromScene(scene_pos))
        )
        self.clear()
        if should_enter:
            self._view._enter_text_edit_mode(item)

    def clear(self):
        self.item = None
        self._press_pos = None
        self._moved = False


class ManualItemDrag:
    """View 全程接管的图元拖动（穿透命中时用）。"""

    def __init__(self, view):
        self._view = view
        self.active = False
        self._last_scene_pos = None

    def begin(self, scene_pos: QPointF):
        self.active = True
        self._last_scene_pos = QPointF(scene_pos)

    def perform(self, event, scene_pos: QPointF):
        """跟着鼠标挪选中的那个图元。

        位移按"上一次落点到这一次"算，而不是按手势起点算：起点到指针之间还隔着
        一个拖动阈值，按起点算会在越过阈值的那一帧突然跳一下。
        """
        view = self._view
        controller = view.smart_edit_controller
        selected_item = controller.selected_item
        controller.handle_move(event.pos(), scene_pos)
        if controller.is_dragging and selected_item is not None:
            last_pos = self._last_scene_pos or scene_pos
            delta = scene_pos - last_pos
            if not delta.isNull():
                selected_item.moveBy(delta.x(), delta.y())
            self._last_scene_pos = QPointF(scene_pos)
            view._update_edit_handles()
        view.setCursor(Qt.CursorShape.SizeAllCursor)

    def finish(self, *, commit: bool):
        """结束或取消这次手势，把 View 和控制器两边的状态一起清干净。"""
        controller = getattr(self._view, "smart_edit_controller", None)
        if not self.active:
            if controller is not None:
                controller.press_requires_manual_dispatch = False
            return

        if controller is not None:
            if commit and controller.is_dragging and controller.selected_item is not None:
                controller._finalize_move_edit()
            controller.is_dragging = False
            controller.drag_start_pos = None
            controller._move_initial_state = None
            controller.press_requires_manual_dispatch = False
            mode_type = type(controller.mode)
            if controller.mode == mode_type.DRAGGING_MOVE:
                controller.mode = mode_type.SELECTED
            editor = getattr(controller, "layer_editor", None)
            if editor is not None:
                editor.is_moving_item = False

        self.active = False
        self._last_scene_pos = None
