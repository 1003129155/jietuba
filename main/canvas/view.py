"""
画布视图 - 处理用户交互
"""

from typing import Optional

from PySide6.QtWidgets import QApplication, QGraphicsView, QGraphicsTextItem
from PySide6.QtCore import Qt, QPointF, QRectF, QTimer
from PySide6.QtGui import QPen, QColor, QBrush, QCursor
import shiboken6
from canvas.items import (
    StrokeItem,
    RectItem,
    EllipseItem,
    ArrowItem,
    TextItem,
    NumberItem,
)
from core import log_debug, log_info, log_warning, log_error, safe_event
from core.logger import T


def _rect_area(rect) -> int:
    """[x1, y1, x2, y2] 形式矩形的面积，用于比较嵌套链上相邻两级的大小。"""
    return (rect[2] - rect[0]) * (rect[3] - rect[1])


# 两级之间每条边都不超过这个差距就算同一级。高分屏上一个逻辑像素的边框是两个
# 物理像素，取 1 会漏掉最常见的那种父子差。
LEVEL_TOLERANCE_PX = 2


def _append_level(chain: list, rect: list):
    """把一级追加到由细到粗的嵌套链末尾，和上一级没有实质区别的并掉。

    两种情形都会让滚轮空转一格，所以用同一条规则挡掉：嵌套树里父子常因为
    border、padding 报出只差一两个像素的矩形；而横跨窗口或屏幕边界的矩形裁剪
    之后可能比里面一级还小，那它就不是"更外面的一级"。
    """
    if not chain:
        chain.append(rect)
        return
    previous = chain[-1]
    if _rect_area(rect) < _rect_area(previous):
        return
    if all(abs(rect[i] - previous[i]) <= LEVEL_TOLERANCE_PX for i in range(4)):
        return
    chain.append(rect)


class CanvasView(QGraphicsView):
    """
    画布视图
    """
    
    def __init__(
        self,
        scene,
        parent=None,
        confirm_on_double_click=False,
        cross_tool_select=False,
    ):
        super().__init__(scene, parent)
        
        self.canvas_scene = scene
        self._is_closed = False
        self.confirm_on_double_click = bool(confirm_on_double_click)
        self._double_click_candidate = None
        
        # 设置渲染选项 - 关闭抗锯齿以提高性能
        # self.setRenderHint(QPainter.RenderHint.Antialiasing)  # 关闭抗锯齿
        # self.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)  # 关闭平滑变换
        
        # 使用智能视口更新模式
        self.setViewportUpdateMode(QGraphicsView.ViewportUpdateMode.SmartViewportUpdate)
        
        # 禁用滚动条
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        
        # 左上角对齐：禁止 QGraphicsView 在 resizeEvent 时自动居中场景，
        # 防止内部 scroll offset 偶发偏移导致 mapFromScene 结果不稳定
        self.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
        
        # 移除边框，确保视图完全填充窗口
        self.setFrameShape(QGraphicsView.Shape.NoFrame)
        self.setContentsMargins(0, 0, 0, 0)
        self.setViewportMargins(0, 0, 0, 0)

        
        # 重要：设置视图变换，确保场景坐标和窗口坐标 1:1 对应
        # 场景使用全局屏幕坐标（可能不是从 0,0 开始），需要将场景原点映射到视图原点
        self.resetTransform()  # 重置变换
        # 将场景的 topLeft (可能是负数或正数) 映射到视图的 (0,0)
        scene_rect = scene.sceneRect()
        self.translate(-scene_rect.x(), -scene_rect.y())
        
        # 禁用视图自动改变光标（避免与 CursorManager 冲突）
        self.viewport().setMouseTracking(True)
        
        # 交互状态
        
        # 启用鼠标追踪以支持悬停检测
        self.setMouseTracking(True)
        
        
        # 智能选区相关
        self.smart_selection_enabled = False
        self.smart_selection_drills_to_elements = False  # 是否在窗口之下再取控件
        self.smart_selection_animated = False  # 选区跳转时补间，由设置开启
        self.window_finder = None  # WindowFinder 实例（按需创建）
        self.element_finder = None  # UIAElementFinder（按需创建）
        self._smart_selection_level = 0  # 滚轮选的粒度：0 是最细一级，往上是包围它的矩形
        self._smart_selection_level_anchor = None  # 档位锚定在哪个最细矩形上
        from canvas.smart_selection_anim import SmartSelectionAnimator
        self._smart_selection_anim = SmartSelectionAnimator(self._set_selection_rect_now, self)
        
        # 初始化光标管理器
        from tools.cursor_manager import CursorManager
        self.cursor_manager = CursorManager(self)
        
        # 初始化智能编辑控制器
        from canvas.smart_edit_controller import SmartEditController
        self.smart_edit_controller = SmartEditController(self.canvas_scene)
        self.smart_edit_controller.cross_tool_select_enabled = bool(cross_tool_select)
        
        # 连接 Scene 的解耦信号（替代 scene.view = self 的循环引用）
        self.canvas_scene.cursor_color_update_requested.connect(
            self.cursor_manager.update_tool_cursor_color
        )
        self.canvas_scene.cursor_opacity_update_requested.connect(
            self.cursor_manager.update_tool_cursor_opacity
        )
        self.canvas_scene.cursor_tool_update_requested.connect(
            self._on_cursor_tool_update_requested
        )
        self.canvas_scene.item_auto_select_requested.connect(
            self._on_item_auto_select_requested
        )
        self.canvas_scene.editing_cleanup_requested.connect(self._on_editing_cleanup)
        
        # 连接智能编辑控制器的信号
        self.smart_edit_controller.cursor_change_request.connect(self._on_edit_cursor_change)
        self.smart_edit_controller.selection_changed.connect(self._on_edit_selection_changed)
        self.smart_edit_controller.tool_switch_requested.connect(self._on_cross_tool_switch_requested)
        
        # 监听工具切换，同步到智能编辑控制器
        self.canvas_scene.tool_controller.add_tool_changed_callback(self._on_tool_changed_for_edit)
        
        # 同步当前工具光标
        current_tool = self.canvas_scene.tool_controller.current_tool
        if current_tool:
            self.cursor_manager.set_tool_cursor(current_tool.id)
            self.smart_edit_controller.set_tool(current_tool.id)
        
        # Pending 单击文字进入编辑的状态

        # 两种由 View 全程接管的手势，各自管着自己那一摊状态（见 canvas/gestures.py）
        from canvas.gestures import (
            DrawingStroke, ManualItemDrag, PendingTextEdit, SelectionDrag, TextEdgeDrag,
        )
        self.drawing = DrawingStroke(self)
        self.selection_drag = SelectionDrag(self)
        self.text_drag = TextEdgeDrag(self)
        self.item_drag = ManualItemDrag(self)
        self.pending_text_edit = PendingTextEdit(self)

        # 控制点画在 viewport 之上的独立浮层里，不进 QGraphicsScene 的渲染管线。
        # 这样内容层的脏区只需要描述内容，不必再为"手柄能凸出多远"外扩。
        from canvas.handle_overlay import HandleOverlayWidget
        self._handle_overlay = HandleOverlayWidget(self)
        self.smart_edit_controller.layer_editor.repaint_requested = (
            self.request_handles_repaint
        )

        # 手柄位置是图元几何的派生量，所以直接跟着场景变化走，而不是依赖每条
        # 改动路径记得通知。
        #
        # 靠调用方通知过一次，漏了：图元在编辑态被 Qt 原生拖走时
        # （_handle_selected_item_drag 里"文字编辑中拖拽=选文字"那条分支把手势
        # 交给 super().mouseMoveEvent 后直接 return），既没刷浮层也没更新场景，
        # 旧手柄像素没人覆盖 —— 就是旋转后平移留下的一串手柄残影。
        # 挂在 changed 上之后，任何路径（含以后新加的）改了几何都会刷到。
        self.canvas_scene.changed.connect(self._on_scene_changed_for_handles)

    def request_handles_repaint(self):
        """重绘手柄浮层。整层重画，所以不存在算漏脏区留残影的可能。"""
        overlay = getattr(self, "_handle_overlay", None)
        if overlay is not None and shiboken6.isValid(overlay):
            overlay.refresh()

    def _on_scene_changed_for_handles(self, _region=None):
        """场景一变就把手柄重新贴到图元当前几何上。

        非编辑态时 _update_edit_handles 立刻返回，所以画笔涂抹这类高频变化
        在这里几乎没有开销。
        """
        if self._is_closed:
            return
        self._update_edit_handles()

    @safe_event
    def resizeEvent(self, event):
        # 浮层铺满 viewport，尺寸得跟着走（refresh 里做）。视图变换和滚动
        # 会整块重绘 viewport，压在上面的浮层被一并重绘，不需要额外通知。
        super().resizeEvent(event)
        self.request_handles_repaint()

    def setCursor(self, cursor):
        """同时更新视图和 viewport，避免 Qt 只在父部件上应用光标"""
        super().setCursor(cursor)
        viewport = self.viewport()
        if viewport is not None:
            viewport.setCursor(cursor)

    def cleanup(self):
        """断开会话级信号和引用，避免旧 view 在销毁期收到晚到回调。"""
        if self._is_closed:
            return
        self.item_drag.finish(commit=True)
        self._is_closed = True

        from core.qt_utils import safe_disconnect

        scene = getattr(self, "canvas_scene", None)
        cursor_manager = getattr(self, "cursor_manager", None)
        controller = getattr(self, "smart_edit_controller", None)

        if scene is not None:
            if cursor_manager is not None:
                safe_disconnect(
                    scene.cursor_color_update_requested,
                    cursor_manager.update_tool_cursor_color,
                )
                safe_disconnect(
                    scene.cursor_opacity_update_requested,
                    cursor_manager.update_tool_cursor_opacity,
                )
            safe_disconnect(scene.cursor_tool_update_requested, self._on_cursor_tool_update_requested)
            safe_disconnect(scene.item_auto_select_requested, self._on_item_auto_select_requested)
            safe_disconnect(scene.editing_cleanup_requested, self._on_editing_cleanup)
            safe_disconnect(scene.changed, self._on_scene_changed_for_handles)

            tool_controller = getattr(scene, "tool_controller", None)
            if tool_controller is not None and hasattr(tool_controller, "remove_tool_changed_callback"):
                tool_controller.remove_tool_changed_callback(self._on_tool_changed_for_edit)

        layer_editor = getattr(controller, "layer_editor", None)
        if layer_editor is not None:
            layer_editor.repaint_requested = None

        if controller is not None:
            safe_disconnect(controller.cursor_change_request, self._on_edit_cursor_change)
            safe_disconnect(controller.selection_changed, self._on_edit_selection_changed)
            safe_disconnect(controller.tool_switch_requested, self._on_cross_tool_switch_requested)
            if hasattr(controller, "cleanup"):
                controller.cleanup()

        if cursor_manager is not None:
            try:
                cursor_manager.hide_brush_indicator()
            except Exception as exc:
                log_warning(T("清理画笔指示器失败: {exc}", exc=exc), "CanvasView")
            cursor_manager.current_cursor = None
            cursor_manager.view = None
            cursor_manager.scene = None

        if getattr(self, "window_finder", None):
            self.window_finder.clear()
            self.window_finder = None

        if getattr(self, "element_finder", None):
            self.element_finder.close()
            self.element_finder = None

        if getattr(self, "_smart_selection_anim", None):
            self._smart_selection_anim.stop()

        self.cursor_manager = None
        self.smart_edit_controller = None
        self.canvas_scene = None

    def _on_cursor_tool_update_requested(self, tool_id: str, force: bool):
        if self._is_closed:
            return
        cursor_manager = getattr(self, "cursor_manager", None)
        if cursor_manager is None:
            return
        try:
            cursor_manager.set_tool_cursor(tool_id, force=force)
        except RuntimeError as exc:
            log_warning(T("更新工具光标失败: {exc}", exc=exc), "CanvasView")

    def _on_item_auto_select_requested(self, item):
        if self._is_closed:
            return
        controller = getattr(self, "smart_edit_controller", None)
        if controller is None:
            return
        try:
            controller.select_item(item, auto_select=True)
        except RuntimeError as exc:
            log_warning(T("自动选择图元失败: {exc}", exc=exc), "CanvasView")

    def can_apply_tool_cursor(self) -> bool:
        """工具预览光标只有在编辑状态机不拥有光标时才可直接应用。"""
        if not self.canvas_scene or not self.canvas_scene.selection_model.is_confirmed:
            return True

        controller = getattr(self, "smart_edit_controller", None)
        if not controller or not controller.selected_item:
            return True

        mode_name = getattr(getattr(controller, "mode", None), "name", "")
        if mode_name in {"CLICKING_HANDLE", "DRAGGING_HANDLE", "DRAGGING_MOVE"}:
            return False

        view_pos = self.mapFromGlobal(QCursor.pos())
        viewport = self.viewport()
        if viewport is not None and not viewport.rect().contains(view_pos):
            return True

        scene_pos = self.mapToScene(view_pos)
        layer_editor = controller.layer_editor
        if layer_editor and layer_editor.is_editing() and layer_editor.hit_test(scene_pos):
            return False

        selected_item = controller.selected_item
        try:
            if selected_item.contains(selected_item.mapFromScene(scene_pos)):
                return False
        except Exception:
            return True

        return True

    @safe_event
    def enterEvent(self, event):
        """
        鼠标进入画布时强制应用当前光标，并刷新放大镜位置
        
        解决问题：点击工具栏按钮后，鼠标移回画布时光标可能不正确；
                  鼠标离开再回来时放大镜不跟随（enterEvent 比首个 mouseMoveEvent 早触发）
        """
        if not self.canvas_scene.selection_model.is_confirmed:
            self.setCursor(Qt.CursorShape.CrossCursor)
        elif hasattr(self, 'cursor_manager') and self.cursor_manager and self.cursor_manager.current_cursor:
            # 强制重新应用光标
            self.cursor_manager._apply_cursor(self.cursor_manager.current_cursor)
        # 鼠标重新进入时，用当前鼠标全局坐标刷新放大镜，
        # 避免等到第一个 mouseMoveEvent 才恢复显示
        overlay = self._get_magnifier_overlay()
        if overlay is not None:
            from PySide6.QtGui import QCursor
            from PySide6.QtCore import QPointF
            gpos = QCursor.pos()
            overlay.update_cursor(QPointF(gpos.x(), gpos.y()))
        super().enterEvent(event)

    @safe_event
    def showEvent(self, event):
        """窗口显示后强制刷新光标，避免沿用上一次截图的光标"""
        super().showEvent(event)
        if not self.canvas_scene.selection_model.is_confirmed:
            def _safe_set_cursor():
                if shiboken6.isValid(self):
                    self.setCursor(Qt.CursorShape.CrossCursor)
            QTimer.singleShot(0, _safe_set_cursor)
    
    # ========================================================================
    # 智能选区功能
    # ========================================================================
    
    def enable_smart_selection(self, mode):
        """启用智能选区。

        控件不是与窗口并列的另一种模式，而是窗口之下的一级粒度：``element``
        在窗口矩形的基础上再下钻一级取 UI Automation 的控件矩形，UIA 缺失或
        目标没有暴露无障碍树时，这一级自然不存在，剩下的就是窗口检测。保留
        bool 入参是为了兼容旧调用方：True 等同于 ``element``。
        """
        if isinstance(mode, bool):
            mode = "element" if mode else "off"
        mode = str(mode or "off").lower()
        if mode not in {"off", "window", "element"}:
            mode = "off"

        self.smart_selection_enabled = mode != "off"
        self.smart_selection_drills_to_elements = mode == "element"
        # 每次截图都从最细一级开始，不继承上一次滚轮停在哪一档。
        self._smart_selection_level = 0
        self._smart_selection_level_anchor = None

        if self.element_finder:
            self.element_finder.close()
            self.element_finder = None

        if self.smart_selection_enabled:
            # 检查窗口检测依赖
            from capture.window_finder import is_smart_selection_available
            if not is_smart_selection_available():
                log_warning(T("win32gui 未安装，智能选区功能不可用"), "SmartSelect")
                self.smart_selection_enabled = False
                self.smart_selection_drills_to_elements = False
                return

            # 创建 WindowFinder 实例
            if not self.window_finder:
                from capture.window_finder import WindowFinder
                # CanvasScene 使用物理屏幕坐标，因此不需要坐标偏移。
                self.window_finder = WindowFinder(0, 0)

            # 截图浮层显示前先缓存底层 HWND；UIA 后续以该句柄为根，避免
            # ElementFromPoint 命中全屏截图浮层自身。
            self.window_finder.find_windows()
            log_debug(
                T("已启用，找到 {window_count} 个窗口", window_count=len(self.window_finder.windows)),
                "SmartSelect",
            )

            if self.smart_selection_drills_to_elements:
                from capture.uia_element_finder import UIAElementFinder, is_uia_available

                if is_uia_available():
                    self.element_finder = UIAElementFinder(self)
                    self.element_finder.cache_updated.connect(self._on_uia_cache_updated)
                    cursor_pos = QCursor.pos()
                    window_hit = self.window_finder.find_window_at_point_info(
                        cursor_pos.x(), cursor_pos.y()
                    )
                    if window_hit:
                        self.element_finder.request_refresh_if_needed(window_hit[0])
                    # 光标底下那个插过队了，其余按"露出来多少"预热：用户挪鼠标
                    # 的这几百毫秒里把它们扫完，移过去时就没有等待可言。被完全
                    # 遮住的窗口选都选不中，不在其列。
                    self.element_finder.prewarm(
                        self.window_finder.windows_by_exposed_area()
                    )
                else:
                    # 缺少可选依赖时只是少了下钻这一级，不该让截图失败。
                    self.smart_selection_drills_to_elements = False
                    log_warning(T("comtypes 未安装，已回退为窗口检测"), "SmartSelect")
        else:
            log_debug(T("已禁用"), "SmartSelect")
            self._smart_selection_anim.stop()
            if self.window_finder:
                self.window_finder.clear()
    
    def _get_smart_selection_rect(self, scene_pos: QPointF) -> QRectF:
        """返回鼠标下方的智能选区矩形（物理屏幕坐标），截图范围之外的部分裁掉。

        不缓存上一次的结果：selection_model.set_rect 本身对相同矩形短路，而
        命中测试只是在几百个矩形里取最小，按距离防抖省不下什么，却要为窗口和
        控件两种粒度各配一套阈值——控件小到几像素，窗口大到整屏。
        """
        if not self.smart_selection_enabled or not self.window_finder:
            return QRectF()

        x, y = int(scene_pos.x()), int(scene_pos.y())
        # 备选矩形取全场景（场景用物理屏幕坐标，含负坐标显示器）
        scene_rect = self.canvas_scene.scene_rect
        capture_rect = [
            int(scene_rect.x()),
            int(scene_rect.y()),
            int(scene_rect.x() + scene_rect.width()),
            int(scene_rect.y() + scene_rect.height()),
        ]

        rect = self._pick_smart_selection_level(
            self._smart_selection_chain(x, y, capture_rect)
        )
        if not rect:
            return QRectF()

        return QRectF(
            float(rect[0]),
            float(rect[1]),
            float(rect[2] - rect[0]),
            float(rect[3] - rect[1]),
        ).intersected(scene_rect)

    def _smart_selection_chain(self, x: int, y: int, capture_rect):
        """鼠标下方由细到粗的嵌套矩形链：控件 → 窗口 → 鼠标所在那块屏幕。

        屏幕是最外一级，鼠标不在任何窗口上时也是唯一一级——滚到头总能框住整屏。
        """
        chain = []
        window_hit = self.window_finder.find_window_at_point_info(x, y)
        if window_hit:
            for rect in self._element_rects(window_hit, x, y, capture_rect):
                _append_level(chain, rect)
            _append_level(chain, list(window_hit[1]))

        screen = self.window_finder.find_monitor_rect_at_point(x, y)
        _append_level(chain, [
            max(screen[0], capture_rect[0]), max(screen[1], capture_rect[1]),
            min(screen[2], capture_rect[2]), min(screen[3], capture_rect[3]),
        ])
        return chain

    def _element_rects(self, window_hit, x: int, y: int, capture_rect):
        """窗口之下命中该点的控件矩形，由细到粗。

        扫描一个窗口要几十到上百毫秒，期间这一级是空的、由窗口顶着，结果回来时
        由 cache_updated 收紧到控件——这一级缺席时的表现就是纯窗口检测。
        """
        hwnd, window_rect, _title = window_hit
        if not self.smart_selection_drills_to_elements or not self.element_finder:
            return []

        min_size = self.canvas_scene.selection_model.min_size
        elements = self.element_finder.elements_at(
            hwnd, x, y, min_size=(min_size.width(), min_size.height())
        )
        self.element_finder.request_refresh_if_needed(hwnd)

        rects = []
        for element in elements:
            left, top, right, bottom = element.rect
            clipped = [max(left, window_rect[0], capture_rect[0]),
                       max(top, window_rect[1], capture_rect[1]),
                       min(right, window_rect[2], capture_rect[2]),
                       min(bottom, window_rect[3], capture_rect[3])]
            # 控件可能横跨窗口或截图边界，裁完剩下的那块未必还盖着鼠标，也可能
            # 塌到最小选区以下——这样的控件不该占链上一级，外面一级会顶替它。
            if (clipped[0] <= x < clipped[2] and clipped[1] <= y < clipped[3]
                    and clipped[2] - clipped[0] >= min_size.width()
                    and clipped[3] - clipped[1] >= min_size.height()):
                rects.append(clipped)
        return rects

    def _pick_smart_selection_level(self, chain):
        """按当前档位从嵌套链里取一级，越界的档位贴住链的两端。

        档位锚在链最细一级上：鼠标在同一个控件里移动时保持滚轮选好的粒度，
        移到别的控件才重新从最细一级开始。锚成位置的话，挪一像素就会把用户
        刚滚出来的粒度清掉。还没有锚只是这个位置第一次算链——滚轮可能就发生在
        这一帧之前，这时归零会把用户刚滚的那一格吃掉。
        """
        if not chain:
            return None
        anchor = tuple(chain[0])
        if self._smart_selection_level_anchor not in (None, anchor):
            self._smart_selection_level = 0
        self._smart_selection_level_anchor = anchor
        self._smart_selection_level = min(self._smart_selection_level, len(chain) - 1)
        return chain[self._smart_selection_level]

    def _adjust_smart_selection_level(self, delta: int, scene_pos: QPointF) -> bool:
        """滚轮切粒度：向上滚选包围当前矩形的更大一级，向下滚回到更细一级。

        滚轮归谁只看检测方式这一个开关：开着控件检测就一直归粒度，哪怕鼠标底下
        只有一级、滚了没有变化；关着就一直归放大镜倍数。按链上有几级动态决定的
        话，同一个动作在不同位置表现不同，而"有几级"用户根本看不见。选区确认
        或正在拉框时没有链可走，那时仍归放大镜。
        """
        if (self._is_closed or not self.smart_selection_drills_to_elements
                or self.canvas_scene is None
                or self.canvas_scene.selection_model.is_confirmed
                or self.selection_drag.active):
            return False

        self._smart_selection_level = max(
            0, self._smart_selection_level + (1 if delta > 0 else -1)
        )
        # 走 hover 这条正路，换档和鼠标移动引起的选区更新是同一件事。
        self._handle_hover_preview(scene_pos)
        return True

    @safe_event
    def _on_uia_cache_updated(self, hwnd: int):
        """扫描结果迟到，按当前鼠标位置把选区从窗口矩形收紧到控件矩形。

        用户已经滚出粒度时不收紧：扫描要几十到上百毫秒，正好覆盖"移过去马上滚
        一格"这个节奏，这时候结果一回来链的最细一级就换了人，档位锚不住会归零，
        用户刚滚到的整屏会当场塌回一个小按钮。
        """
        if (self._is_closed or not self.smart_selection_enabled
                or not self.smart_selection_drills_to_elements
                or self._smart_selection_level > 0
                or not self.window_finder or not self.canvas_scene
                or self.canvas_scene.selection_model.is_confirmed
                or self.selection_drag.active):
            return

        cursor_pos = QCursor.pos()
        window_hit = self.window_finder.find_window_at_point_info(
            cursor_pos.x(), cursor_pos.y()
        )
        if window_hit and window_hit[0] == hwnd:
            # 走 hover 这条正路：迟到的结果和鼠标移动引起的更新没有区别，
            # 收紧同样该补间，否则选区会在原地硬跳一下。
            self._handle_hover_preview(QPointF(cursor_pos.x(), cursor_pos.y()))

    def _set_selection_rect_now(self, rect: QRectF):
        """补间每帧的落点。视图关闭后 canvas_scene 会被置空，定时器可能还差一拍。"""
        if self._is_closed or self.canvas_scene is None:
            return
        self.canvas_scene.selection_model.set_rect(rect)
    
    def _apply_smart_selection_rect(self, rect: QRectF, *, animate: bool):
        """把智能选区矩形送进 selection_model。

        animate=False 用于按下、首次出现这类必须落在真实矩形上的时机——补间
        中途的矩形不对应任何一个窗口或控件，此时截图会截出插值结果。hover 一律
        补间：窗口和控件用的是同一条路径，位移小于一屏的跳转由动画自身按位移
        判定要不要走完（见 SmartSelectionAnimator.MIN_TRAVEL_PX）。
        """
        if animate and self.smart_selection_animated:
            self._smart_selection_anim.animate_to(
                self.canvas_scene.selection_model.rect(), rect
            )
        else:
            self._smart_selection_anim.snap_to(rect)
    
    # ========================================================================
    # 智能编辑控制器回调
    # ========================================================================
    
    def _on_editing_cleanup(self):
        """响应 editing_cleanup_requested 信号，清除编辑状态"""
        self.item_drag.finish(commit=True)
        if self.smart_edit_controller.selected_item:
            log_debug(T("取消智能编辑选择"), "CanvasView")
            self.smart_edit_controller.clear_selection(suppress_block=True)
        if hasattr(self.cursor_manager, 'hide_brush_indicator'):
            self.cursor_manager.hide_brush_indicator()

    def _on_tool_changed_for_edit(self, tool_id: str):
       self.item_drag.finish(commit=True)
       self.smart_edit_controller.set_tool(tool_id)

       # 工具切换时立即更新光标
       self.cursor_manager.set_tool_cursor(tool_id)
       if self.cursor_manager.current_cursor:
        self.setCursor(self.cursor_manager.current_cursor)

    
    def _on_edit_cursor_change(self, cursor_type: str):
        """智能编辑控制器请求光标变化"""
        # 将字符串类型映射到 Qt.CursorShape
        from PySide6.QtCore import Qt
        cursor_map = {
            "cross": Qt.CursorShape.CrossCursor,
            "default": Qt.CursorShape.ArrowCursor,
            "move": Qt.CursorShape.SizeAllCursor,
            "resize": Qt.CursorShape.SizeFDiagCursor,
        }
        cursor_shape = cursor_map.get(cursor_type, Qt.CursorShape.ArrowCursor)
        self.setCursor(cursor_shape)

    def _on_cross_tool_switch_requested(self, tool_id: str):
        """Ctrl 选中异类图元：像真的点了工具栏按钮一样永久切到该工具。

        必须在 smart_edit_controller.select_item() 之前完成（由信号发射
        顺序保证），否则 select_tool -> set_tool 清空选择会把刚选中的图元
        又清掉。

        不能简单调用 toolbar.select_tool()：它发出的 tool_changed 信号在
        真实窗口里被 ScreenshotWindow.on_tool_changed / PinCanvas 之类的
        宿主监听，会再调一次 scene.activate_tool()，与下面这里的调用重复
        触发 ToolController.activate（每次都会 on_deactivate/on_activate
        并重跑 tool_changed_callbacks，第二次的 set_tool() 会把刚选中的
        图元清掉）。所以这里 blockSignals 让 select_tool 只做按钮高亮和
        current_tool 记账，工具引擎激活与面板同步由本函数唯一负责一次。
        """
        toolbar = self._get_active_toolbar()
        if toolbar and hasattr(toolbar, "select_tool"):
            toolbar.blockSignals(True)
            try:
                toolbar.select_tool(tool_id)
            finally:
                toolbar.blockSignals(False)

        if self.canvas_scene:
            self.canvas_scene.activate_tool(tool_id)

        if toolbar and hasattr(toolbar, "restore_active_tool_state"):
            ctx = getattr(self.canvas_scene.tool_controller, "ctx", None) if self.canvas_scene else None
            toolbar.blockSignals(True)
            try:
                toolbar.restore_active_tool_state(tool_id, ctx, self.canvas_scene)
            finally:
                toolbar.blockSignals(False)

    def _on_edit_selection_changed(self, item):
        """智能编辑选择变化"""
        if item:
            log_debug(T("选中: {item_type}", item_type=type(item).__name__), "SmartEdit")
        else:
            self.item_drag.finish(commit=False)
            log_debug(T("取消选择"), "SmartEdit")
            self._sync_highlighter_panel_mode()
        self._sync_selection_style_to_toolbar(item)

    def _sync_highlighter_panel_mode(self):
        toolbar = self._get_active_toolbar()
        tool_controller = getattr(self.canvas_scene, "tool_controller", None)
        if not toolbar or not tool_controller:
            return
        current_tool = getattr(tool_controller, "current_tool", None)
        if not current_tool or current_tool.id != "highlighter":
            return

        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            settings = manager.get_tool_settings("highlighter") if manager else None
            mode = settings.get("draw_mode", "freehand") if settings else "freehand"
        except Exception as exc:
            log_warning(T("读取荧光笔模式失败: {exc}", exc=exc), "CanvasView")
            mode = "freehand"

        if hasattr(toolbar, "paint_panel") and hasattr(toolbar.paint_panel, "set_highlighter_mode"):
            toolbar.paint_panel.set_highlighter_mode(mode)

    def _get_active_toolbar(self):
        window = self.window()
        if window is None:
            return None
        toolbar = getattr(window, "toolbar", None)
        return toolbar if toolbar else None

    def _sync_selection_style_to_toolbar(self, item):
        toolbar = self._get_active_toolbar()
        tool_controller = getattr(self.canvas_scene, "tool_controller", None)
        if not toolbar or not tool_controller:
            return

        if not item:
            current_tool = getattr(tool_controller, "current_tool", None)
            tool_id = getattr(current_tool, "id", None) or "cursor"
            if hasattr(toolbar, "restore_active_tool_state"):
                toolbar.restore_active_tool_state(
                    tool_id,
                    getattr(tool_controller, "ctx", None),
                    self.canvas_scene,
                )
            else:
                toolbar.set_temporary_edit_active(False)
            return

        controller = getattr(self, "smart_edit_controller", None)
        is_cross_tool = bool(controller and controller.is_cross_tool_selection())
        toolbar.set_temporary_edit_active(is_cross_tool)

        is_text_item = isinstance(item, QGraphicsTextItem)
        width_value = None if is_text_item else self._extract_selection_width(item)
        opacity_value = self._extract_selection_opacity(item)

        style_kwargs = {}
        if width_value is not None:
            style_kwargs["width"] = max(1.0, float(width_value))
        if opacity_value is not None:
            style_kwargs["opacity"] = max(0.0, min(1.0, float(opacity_value)))

        if style_kwargs and not is_cross_tool:
            tool_controller.update_style(**style_kwargs)
            if "width" in style_kwargs and getattr(self, "cursor_manager", None):
                ctx_width = int(max(1, round(tool_controller.ctx.stroke_width)))
                self.cursor_manager.update_tool_cursor_size(ctx_width)

        if width_value is not None:
            toolbar.set_stroke_width(int(round(width_value)))

        if opacity_value is not None:
            toolbar.set_opacity(int(round(opacity_value * 255)))

        # 线型按图元的归属工具同步，不按类名：荧光笔矩形、聚光灯的孔也是 RectItem，
        # 按类名判断会把它们的画笔当成矩形工具的线型写进设置
        owner_tool_id = controller.get_item_tool_id(item) if controller else None
        line_style_panel = {
            "pen": "paint_panel", "highlighter": "paint_panel",
            "rect": "shape_panel", "ellipse": "shape_panel",
        }.get(owner_tool_id)
        if line_style_panel and hasattr(toolbar, line_style_panel):
            try:
                from PySide6.QtCore import Qt
                pen = item.pen()
                pen_style = pen.style()
                dash_pattern = [round(x, 1) for x in pen.dashPattern()]
                has_dash = bool(dash_pattern)
                if has_dash and dash_pattern[:2] == [1.0, 2.0]:
                    line_style = "dashed_dense"
                elif pen_style in (Qt.PenStyle.DashLine, Qt.PenStyle.CustomDashLine) or has_dash:
                    line_style = "dashed"
                else:
                    line_style = "solid"
                getattr(toolbar, line_style_panel).line_style = line_style
                if not is_cross_tool:
                    from settings import get_tool_settings_manager
                    get_tool_settings_manager().update_settings(owner_tool_id, line_style=line_style)
            except Exception as exc:
                log_warning(T("无法同步线条样式: {exc}", exc=exc), "CanvasView")

        # 根据选中的图元类型，显示对应的设置面板（二次编辑时支持修改样式）
        self._show_panel_for_selection(item, toolbar)

    def _show_panel_for_selection(self, item, toolbar):
        """根据选中的图元类型显示对应的设置面板（二次编辑支持）。

        统一顺序：先 _show_panel_for_tool 把面板回填成工具默认值，再把选中的这
        一个图元的状态盖上去。反过来写会被默认值冲掉——面板是跨工具共用的，
        回填是它显示时的固有动作，不能指望调用方先做完差异化同步再显示。

        也正因为这个顺序，同步失败只需要在最外面兜一次：面板早就显示出来了，
        兜住之后它停在工具默认值上，本来就是这里能给出的最好结果。

        面板按图元的归属工具（SmartEditController.get_item_tool_id）选，不按类名：
        荧光笔矩形、聚光灯的孔都是 RectItem，按类名判断就得记住它们必须排在矩形前面。
        """
        try:
            if isinstance(item, QGraphicsTextItem):
                tool_id = "text"
            else:
                controller = getattr(self, "smart_edit_controller", None)
                tool_id = controller.get_item_tool_id(item) if controller else None
            if tool_id is None:
                return

            toolbar._show_panel_for_tool(tool_id)
            if tool_id == "text":
                toolbar.text_panel.set_state_from_item(item)
            elif tool_id == "arrow":
                toolbar.arrow_panel.arrow_style = getattr(item, "_arrow_style", "single")
                if hasattr(item, "color"):
                    toolbar.arrow_panel.set_color(item.color)
            elif tool_id in ("pen", "highlighter"):
                toolbar.paint_panel.set_state_from_item(item)
                if isinstance(item, RectItem):
                    toolbar.paint_panel.set_highlighter_mode("rect")
            elif tool_id in ("rect", "ellipse"):
                toolbar.shape_panel.set_state_from_item(item)
            elif tool_id == "number":
                toolbar.number_panel.set_style(item.style)
            # 马赛克图元（框选或自由涂抹都在这，跨工具/钉图选中时也要弹出对应面板）
            elif tool_id == "mosaic":
                toolbar.mosaic_panel.set_draw_mode("rect" if item.fill_mode() else "freehand")
                toolbar.mosaic_panel.set_style("blur" if item.smooth() else "pixelate")
                # 粒度也得跟着选中的这一块：面板「再点一次当前档不算改动」，
                # 漏掉这行的话，高亮的是工具默认档、图元却是另一档，用户点
                # 那个高亮档想改它会被当成重复点击直接吃掉。
                toolbar.mosaic_panel.set_block_size(item.block_size())
        except Exception as e:
            log_warning(T("显示编辑面板失败: {e}", e=e), "CanvasView")

    def _extract_selection_width(self, item):
        if hasattr(item, 'get_stroke_width'):
            return item.get_stroke_width()
        return None

    def _extract_selection_opacity(self, item):
        if not item:
            return None
        if hasattr(item, 'get_visual_opacity'):
            result = item.get_visual_opacity()
            if result is not None:
                return result
        direct = max(0.0, min(1.0, float(item.opacity())))
        return direct

    def apply_cross_tool_selection_style(
        self,
        *,
        color: Optional[QColor] = None,
        width: Optional[float] = None,
        opacity: Optional[float] = None,
    ) -> Optional[bool]:
        """Apply one style to a foreign selected item without touching tool defaults.

        None means the selection is not cross-tool and the caller may use its
        existing active-tool path. True/False both consume the event; False
        means the target does not support that property.
        """
        controller = getattr(self, "smart_edit_controller", None)
        if not controller or not controller.is_cross_tool_selection():
            return None
        item = getattr(controller, "selected_item", None)
        if item is None:
            return None

        applied = False
        if width is not None:
            current_width = self._extract_selection_width(item)
            if (
                current_width is None
                or current_width <= 0
                or not hasattr(item, "scale_stroke_width")
            ):
                return False
            applied = bool(item.scale_stroke_width(max(1.0, float(width)) / current_width))

        if opacity is not None:
            applied = self._update_item_visual_opacity(item, opacity) or applied

        if color is not None:
            if isinstance(item, TextItem):
                # Text is updated by SmartEditController's existing signal slot.
                applied = True
            elif isinstance(item, (StrokeItem, RectItem, EllipseItem)):
                pen = QPen(item.pen())
                new_color = QColor(color)
                new_color.setAlpha(pen.color().alpha())
                pen.setColor(new_color)
                item.setPen(pen)
                if isinstance(item, RectItem) and getattr(item, "is_highlighter_rect", False):
                    brush = QBrush(item.brush())
                    brush_color = QColor(color)
                    brush_color.setAlpha(brush.color().alpha())
                    brush.setColor(brush_color)
                    item.setBrush(brush)
                item.update()
                applied = True
            elif isinstance(item, (ArrowItem, NumberItem)):
                old_color = QColor(item.color)
                new_color = QColor(color)
                new_color.setAlpha(old_color.alpha())
                item.color = new_color
                if isinstance(item, ArrowItem) and hasattr(item, "update_geometry"):
                    item.update_geometry()
                item.update()
                applied = True
            else:
                return False

        if applied:
            editor = getattr(controller, "layer_editor", None)
            if editor and editor.is_editing() and not isinstance(item, TextItem):
                editor.start_edit(item)
            self.canvas_scene.update()
        return applied

    @safe_event
    def mousePressEvent(self, event):
        """
        鼠标按下
        
        优先级逻辑：
        0. 右键 → 直接退出截图（仅截图窗口）
        1. 选区未确认 → 创建选区
        2. 选区已确认：
           a. 优先检查智能编辑（选中已有图元 + 控制点拖拽）
           b. 如果未处理，再执行绘图工具逻辑
        """
        # 右键直接退出截图（复用 ESC 的清理逻辑）
        # 只在截图窗口中生效，钉图窗口不响应
        if event.button() == Qt.MouseButton.RightButton:
            # 检查父窗口类型，只对 ScreenshotWindow 生效
            parent_window = self.window()
            if parent_window and parent_window.__class__.__name__ == 'ScreenshotWindow':
                log_debug(T("右键退出截图"), "CanvasView")
                event.accept()  # 立即接受事件
                # 复用 cleanup_and_close 方法，与 ESC 保持一致
                if hasattr(parent_window, 'cleanup_and_close'):
                    parent_window.cleanup_and_close()
                else:
                    parent_window.close()
                return
            # 钉图窗口：不处理右键，让事件继续传递（显示右键菜单）
        
        scene_pos = self.mapToScene(event.pos())
        self._double_click_candidate = None
        # 新的一次点击开始前重置单击编辑状态
        self.pending_text_edit.clear()
        
        if not self.canvas_scene.selection_model.is_confirmed:
            # 选区未确认：拖拽创建选区
            self.selection_drag.begin(scene_pos)
        else:
            # 选区已确认：优先尝试智能编辑
            # 先快照"按下之前"的可回滚状态，但只有穿过下面的编辑分支
            # （文本编辑、控制点）之后，这次点击才算双击确认的候选。
            double_click_candidate = self._build_double_click_candidate(
                scene_pos, event
            )
            current_tool_id = self.canvas_scene.tool_controller.current_tool_id

            log_debug(T("选区已确认，当前工具: {current_tool_id}", current_tool_id=current_tool_id), "CanvasView")
            
            # 步骤0：正在编辑文本时，点击外部先结算这次编辑，不创建新文本；
            # 若点在另一段文字上，同一次点击顺势切过去继续编辑那一段
            if self._is_text_editing():
                focus_item = self._get_active_text_item()
                if focus_item is None:
                    return
                edit_handled = self.smart_edit_controller.handle_edit_press(
                    scene_pos,
                    event.pos(),
                    event.button(),
                    event.modifiers(),
                )
                if edit_handled:
                    log_debug("文字宽度控制点拖拽被处理", "CanvasView")
                    return
                if isinstance(focus_item, QGraphicsTextItem) and \
                        self.text_drag.is_point_on_edge(focus_item, scene_pos):
                    self.text_drag.begin(focus_item, scene_pos)
                    return
                # 检查点击位置是否在当前编辑的文本框内
                if focus_item.contains(focus_item.mapFromScene(scene_pos)):
                    # 点击在文本框内，正常传递事件（移动光标等）
                    super().mousePressEvent(event)
                    return
                # 点击在文本框外：先让当前这段结束编辑（失焦时图元自己结算内容）
                switch_target = self._text_switch_target(
                    scene_pos, focus_item, event.modifiers()
                )
                self._end_text_edit(focus_item)
                if switch_target is None or switch_target.scene() is None:
                    # 没点到别的文字，这一下只用来确认编辑，不再触发新绘图
                    return
                # 点到了另一段文字：同一次点击继续往下走完选中流程，单击就切过去，
                # 而不是被吞掉、还要再点第二下。这一下已经有了自己的用途，不能
                # 再当"双击确认截图"的候选（步骤1 之后才会赋值给 self）。
                double_click_candidate = None

            # 步骤1：优先检查控制点拖拽（如果已选中图元）
            edit_handled = self.smart_edit_controller.handle_edit_press(
                scene_pos, event.pos(), event.button(), event.modifiers()
            )
            
            if edit_handled:
                # 控制点拖拽被处理，不继续
                log_debug(T("控制点拖拽被处理"), "CanvasView")
                layer_editor = self.smart_edit_controller.layer_editor
                if layer_editor.hovered_handle:
                    self.setCursor(layer_editor.get_cursor(scene_pos))
                return

            # 走到这里说明这次点击既不是文本编辑也没命中控制点：
            # 连点控制点（如序号 +/-）不会被后续双击当成确认截图。
            self._double_click_candidate = double_click_candidate

            # 步骤2：检查是否点击了可选中的图元
            selection_handled = self.smart_edit_controller.handle_press(
                event.pos(), 
                scene_pos, 
                event.button(), 
                event.modifiers()
            )
            
            if selection_handled:
                # 选中了图元，阻止绘图
                # 传递给 Scene（让图元处理拖拽）
                log_debug(T("图元选择被处理，阻止绘图"), "CanvasView")
                self.pending_text_edit.arm(event, scene_pos)
                if self.smart_edit_controller.press_requires_manual_dispatch:
                    # Qt 默认把事件交给 z 轴最上方图元；当控制器有意向下
                    # 命中兼容目标时，由 View 接管本次拖动，避免顶层文字抢走事件。
                    self.item_drag.begin(scene_pos)
                    event.accept()
                    return
                super().mousePressEvent(event)
                return
            
            # 如果刚刚清除了选择，这次点击仅用于取消选择，不应该开始绘图
            if getattr(self.smart_edit_controller, '_just_cleared_selection', False):
                self.smart_edit_controller._just_cleared_selection = False
                log_debug(T("刚清除选择，跳过本次绘图"), "CanvasView")
                return
            
            # 步骤3：如果是绘图工具且未选中图元，执行绘图
            is_drawing_tool = current_tool_id != "cursor"
            
            if is_drawing_tool:
                # 绘图工具激活：绘图
                log_debug(T("开始绘图"), "CanvasView")
                self.drawing.begin(scene_pos, event.button())
            else:
                # cursor 工具：传递给 Scene（可能拖拽窗口/选区）
                log_debug(T("cursor工具，传递给Scene"), "CanvasView")
                super().mousePressEvent(event)
    
    @safe_event
    def mouseDoubleClickEvent(self, event):
        """
        鼠标双击事件
        快速双击时 Windows 将第二次按下转为 WM_LBUTTONDBLCLK，
        Qt 将其映射为 mouseDoubleClickEvent 而非 mousePressEvent，
        导致序号 +/- 按钮等点击型控制点丢失第二次点击。
        此处将双击事件重新路由到 handle_edit_press，确保快速连点生效。
        """
        # 仅处理选区已确认的情况（与 mousePressEvent 一致）
        if not self.canvas_scene or not self.canvas_scene.selection_model.is_confirmed:
            super().mouseDoubleClickEvent(event)
            return
        
        scene_pos = self.mapToScene(event.pos())

        # Consume the first-press candidate before provisional content can
        # expose edit handles under the second click (notably NumberItem +/-).
        if self._consume_double_click_candidate(scene_pos, event):
            return

        edit_handled = self.smart_edit_controller.handle_edit_press(
            scene_pos, event.pos(), event.button(), event.modifiers()
        )
        
        if edit_handled:
            self._double_click_candidate = None
            log_debug(T("双击→控制点点击被处理"), "CanvasView")
            layer_editor = self.smart_edit_controller.layer_editor
            if layer_editor.hovered_handle:
                self.setCursor(layer_editor.get_cursor(scene_pos))
            return

        # 非控制点双击 → 默认行为
        super().mouseDoubleClickEvent(event)

    def _build_double_click_candidate(self, scene_pos, event):
        """Capture the reversible state before any confirmed-selection routing."""
        if not self.confirm_on_double_click:
            return None
        if event.button() != Qt.MouseButton.LeftButton:
            return None
        if event.modifiers() != Qt.KeyboardModifier.NoModifier:
            return None
        if self.selection_drag.active or self.drawing.active or self.selection_drag.dragging:
            return None
        if self.text_drag.active:
            return None

        selection = self.canvas_scene.selection_model.rect()
        if selection.isEmpty() or not selection.contains(scene_pos):
            return None

        controller = self.canvas_scene.tool_controller
        tool = controller.current_tool
        stack = self.canvas_scene.undo_stack
        return {
            "scene_pos": QPointF(scene_pos),
            "selection_rect": QRectF(selection),
            "tool_id": tool.id if tool else None,
            "count": stack.count(),
            "index": stack.index(),
            "had_redo": stack.canRedo(),
            "dragged": False,
        }

    def _consume_double_click_candidate(self, scene_pos, event):
        candidate = self._double_click_candidate
        self._double_click_candidate = None
        if not self.confirm_on_double_click or not candidate:
            return False
        if event.button() != Qt.MouseButton.LeftButton:
            return False
        if event.modifiers() != Qt.KeyboardModifier.NoModifier:
            return False
        if candidate["dragged"] or self.drawing.active or self.selection_drag.active:
            return False

        original_selection = candidate["selection_rect"]
        if not original_selection.contains(scene_pos):
            return False

        distance = (scene_pos - candidate["scene_pos"]).manhattanLength()
        if distance > QApplication.startDragDistance():
            return False

        controller = self.canvas_scene.tool_controller
        tool = controller.current_tool
        if (tool.id if tool else None) != candidate["tool_id"]:
            return False

        handler = getattr(self.window(), "_handle_confirm", None)
        if not callable(handler):
            return False

        # 下面靠撤销栈的前后差异判断第一下点击留下了什么，前提是它的持久改动
        # 都已经落到栈上。文字编辑是例外：内容要到失焦才结算（临时文字到那时
        # 才入栈或被丢弃）。有候选就说明第一下点击前没有在编辑文字（编辑中的
        # 点击走 mousePressEvent 的步骤 0，不产生候选），所以此刻的编辑一定是
        # 第一下点击开启的——先像"点击外部"一样结束它，再看差异。
        active_text_item = self._get_active_text_item()
        if active_text_item is not None:
            self._end_text_edit(active_text_item)

        stack = self.canvas_scene.undo_stack
        command_to_undo = None

        old_count = candidate["count"]
        old_index = candidate["index"]

        if stack.index() == old_index and stack.count() == old_count:
            # 第一次点击没有产生任何命令
            pass
        elif stack.index() == old_index + 1 and stack.count() == (
            old_index + 1 if candidate["had_redo"] else old_count + 1
        ):
            from canvas.undo import (
                AddItemCommand,
                BatchRemoveCommand,
                EditItemCommand,
                RemoveItemCommand,
            )

            command_to_undo = stack.command(old_index)
            if not isinstance(
                command_to_undo,
                (AddItemCommand, BatchRemoveCommand, RemoveItemCommand, EditItemCommand),
            ):
                return False
            if isinstance(command_to_undo, AddItemCommand):
                item = getattr(command_to_undo, "item", None)
                if isinstance(item, TextItem) and item.toPlainText().strip():
                    return False
        else:
            return False

        if command_to_undo is not None:
            stack.undo()
            if stack.index() != old_index:
                return False

        # A crop handle can mutate the selection without an undo command.
        self.canvas_scene.selection_model.set_rect(QRectF(original_selection))
        self.canvas_scene.selection_model.stop_dragging()
        self.selection_drag.dragging = False

        event.accept()
        handler()
        return True

    def invalidate_double_click_candidate(self):
        self._double_click_candidate = None
    
    @safe_event
    def mouseMoveEvent(self, event):
        """
        鼠标移动事件 - 状态机路由
        
        状态优先级（互斥）：
        1. 创建选区 (selection_drag.active)
        2. 绘图中 (drawing.active)
        3. 文字拖拽 (text_drag.active)
        4. 选区已确认 - 编辑模式
        5. 选区未确认 - 悬停预览
        """
        candidate = self._double_click_candidate
        if candidate and not candidate["dragged"]:
            scene_pos = self.mapToScene(event.pos())
            distance = (scene_pos - candidate["scene_pos"]).manhattanLength()
            if distance > QApplication.startDragDistance():
                candidate["dragged"] = True

        # 窗口关闭时 scene 可能已被清理，忽略残留的鼠标事件
        if self.canvas_scene is None or self.scene() is None:
            return
        scene_pos = self.mapToScene(event.pos())

        # ====================================================================
        # 状态1：创建选区（拖拽选框）
        # ====================================================================
        if self.selection_drag.active:
            self.selection_drag.perform(scene_pos)
            return
        
        # ====================================================================
        # 状态2：绘图中（使用画笔/矩形/箭头等工具）
        # ====================================================================
        if self.drawing.active:
            self.drawing.perform(scene_pos)
            return
        
        # ====================================================================
        # 状态3：文字拖拽（拖动文字框边缘调整大小）
        # ====================================================================
        if self.text_drag.active:
            self._handle_text_drag_move(scene_pos)
            return
        
        # ====================================================================
        # 状态4：选区已确认 - 编辑模式（智能编辑、悬停、拖拽图元）
        # ====================================================================
        if self.canvas_scene.selection_model.is_confirmed:
            self._handle_edit_mode_move(event, scene_pos)
            return
        
        # ====================================================================
        # 状态5：选区未确认 - 悬停预览（智能选区）
        # ====================================================================
        self._handle_hover_preview(scene_pos)

    # ========================================================================
    # 状态处理器：创建选区
    # ========================================================================
    
    def _handle_text_drag_move(self, scene_pos: QPointF):
        """处理文字框边缘拖拽（状态3）"""
        self._update_magnifier_overlay(scene_pos)
        self.text_drag.set_cursor(True)
        self.text_drag.perform(scene_pos)
    
    # ========================================================================
    # 状态处理器：编辑模式（选区已确认）
    # ========================================================================
    
    def _handle_edit_mode_move(self, event, scene_pos: QPointF):
        """处理编辑模式的鼠标移动（状态4）"""
        self.pending_text_edit.track(event)
        self._update_magnifier_overlay(scene_pos)
        
        # 检查是否正在编辑文字（需要检测文字框边缘拖拽）
        # 左键按住时用户可能在拖拽选文字，不应检测边缘拖拽 hover
        is_left_pressed = bool(event.buttons() & Qt.MouseButton.LeftButton)
        if self._is_text_editing() and not is_left_pressed:
            self.text_drag.update_hover(scene_pos)

        # 向下命中的兼容图元由 View 全程拥有这次手势，优先于顶层图元
        # 的控制点/hover 分发。
        if self.item_drag.active and is_left_pressed:
            self._handle_selected_item_drag(event, scene_pos)
            return
        
        # 子状态1：正在拖拽控制点/手柄
        if self._handle_edit_handle_drag(scene_pos):
            return
        
        # 子状态2：悬停在控制手柄上
        if self._handle_edit_handle_hover(event, scene_pos):
            return
        
        # 子状态3：拖拽已选中的图元
        if self._handle_selected_item_drag(event, scene_pos):
            return
        
        # 子状态4：悬停检测（是否在可编辑图元上）
        self._handle_edit_hover_detection(event, scene_pos)
    
    def _handle_edit_handle_drag(self, scene_pos: QPointF) -> bool:
        """处理控制点/手柄拖拽（编辑模式子状态1）"""
        edit_move_handled = self.smart_edit_controller.handle_edit_move(scene_pos)
        if edit_move_handled:
            if self.smart_edit_controller.layer_editor.dragging_handle:
                dragging_cursor = self.smart_edit_controller.layer_editor.get_cursor(scene_pos)
                self.setCursor(dragging_cursor)
            elif self.smart_edit_controller.layer_editor.hovered_handle:
                self.setCursor(self.smart_edit_controller.layer_editor.hovered_handle.cursor)
            return True
        return False
    
    def _handle_edit_handle_hover(self, event, scene_pos: QPointF) -> bool:
        """处理控制手柄悬停（编辑模式子状态2）"""
        if not self.smart_edit_controller.selected_item:
            return False
        
        if not self.smart_edit_controller.layer_editor.is_editing():
            return False
        
        is_left_button_pressed = bool(event.buttons() & Qt.MouseButton.LeftButton)

        is_moving_item = getattr(self.smart_edit_controller.layer_editor, "is_moving_item", False)
        
        if is_left_button_pressed and is_moving_item:
            return False
        
        self.smart_edit_controller.layer_editor.update_hover(scene_pos)
        if self.smart_edit_controller.layer_editor.hovered_handle:
            handle_cursor = self.smart_edit_controller.layer_editor.get_cursor(scene_pos)
            self.setCursor(handle_cursor)
            return True
        
        return False
    
    def _handle_selected_item_drag(self, event, scene_pos: QPointF) -> bool:
        """处理已选中图元的拖拽（编辑模式子状态3）"""
        if not self.smart_edit_controller.selected_item:
            return False
        
        selected_item = self.smart_edit_controller.selected_item
        is_on_selected_item = selected_item.contains(
            selected_item.mapFromScene(scene_pos)
        )
        is_left_button_pressed = bool(event.buttons() & Qt.MouseButton.LeftButton)

        if self.item_drag.active and is_left_button_pressed:
            self.item_drag.perform(event, scene_pos)
            return True

        if is_left_button_pressed:
            # 按住左键 → 正在拖拽
            # 但如果选中的是文字图元且处于编辑模式，左键拖拽是在选文字，不是移动图元
            is_text_editing = self._is_text_editing() and isinstance(selected_item, QGraphicsTextItem)
            if is_text_editing:
                # 文字编辑中拖拽 = 选中文字，保持工字光标，交由 Qt 原生处理
                self.setCursor(Qt.CursorShape.IBeamCursor)
                super().mouseMoveEvent(event)
                return True
            self.smart_edit_controller.handle_move(event.pos(), scene_pos)
            self.setCursor(Qt.CursorShape.SizeAllCursor)
            super().mouseMoveEvent(event)
            self._update_edit_handles()
            return True
        elif is_on_selected_item:
            # 悬停在选中的图元上
            if self._is_text_editing() and isinstance(selected_item, QGraphicsTextItem):
                # 文字编辑模式：光标由 TextItem.hoverMoveEvent 负责（工字/拖拽区分）
                super().mouseMoveEvent(event)
                return True
            # 非文字：十字光标
            self.setCursor(Qt.CursorShape.CrossCursor)
            super().mouseMoveEvent(event)
            return True
        
        return False

    def _handle_edit_hover_detection(self, event, scene_pos: QPointF):
        """处理悬停检测（编辑模式子状态4）"""
        is_hovering = self.smart_edit_controller.handle_hover(event.pos(), scene_pos)
        if is_hovering:
            self.setCursor(Qt.CursorShape.CrossCursor)
        else:
            self._apply_tool_cursor()
        
        super().mouseMoveEvent(event)
    
    # ========================================================================
    # 状态处理器：悬停预览（选区未确认）
    # ========================================================================
    
    def _handle_hover_preview(self, scene_pos: QPointF):
        """处理智能选区悬停预览（状态5）"""
        self._update_magnifier_overlay(scene_pos)
        
        if self.smart_selection_enabled:
            smart_rect = self._get_smart_selection_rect(scene_pos)
            if not smart_rect.isEmpty():
                self.canvas_scene.selection_model.activate()
                if not self.canvas_scene.selection_model.is_dragging:
                    self.canvas_scene.selection_model.start_dragging()
                self._apply_smart_selection_rect(smart_rect, animate=True)
            else:
                self._smart_selection_anim.stop()
                if self.canvas_scene.selection_model.is_dragging:
                    self.canvas_scene.selection_model.stop_dragging()
        else:
            self._smart_selection_anim.stop()
            if self.canvas_scene.selection_model.is_dragging:
                self.canvas_scene.selection_model.stop_dragging()
    
    def _apply_tool_cursor(self):
        """应用当前工具的光标（SVG 光标）"""
        if (self.cursor_manager and 
            self.cursor_manager.current_cursor and
            self.cursor_manager.current_tool_id != "cursor"):
            self.setCursor(self.cursor_manager.current_cursor)

    @safe_event
    def leaveEvent(self, event):
        """鼠标离开画布时隐藏放大镜"""
        self._clear_magnifier_overlay()
        super().leaveEvent(event)
    
    def _update_edit_handles(self):
        """更新编辑控制点位置（图元移动后调用）。

        只重绘手柄浮层：图元自身的失效由 Qt 负责，浮层整层重画，
        不需要再猜手柄凸出多远去外扩内容层的脏区。
        """
        layer_editor = self.smart_edit_controller.layer_editor
        if layer_editor.is_editing():
            item = self.smart_edit_controller.selected_item
            if item:
                layer_editor.handles = layer_editor._generate_handles(item)
                self.request_handles_repaint()
    
    @safe_event
    def mouseReleaseEvent(self, event):
        """
        鼠标释放
        
        逻辑：
        1. selection_drag.active → 完成选区创建，确认选区
        2. drawing.active → 完成绘图，调用工具的 on_release
        3. 智能编辑控制点拖拽 → LayerEditor 处理
        4. 其他情况 → 智能编辑 + 传递给 Scene
        """
        scene_pos = self.mapToScene(event.pos())
        
        if self.text_drag.active:
            self.text_drag.end()
            return

        if self.selection_drag.active:
            self.selection_drag.end()
            return
        
        if self.drawing.active:
            self.drawing.end(scene_pos)
            return

        if self.item_drag.active:
            self.item_drag.finish(commit=True)
            if self.smart_edit_controller.selected_item:
                self._update_edit_handles()
            event.accept()
            return
        
        # 检查是否在释放控制点拖拽
        edit_release_handled = self.smart_edit_controller.handle_edit_release(
            scene_pos, event.button()
        )
        
        if edit_release_handled:
            # 控制点拖拽释放，不传递事件
            return
        
        # 智能编辑：处理释放
        self.smart_edit_controller.handle_release(event.pos(), scene_pos, event.button())
        
        # 如果有选中的图元，更新控制点（可能刚拖拽完成）
        if self.smart_edit_controller.selected_item:
            self._update_edit_handles()
        
        self.pending_text_edit.settle(event, scene_pos)
        # 传递给场景处理（可能是在释放图元拖拽）
        super().mouseReleaseEvent(event)
    
    @safe_event
    def wheelEvent(self, event):
        """
        鼠标滚轮事件 - 切换智能选区粒度、调整画笔大小或放大镜倍数
        """
        self.invalidate_double_click_candidate()
        # 只在绘图工具激活时响应
        current_tool = self.canvas_scene.tool_controller.current_tool
        if self.canvas_scene.tool_controller.current_tool_id == "cursor":
            delta = event.angleDelta().y()
            # 控件粒度悬停时滚轮归粒度切换，放大镜倍数另有快捷键（默认 PgUp/PgDn）；
            # 没有嵌套链可走时滚轮才还给放大镜。
            if delta != 0 and self._adjust_smart_selection_level(
                delta, self.mapToScene(event.position().toPoint())
            ):
                event.accept()
                return

            window = self.window()
            # 确保不是钉图窗口（钉图窗口没有放大镜）
            if (hasattr(window, 'magnifier_overlay') and 
                window.magnifier_overlay and 
                window.magnifier_overlay.cursor_scene_pos is not None and 
                window.magnifier_overlay._should_render()):
                if delta != 0:
                    # 向上滚动增加倍数，向下滚动减少倍数
                    zoom_delta = 1 if delta > 0 else -1
                    window.magnifier_overlay.adjust_zoom(zoom_delta)
                    event.accept()
                    return
            super().wheelEvent(event)
            return
            
        # 获取滚轮方向
        delta = event.angleDelta().y()
        modifiers = event.modifiers()

        # 跨工具临时编辑只改变目标图元，不改当前工具默认值。Shift 也按宽度
        # 处理，避免当前工具为序号时误改“下一个数字”。
        controller = getattr(self, "smart_edit_controller", None)
        if controller and controller.is_cross_tool_selection():
            item = controller.selected_item
            toolbar = getattr(self.window(), "toolbar", None)
            if delta != 0:
                if modifiers & Qt.KeyboardModifier.ControlModifier:
                    current_opacity = self._extract_selection_opacity(item)
                    if current_opacity is not None:
                        new_opacity = max(
                            0.0,
                            min(1.0, current_opacity + (0.05 if delta > 0 else -0.05)),
                        )
                        self.apply_cross_tool_selection_style(opacity=new_opacity)
                        if toolbar and hasattr(toolbar, "set_opacity"):
                            toolbar.set_opacity(int(round(new_opacity * 255)))
                else:
                    current_width = self._extract_selection_width(item)
                    if current_width is not None:
                        from tools.base import Tool
                        from tools.number import NumberTool
                        clamp_cls = NumberTool if isinstance(item, NumberItem) else Tool
                        new_width = clamp_cls.clamp_width(
                            current_width + (1.0 if delta > 0 else -1.0)
                        )
                        self.apply_cross_tool_selection_style(width=new_width)
                        if toolbar and hasattr(toolbar, "set_stroke_width"):
                            toolbar.set_stroke_width(int(round(new_width)))
            event.accept()
            return

        # Ctrl + 滚轮：调整透明度（0-255）
        if modifiers & Qt.KeyboardModifier.ControlModifier:
            if delta != 0:
                ctx = self.canvas_scene.tool_controller.context
                current_opacity = ctx.opacity if ctx.opacity is not None else 1.0
                step = 0.05
                new_opacity = current_opacity + (step if delta > 0 else -step)
                new_opacity = max(0.0, min(1.0, float(new_opacity)))

                self.canvas_scene.update_style(opacity=new_opacity)
                self._apply_opacity_change_to_selection(new_opacity)

                toolbar = getattr(self.window(), 'toolbar', None)
                if toolbar and hasattr(toolbar, 'set_opacity'):
                    toolbar.set_opacity(int(round(new_opacity * 255)))

            event.accept()
            return

        # Shift + 滚轮：调整序号工具的下一次数字
        if (
            current_tool.id == "number"
            and modifiers & Qt.KeyboardModifier.ShiftModifier
        ):
            if delta != 0:
                step = 1 if delta > 0 else -1
                ctx = self.canvas_scene.tool_controller.context
                next_value = current_tool.adjust_next_number(ctx.scene, step)
                current_tool.refresh_next_number(ctx.scene, next_value)
            event.accept()
            return
        
        # 特殊处理文字工具
        if current_tool.id == "text":
            toolbar = self.window().toolbar if hasattr(self.window(), "toolbar") else None
            controller = getattr(self, "smart_edit_controller", None)
            active_text = self._get_active_text_item()
            selected_text = None
            if controller and isinstance(controller.selected_item, (TextItem, QGraphicsTextItem)):
                selected_text = controller.selected_item

            # 获取当前字号：优先取正在编辑或选中的文字，若没有则退回面板，最后用默认值
            current_size = None
            if active_text:
                current_size = self._get_text_point_size(active_text)
            elif selected_text:
                current_size = self._get_text_point_size(selected_text)
            elif toolbar and hasattr(toolbar, "text_menu"):
                current_size = toolbar.text_menu.size_spin.value()
            if current_size is None:
                current_size = 16
            
            # 调整字号（每次滚动 ±2）
            step = 2
            if delta > 0:
                new_size = min(current_size + step, 144)
            else:
                new_size = max(current_size - step, 8)

            self._apply_text_point_size(active_text, selected_text, new_size)
            if toolbar and hasattr(toolbar, "text_menu"):
                toolbar.text_menu.size_spin.setValue(int(new_size))
                
            event.accept()
            return
        
        # 获取当前笔触宽度
        ctx = self.canvas_scene.tool_controller.context
        current_width = max(1.0, float(ctx.stroke_width))

        # 调整宽度（每次滚动 ±1，范围由当前工具的 clamp_width 决定，与滑块一致）
        step = 1.0 if delta > 0 else -1.0
        new_width = current_tool.clamp_width(current_width + step)
        
        # 更新笔触宽度
        self.canvas_scene.tool_controller.update_style(width=int(new_width))
        
        # 更新光标上的虚线圈大小
        self.cursor_manager.update_tool_cursor_size(int(new_width))

        # 同步到当前选中的图元（若有）
        scale = new_width / current_width if current_width > 0 else 1.0
        if abs(scale - 1.0) > 1e-6:
            self._apply_size_change_to_selection(scale)
        
        # 同步到 toolbar 的滑块
        toolbar = getattr(self.window(), 'toolbar', None)
        if toolbar and hasattr(toolbar, 'set_stroke_width'):
            toolbar.set_stroke_width(int(new_width))
        
        event.accept()
    
    @safe_event
    def keyPressEvent(self, event):
        """
        键盘事件
        
        View 只处理文字编辑模式下的特殊按键（回车换行）。
        所有窗口级快捷键（ESC、Enter确认、Ctrl+Z/Y、Ctrl+C/D 等）
        统一由 ScreenshotWindow.keyPressEvent 处理，避免重复和冲突。
        """
        self.invalidate_double_click_candidate()
        is_text_editing = self._is_text_editing()

        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            # 文字编辑模式下允许换行，不传递给父窗口
            if is_text_editing:
                super().keyPressEvent(event)
                self._update_edit_handles()
                return

        if is_text_editing:
            # 文字编辑模式下，Ctrl+Z/Y 交给 QGraphicsTextItem 处理内部撤销
            if (event.key() in (Qt.Key.Key_Z, Qt.Key.Key_Y)
                    and event.modifiers() == Qt.KeyboardModifier.ControlModifier):
                super().keyPressEvent(event)
                self._update_edit_handles()
                return
            # 文字编辑模式下，所有普通按键交给 QGraphicsTextItem 处理
            super().keyPressEvent(event)
            self._update_edit_handles()
            return

        # 非文字编辑模式：其余按键一律 ignore，让事件冒泡到父窗口统一处理
        event.ignore()

    @safe_event
    def inputMethodEvent(self, event):
        """IME composition between clicks invalidates screenshot confirmation."""
        self.invalidate_double_click_candidate()
        super().inputMethodEvent(event)
        self._update_edit_handles()

    def _is_text_editing(self) -> bool:
        """判断当前是否在编辑文字图元"""
        return self._get_active_text_item() is not None

    def _get_active_text_item(self):
        focus_item = self.canvas_scene.focusItem() if hasattr(self.canvas_scene, 'focusItem') else None
        if isinstance(focus_item, QGraphicsTextItem) and focus_item.hasFocus():
            flags = focus_item.textInteractionFlags()
            if bool(flags & Qt.TextInteractionFlag.TextEditorInteraction):
                return focus_item
        return None

    def _text_switch_target(self, scene_pos: QPointF, editing_item, modifiers):
        """编辑中点在文字外时，这一下点中的另一段文字；没点中就是 None。

        命中判断借控制器的 can_select_item，与真正执行选中的 handle_press 同一套
        规则：谁在上面、当前工具选不选得中、要不要按 Ctrl，只有一处说了算。若这一
        下点中的是别的类型（例如 Ctrl 跨工具选形状），按老规矩只结束编辑，不转手。
        """
        controller = getattr(self, "smart_edit_controller", None)
        if controller is None:
            return None
        for item in self.canvas_scene.items(scene_pos):
            if item is editing_item:
                continue
            if not controller.can_select_item(item, modifiers):
                continue
            return item if isinstance(item, TextItem) else None
        return None

    def _apply_size_change_to_selection(self, scale: float):
        controller = getattr(self, "smart_edit_controller", None)
        if not controller or scale <= 0:
            return

        handled_text = False
        active_text = self._get_active_text_item()
        if active_text:
            self._scale_text_item(active_text, scale)
            handled_text = True

        selected_item = getattr(controller, "selected_item", None)
        if selected_item:
            if handled_text and selected_item is active_text:
                return
            if self._scale_item_size(selected_item, scale):
                editor = controller.layer_editor
                if (
                    editor
                    and editor.is_editing()
                    and controller.selected_item is selected_item
                ):
                    editor.start_edit(selected_item)
                self.canvas_scene.update()
            else:
                pass

    def _scale_item_size(self, item, scale: float) -> bool:
        # 优先使用统一接口
        if hasattr(item, 'scale_stroke_width'):
            return item.scale_stroke_width(scale)
        # 兜底：QGraphicsTextItem（非 TextItem 子类）
        if isinstance(item, QGraphicsTextItem):
            self._scale_text_item(item, scale)
            return True
        return False

    def _scale_text_item(self, item: QGraphicsTextItem, scale: float):
        point_size = self._get_text_point_size(item)
        new_size = max(6.0, point_size * scale)
        self._set_text_item_point_size(item, new_size)
    
    def _get_text_point_size(self, item: QGraphicsTextItem) -> float:
        font = item.font()
        point_size = font.pointSizeF()
        if point_size <= 0:
            point_size = float(font.pointSize() or 12)
        return point_size

    def _set_text_item_point_size(self, item: QGraphicsTextItem, point_size: float):
        if not item:
            return
        font = item.font()
        font.setPointSizeF(max(6.0, float(point_size)))
        item.setFont(font)
        item.update()

    def _apply_text_point_size(
        self,
        active_item: Optional[QGraphicsTextItem],
        selected_item: Optional[QGraphicsTextItem],
        point_size: float,
    ):
        applied = False
        if active_item:
            self._set_text_item_point_size(active_item, point_size)
            applied = True
        if selected_item and selected_item is not active_item:
            self._set_text_item_point_size(selected_item, point_size)
            applied = True
        if applied:
            self.canvas_scene.update()

    def _apply_opacity_change_to_selection(self, opacity: float):
        controller = getattr(self, "smart_edit_controller", None)
        if not controller:
            return

        opacity = max(0.0, min(1.0, float(opacity)))

        updated = False
        active_text = self._get_active_text_item()
        if active_text:
            if self._update_item_visual_opacity(active_text, opacity):
                updated = True

        selected_item = getattr(controller, "selected_item", None)
        if selected_item and selected_item is not active_text:
            if self._update_item_visual_opacity(selected_item, opacity):
                updated = True

        if updated:
            self.canvas_scene.update()

    def _apply_line_style_change_to_selection(self, style: str):
        controller = getattr(self, "smart_edit_controller", None)
        if not controller:
            return

        selected_item = getattr(controller, "selected_item", None)
        # 只有画笔、矩形、椭圆有线型。按归属工具判断：荧光笔（含荧光笔矩形）、聚光灯的孔
        # 也是 StrokeItem/RectItem，按类名判断会给它们套上虚线
        if selected_item is None or controller.get_item_tool_id(selected_item) not in ("pen", "rect", "ellipse"):
            return

        from PySide6.QtCore import Qt
        pen = selected_item.pen()
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        if style == "dashed":
            pen.setStyle(Qt.PenStyle.DashLine)
            pen.setDashPattern([3, 2])
        elif style == "dashed_dense":
            pen.setStyle(Qt.PenStyle.DashLine)
            pen.setDashPattern([1, 2])
        else:
            pen.setStyle(Qt.PenStyle.SolidLine)
            pen.setDashPattern([])
        selected_item.setPen(pen)
        selected_item.update()
        self.canvas_scene.update()

    def _update_item_visual_opacity(self, item, opacity: float) -> bool:
        opacity = max(0.0, min(1.0, float(opacity)))

        # 优先使用统一接口
        if hasattr(item, 'set_visual_opacity'):
            return item.set_visual_opacity(opacity)

        # 兜底：通用 QGraphicsItem
        if hasattr(item, "setOpacity"):
            item.setOpacity(opacity)
            item.update()
            return True

        return False
    
    def _update_magnifier_overlay(self, scene_pos: QPointF):
        overlay = self._get_magnifier_overlay()
        if overlay:
            overlay.update_cursor(scene_pos)

    def _clear_magnifier_overlay(self):
        overlay = self._get_magnifier_overlay()
        if overlay:
            overlay.clear_cursor()

    def _get_magnifier_overlay(self):
        window = self.window()
        if window and hasattr(window, "magnifier_overlay"):
            return window.magnifier_overlay
        return None
    
    def _enter_text_edit_mode(self, item: TextItem):
        self._connect_text_geometry_updates(item)
        item.setTextInteractionFlags(Qt.TextInteractionFlag.TextEditorInteraction)
        item.setFocus(Qt.FocusReason.MouseFocusReason)
        cursor = item.textCursor()
        cursor.movePosition(cursor.MoveOperation.End)  # 光标移到末尾，不全选
        item.setTextCursor(cursor)
        if hasattr(self.smart_edit_controller, "select_item"):
            self.smart_edit_controller.select_item(item, auto_select=False)
    
    def _connect_text_geometry_updates(self, item: QGraphicsTextItem):
        """文字内容变化会改变包围盒，控制点得跟着重画。

        控制点已经移到独立浮层，浮层整层重画，所以这里只需要重算手柄位置并
        通知它；文字自身的重绘由 QGraphicsTextItem 的布局失效负责。
        """
        document = item.document()
        if document is None or getattr(item, "_geometry_update_connected", False):
            return

        def _on_contents_changed():
            if item.scene() is None:
                return
            self._update_edit_handles()

        document.contentsChanged.connect(_on_contents_changed)
        item._geometry_update_connected = True
        item._geometry_update_slot = _on_contents_changed

    def _disconnect_text_geometry_updates(self, item):
        slot = getattr(item, "_geometry_update_slot", None)
        if slot is None:
            return
        try:
            item.document().contentsChanged.disconnect(slot)
        except (RuntimeError, TypeError) as e:
            log_warning(T("断开文字几何更新失败: {exc}", exc=e), "CanvasView")
        item._geometry_update_connected = False
        item._geometry_update_slot = None

    def _end_text_edit(self, text_item: QGraphicsTextItem):
        """结束文字编辑：失焦让图元自己结算内容（入栈/丢弃/可撤销清空），再清理视图侧状态。"""
        log_debug(T("结束文本编辑"), "CanvasView")
        text_item.clearFocus()
        self._finalize_text_edit_state(text_item)

    def _finalize_text_edit_state(self, text_item: QGraphicsTextItem):
        if text_item is not None:
            self._disconnect_text_geometry_updates(text_item)
        controller = getattr(self, "smart_edit_controller", None)
        if controller and controller.selected_item is text_item:
            controller.clear_selection(suppress_block=True)
        self.text_drag.reset()
        self.pending_text_edit.clear()
    
    def export_and_close(self):
        """
        导出并关闭
        """
        from core.export import ExportService
        
        # 创建导出服务（传入整个scene）
        exporter = ExportService(self.canvas_scene)
        
        # 导出选区图像
        selection_rect = self.canvas_scene.selection_model.rect()
        log_debug(T("准备导出选区: {selection_rect}", selection_rect=selection_rect), module="CanvasView")
        
        result = exporter.export(selection_rect)
        
        if result:
            log_info(T("导出成功，图像大小: {width}x{height}", width=result.width(), height=result.height()), module="CanvasView")
            from core.clipboard_utils import deliver_image_async
            deliver_image_async(result)
            log_info(T("已完成复制到剪贴板"), module="CanvasView")
            self.window().close()
        else:
            log_error(T("导出失败！"), module="CanvasView")
