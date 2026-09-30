"""
钉图悬停控件（右上角按钮、工具栏）的显隐

显隐只在 sync() 里决定。悬停、右键菜单、编辑状态、缩略图模式、手动切换都只更新
输入再调 sync()，其他地方不直接 show/hide 这些控件。

工具栏规则，先匹配的生效：
1. 缩略图模式 → 隐藏
2. 用户手动开/关过 → 按用户的
3. 正在用绘图工具 → 显示
4. 开了「自动显示工具栏」且在悬停会话中 → 显示
5. 其余 → 隐藏

悬停会话：鼠标在钉图、工具栏或右键菜单上时开始，全部离开后再过宽限期才结束，
编辑中不结束。自动模式下会话结束会清掉手动选择，下次移入照常自动弹出；
手动模式下手动选择一直保留。
"""

from PySide6.QtCore import QTimer
from PySide6.QtGui import QCursor
from PySide6.QtWidgets import QApplication


class PinHoverControls:

    # 够鼠标从钉图挪到工具栏上
    TOOLBAR_GRACE_MS = 2000
    BUTTONS_GRACE_MS = 300

    def __init__(self, win):
        self._win = win
        self._pin_hovered = False
        self._toolbar_hovered = False
        self._menu_open = False
        self._in_session = False
        # None 跟随规则；True / False 是用户手动开 / 关
        self._toolbar_choice = None

        self._session_timer = QTimer(win)
        self._session_timer.setSingleShot(True)
        self._session_timer.setInterval(self.TOOLBAR_GRACE_MS)
        self._session_timer.timeout.connect(self._end_session)

        self._buttons_timer = QTimer(win)
        self._buttons_timer.setSingleShot(True)
        self._buttons_timer.setInterval(self.BUTTONS_GRACE_MS)
        self._buttons_timer.timeout.connect(self.sync)

    # ------------------------------------------------------------------
    # 输入
    # ------------------------------------------------------------------

    def set_pin_hovered(self, hovered: bool):
        if hovered == self._pin_hovered:
            return
        self._store_pin_hovered(hovered)
        self._on_hover_changed()

    def recheck_pin_hovered(self):
        """Leave 在鼠标移到钉图自己的子控件上时也会触发，以鼠标下的窗口为准"""
        self.set_pin_hovered(self._mouse_over_pin())

    def set_toolbar_hovered(self, hovered: bool):
        if hovered == self._toolbar_hovered:
            return
        self._toolbar_hovered = hovered
        self._on_hover_changed()

    def set_menu_open(self, menu_open: bool):
        self._menu_open = menu_open
        if self._win._is_closed:
            return
        if not menu_open:
            # 菜单弹出时钉图收到过 Leave，菜单关掉后不一定再收到 Enter
            self._store_pin_hovered(self._mouse_over_pin())
        self._on_hover_changed()

    def on_editing_changed(self):
        self._on_hover_changed()

    def toggle_toolbar(self):
        if self._win._thumbnail_mode:
            return
        self._toolbar_choice = not self._toolbar_visible()
        self._in_session = True
        if not self._hover_active():
            self._session_timer.start()
        self.sync()

    def stop(self):
        self._session_timer.stop()
        self._buttons_timer.stop()

    # ------------------------------------------------------------------
    # 决策
    # ------------------------------------------------------------------

    def sync(self):
        win = self._win
        if win._is_closed:
            return

        want_toolbar = self._toolbar_wanted()
        if want_toolbar != self._toolbar_visible():
            if want_toolbar:
                win._show_toolbar()
            else:
                win._hide_toolbar()

        show_buttons = self._hover_buttons_enabled() and (
            self._pin_hovered or self._menu_open or self._buttons_timer.isActive()
        )
        win._control_buttons.set_visible(
            close=show_buttons,
            toolbar=show_buttons and not self._auto_toolbar() and not win._thumbnail_mode,
        )

    def _toolbar_wanted(self) -> bool:
        win = self._win
        if win._thumbnail_mode:
            return False
        if self._toolbar_choice is not None:
            return self._toolbar_choice
        if win._is_editing:
            return True
        return self._auto_toolbar() and self._in_session

    def _hover_active(self) -> bool:
        return self._pin_hovered or self._toolbar_hovered or self._menu_open

    def _store_pin_hovered(self, hovered: bool):
        self._pin_hovered = hovered
        if not hovered:
            self._buttons_timer.start()

    def _mouse_over_pin(self) -> bool:
        under = QApplication.widgetAt(QCursor.pos())
        return under is not None and under.window() is self._win

    def _on_hover_changed(self):
        if self._hover_active():
            self._in_session = True
            self._session_timer.stop()
        elif self._in_session and not self._session_timer.isActive():
            self._session_timer.start()
        self.sync()

    def _end_session(self):
        if self._hover_active() or self._win._is_editing:
            # 编辑结束时 on_editing_changed 会重新计时
            return
        self._in_session = False
        if self._auto_toolbar():
            self._toolbar_choice = None
        self.sync()

    def _toolbar_visible(self) -> bool:
        toolbar = self._win.toolbar
        return toolbar is not None and toolbar.isVisible()

    def _auto_toolbar(self) -> bool:
        cfg = self._win.config_manager
        return cfg.get_pin_auto_toolbar() if cfg else True

    def _hover_buttons_enabled(self):
        cfg = self._win.config_manager
        return bool(cfg.get_app_setting("pin_hover_buttons")) if cfg else False
