"""截图窗口 - 核心事务控制

主程序的截图功能中枢，协调画布、工具栏、选区面板、遮罩等组件。
实现窗口复用、会话存活周期管理、工具信号转发、导出触发等功能。
"""

import sys
import os
import gc
from datetime import datetime
from PySide6.QtWidgets import QApplication, QWidget, QGraphicsTextItem
from PySide6.QtCore import Qt, QTimer, QRect
from PySide6.QtGui import QPixmap, QImage
from ui.dialogs import show_modeless_warning_dialog, show_warning_dialog

from canvas import CanvasScene, CanvasView
from capture.capture_service import CaptureService
from ui.toolbar import Toolbar
from ui.magnifier import MagnifierOverlay
from ui.mask_overlay import MaskOverlayWidget
from ui.selection_info import SelectionInfoPanel, SelectionInfoController
from tools.action import ActionTools
from settings import get_tool_settings_manager
from stitch.scroll_window import ScrollCaptureWindow
from core.logger import log_debug, log_info, log_warning, log_error, log_exception
from core import safe_event
from core.shortcut_manager import ShortcutManager, ShortcutHandler


class ScreenshotShortcutHandler(ShortcutHandler):
    """截图窗口快捷键处理器 - 优先级最高(100)"""

    def __init__(self, window: 'ScreenshotWindow'):
        self._window = window
        # 从配置读取应用内快捷键（一次性，截图窗口生命周期内不变）
        from core.shortcut_manager import load_inapp_bindings, load_move_keys
        self._bindings = load_inapp_bindings([
            "inapp_confirm", "inapp_pin", "inapp_undo", "inapp_redo",
            "inapp_zoom_in", "inapp_zoom_out", "inapp_translate",
        ])
        self._move_keys = load_move_keys()

    @property
    def priority(self) -> int:
        return 100

    @property
    def handler_name(self) -> str:
        return "ScreenshotWindow"

    def is_active(self) -> bool:
        w = self._window
        return (w is not None
                and not getattr(w, '_is_closing', True)
                and w.isVisible())

    def _match(self, event, cfg_key: str) -> bool:
        """检查按键事件是否匹配某个绑定"""
        binding = self._bindings.get(cfg_key)
        if not binding:
            return False
        want_key, want_mods = binding
        return event.key() == want_key and event.modifiers() == want_mods

    def handle_key(self, event) -> bool:
        w = self._window
        is_text_editing = w._is_text_editing()

        # 文字编辑模式下，部分按键交给 QGraphicsTextItem
        if is_text_editing:
            if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
                return False
            if event.key() in (Qt.Key.Key_C, Qt.Key.Key_D):
                return False
            if (event.key() in (Qt.Key.Key_Z, Qt.Key.Key_Y)
                    and event.modifiers() == Qt.KeyboardModifier.ControlModifier):
                return False

        # ── 鼠标微移 ──
        if not is_text_editing:
            delta = self._move_keys.get(event.key())
            if delta and event.modifiers() == Qt.KeyboardModifier.NoModifier:
                from PySide6.QtGui import QCursor
                p = QCursor.pos()
                QCursor.setPos(p.x() + delta[0], p.y() + delta[1])
                return True

        # ESC — 固定不可自定义
        if event.key() == Qt.Key.Key_Escape:
            w.cleanup_and_close()
            return True

        # 确认截图
        if self._match(event, "inapp_confirm"):
            if w.scene and w.scene.selection_model.is_confirmed:
                w.action_handler.handle_confirm()
                return True

        # 钉图
        if self._match(event, "inapp_pin"):
            if w.scene and w.scene.selection_model.is_confirmed:
                w.action_handler.handle_pin()
                return True

        # 撤销
        if self._match(event, "inapp_undo"):
            if w.scene and w.scene.undo_stack.canUndo():
                w.scene.undo_stack.undo()
            return True

        # 重做
        if self._match(event, "inapp_redo"):
            if w.scene and w.scene.undo_stack.canRedo():
                w.scene.undo_stack.redo()
            return True

        # 截图翻译
        if self._match(event, "inapp_translate"):
            if w.scene and w.scene.selection_model.is_confirmed:
                if hasattr(w, 'toolbar') and w.toolbar:
                    w.toolbar.screenshot_translate_clicked.emit()
                return True

        # 取色（单键 C，无修饰键 — 保留硬编码）
        if event.key() == Qt.Key.Key_C:
            if event.modifiers() == Qt.KeyboardModifier.NoModifier:
                mo = getattr(w, 'magnifier_overlay', None)
                if mo and mo.cursor_scene_pos is not None and mo._should_render():
                    if mo.copy_color_info():
                        w.cleanup_and_close()
                        return True

        # Enter 确认（固定）
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            if w.scene and w.scene.selection_model.is_confirmed:
                w.action_handler.handle_confirm()
                return True

        # 放大镜缩放
        if self._match(event, "inapp_zoom_in"):
            mo = getattr(w, 'magnifier_overlay', None)
            if mo and mo.cursor_scene_pos is not None and mo._should_render():
                mo.adjust_zoom(1)
                return True

        if self._match(event, "inapp_zoom_out"):
            mo = getattr(w, 'magnifier_overlay', None)
            if mo and mo.cursor_scene_pos is not None and mo._should_render():
                mo.adjust_zoom(-1)
                return True

        return False


class ScreenshotWindow(QWidget):
    def __init__(self, config_manager=None, prefetched_image=None, prefetched_rect=None):
        super().__init__()
        
        # 不设置 WA_DeleteOnClose —— 窗口复用，由 main_app 管理生命周期
        
        # Config manager for auto-save settings
        self.config_manager = config_manager if config_manager else get_tool_settings_manager()
        # 关闭标记，防止已销毁窗口继续响应回调
        self._is_closing = False
        self._session_active = False
        
        import time
        _t0 = time.perf_counter()
        _timings = {}   # 收集各阶段耗时，最后统一输出
        
        # 1. 获取屏幕截图：优先使用调用方预取的图像（已在后台线程截好），否则同步截图
        if prefetched_image is not None and prefetched_rect is not None:
            self.original_image = prefetched_image
            rect = prefetched_rect
            _timings['截屏'] = 0.0  # 预取，主线程耗时为 0
        else:
            capture_service = CaptureService()
            self.original_image, rect = capture_service.capture_all_screens()
            _timings['截屏'] = (time.perf_counter() - _t0) * 1000
        
        _t1 = time.perf_counter()
        
        self.virtual_x = rect.x()
        self.virtual_y = rect.y()
        self.virtual_width = rect.width()
        self.virtual_height = rect.height()
        
        log_debug(f"虚拟桌面: {self.virtual_width}x{self.virtual_height} at ({self.virtual_x}, {self.virtual_y})", "ScreenshotWindow")
        log_debug(f"图像尺寸: {self.original_image.width()}x{self.original_image.height()}", "ScreenshotWindow")

        # 2. 窗口属性 & 几何形状（必须在创建子控件之前完成）
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint | Qt.WindowType.Tool)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        self.setGeometry(int(self.virtual_x), int(self.virtual_y), int(self.virtual_width), int(self.virtual_height))

        # 3. 初始化场景和视图
        self.scene = CanvasScene(self.original_image, rect)
        self.view = CanvasView(self.scene, self)
        self.view.setGeometry(0, 0, int(self.virtual_width), int(self.virtual_height))
        
        _t2 = time.perf_counter()
        _timings['Scene+View'] = (_t2 - _t1) * 1000
        
        # 启用智能选区（从配置读取）
        self.smart_selection_enabled = self.config_manager.get_smart_selection()
        self.view.enable_smart_selection(self.smart_selection_enabled)
        
        # 4. 初始化工具栏（一次性创建，后续复用）
        self.toolbar = Toolbar(self)
        self.toolbar.hide() # 初始隐藏，选区确认后显示
        
        _t3 = time.perf_counter()
        _timings['Toolbar'] = (_t3 - _t2) * 1000
        
        # 5. 创建ActionTools来处理工具栏按钮逻辑
        self.action_handler = ActionTools(
            scene=self.scene,
            config_manager=self.config_manager,
            parent_window=self
        )

        # 6. 遮罩叠层（QWidget），覆盖整个窗口，位于 View 之上
        self.mask_overlay = MaskOverlayWidget(self, self.scene.selection_model)
        self.mask_overlay.setGeometry(0, 0, int(self.virtual_width), int(self.virtual_height))

        _t4 = time.perf_counter()
        _timings['Action+Mask'] = (_t4 - _t3) * 1000

        # 6.5 选区信息面板（坐标/尺寸 + 圆角/锁定比例等快捷按钮）
        self.info_panel = SelectionInfoPanel(self, self.view)
        self.info_controller = SelectionInfoController(
            panel=self.info_panel,
            selection_model=self.scene.selection_model,
            selection_item=self.scene.selection_item,
            export_service=self.action_handler.export_service,
            mask_overlay=self.mask_overlay,
            parent_widget=self,
            config_manager=self.config_manager,
        )

        _t5 = time.perf_counter()
        _timings['InfoPanel'] = (_t5 - _t4) * 1000

        # 7. 叠加鼠标放大镜，复刻老版 UI 的色彩信息视图
        self.magnifier_overlay = MagnifierOverlay(self, self.scene, self.view, self.config_manager)
        # 放大镜现在是独立小浮层，无需 setGeometry(self.rect())
        
        # 工具栏初始位置（底部居中）
        self.update_toolbar_position()
        
        # 6. 连接信号（一次性 toolbar→self + 本次 session 信号）
        self._connect_toolbar_signals()
        self._connect_session_signals()

        _t6 = time.perf_counter()
        _timings['Magnifier+信号'] = (_t6 - _t5) * 1000
        _timings['总计'] = (_t6 - _t0) * 1000

        # ── 统一输出初始化计时 ──
        parts = [f"{k}={v:.1f}ms" for k, v in _timings.items()]
        log_debug(f"[计时] 初始化完成 | {' | '.join(parts)}", "ScreenshotWindow")
        
        # 截图期间压制 pin 窗口的置顶，避免同级 TopMost 窗口抢 Z-order
        from pin.pin_manager import PinManager
        PinManager.instance().suppress_topmost()
        
        # 注册全局快捷键处理器（优先级最高）
        self._shortcut_handler = ScreenshotShortcutHandler(self)
        ShortcutManager.instance().register(self._shortcut_handler)
        
        self._session_active = True
        self.show()
        self.activateWindow()
        self.raise_()
        self.setFocus()
        
        # Ensure focus after a short delay (workaround for Windows focus stealing prevention)
        QTimer.singleShot(50, self._safe_activate_and_focus)
        
        # 初始状态：进入选区模式
        # CanvasView 默认处理鼠标按下进入选区

    # ------------------------------------------------------------------
    # 窗口复用：准备新的截图会话
    # ------------------------------------------------------------------
    def prepare_new_session(self, prefetched_image, prefetched_rect):
        """复用已有窗口，准备新一次截图会话。
        
        释放旧会话的重数据（~66MB 图像），保留轻量 UI 壳（toolbar/magnifier 等），
        然后用新截图数据重建 Scene/View，节省 ~250ms 创建耗时。
        """
        import time
        _t0 = time.perf_counter()
        
        self._is_closing = False
        
        image = prefetched_image
        rect = prefetched_rect
        
        self.original_image = image
        self.virtual_x = rect.x()
        self.virtual_y = rect.y()
        self.virtual_width = rect.width()
        self.virtual_height = rect.height()
        
        # 更新窗口几何形状（多显示器可能变化）
        self.setGeometry(int(self.virtual_x), int(self.virtual_y),
                         int(self.virtual_width), int(self.virtual_height))
        
        # 创建新的 Scene + View（每次截图内容不同，不可复用）
        self.scene = CanvasScene(image, rect)
        self.view = CanvasView(self.scene, self)
        self.view.setGeometry(0, 0, int(self.virtual_width), int(self.virtual_height))
        self.view.lower()  # 确保 view 在最底层，overlay 在上方
        
        self.smart_selection_enabled = self.config_manager.get_smart_selection()
        self.view.enable_smart_selection(self.smart_selection_enabled)
        
        # 创建新的 ActionHandler（引用新 scene）
        self.action_handler = ActionTools(
            scene=self.scene,
            config_manager=self.config_manager,
            parent_window=self
        )
        
        # 复用 mask_overlay —— 重新绑定到新的 selection_model
        self.mask_overlay.rebind_model(self.scene.selection_model)
        self.mask_overlay.setGeometry(0, 0, int(self.virtual_width), int(self.virtual_height))
        self.mask_overlay.raise_()
        
        # 复用 info_panel —— swap view 引用，重建 controller
        self.info_panel._view = self.view
        self.info_controller = SelectionInfoController(
            panel=self.info_panel,
            selection_model=self.scene.selection_model,
            selection_item=self.scene.selection_item,
            export_service=self.action_handler.export_service,
            mask_overlay=self.mask_overlay,
            parent_widget=self,
            config_manager=self.config_manager,
        )
        
        # 复用 magnifier_overlay —— 重新指向新 scene/view
        self.magnifier_overlay.rebind(self.scene, self.view)
        
        # 复用 toolbar —— 重置状态
        self.toolbar.reset_session_state()
        self.toolbar.raise_()
        
        # 连接本次会话的信号
        self._connect_session_signals()
        
        # 工具栏定位
        self.update_toolbar_position()
        
        # 压制 pin 窗口
        from pin.pin_manager import PinManager
        PinManager.instance().suppress_topmost()
        
        # 注册快捷键
        self._shortcut_handler = ScreenshotShortcutHandler(self)
        ShortcutManager.instance().register(self._shortcut_handler)
        
        self._session_active = True
        self.show()
        self.activateWindow()
        self.raise_()
        self.setFocus()
        QTimer.singleShot(50, self._safe_activate_and_focus)
        
        _elapsed = (time.perf_counter() - _t0) * 1000
        log_debug(f"[计时] 复用窗口会话准备完成 | 耗时={_elapsed:.1f}ms", "ScreenshotWindow")

    # ------------------------------------------------------------------
    # 会话结束：释放重数据，保留 UI 壳
    # ------------------------------------------------------------------
    def _teardown_session(self):
        """释放本次截图会话的重数据（~66MB 图像 + scene/view），
        保留 toolbar/magnifier/mask_overlay/info_panel 等可复用的轻量 UI 壳。
        """
        if not self._session_active:
            return
        self._session_active = False
        self._is_closing = True
        
        # 立即隐藏窗口
        self.hide()
        
        log_debug("开始释放截图会话资源（保留 UI 壳）", "ScreenshotWindow")
        
        # 恢复窗口对截图 API 的可见性
        if getattr(self, '_exclude_from_capture_set', False):
            self._set_exclude_from_capture(False)
            self._exclude_from_capture_set = False
        
        # 注销快捷键处理器
        if hasattr(self, '_shortcut_handler') and self._shortcut_handler:
            ShortcutManager.instance().unregister(self._shortcut_handler)
            self._shortcut_handler._window = None
            self._shortcut_handler = None
        
        # 恢复 pin 窗口
        try:
            from pin.pin_manager import PinManager
            PinManager.instance().restore_topmost()
        except Exception as e:
            log_exception(e, "恢复 pin 窗口")
        
        # 断开 toolbar → smart_edit_controller 的会话信号
        # （toolbar 是持久对象，不 disconnect 会导致信号累积，每次截图多加一个 handler）
        self._disconnect_session_signals()
        
        # 停止定时器
        if hasattr(self, 'visibility_timer') and self.visibility_timer:
            self.visibility_timer.stop()
            self.visibility_timer.deleteLater()
            self.visibility_timer = None
        
        # 释放大图片内存
        if hasattr(self, 'original_image'):
            self.original_image = None
        
        # 清空放大镜缓存（保留 widget）
        if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
            self.magnifier_overlay.rebind(None, None)
        
        # 销毁 info_controller（复杂，必须重建）
        if hasattr(self, 'info_controller') and self.info_controller:
            self.info_controller.cleanup()
            self.info_controller = None
        
        # 重置 info_panel 的 checkable 按钮状态，防止下次会话遗留
        if hasattr(self, 'info_panel') and self.info_panel:
            for btn in (self.info_panel.btn_rounded, self.info_panel.btn_lock, self.info_panel.btn_border):
                btn.setChecked(False)
            self.info_panel.set_confirmed(False)
            self.info_panel.hide()
        
        # 清理 Scene
        if hasattr(self, 'scene') and self.scene:
            if hasattr(self.scene, 'background') and self.scene.background:
                self.scene.background.release_image_cache()
                self.scene.background.setPixmap(QPixmap())
            if hasattr(self.scene, 'tool_controller'):
                self.scene.tool_controller = None
            if hasattr(self.scene, 'undo_stack'):
                self.scene.undo_stack.clear()
            self.scene._layer_editor = None
            # 释放所有图元的大图内存（Pixmap），然后交给 deleteLater 统一销毁
            for item in self.scene.items():
                if hasattr(item, 'setPixmap'):
                    item.setPixmap(QPixmap())
            self.scene.deleteLater()
            self.scene = None
        
        # 清理 View
        if hasattr(self, 'view') and self.view:
            if hasattr(self.view, 'smart_edit_controller'):
                sec = self.view.smart_edit_controller
                sec.selected_item = None
                sec.hovered_item = None
                sec.scene = None
                if sec.layer_editor:
                    sec.layer_editor.stop_edit()
                sec.layer_editor = None
            if hasattr(self.view, 'cursor_manager'):
                self.view.cursor_manager = None
            if hasattr(self.view, 'window_finder') and self.view.window_finder:
                self.view.window_finder.clear()
                self.view.window_finder = None
            self.view.setScene(None)
            self.view.setParent(None)
            self.view.deleteLater()
            self.view = None
        
        # 清理 ActionHandler
        if hasattr(self, 'action_handler') and self.action_handler:
            self.action_handler.scene = None
            self.action_handler.parent_window = None
            self.action_handler.export_service = None
            self.action_handler.save_service = None
            self.action_handler = None
        
        gc.collect()
        log_info("截图会话资源释放完成", "ScreenshotWindow")

    def _disconnect_session_signals(self):
        """断开本次会话连接到持久 toolbar 上的信号。
        
        _connect_session_signals() 每次会话把 toolbar 信号连到新的
        smart_edit_controller 上；如果不在 teardown 中 disconnect，
        信号会累积——第 N 次截图时触发 N 个 handler。
        """
        if not hasattr(self, 'view') or not self.view:
            return
        controller = getattr(self.view, 'smart_edit_controller', None)
        if not controller:
            return
        from core.qt_utils import safe_disconnect
        safe_disconnect(self.toolbar.text_font_changed, controller.on_text_font_changed)
        safe_disconnect(self.toolbar.color_changed, controller.on_text_color_changed)
        if hasattr(self.toolbar, 'text_background_changed'):
            safe_disconnect(self.toolbar.text_background_changed, controller.on_text_background_changed)

    # ------------------------------------------------------------------
    # 窗口截图可见性控制
    # ------------------------------------------------------------------
    def _set_exclude_from_capture(self, exclude: bool):
        """设置本窗口是否对屏幕截图 API 不可见。
        
        使用 Windows 10 2004+ 的 WDA_EXCLUDEFROMCAPTURE 标志，
        让 mss / BitBlt / DXGI 等截图 API 在捕获屏幕时跳过本窗口。
        窗口在屏幕上仍然正常显示，用户看得到。
        """
        from core.platform_utils import set_window_exclude_from_capture, get_last_error
        hwnd = int(self.winId())
        result = set_window_exclude_from_capture(hwnd, exclude)
        if result:
            log_debug(f"SetWindowDisplayAffinity({'EXCLUDE' if exclude else 'NONE'}) 成功", "ScreenshotWindow")
        else:
            log_debug(f"SetWindowDisplayAffinity 失败, GetLastError={get_last_error()}", "ScreenshotWindow")

    def _connect_toolbar_signals(self):
        """连接工具栏信号（一次性，toolbar→self 的稳定连接）。
        
        注意：toolbar→action_handler 的信号用 wrapper 方法间接调用，
        因为 action_handler 每次会话都会重建。
        """
        # 工具切换
        self.toolbar.tool_changed.connect(self.on_tool_changed)
        
        # 样式改变
        self.toolbar.color_changed.connect(self.on_color_changed)
        self.toolbar.stroke_width_changed.connect(self.on_stroke_width_changed)
        self.toolbar.opacity_changed.connect(self.on_opacity_changed)
        
        # 箭头/线条/序号样式为self方法，安全
        self.toolbar.arrow_style_changed.connect(self.on_arrow_style_changed)
        self.toolbar.line_style_changed.connect(self.on_line_style_changed)
        if hasattr(self.toolbar, "number_next_changed"):
            self.toolbar.number_next_changed.connect(self.on_number_next_changed)
        
        # 操作按钮 → wrapper 方法（间接调用 action_handler）
        self.toolbar.undo_clicked.connect(self.on_undo)
        self.toolbar.redo_clicked.connect(self.on_redo)
        self.toolbar.confirm_clicked.connect(self._handle_confirm)
        self.toolbar.cancel_clicked.connect(self.cleanup_and_close)
        self.toolbar.copy_clicked.connect(self._handle_copy)
        self.toolbar.save_clicked.connect(self._handle_save)
        self.toolbar.pin_clicked.connect(self._handle_pin)
        self.toolbar.long_screenshot_clicked.connect(self.start_long_screenshot_mode)
        self.toolbar.screenshot_translate_clicked.connect(self._handle_screenshot_translate)
        self.toolbar.gif_record_clicked.connect(self.start_gif_record_mode)

    def _connect_session_signals(self):
        """连接每次会话的信号（scene→self + toolbar→view.smart_edit_controller）。"""
        # 场景信号
        self.scene.selectionConfirmed.connect(self.on_selection_confirmed)
        self.scene.selection_model.rectChanged.connect(self.update_toolbar_position)
        self.scene.selection_model.draggingChanged.connect(self.on_selection_dragging_changed)
        
        # 文字工具信号连接（新 view 的 controller）
        if hasattr(self.view, 'smart_edit_controller'):
            controller = self.view.smart_edit_controller
            self.toolbar.text_font_changed.connect(controller.on_text_font_changed)
            self.toolbar.color_changed.connect(controller.on_text_color_changed)
            if hasattr(self.toolbar, 'text_background_changed'):
                self.toolbar.text_background_changed.connect(controller.on_text_background_changed)

    # -- action_handler wrapper 方法（toolbar 信号的稳定接收端）--
    def _handle_confirm(self):
        if self.action_handler:
            self.action_handler.handle_confirm()

    def _handle_copy(self):
        if self.action_handler:
            self.action_handler.handle_copy()

    def _handle_save(self):
        if self.action_handler:
            self.action_handler.handle_save()

    def _handle_pin(self):
        if self.action_handler:
            self.action_handler.handle_pin()

    def _handle_screenshot_translate(self):
        if self.action_handler:
            self.action_handler.handle_screenshot_translate()

    def _safe_activate_and_focus(self):
        """避免已销毁窗口执行激活/聚焦导致崩溃"""
        if getattr(self, "_is_closing", False):
            return
        try:
            import shiboken6
            if not shiboken6.isValid(self):
                return
        except Exception as e:
            log_exception(e, "shiboken6 有效性检查")
        try:
            if not self.isVisible():
                return
            self.activateWindow()
            self.setFocus()
        except RuntimeError:
            # 窗口已被删除或无效
            return

    def on_selection_dragging_changed(self, is_dragging: bool):
        """
        选区拖拽状态改变时的处理
        拖拽时隐藏工具栏，结束拖拽后显示并更新位置
        """
        if self._is_closing:
            return
        if not hasattr(self, 'toolbar'):
            return
            
        if is_dragging:
            # 开始拖拽：隐藏工具栏以减少重绘
            self.toolbar.hide()
            # 同时隐藏二级菜单
            if hasattr(self.toolbar, 'paint_menu') and self.toolbar.paint_menu.isVisible():
                self.toolbar.paint_menu.hide()
        else:
            # 结束拖拽：显示工具栏并更新位置
            if self.scene.selection_model.is_confirmed:
                self.toolbar.show()
                self.toolbar.raise_()
                self.update_toolbar_position()

    def on_selection_confirmed(self):
        if self._is_closing:
            return
        
        # 选区确认后，显示工具栏
        self.toolbar.show()
        self.toolbar.raise_()  # 只提升到顶层，不激活窗口
        self.update_toolbar_position()
        if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
            self.magnifier_overlay.refresh()
        
        # 确保主窗口保持焦点
        self.activateWindow()
        self.setFocus()

    @safe_event
    def showEvent(self, event):
        """
        窗口显示事件 - 在窗口首次显示时初始化智能选区和放大镜
        """
        super().showEvent(event)

        if self._is_closing:
            return
        
        # 智能选区：根据当前鼠标位置立即显示选区预览
        if self.smart_selection_enabled:
            self._init_smart_selection_at_cursor()
        
        # 放大镜：在当前鼠标位置初始化
        self._init_magnifier_at_cursor()

    def _init_smart_selection_at_cursor(self):
        """根据当前鼠标位置初始化智能选区预览，在窗口显示后立即调用"""
        from PySide6.QtGui import QCursor
        from PySide6.QtCore import QPointF
        
        cursor_pos = QCursor.pos()
        scene_pos = QPointF(cursor_pos.x(), cursor_pos.y())
        
        if hasattr(self.view, '_get_smart_selection_rect'):
            smart_rect = self.view._get_smart_selection_rect(scene_pos)
            if not smart_rect.isEmpty():
                self.scene.selection_model.activate()
                self.scene.selection_model.set_rect(smart_rect)
                log_debug(f"智能选区初始化: 鼠标位置({cursor_pos.x()}, {cursor_pos.y()}) -> 选区{smart_rect}", "ScreenshotWindow")

    def _init_magnifier_at_cursor(self):
        """在当前鼠标位置初始化放大镜，在窗口显示后立即调用"""
        from PySide6.QtGui import QCursor
        from PySide6.QtCore import QPointF
        
        if not (hasattr(self, 'magnifier_overlay') and self.magnifier_overlay):
            return
        cursor_pos = QCursor.pos()
        self.magnifier_overlay.update_cursor(QPointF(cursor_pos.x(), cursor_pos.y()))
        log_debug(f"放大镜初始化: 位置({cursor_pos.x()}, {cursor_pos.y()})", "ScreenshotWindow")

    @safe_event
    def resizeEvent(self, event):
        if self._is_closing:
            return
        self.view.setGeometry(self.rect())
        self.mask_overlay.setGeometry(self.rect())
        # 放大镜是独立小浮层，无需在 resizeEvent 中同步尺寸
        if self.scene.selection_model.is_confirmed:
            self.update_toolbar_position()
        super().resizeEvent(event)
        
    def update_toolbar_position(self):
        """更新工具栏位置 - 完全参考老代码的逻辑"""
        if self._is_closing:
            return
        if not hasattr(self, 'toolbar') or not self.toolbar.isVisible():
            return
            
        rect = self.scene.selection_model.rect()
        if rect.isEmpty():
            return
            
        # 将场景坐标转换为视图坐标
        view_polygon = self.view.mapFromScene(rect)
        view_rect = view_polygon.boundingRect()
        
        # 使用 View 作为父窗口进行坐标转换
        self.toolbar.position_near_rect(view_rect, self.view)
        
        # 如果二级菜单可见，也更新其位置（但不重复调用 show_paint_menu）
        if hasattr(self.toolbar, 'paint_menu') and self.toolbar.paint_menu.isVisible():
            # 直接更新二级菜单位置，不重新显示
            toolbar_pos = self.toolbar.pos()
            menu_x = toolbar_pos.x()
            menu_y = toolbar_pos.y() + self.toolbar.height() + 5
            
            # 检查屏幕边界
            screen = QApplication.screenAt(toolbar_pos)
            if screen:
                screen_rect = screen.geometry()
                if menu_y + self.toolbar.paint_menu.height() > screen_rect.y() + screen_rect.height():
                    menu_y = toolbar_pos.y() - self.toolbar.paint_menu.height() - 5
                if menu_x + self.toolbar.paint_menu.width() > screen_rect.x() + screen_rect.width():
                    menu_x = screen_rect.x() + screen_rect.width() - self.toolbar.paint_menu.width() - 5
            
            if self.toolbar.paint_menu.pos().x() != menu_x or self.toolbar.paint_menu.pos().y() != menu_y:
                self.toolbar.paint_menu.move(menu_x, menu_y)
    
    def cleanup_and_close(self):
        """结束当前截图会话 - 释放重数据，保留 UI 壳供下次复用。"""
        self._teardown_session()
        # 释放工作集（去抖：多次快速截图只触发最后一次，避免 page fault 风暴）
        from core.platform_utils import request_trim_working_set
        request_trim_working_set(1500)

    def full_destroy(self):
        """完全销毁窗口（应用退出时调用）。"""
        self._teardown_session()
        
        # 销毁所有缓存的 UI 壳
        if hasattr(self, 'toolbar') and self.toolbar:
            panel_names = ['paint_panel', 'shape_panel', 'arrow_panel', 'number_panel', 'text_panel']
            for panel_name in panel_names:
                panel = getattr(self.toolbar, panel_name, None)
                if panel:
                    panel.close()
                    panel.deleteLater()
            self.toolbar.close()
            self.toolbar.deleteLater()
            self.toolbar = None
        
        if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
            self.magnifier_overlay.deleteLater()
            self.magnifier_overlay = None
        
        if hasattr(self, 'mask_overlay') and self.mask_overlay:
            self.mask_overlay.deleteLater()
            self.mask_overlay = None
        
        if hasattr(self, 'info_panel') and self.info_panel:
            self.info_panel.deleteLater()
            self.info_panel = None
        
        gc.collect()
        self.close()
        self.deleteLater()

    @safe_event
    def keyPressEvent(self, event):
        """
        键盘事件处理 - 快捷键已委托给 ShortcutManager，
        此处仅保留文字编辑模式的事件传递。
        """
        super().keyPressEvent(event)

    def _is_text_editing(self) -> bool:
        focus_item = self.scene.focusItem() if hasattr(self.scene, 'focusItem') else None
        if isinstance(focus_item, QGraphicsTextItem) and focus_item.hasFocus():
            flags = focus_item.textInteractionFlags()
            return bool(flags & Qt.TextInteractionFlag.TextEditorInteraction)
        return False
    # --- 槽函数 ---

    def on_tool_changed(self, tool_id):
        """工具切换 - 确保主窗口保持焦点"""
        self.scene.activate_tool(tool_id)
        
        # 同步 UI：工具激活后，其设置已加载到 ToolContext，现在同步到工具栏 UI
        ctx = self.scene.tool_controller.ctx
        
        # 阻止工具栏信号，避免 set_xxx() 触发回调形成循环
        self.toolbar.blockSignals(True)
        try:
            # 更新工具栏 UI 显示当前工具的设置
            self.toolbar.set_current_color(ctx.color)
            self.toolbar.set_stroke_width(ctx.stroke_width)
            self.toolbar.set_opacity(int(ctx.opacity * 255))
            if tool_id == "number" and hasattr(self.toolbar, "set_number_next_value"):
                from tools.number import NumberTool
                self.toolbar.set_number_next_value(NumberTool.get_next_number(self.scene))
        finally:
            self.toolbar.blockSignals(False)
        
        # 切换工具后，将焦点还给 View（确保快捷键可用）
        from PySide6.QtCore import QTimer
        QTimer.singleShot(0, self.view.setFocus)
        if hasattr(self, 'magnifier_overlay') and self.magnifier_overlay:
            self.magnifier_overlay.refresh()

    def on_number_next_changed(self, next_value: int):
        """序号工具下一数字变化 - 更新场景偏移和光标预览"""
        from tools.number import NumberTool

        actual = NumberTool.set_next_number(self.scene, next_value)
        if hasattr(self.toolbar, "set_number_next_value"):
            self.toolbar.set_number_next_value(actual)

        view = getattr(self, "view", None)
        if view and hasattr(view, "cursor_manager"):
            view.cursor_manager.set_tool_cursor("number", force=True)
        
    def on_color_changed(self, color):
        self.scene.update_style(color=color)
        
    def on_stroke_width_changed(self, width):
        ctx = getattr(self.scene.tool_controller, 'ctx', None)
        prev_width = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)
        self.scene.update_style(width=width)
        new_width = max(1.0, float(getattr(ctx, 'stroke_width', width))) if ctx else float(width)

        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_size_change_to_selection') and prev_width > 0:
            scale = new_width / prev_width
            if abs(scale - 1.0) > 1e-6:
                view._apply_size_change_to_selection(scale)

        if view and hasattr(view, 'cursor_manager'):
            view.cursor_manager.update_tool_cursor_size(int(width))
        
    def on_opacity_changed(self, opacity_int):
        # opacity_int 是 0-255，转换为 0.0-1.0
        opacity = opacity_int / 255.0
        self.scene.update_style(opacity=opacity)
        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_opacity_change_to_selection'):
            view._apply_opacity_change_to_selection(opacity)

    def on_arrow_style_changed(self, style: str):
        """箭头样式变化 - 更新当前选中的箭头图元"""
        
        # 获取 SmartEditController 中选中的图元
        if not hasattr(self.view, 'smart_edit_controller'):
            return
        
        controller = self.view.smart_edit_controller
        item = controller.selected_item
        
        # 检查是否是箭头图元
        from canvas.items import ArrowItem
        if isinstance(item, ArrowItem):
            if hasattr(item, 'arrow_style'):
                # 记录旧状态用于撤销
                old_state = self._capture_arrow_state(item)
                
                # 应用新样式
                item.arrow_style = style
                
                # 记录新状态
                new_state = self._capture_arrow_state(item)
                
                # 推送撤销命令
                from canvas.undo import EditItemCommand
                undo_stack = getattr(self.scene, 'undo_stack', None)
                if undo_stack:
                    command = EditItemCommand(item, old_state, new_state, "修改箭头样式")
                    undo_stack.push(command)
                
                item.update()
                log_debug(f"箭头样式已更新: {style}", "ScreenshotWindow")

    def on_line_style_changed(self, style: str):
        """线条样式变化 - 更新当前选中的画笔图元"""
        log_debug(f"line style change -> {style}", "ScreenshotWindow")
        view = getattr(self, 'view', None)
        if view and hasattr(view, '_apply_line_style_change_to_selection'):
            view._apply_line_style_change_to_selection(style)
            log_debug(f"线条样式已更新: {style}", "ScreenshotWindow")

    def _capture_arrow_state(self, item) -> dict:
        """捕获箭头图元的状态"""
        from PySide6.QtCore import QPointF
        state = {}
        if hasattr(item, 'start_pos'):
            state['start'] = QPointF(item.start_pos)
        if hasattr(item, 'end_pos'):
            state['end'] = QPointF(item.end_pos)
        if hasattr(item, '_control_pos'):
            state['control'] = QPointF(item._control_pos)
        if hasattr(item, '_control_modified'):
            state['control_modified'] = item._control_modified
        if hasattr(item, '_arrow_style'):
            state['arrow_style'] = item._arrow_style
        return state

    def on_undo(self):
        """撤销"""
        if self.scene.undo_stack.canUndo():
            self.scene.undo_stack.undo()
        
    def on_redo(self):
        """重做"""
        if self.scene.undo_stack.canRedo():
            self.scene.undo_stack.redo()
    
    def start_gif_record_mode(self):
        """启动 GIF 录制模式"""
        log_info("启动GIF录制模式", "ScreenshotWindow")
        
        if self.scene.selection_model.is_confirmed:
            selection_rect = self.scene.selection_model.rect()
            
            real_x = int(selection_rect.x())
            real_y = int(selection_rect.y())
            real_width = int(selection_rect.width())
            real_height = int(selection_rect.height())
            
            capture_rect = QRect(real_x, real_y, real_width, real_height)
            log_debug(f"GIF录制区域: x={real_x}, y={real_y}, w={real_width}, h={real_height}", "ScreenshotWindow")
            
            from gif import GifRecordWindow
            
            # 若已有旧的录制窗口（单例），先关闭并释放资源
            app = QApplication.instance()
            old_win = getattr(app, "_gif_window", None)
            if old_win is not None:
                try:
                    old_win.close_all()
                except Exception as e:
                    log_exception(e, "关闭旧 GIF 窗口")
                app._gif_window = None

            gif_win = GifRecordWindow(capture_rect)
            log_debug(f"GifRecordWindow已创建, overlay visible={gif_win._overlay.isVisible()}", "ScreenshotWindow")
            
            # 把引用挂到 QApplication，防止截图窗口销毁后被 GC
            app = QApplication.instance()
            app._gif_window = gif_win
            
            # 窗口关闭后自动清除全局引用，释放内存
            gif_win.destroyed.connect(lambda: setattr(app, '_gif_window', None))
            log_info("GIF录制窗口已启动", "ScreenshotWindow")
            
            # 关闭截图窗口
            self.cleanup_and_close()
        else:
            show_modeless_warning_dialog(self, "警告", "请先选择一个有效的截图区域！")

    def start_long_screenshot_mode(self):
        """启动长截图模式"""
        log_info("启动长截图模式", "ScreenshotWindow")
        
        # 获取当前选中的区域
        if self.scene.selection_model.is_confirmed:
            selection_rect = self.scene.selection_model.rect()
            
            log_debug(f"selection_rect（场景坐标）: x={selection_rect.x()}, y={selection_rect.y()}, w={selection_rect.width()}, h={selection_rect.height()}", "LongScreenshot")
            log_debug(f"virtual偏移: x={self.virtual_x}, y={self.virtual_y}", "LongScreenshot")
            
            # 场景坐标已经是屏幕的全局坐标，背景图层通过 setOffset 保留了系统提供的虚拟桌面偏移
            # 因此此处不需要再次叠加 virtual_x / virtual_y，否则会导致坐标被重复平移
            real_x = int(selection_rect.x())
            real_y = int(selection_rect.y())
            real_width = int(selection_rect.width())
            real_height = int(selection_rect.height())
            
            # 创建屏幕坐标的选区矩形
            capture_rect = QRect(real_x, real_y, real_width, real_height)
            
            log_debug(f"选中区域（屏幕坐标）: x={real_x}, y={real_y}, w={real_width}, h={real_height}", "LongScreenshot")
            
            # 保存配置，用于长截图窗口
            save_dir = self.config_manager.get_screenshot_save_path()
            
            # 创建独立的长截图窗口（不传递 parent，让它独立运行）
            scroll_window = ScrollCaptureWindow(capture_rect, parent=None, config_manager=self.config_manager)
            scroll_window.set_save_directory(save_dir)  # 设置保存目录
            
            # 把引用挂到 QApplication，防止截图窗口销毁后被 GC 回收
            app = QApplication.instance()
            old_scroll = getattr(app, "_scroll_window", None)
            if old_scroll is not None:
                try:
                    old_scroll.close()
                except Exception as e:
                    log_exception(e, "关闭旧滚动截图窗口")
            app._scroll_window = scroll_window
            
            # 窗口关闭后自动清除全局引用，释放内存
            def _on_scroll_window_destroyed():
                setattr(app, '_scroll_window', None)
                from core.platform_utils import request_trim_working_set
                request_trim_working_set(1000)
            scroll_window.destroyed.connect(_on_scroll_window_destroyed)
            
            # 显示长截图窗口
            log_debug("长截图窗口创建完成，准备显示", "LongScreenshot")
            scroll_window.show()
            scroll_window.raise_()
            scroll_window.activateWindow()
            
            log_info("滚动截图窗口已显示并激活", "LongScreenshot")
            
            # 立即关闭截图窗口，释放内存
            log_debug("释放截图窗口内存", "LongScreenshot")
            self.cleanup_and_close()
        else:
            # 如果没有确认选区，显示提示
            show_modeless_warning_dialog(self, "警告", "请先选择一个有效的截图区域！")

    @safe_event
    def closeEvent(self, event):
        """窗口关闭事件 - 确保资源被正确释放
        
        统一走 cleanup_and_close() 路径，避免遗漏资源释放。
        如果已经清理过（_is_closing=True），直接放行。
        """
        if not getattr(self, '_is_closing', False):
            # 尚未走过 cleanup_and_close，在这里补救
            self.cleanup_and_close()
        super().closeEvent(event)
 