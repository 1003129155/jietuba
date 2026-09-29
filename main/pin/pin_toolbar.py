"""
钉图工具栏 - 基于截图工具栏做钉图场景定制
"""

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from ui.toolbar import Toolbar
from core import safe_event
from core.ui_theme import set_own_style


class PinToolbar(Toolbar):
    """
    钉图工具栏：
    1. 固定排布：没有截图专属按钮和「…」，多一个复制按钮，不读截图工具栏的排布配置
    2. 作为独立顶层窗口吸附在钉图附近
    3. 支持手动拖拽；显隐由钉图的 PinHoverControls 决定
    """

    # 绘制工具的顺序与截图工具栏的默认排布一致，用户在两个窗口里找同一个按钮时位置不变
    LAYOUT = (
        "save", "copy",
        "pen", "highlighter", "mosaic", "arrow", "number", "rect", "ellipse", "text", "eraser",
        "undo", "redo",
    )

    def __init__(self, parent_pin_window=None, config_manager=None):
        super().__init__(parent=None)

        self.parent_pin_window = parent_pin_window
        self.config_manager = config_manager

    def _host_window(self):
        """钉图工具栏是独立顶层窗口，parent() 为 None，宿主从这里拿。"""
        return self.parent_pin_window

    def reload_layout(self):
        """钉图的排布是固定的，不跟随用户对截图工具栏的配置"""
        self._arrange(self.LAYOUT)

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
        """显示工具栏时同步吸附到钉图附近。"""
        if self.parent_pin_window:
            self.position_near_window(self.parent_pin_window)

        super().show()

    @safe_event
    def enterEvent(self, event):
        super().enterEvent(event)
        self._report_hover(True)

    @safe_event
    def leaveEvent(self, event):
        super().leaveEvent(event)
        self._report_hover(False)

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
