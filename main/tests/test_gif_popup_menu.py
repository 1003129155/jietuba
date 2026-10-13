"""
录制工具栏的选项浮层（帧率、延时、音频）固定是浅色底，深色界面主题下文字也必须是深色。
"""
from PySide6.QtGui import QAction
from PySide6.QtWidgets import QMenu, QPushButton

from core.ui_theme import DARK_TOKENS, UIThemeManager
from gif._widgets import popup_menu


def test_popup_menu_text_stays_dark_under_dark_theme(qapp):
    original_sheet = qapp.styleSheet()
    qapp.setStyleSheet(UIThemeManager._build_global_stylesheet(DARK_TOKENS))
    anchor = QPushButton("fps")
    try:
        anchor.resize(40, 20)
        anchor.show()
        menu = QMenu(anchor)
        for text in ("5 fps", "24 fps"):
            action = QAction(text, menu)
            action.setCheckable(True)
            menu.addAction(action)
        popup_menu(menu, anchor)
        qapp.processEvents()

        image = menu.grab().toImage()
        darkest = min(
            image.pixelColor(x, y).lightness()
            for x in range(image.width())
            for y in range(image.height())
        )
        assert darkest < 100
        menu.close()
    finally:
        anchor.deleteLater()
        qapp.setStyleSheet(original_sheet)
