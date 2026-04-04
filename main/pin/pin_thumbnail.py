"""
钉图缩略图模式

负责缩略图模式的进入、退出、视图更新逻辑。
"""

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QCursor
from core import log_debug


class PinThumbnailMode:
    """
    钉图缩略图模式管理器

    状态数据和操作逻辑全部内聚在此类中，
    PinWindow 只需调用 toggle() 并在 resizeEvent 中调用 update_view()。
    """

    def __init__(self, pin_window):
        self._win = pin_window
        self._active = False
        self._size = 100               # 缩略图尺寸（像素）
        self._prev_geometry = None      # 进入前的窗口几何
        self._scene_center = None       # 缩略图显示的场景中心点

    # ------------------------------------------------------------------
    # 公开属性
    # ------------------------------------------------------------------

    @property
    def active(self) -> bool:
        return self._active

    @property
    def size(self) -> int:
        return self._size

    # ------------------------------------------------------------------
    # 切换
    # ------------------------------------------------------------------

    def toggle(self):
        """切换缩略图模式"""
        if not self._active:
            self._enter()
        else:
            self._exit()

    # ------------------------------------------------------------------
    # 进入缩略图模式
    # ------------------------------------------------------------------

    def _enter(self):
        win = self._win
        # 保存当前窗口几何
        self._prev_geometry = win.geometry()

        # 获取鼠标全局位置
        cursor_pos = QCursor.pos()
        window_rect = win.geometry()

        if window_rect.contains(cursor_pos):
            local_pos = win.mapFromGlobal(cursor_pos)
            scene_x = local_pos.x() / window_rect.width() * win._orig_size.width()
            scene_y = local_pos.y() / window_rect.height() * win._orig_size.height()
        else:
            scene_x = win._orig_size.width() / 2
            scene_y = win._orig_size.height() / 2
            cursor_pos = window_rect.center()

        self._scene_center = QPointF(scene_x, scene_y)

        size = self._size
        new_x = cursor_pos.x() - size // 2
        new_y = cursor_pos.y() - size // 2

        win.setGeometry(new_x, new_y, size, size)

        # 更新视图
        self.update_view()

        # 禁用 OCR 层
        if hasattr(win, '_ocr_mgr'):
            win._ocr_mgr.set_enabled(False)

        # 退出绘制/编辑状态
        if win.canvas and win.canvas.is_editing:
            win.canvas.deactivate_tool()
            # 同步工具栏按钮状态
            if win.toolbar and hasattr(win.toolbar, 'current_tool') and win.toolbar.current_tool:
                for btn in win.toolbar.tool_buttons.values():
                    btn.setChecked(False)
                win.toolbar.current_tool = None

        # 隐藏控制按钮和工具栏
        win._set_control_buttons_visible(False)
        if win.toolbar and win.toolbar.isVisible():
            # 先关闭二级设置面板（颜色、线宽等弹出面板）
            if hasattr(win.toolbar, '_hide_all_panels'):
                win.toolbar._hide_all_panels()
            win.toolbar.hide()

        self._active = True
        log_debug(f"进入缩略图模式，场景中心: ({scene_x:.1f}, {scene_y:.1f})", "PinWindow")

    # ------------------------------------------------------------------
    # 退出缩略图模式
    # ------------------------------------------------------------------

    def _exit(self):
        if not self._prev_geometry or not self._scene_center:
            self._active = False
            return

        win = self._win
        prev_size = self._prev_geometry.size()
        current_center = win.geometry().center()

        rel_x = self._scene_center.x() / win._orig_size.width()
        rel_y = self._scene_center.y() / win._orig_size.height()

        new_x = int(current_center.x() - rel_x * prev_size.width())
        new_y = int(current_center.y() - rel_y * prev_size.height())

        # 先重置标志，让 resizeEvent 正常处理
        self._active = False
        self._scene_center = None
        self._prev_geometry = None

        win.setGeometry(new_x, new_y, prev_size.width(), prev_size.height())
        win._update_view_transform()

        # 恢复 OCR 层
        if hasattr(win, '_ocr_mgr'):
            win._ocr_mgr.set_enabled(True)
            cr = win.content_rect()
            win._ocr_mgr.update_geometry(cr.toRect())

        # 恢复控制按钮
        win._set_control_buttons_visible(True)
        win.update_button_positions()

        log_debug("退出缩略图模式", "PinWindow")

    # ------------------------------------------------------------------
    # 视图更新（resizeEvent 中调用）
    # ------------------------------------------------------------------

    def update_view(self):
        """更新缩略图视图，显示以场景中心点为中心的区域"""
        win = self._win
        if not win.view or not self._scene_center:
            return

        half = self._size / 2
        scene_left = self._scene_center.x() - half
        scene_top = self._scene_center.y() - half

        max_left = max(0, win._orig_size.width() - self._size)
        max_top = max(0, win._orig_size.height() - self._size)
        scene_left = max(0, min(scene_left, max_left))
        scene_top = max(0, min(scene_top, max_top))

        # 更新场景中心（可能被限制了）
        self._scene_center = QPointF(scene_left + half, scene_top + half)

        win.view.resetTransform()
        view_rect = QRectF(scene_left, scene_top, self._size, self._size)
        win.view.fitInView(view_rect, Qt.AspectRatioMode.KeepAspectRatio)
 