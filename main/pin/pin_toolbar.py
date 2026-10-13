"""
钉图工具栏 - 基于截图工具栏做钉图场景定制
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from ui.toolbar import Toolbar
from ui.toolbar_layout import PIN
from core import safe_event
from core.ui_theme import set_own_style


class PinToolbar(Toolbar):
    """
    钉图工具栏：
    1. 排布读钉图自己的配置（ui.toolbar_layout.PIN），和截图工具栏各调各的
    2. 作为独立顶层窗口吸附在钉图附近
    3. 支持手动拖拽；显隐由钉图的 PinHoverControls 决定
    """

    LAYOUT_PROFILE = PIN

    def __init__(self, parent_pin_window=None, config_manager=None):
        super().__init__(parent=None)

        self.parent_pin_window = parent_pin_window
        self.config_manager = config_manager
        # 工具栏和「…」弹层是两个顶层窗口，鼠标在哪个上面都算停在工具栏上
        self._hovered_parts = set()

    def _host_window(self):
        """钉图工具栏是独立顶层窗口，parent() 为 None，宿主从这里拿。"""
        return self.parent_pin_window

    def _reposition_self(self):
        if self.isVisible() and self.parent_pin_window is not None:
            self.position_near_window(self.parent_pin_window)

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
        """显示时重读排布（可能在别的钉图里改过），并吸附到钉图附近。"""
        self.reload_layout()
        if self.parent_pin_window:
            self.position_near_window(self.parent_pin_window)

        super().show()

    @safe_event
    def enterEvent(self, event):
        super().enterEvent(event)
        self._set_part_hovered("toolbar", True)

    @safe_event
    def leaveEvent(self, event):
        super().leaveEvent(event)
        self._set_part_hovered("toolbar", False)

    def _show_more_popup(self):
        created = self._more_popup is None
        super()._show_more_popup()
        if created:
            self._more_popup.hover_changed.connect(lambda hovered: self._set_part_hovered("popup", hovered))

    def _hide_more_popup(self):
        super()._hide_more_popup()
        # 鼠标停在弹层上时点了里面的按钮，弹层收起后不一定再收到 Leave
        self._set_part_hovered("popup", False)

    def _open_layout_dialog(self):
        # 对话框开着时鼠标不在钉图和工具栏上，按打开菜单处理，免得工具栏中途收起
        hover_controls = getattr(self.parent_pin_window, "hover_controls", None)
        if hover_controls is None:
            super()._open_layout_dialog()
            return
        hover_controls.set_menu_open(True)
        try:
            super()._open_layout_dialog()
        finally:
            hover_controls.set_menu_open(False)

    def _set_part_hovered(self, part: str, hovered: bool):
        if hovered:
            self._hovered_parts.add(part)
        else:
            self._hovered_parts.discard(part)
        self._report_hover(bool(self._hovered_parts))

    def _report_hover(self, hovered: bool):
        hover_controls = getattr(self.parent_pin_window, "hover_controls", None)
        if hover_controls is not None:
            hover_controls.set_toolbar_hovered(hovered)

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


if __name__ == "__main__":
    import sys
    from PySide6.QtWidgets import QApplication, QWidget, QLabel

    app = QApplication(sys.argv)

    mock_pin = QWidget()
    mock_pin.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.WindowStaysOnTopHint)
    mock_pin.setGeometry(100, 100, 400, 300)
    set_own_style(mock_pin, "background-color: lightblue; border: 2px solid black;")

    label = QLabel("Mock pin window", mock_pin)
    label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    label.setGeometry(0, 0, 400, 300)
    mock_pin.show()

    toolbar = PinToolbar(parent_pin_window=mock_pin)

    toolbar.tool_changed.connect(lambda tool: print(f"Tool changed: {tool}"))
    toolbar.save_clicked.connect(lambda: print("Save clicked"))
    toolbar.copy_clicked.connect(lambda: print("Copy clicked"))
    toolbar.undo_clicked.connect(lambda: print("Undo clicked"))
    toolbar.redo_clicked.connect(lambda: print("Redo clicked"))

    toolbar.show()

    sys.exit(app.exec())
