"""
钉图工具栏 - 基于截图工具栏做钉图场景定制
"""

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import QApplication

from ui.toolbar import Toolbar
from core import log_debug, safe_event


class PinToolbar(Toolbar):
    """
    钉图工具栏：
    1. 隐藏钉图场景不需要的按钮
    2. 显示复制按钮
    3. 作为独立顶层窗口吸附在钉图附近
    4. 支持自动隐藏与手动拖拽
    """

    def __init__(self, parent_pin_window=None, config_manager=None):
        super().__init__(parent=None)

        self.parent_pin_window = parent_pin_window
        self.config_manager = config_manager

        self._customize_for_pin()

        self.auto_hide_timer = QTimer(self)
        self.auto_hide_timer.setSingleShot(True)
        self.auto_hide_timer.setInterval(2000)
        self.auto_hide_timer.timeout.connect(self._auto_hide)

        self.auto_hide_enabled = False
        self._parent_hovering = False

    def _customize_for_pin(self):
        """按钉图模式重排并隐藏不需要的按钮。"""
        s = self.SCALE
        btn_width = round(45 * s)
        btn_height = round(45 * s)
        wide_w = round(50 * s)
        handle_w = round(btn_height * 0.32) if hasattr(self, "drag_handle") else 0
        left_x = handle_w

        if hasattr(self, "confirm_btn"):
            self.confirm_btn.hide()
        if hasattr(self, "long_screenshot_btn"):
            self.long_screenshot_btn.hide()
        if hasattr(self, "pin_btn"):
            self.pin_btn.hide()
        if hasattr(self, "cancel_btn"):
            self.cancel_btn.hide()
        if hasattr(self, "gif_btn"):
            self.gif_btn.hide()

        if hasattr(self, "screenshot_translate_btn"):
            self.screenshot_translate_btn.setGeometry(left_x, 0, wide_w, btn_height)
            self.screenshot_translate_btn.show()
            left_x += wide_w

        if hasattr(self, "save_btn"):
            self.save_btn.setGeometry(left_x, 0, wide_w, btn_height)
            self.save_btn.show()
            left_x += wide_w

        if hasattr(self, "copy_btn"):
            self.copy_btn.setGeometry(left_x, 0, wide_w, btn_height)
            self.copy_btn.show()
            left_x += wide_w

        for attr in (
            "pen_btn",
            "highlighter_btn",
            "arrow_btn",
            "number_btn",
            "rect_btn",
            "ellipse_btn",
            "text_btn",
            "eraser_btn",
            "undo_btn",
            "redo_btn",
        ):
            button = getattr(self, attr, None)
            if button:
                button.setGeometry(left_x, 0, btn_width, btn_height)
                left_x += btn_width

        self.resize(left_x, btn_height)

    def position_near_window(self, pin_window):
        """
        将工具栏定位到钉图正下方，右对齐。

        规则：
        1. 优先放在钉图下方；判断时把二级菜单最大高度一并计入，确保展开后也不超屏
        2. 下方+二级菜单放不下则放到上方
        3. 上下都超出则夹紧到屏幕边缘
        """
        if getattr(self, "_manual_positioned", False):
            return

        if not pin_window:
            return

        pin_pos = pin_window.pos()
        pin_size = pin_window.size()

        screen = pin_window.screen() or QApplication.primaryScreen()
        screen_rect = screen.geometry()

        spacing = 2
        toolbar_width = self.width()
        toolbar_height = self.height()
        panel_extra = self._get_max_panel_height()

        toolbar_x = pin_pos.x() + pin_size.width() - toolbar_width
        below_y = pin_pos.y() + pin_size.height() + spacing
        above_y = pin_pos.y() - toolbar_height - spacing

        # 下方判断含二级菜单总高度，与截图工具栏逻辑一致
        if below_y + toolbar_height + panel_extra <= screen_rect.bottom():
            toolbar_y = below_y
            toolbar_below = True
        else:
            toolbar_y = above_y
            toolbar_below = False

        # X 轴夹紧在屏幕内；Y 轴兜底夹紧
        toolbar_x = max(screen_rect.left(), min(toolbar_x, screen_rect.right() - toolbar_width))
        toolbar_y = max(screen_rect.top(), min(toolbar_y, screen_rect.bottom() - toolbar_height))

        self._toolbar_below_selection = toolbar_below
        self.move(toolbar_x, toolbar_y)

    def show(self):
        """显示工具栏时同步吸附到钉图附近。"""
        if self.parent_pin_window:
            self.position_near_window(self.parent_pin_window)

        super().show()

        if self.auto_hide_timer.isActive():
            self.auto_hide_timer.stop()

    @safe_event
    def enterEvent(self, event):
        super().enterEvent(event)
        if self.auto_hide_timer.isActive():
            self.auto_hide_timer.stop()

    @safe_event
    def leaveEvent(self, event):
        super().leaveEvent(event)
        if self._should_auto_hide():
            self.auto_hide_timer.start()

    def _auto_hide(self):
        if not self.auto_hide_enabled:
            return

        if self._is_parent_editing() or self._parent_hovering:
            self.auto_hide_timer.start()
            return

        self.hide()

    def enable_auto_hide(self, enabled: bool = True):
        self.auto_hide_enabled = enabled

        if enabled and self._should_auto_hide():
            self.auto_hide_timer.start()
        else:
            self.auto_hide_timer.stop()

    def set_auto_hide_delay(self, milliseconds: int):
        self.auto_hide_timer.setInterval(milliseconds)

    def _reset_auto_position(self):
        """双击拖拽手柄后恢复自动吸附。"""
        self._set_manual_positioned(False)
        if self.parent_pin_window:
            self.position_near_window(self.parent_pin_window)

    def sync_with_pin_window(self):
        """在钉图窗口移动、缩放后同步工具栏和所有二级面板。"""
        if self.isVisible() and self.parent_pin_window:
            self.position_near_window(self.parent_pin_window)
            self._sync_all_panels_position()

    def on_parent_editing_state_changed(self, editing: bool):
        if editing:
            if self.auto_hide_timer.isActive():
                self.auto_hide_timer.stop()
        else:
            if self._should_auto_hide() and not self.underMouse():
                self.auto_hide_timer.start()

    def _is_parent_editing(self) -> bool:
        return bool(self.parent_pin_window and getattr(self.parent_pin_window, "_is_editing", False))

    def on_parent_hover(self, hovering: bool):
        self._parent_hovering = hovering
        if hovering:
            if self.auto_hide_timer.isActive():
                self.auto_hide_timer.stop()
        else:
            if self._should_auto_hide() and not self.underMouse():
                self.auto_hide_timer.start()

    def _should_auto_hide(self) -> bool:
        return (
            self.auto_hide_enabled
            and self.isVisible()
            and not self._is_parent_editing()
            and not self._parent_hovering
        )


if __name__ == "__main__":
    import sys
    from PySide6.QtWidgets import QApplication, QWidget, QLabel

    app = QApplication(sys.argv)

    mock_pin = QWidget()
    mock_pin.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint)
    mock_pin.setGeometry(100, 100, 400, 300)
    mock_pin.setStyleSheet("background-color: lightblue; border: 2px solid black;")

    label = QLabel("模拟钉图窗口", mock_pin)
    label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    label.setGeometry(0, 0, 400, 300)
    mock_pin.show()

    toolbar = PinToolbar(parent_pin_window=mock_pin)
    toolbar.enable_auto_hide(True)
    toolbar.set_auto_hide_delay(3000)

    toolbar.tool_changed.connect(lambda tool: print(f"工具切换: {tool}"))
    toolbar.save_clicked.connect(lambda: print("保存点击"))
    toolbar.copy_clicked.connect(lambda: print("复制点击"))
    toolbar.undo_clicked.connect(lambda: print("撤销点击"))
    toolbar.redo_clicked.connect(lambda: print("重做点击"))

    toolbar.show()

    sys.exit(app.exec())
 