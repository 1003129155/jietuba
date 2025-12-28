"""
钉图右键菜单

负责创建和显示钉图窗口的右键上下文菜单
"""

from PyQt6.QtWidgets import QMenu, QWidget
from PyQt6.QtGui import QAction, QFont
from PyQt6.QtCore import Qt, QPoint


class PinContextMenu:
    """
    钉图右键菜单管理器
    
    创建和显示钉图窗口的上下文菜单
    """
    
    def __init__(self, parent: QWidget):
        """
        初始化右键菜单管理器
        
        Args:
            parent: 父窗口（PinWindow）
        """
        self.parent = parent
    
    def show(self, global_pos: QPoint, state: dict):
        """
        显示右键菜单
        
        Args:
            global_pos: 全局坐标位置
            state: 当前状态字典，包含：
                - toolbar_visible: 工具栏是否可见
                - stay_on_top: 是否置顶
                - shadow_enabled: 阴影是否启用
                - has_ocr_result: 是否有 OCR 结果
        """
        menu = QMenu(self.parent)
        
        # 设置字体
        self._setup_font(menu)
        
        # 设置样式
        menu.setStyleSheet(self._get_menu_style())
        
        # 添加菜单项
        self._add_menu_items(menu, state)
        
        # 显示菜单
        menu.exec(global_pos)
    
    def _setup_font(self, menu: QMenu):
        """设置菜单字体"""
        menu_font = QFont("Microsoft YaHei UI", 9)
        if not menu_font.exactMatch():
            menu_font = QFont("Segoe UI", 9)
        menu.setFont(menu_font)
    
    def _get_menu_style(self) -> str:
        """获取菜单样式"""
        return """
            QMenu {
                background-color: white;
                border: 1px solid #ccc;
                border-radius: 4px;
                padding: 5px;
                font-family: "Microsoft YaHei UI", "Segoe UI", "Yu Gothic UI", sans-serif;
                font-size: 9pt;
                color: #000000;
            }
            QMenu::item {
                padding: 5px 30px 5px 30px;
                border-radius: 3px;
                color: #000000;
                background-color: transparent;
            }
            QMenu::item:selected {
                background-color: #e3f2fd;
                color: #000000;
            }
            QMenu::separator {
                height: 1px;
                background: #ddd;
                margin: 5px 0px;
            }
        """
    
    def _add_menu_items(self, menu: QMenu, state: dict):
        """添加菜单项"""
        # 📋 复制内容
        copy_action = QAction("📋 " + self.parent.tr("Copy"), self.parent)
        copy_action.triggered.connect(self.parent.copy_to_clipboard)
        menu.addAction(copy_action)
        
        # 💾 保存图片
        save_action = QAction("💾 " + self.parent.tr("Save as"), self.parent)
        save_action.triggered.connect(self.parent.save_image)
        menu.addAction(save_action)
        
        # 🌐 翻译（仅在有 OCR 结果时可用）
        translate_action = QAction("🌐 " + self.parent.tr("Translate"), self.parent)
        translate_action.triggered.connect(self.parent._on_translate_clicked)
        translate_action.setEnabled(state.get('has_ocr_result', False))
        menu.addAction(translate_action)
        
        menu.addSeparator()
        
        # 🔧 显示/隐藏工具栏
        toolbar_visible = state.get('toolbar_visible', False)
        toolbar_action = QAction(
            f"{'✓ ' if toolbar_visible else '   '}🔧 " + self.parent.tr("Toolbar"),
            self.parent
        )
        toolbar_action.triggered.connect(self.parent.toggle_toolbar)
        menu.addAction(toolbar_action)
        
        # 📌 切换置顶
        stay_on_top = state.get('stay_on_top', False)
        toggle_top_action = QAction(
            f"{'✓ ' if stay_on_top else '   '}📌 " + self.parent.tr("Always on top"),
            self.parent
        )
        toggle_top_action.triggered.connect(self.parent.toggle_stay_on_top)
        menu.addAction(toggle_top_action)
        
        # 🌟 切换阴影效果
        shadow_enabled = state.get('shadow_enabled', True)
        shadow_action = QAction(
            f"{'✓ ' if shadow_enabled else '   '}🌟 " + self.parent.tr("Shadow effect"),
            self.parent
        )
        shadow_action.triggered.connect(self.parent.toggle_shadow_effect)
        menu.addAction(shadow_action)
        
        menu.addSeparator()
        
        # ❌ 关闭钉图
        close_action = QAction("❌ " + self.parent.tr("Close"), self.parent)
        close_action.triggered.connect(self.parent.close_window)
        menu.addAction(close_action)
