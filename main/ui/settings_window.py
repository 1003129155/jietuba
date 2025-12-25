import sys
import os
import platform
from PyQt6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
    QListWidget, QListWidgetItem, QStackedWidget, QWidget,
    QFrame, QSpinBox, QDoubleSpinBox, QGridLayout, QScrollArea,
    QLineEdit, QComboBox, QFileDialog, QMessageBox, QApplication
)
from PyQt6.QtCore import Qt, QSize, QPropertyAnimation, QEasingCurve, pyqtProperty, pyqtSignal
from PyQt6.QtGui import QColor, QPainter, QFont, QPen

# ==========================================
# 1. UI 组件库 (仿微信/iOS 风格)
# ==========================================

class ToggleSwitch(QWidget):
    """自定义仿iOS/微信风格开关"""
    
    # 🔥 定义 PyQt 信号
    toggled = pyqtSignal(bool)  # 开关状态改变时发射
    
    def __init__(self, parent=None, width=44, height=24, bg_color="#E5E5E5", active_color="#07C160"):
        super().__init__(parent)
        self.setFixedSize(width, height)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self._bg_color = bg_color
        self._circle_color = "#FFFFFF"
        self._active_color = active_color
        self._circle_position = 3
        self._checked = False

    def isChecked(self):
        return self._checked

    def setChecked(self, checked):
        """设置开关状态（不触发信号）"""
        if self._checked == checked:
            return
        self._checked = checked
        if checked:
            self._circle_position = self.width() - 21
        else:
            self._circle_position = 3
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        
        # 绘制背景
        color = self._active_color if self._checked else self._bg_color
        painter.setBrush(QColor(color))
        painter.setPen(Qt.PenStyle.NoPen)
        rect = self.rect()
        painter.drawRoundedRect(0, 0, rect.width(), rect.height(), rect.height() / 2, rect.height() / 2)

        # 绘制圆圈
        painter.setBrush(QColor(self._circle_color))
        painter.drawEllipse(self._circle_position, 3, 18, 18)
        painter.end()

    def mousePressEvent(self, event):
        """鼠标点击切换状态"""
        self._checked = not self._checked
        # 动画
        self.anim = QPropertyAnimation(self, b"circle_position")
        self.anim.setDuration(200)
        self.anim.setEasingCurve(QEasingCurve.Type.OutQuad)
        self.anim.setEndValue(self.width() - 21 if self._checked else 3)
        self.anim.start()
        
        # 🔥 发射 PyQt 信号
        self.toggled.emit(self._checked)
        self.update()

    @pyqtProperty(int)
    def circle_position(self):
        return self._circle_position

    @circle_position.setter
    def circle_position(self, pos):
        self._circle_position = pos
        self.update()

class SettingCard(QFrame):
    """白底圆角卡片容器"""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("Card")
        self.setStyleSheet("""
            #Card {
                background-color: #FFFFFF;
                border-radius: 8px;
                border: 1px solid #E5E5E5;
            }
        """)
        self.layout = QVBoxLayout(self)
        self.layout.setContentsMargins(20, 20, 20, 20)
        self.layout.setSpacing(15)

class HLine(QFrame):
    """分割线"""
    def __init__(self):
        super().__init__()
        self.setFrameShape(QFrame.Shape.HLine)
        self.setFrameShadow(QFrame.Shadow.Sunken)
        self.setStyleSheet("background-color: #F0F0F0; border: none; max-height: 1px;")

# ==========================================
# 2. 设置对话框主逻辑
# ==========================================

class SettingsDialog(QDialog):
    """现代化设置对话框 - 微信PC版风格"""

    def __init__(self, config_manager=None, current_hotkey="ctrl+shift+a", parent=None):
        super().__init__(parent)
        self.config_manager = config_manager
        self.current_hotkey = current_hotkey
        self.main_window = parent
        
        # 如果没有提供 config_manager，使用 Mock
        if self.config_manager is None:
            self.config_manager = MockConfig()
    
        self.setWindowTitle("jietuba.20151220-v.1.0.0 -RIJYAARU")
        self.resize(850, 600)
        self.setFont(QFont("Microsoft YaHei", 9)) # 使用微软雅黑
        # 全局背景色
        self.setStyleSheet("""
            QDialog { background-color: #F5F5F5; color: #333333; }
            QLabel { color: #333333; background-color: transparent; }
            QScrollArea { background-color: transparent; border: none; }
            QScrollBar:vertical {
                border: none; background: transparent; width: 6px; margin: 0px;
            }
            QScrollBar::handle:vertical {
                background: #CCCCCC; min-height: 20px; border-radius: 3px;
            }
        """)
        
        self._setup_ui()

    def _setup_ui(self):
        main_layout = QHBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # 1. 左侧导航栏
        self.nav_list = self._create_navigation()
        main_layout.addWidget(self.nav_list)

        # 2. 右侧内容区 (ScrollArea 包裹，防止小屏幕显示不全)
        right_area = QWidget()
        right_area.setStyleSheet("background-color: #F5F5F5;")
        right_layout = QVBoxLayout(right_area)
        right_layout.setContentsMargins(30, 20, 30, 20)
        right_layout.setSpacing(15)

        # 标题栏
        self.content_title = QLabel("ショートカット設定")
        self.content_title.setStyleSheet("font-size: 18px; font-weight: bold; margin-bottom: 10px; background-color: transparent;")
        right_layout.addWidget(self.content_title)

        # 内容堆栈
        self.content_stack = QStackedWidget()
        self.content_stack.addWidget(self._create_hotkey_page())         # 0
        self.content_stack.addWidget(self._create_long_screenshot_page())# 1
        self.content_stack.addWidget(self._create_smart_selection_page())# 2
        self.content_stack.addWidget(self._create_screenshot_save_page())# 3
        self.content_stack.addWidget(self._create_ocr_page())            # 4
        self.content_stack.addWidget(self._create_log_page())            # 5
        self.content_stack.addWidget(self._create_misc_page())           # 6
        self.content_stack.addWidget(self._create_info_page())           # 7
        right_layout.addWidget(self.content_stack)
        
        # 底部按钮栏
        right_layout.addStretch()
        right_layout.addLayout(self._create_button_area())

        main_layout.addWidget(right_area, 1)

        # 导航连接
        self.nav_list.currentRowChanged.connect(self._on_nav_changed)
        self.nav_list.setCurrentRow(0)

    def _create_navigation(self):
        """创建左侧导航栏 - 灰色极简风格"""
        nav_list = QListWidget()
        nav_list.setFixedWidth(180)
        nav_list.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        nav_list.setStyleSheet("""
            QListWidget {
                background-color: #F0F0F0;
                border: none;
                border-right: 1px solid #E5E5E5;
                padding-top: 20px;
                outline: none;
            }
            QListWidget::item {
                height: 40px;
                margin: 2px 10px;
                border-radius: 4px;
                color: #333333;
                font-size: 14px;
                padding-left: 10px;
            }
            QListWidget::item:hover {
                background-color: #E0E0E0;
            }
            QListWidget::item:selected {
                background-color: #D6D6D6;
                color: #000000;
            }
        """)

        items = [
            "⌨️  ショートカット",
            "📸  長いスクショ",
            "🎯  スマート選択",
            "💾  スクショ保存",
            "🎯  OCR設定",
            "📝  ログ設定",
            "⚙️  その他",
            "ℹ️  情報"
        ]
        for t in items:
            nav_list.addItem(t)
        return nav_list

    # ================= 辅助方法 =================
    
    def _create_toggle_row(self, title, desc, checked_state, toggle_obj):
        """创建一个标准的一行设置：左字右开关"""
        row = QHBoxLayout()
        
        text_layout = QVBoxLayout()
        lbl_title = QLabel(title)
        lbl_title.setStyleSheet("font-size: 14px; color: #000; background-color: transparent;")
        text_layout.addWidget(lbl_title)
        
        if desc:
            lbl_desc = QLabel(desc)
            lbl_desc.setStyleSheet("font-size: 12px; color: #888; background-color: transparent;")
            text_layout.addWidget(lbl_desc)
            
        row.addLayout(text_layout)
        row.addStretch()
        
        toggle_obj.setChecked(checked_state)
        row.addWidget(toggle_obj)
        return row

    def _get_input_style(self):
        return """
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
                border: 1px solid #E5E5E5;
                border-radius: 4px;
                padding: 4px 8px;
                background-color: #FAFAFA;
                color: #333;
                font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif;
                font-size: 12px;
            }
            QLineEdit:focus, QSpinBox:focus {
                border: 1px solid #07C160;
                background-color: #FFF;
            }
            QSpinBox, QDoubleSpinBox {
                padding-right: 24px;
            }
            QSpinBox::up-button, QDoubleSpinBox::up-button {
                subcontrol-origin: border;
                subcontrol-position: top right;
                width: 20px;
                border-left: 1px solid #E5E5E5;
                border-bottom: 1px solid #E5E5E5;
                border-top-right-radius: 4px;
                background: #FAFAFA;
            }
            QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover {
                background: #E8F5E9;
            }
            QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed {
                background: #C8E6C9;
            }
            QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
                image: none;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
                border-bottom: 6px solid #666;
                width: 0;
                height: 0;
            }
            QSpinBox::down-button, QDoubleSpinBox::down-button {
                subcontrol-origin: border;
                subcontrol-position: bottom right;
                width: 20px;
                border-left: 1px solid #E5E5E5;
                border-bottom-right-radius: 4px;
                background: #FAFAFA;
            }
            QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
                background: #E8F5E9;
            }
            QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {
                background: #C8E6C9;
            }
            QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
                image: none;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
                border-top: 6px solid #666;
                width: 0;
                height: 0;
            }
            QComboBox::drop-down {
                subcontrol-origin: padding;
                subcontrol-position: top right;
                width: 20px;
                border-left: 1px solid #E5E5E5;
                border-top-right-radius: 4px;
                border-bottom-right-radius: 4px;
                background: #FAFAFA;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 4px solid transparent;
                border-right: 4px solid transparent;
                border-top: 6px solid #666;
                width: 0;
                height: 0;
                margin-right: 6px;
            }
            QComboBox QAbstractItemView {
                border: 1px solid #E5E5E5;
                background: white;
                selection-background-color: #07C160;
                selection-color: white;
                font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif;
                font-size: 12px;
                color: #333;
                outline: none;
            }
            QComboBox QAbstractItemView::item {
                padding: 6px 8px;
                min-height: 24px;
                color: #333;
                background: white;
            }
            QComboBox QAbstractItemView::item:hover {
                background-color: #E8F5E9;
                color: #000;
            }
            QComboBox QAbstractItemView::item:selected {
                background-color: #07C160;
                color: white;
            }
        """

    # ================= 页面创建 =================

    def _create_hotkey_page(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(15)

        # 卡片1: 快捷键
        card1 = SettingCard()
        
        # 快捷键输入
        row1 = QHBoxLayout()
        lbl = QLabel("ホットキー")
        lbl.setStyleSheet("background-color: transparent;")
        self.hotkey_input = QLineEdit()
        self.hotkey_input.setText(self.current_hotkey)
        self.hotkey_input.setPlaceholderText("例: ctrl+shift+a")
        self.hotkey_input.setFixedWidth(200)
        self.hotkey_input.setStyleSheet(self._get_input_style())
        
        row1.addWidget(lbl)
        row1.addStretch()
        row1.addWidget(self.hotkey_input)
        
        card1.layout.addLayout(row1)
        
        layout.addWidget(card1)

        # 提示卡片
        hint_lbl = QLabel("💡 ヒント: Ctrl, Shift, Alt などの修飾キーと組み合わせて使用できます。")
        hint_lbl.setStyleSheet("color: #888; padding: 5px; background-color: transparent;")
        layout.addWidget(hint_lbl)
        
        layout.addStretch()
        return page

    def _create_long_screenshot_page(self):
        # 使用 ScrollArea 因为这个页面很长
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        
        content = QWidget()
        layout = QVBoxLayout(content)
        layout.setContentsMargins(0, 0, 10, 0) # 右侧留点空隙给滚动条
        layout.setSpacing(15)

        # 卡片1: 基础引擎设置
        card1 = SettingCard()
        
        # 引擎选择
        row_engine = QHBoxLayout()
        lbl_eng = QLabel("拼接エンジン")
        lbl_eng.setStyleSheet("background-color: transparent;")
        self.engine_combo = QComboBox()
        self.engine_combo.addItems(["Rustハッシュ値 (推奨)", "Pythonハッシュ値 (デバッグ用)"])
        # 数据映射 (0 -> hash_rust, 1 -> hash_python)
        self.engine_combo.setItemData(0, "hash_rust")
        self.engine_combo.setItemData(1, "hash_python")
        self.engine_combo.setFixedWidth(200)
        self.engine_combo.setStyleSheet(self._get_input_style())
        
        # 恢复选中状态
        current_eng = self.config_manager.get_long_stitch_engine()
        if current_eng == "hash_python":
            self.engine_combo.setCurrentIndex(1)
        else:
            self.engine_combo.setCurrentIndex(0) # Default hash_rust

        row_engine.addWidget(lbl_eng)
        row_engine.addStretch()
        row_engine.addWidget(self.engine_combo)
        card1.layout.addLayout(row_engine)
        
        card1.layout.addWidget(HLine())

        # 滚动冷却时间
        row_cooldown = QHBoxLayout()
        lbl_cooldown = QLabel("待機時間")
        lbl_cooldown.setStyleSheet("background-color: transparent;")
        lbl_cooldown_desc = QLabel("スクロール後のキャプチャ待機時間 (秒)")
        lbl_cooldown_desc.setStyleSheet("font-size: 12px; color: #888; background-color: transparent;")
        
        self.cooldown_spinbox = QDoubleSpinBox()
        self.cooldown_spinbox.setRange(0.05, 1.0)
        self.cooldown_spinbox.setSingleStep(0.01)
        self.cooldown_spinbox.setDecimals(2)
        self.cooldown_spinbox.setValue(
            self.config_manager.settings.value('screenshot/scroll_cooldown', 0.15, type=float)
        )
        self.cooldown_spinbox.setFixedWidth(100)
        self.cooldown_spinbox.setStyleSheet(self._get_input_style())
        
        cooldown_text_layout = QVBoxLayout()
        cooldown_text_layout.addWidget(lbl_cooldown)
        cooldown_text_layout.addWidget(lbl_cooldown_desc)
        
        row_cooldown.addLayout(cooldown_text_layout)
        row_cooldown.addStretch()
        row_cooldown.addWidget(self.cooldown_spinbox)
        
        card1.layout.addLayout(row_cooldown)
        layout.addWidget(card1)

        # 卡片2: Rust 高级参数 (已隐藏，保留变量以供内部使用)
        # 初始化 spinboxes 和 rollback_toggle，使用默认值
        self.spinboxes = {}
        params = [
            ("采样率 (0.1-1.0)", "rust_sample_rate", 0.6, float),
            ("最小采样尺寸", "rust_min_sample_size", 300, int),
            ("最大采样尺寸", "rust_max_sample_size", 800, int),
            ("特征点阈值", "rust_corner_threshold", 30, int),
            ("描述符块大小", "rust_descriptor_patch_size", 9, int),
            ("索引重建阈值", "rust_min_size_delta", 1, int),
            ("距离阈值", "rust_distance_threshold", 0.1, float),
            ("HNSW搜索参数", "rust_ef_search", 32, int),
        ]
        
        # 创建隐藏的spinbox占位符（保存功能仍需要这些引用）
        for label_text, key, default, type_ in params:
            class DummySpinBox:
                def __init__(self, val):
                    self._val = val
                def value(self):
                    return self._val
            
            val = self.config_manager.settings.value(f'screenshot/{key}', default, type=type_)
            self.spinboxes[key] = DummySpinBox(val)
        
        # 创建隐藏的rollback_toggle占位符
        class DummyToggle:
            def __init__(self, checked):
                self._checked = checked
            def isChecked(self):
                return self._checked
        
        self.rollback_toggle = DummyToggle(
            self.config_manager.settings.value('screenshot/rust_try_rollback', True, type=bool)
        )
        
        # 底部说明（移除高级参数警告）
        layout.addStretch()
        
        scroll.setWidget(content)
        return scroll

    def _create_smart_selection_page(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)

        card = SettingCard()
        
        self.smart_toggle = ToggleSwitch()
        row = self._create_toggle_row(
            "スマート選択を有効にする", 
            "マウスカーソル位置のUI要素を自動認識します。",
            self.config_manager.get_smart_selection(),
            self.smart_toggle
        )
        
        card.layout.addLayout(row)
        layout.addWidget(card)
        
        # 图文说明区域（可以用 QLabel 贴图，这里用文字模拟）
        info_card = QLabel(
            "💡 使い方:\n\n"
            "1. キャプチャ時にカーソルをウィンドウ上に移動\n"
            "2. 自動的に青い枠でエリアがハイライトされます\n"
            "3. クリックしてその範囲を選択"
        )
        info_card.setStyleSheet("""
            background-color: #E9F0FD; 
            color: #4C72B0; 
            border-radius: 8px; 
            padding: 20px;
            font-size: 13px;
            line-height: 1.5;
        """)
        layout.addWidget(info_card)
        
        layout.addStretch()
        return page

    def _create_screenshot_save_page(self):
        """创建截图保存设置页面"""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(15)

        card = SettingCard()

        # 保存开关
        self.save_toggle = ToggleSwitch()
        row_save = self._create_toggle_row(
            "スクショを自動保存",
            "キャプチャ時にファイルとして自動保存します。",
            self.config_manager.get_screenshot_save_enabled(),
            self.save_toggle
        )
        card.layout.addLayout(row_save)
        card.layout.addWidget(HLine())

        # 保存路径显示
        path_layout = QHBoxLayout()
        current_dir = self.config_manager.get_screenshot_save_path()
        self.save_path_lbl = QLabel(current_dir)
        self.save_path_lbl.setStyleSheet("color: #576B95; background-color: transparent;")  # 仿链接色
        self.save_path_lbl.setCursor(Qt.CursorShape.PointingHandCursor)
        self.save_path_lbl.setWordWrap(True)
        
        lbl_title = QLabel("保存フォルダ:")
        lbl_title.setStyleSheet("background-color: transparent;")
        path_layout.addWidget(lbl_title)
        path_layout.addWidget(self.save_path_lbl)
        card.layout.addLayout(path_layout)
        
        card.layout.addWidget(HLine())

        # 按钮组
        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(10)
        
        btn_style = """
            QPushButton {
                background-color: #F2F2F2;
                border: none;
                border-radius: 4px;
                padding: 6px 12px;
                color: #333;
            }
            QPushButton:hover { background-color: #E6E6E6; }
        """
        
        btn_change = QPushButton("変更")
        btn_change.setStyleSheet(btn_style)
        btn_change.clicked.connect(self._change_save_dir)
        
        btn_open = QPushButton("開く")
        btn_open.setStyleSheet(btn_style)
        btn_open.clicked.connect(self._open_save_dir)
        
        btn_layout.addStretch()
        btn_layout.addWidget(btn_change)
        btn_layout.addWidget(btn_open)
        
        card.layout.addLayout(btn_layout)
        layout.addWidget(card)
        
        # 提示信息
        info_lbl = QLabel("💡 ヒント: 自動保存をオフにしても、クリップボードにコピーされます。")
        info_lbl.setStyleSheet("color: #888; padding: 5px; background-color: transparent;")
        layout.addWidget(info_lbl)
        
        layout.addStretch()
        return page

    def _create_ocr_page(self):
        """创建 OCR 设置页面"""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(10)  # 减少间距

        # 检测 OCR 文件是否存在
        ocr_files_exist = self._check_ocr_files()
        
        # 如果 OCR 模块不可用，显示紧凑的警告
        if not ocr_files_exist:
            warning_card = SettingCard()
            warning_layout = QVBoxLayout()
            warning_layout.setSpacing(8)
            
            warning_header = QHBoxLayout()
            warning_icon = QLabel("ℹ️")
            warning_icon.setStyleSheet("font-size: 24px; background-color: transparent;")
            warning_header.addWidget(warning_icon)
            
            warning_title = QLabel("無OCR版本 / OCRモジュールが見つかりません")
            warning_title.setStyleSheet("font-size: 14px; font-weight: bold; color: #2196F3; background-color: transparent;")
            warning_header.addWidget(warning_title)
            warning_header.addStretch()
            warning_layout.addLayout(warning_header)
            
            warning_text = QLabel(
                "これは無OCRのカジュアルバージョン。\n\n"
                "OCR機能が必要な場合は、フル版をダウンロードするか、\n"
                "開発者に問い合わせしてください。:\n"
                "RI　JYAARU"
            )
            warning_text.setStyleSheet("font-size: 12px; color: #666; background-color: transparent;")
            warning_text.setWordWrap(True)
            warning_layout.addWidget(warning_text)
            
            warning_card.layout.addLayout(warning_layout)
            layout.addWidget(warning_card)

        # 主设置卡片
        card = SettingCard()

        # OCR 功能开关
        self.ocr_enable_toggle = ToggleSwitch()
        if not ocr_files_exist:
            self.ocr_enable_toggle.setEnabled(False)
            self.ocr_enable_toggle.setChecked(False)
        
        row_ocr_enable = self._create_toggle_row(
            "OCR機能を有効化",
            "ピン留めウィンドウでテキスト認識と選択を有効にします。",
            self.config_manager.get_ocr_enabled() if ocr_files_exist else False,
            self.ocr_enable_toggle
        )
        card.layout.addLayout(row_ocr_enable)
        card.layout.addWidget(HLine())

        # 语言提示 - 紧凑布局
        lang_layout = QHBoxLayout()
        lang_layout.setSpacing(10)
        
        lang_icon = QLabel("🌏")
        lang_icon.setStyleSheet("font-size: 16px; background-color: transparent;")
        lang_layout.addWidget(lang_icon)
        
        lang_info = QLabel("自動言語認識: 中国語・日本語・英語の混合認識に対応")
        lang_info.setStyleSheet("font-size: 12px; color: #666; background-color: transparent;")
        lang_layout.addWidget(lang_info)
        lang_layout.addStretch()
        
        card.layout.addLayout(lang_layout)
        
        # 如果模块可用，添加预处理选项
        if ocr_files_exist:
            card.layout.addWidget(HLine())
            
            # 灰度转换 - 紧凑布局
            gray_layout = QHBoxLayout()
            gray_layout.setSpacing(10)
            
            self.ocr_grayscale_toggle = ToggleSwitch()
            self.ocr_grayscale_toggle.setChecked(self.config_manager.get_ocr_grayscale_enabled())
            gray_layout.addWidget(self.ocr_grayscale_toggle)
            
            gray_label = QLabel("グレースケール変換")
            gray_label.setStyleSheet("font-size: 13px; color: #000; background-color: transparent;")
            gray_layout.addWidget(gray_label)
            
            gray_hint = QLabel("(~5ms)")
            gray_hint.setStyleSheet("font-size: 11px; color: #888; background-color: transparent;")
            gray_layout.addWidget(gray_hint)
            gray_layout.addStretch()
            
            card.layout.addLayout(gray_layout)
            
            # 图像放大 - 紧凑布局
            upscale_layout = QHBoxLayout()
            upscale_layout.setSpacing(10)
            
            self.ocr_upscale_toggle = ToggleSwitch()
            self.ocr_upscale_toggle.setChecked(self.config_manager.get_ocr_upscale_enabled())
            upscale_layout.addWidget(self.ocr_upscale_toggle)
            
            upscale_label = QLabel("画像拡大")
            upscale_label.setStyleSheet("font-size: 13px; color: #000; background-color: transparent;")
            upscale_layout.addWidget(upscale_label)
            
            upscale_hint = QLabel("(~30-50ms)")
            upscale_hint.setStyleSheet("font-size: 11px; color: #888; background-color: transparent;")
            upscale_layout.addWidget(upscale_hint)
            
            # 放大倍数 - 内联
            upscale_layout.addSpacing(20)
            scale_label = QLabel("倍率:")
            scale_label.setStyleSheet("font-size: 12px; color: #666; background-color: transparent;")
            upscale_layout.addWidget(scale_label)
            
            self.ocr_scale_spinbox = QDoubleSpinBox()
            self.ocr_scale_spinbox.setRange(1.0, 3.0)
            self.ocr_scale_spinbox.setSingleStep(0.1)
            self.ocr_scale_spinbox.setDecimals(1)
            self.ocr_scale_spinbox.setValue(self.config_manager.get_ocr_upscale_factor())
            self.ocr_scale_spinbox.setStyleSheet(self._get_input_style())
            self.ocr_scale_spinbox.setFixedWidth(70)
            upscale_layout.addWidget(self.ocr_scale_spinbox)
            
            times_label = QLabel("×")
            times_label.setStyleSheet("font-size: 12px; color: #666; background-color: transparent;")
            upscale_layout.addWidget(times_label)
            
            upscale_layout.addStretch()
            card.layout.addLayout(upscale_layout)
            
            # ====== 🔥 连接 OCR 设置信号（实时保存）======
            # 注意：ToggleSwitch 使用 toggled 信号，不是 stateChanged
            self.ocr_grayscale_toggle.toggled.connect(lambda checked: self.config_manager.set_ocr_grayscale_enabled(checked))
            self.ocr_upscale_toggle.toggled.connect(lambda checked: self.config_manager.set_ocr_upscale_enabled(checked))
            self.ocr_scale_spinbox.valueChanged.connect(lambda value: self.config_manager.set_ocr_upscale_factor(value))
        
        # ====== 🔥 连接 OCR 启用信号（在模块可用的情况下） ======
        if ocr_files_exist:
            self.ocr_enable_toggle.toggled.connect(lambda checked: self.config_manager.set_ocr_enabled(checked))
        
        layout.addWidget(card)
        
        # 底部提示 - 紧凑版
        if ocr_files_exist:
            info_lbl = QLabel("💡 小さい文字が認識できない場合は、画像拡大を有効にしてください。")
            info_lbl.setStyleSheet("color: #888; font-size: 11px; padding: 5px; background-color: transparent;")
            info_lbl.setWordWrap(True)
            layout.addWidget(info_lbl)
        
        layout.addStretch()
        return page
    
    def _check_ocr_files(self):
        """检测 OCR 模块是否可用"""
        try:
            # 使用 find_spec 检查模块是否存在而不实际导入，避免启动卡顿
            import importlib.util
            # 检查 rapidocr_onnxruntime (通常包名是这个) 或者 rapidocr
            spec1 = importlib.util.find_spec("rapidocr_onnxruntime")
            spec2 = importlib.util.find_spec("rapidocr")
            onnx_spec = importlib.util.find_spec("onnxruntime")
            
            # 只要有 rapidocr 相关包和 onnxruntime 即可
            has_rapid = (spec1 is not None) or (spec2 is not None)
            return has_rapid and (onnx_spec is not None)
        except ImportError:
            return False

    def _create_log_page(self):
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)

        card = SettingCard()

        # 日志开关
        self.log_toggle = ToggleSwitch()
        row_log = self._create_toggle_row(
            "ログを保存する",
            "アプリの動作記録をファイルに保存します。",
            self.config_manager.get_log_enabled(),
            self.log_toggle
        )
        card.layout.addLayout(row_log)
        card.layout.addWidget(HLine())

        # 路径显示
        path_layout = QHBoxLayout()
        current_dir = self.config_manager.get_log_dir()
        self.path_lbl = QLabel(current_dir)
        self.path_lbl.setStyleSheet("color: #576B95; background-color: transparent;")  # 仿链接色
        self.path_lbl.setCursor(Qt.CursorShape.PointingHandCursor)  # 设置鼠标指针
        self.path_lbl.setWordWrap(True)
        
        lbl_title = QLabel("保存場所:")
        lbl_title.setStyleSheet("background-color: transparent;")
        path_layout.addWidget(lbl_title)
        path_layout.addWidget(self.path_lbl)
        card.layout.addLayout(path_layout)
        
        card.layout.addWidget(HLine())

        # 按钮组
        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(10)
        
        btn_style = """
            QPushButton {
                background-color: #F2F2F2;
                border: none;
                border-radius: 4px;
                padding: 6px 12px;
                color: #333;
            }
            QPushButton:hover { background-color: #E6E6E6; }
        """
        
        btn_change = QPushButton("変更")
        btn_change.setStyleSheet(btn_style)
        btn_change.clicked.connect(self._change_log_dir)
        
        btn_open = QPushButton("開く")
        btn_open.setStyleSheet(btn_style)
        btn_open.clicked.connect(self._open_log_dir)

        btn_open_latest = QPushButton("最新ログ")
        btn_open_latest.setStyleSheet(btn_style)
        btn_open_latest.clicked.connect(self._open_latest_log_file)
        
        btn_layout.addStretch()
        btn_layout.addWidget(btn_change)
        btn_layout.addWidget(btn_open)
        btn_layout.addWidget(btn_open_latest)
        
        card.layout.addLayout(btn_layout)

        # 当前日志文件提示（如果已经启动logger，这里可以告诉用户文件名）
        self.latest_log_lbl = QLabel("")
        self.latest_log_lbl.setStyleSheet("color: #888; font-size: 12px; background-color: transparent;")
        self.latest_log_lbl.setWordWrap(True)
        self._refresh_latest_log_label()
        card.layout.addWidget(self.latest_log_lbl)

        layout.addWidget(card)
        layout.addStretch()
        return page

    def _refresh_latest_log_label(self):
        """刷新当前/最新日志文件路径显示。"""
        try:
            from core.logger import get_logger
            logger = get_logger()
            log_path = None
            if getattr(logger, "log_file", None) is not None:
                try:
                    log_path = logger.log_file.name
                except Exception:
                    log_path = None
            if log_path:
                self.latest_log_lbl.setText(f"現在のログ: {log_path}")
            else:
                self.latest_log_lbl.setText("現在のログ: (未生成)  ※ログは起動後に作成されます")
        except Exception:
            # 不影响设置页打开
            if hasattr(self, "latest_log_lbl"):
                self.latest_log_lbl.setText("現在のログ: (未生成)")

    def _open_latest_log_file(self):
        """打开日志目录下最新的 runtime_*.log 文件；若不存在则创建目录并提示。"""
        import glob
        path = self.config_manager.get_log_dir()
        if not os.path.exists(path):
            os.makedirs(path, exist_ok=True)

        pattern = os.path.join(path, "runtime_*.log")
        files = glob.glob(pattern)
        if not files:
            QMessageBox.information(self, "ログ", "まだログファイルがありません。まず一度アプリを起動して操作してください。")
            return

        latest = max(files, key=os.path.getmtime)
        if platform.system() == "Windows":
            os.startfile(latest)
        elif platform.system() == "Darwin":
            os.system(f"open {latest}")
        else:
            os.system(f"xdg-open {latest}")

    def _create_misc_page(self):
        """创建杂项设置页面"""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(15)

        card = SettingCard()

        # 主界面显示开关
        self.show_main_window_toggle = ToggleSwitch()
        row_show = self._create_toggle_row(
            "起動時にメインウィンドウを表示",
            "オフにすると、バックグラウンドで起動します。",
            self.config_manager.get_show_main_window(),
            self.show_main_window_toggle
        )
        card.layout.addLayout(row_show)

        card.layout.addWidget(HLine())

        # 钉图工具栏自动显示
        self.pin_auto_toolbar_toggle = ToggleSwitch()
        row_pin_toolbar = self._create_toggle_row( 
            "ピン留めで描画ツールを自動表示",
            "オン: マウスがピン留めウィンドウに入るとツールバーを表示します。\n"
            "オフ: 右クリックでツールバーボタンで表示します。",
            self.config_manager.get_pin_auto_toolbar(),  # 🔥 修复：使用正确的方法名
            self.pin_auto_toolbar_toggle
        )
        card.layout.addLayout(row_pin_toolbar)
        
        layout.addWidget(card)
        
        # 提示信息
        info_lbl = QLabel("💡 ヒント: バックグラウンド起動でも、タスクトレイから操作できます。")
        info_lbl.setStyleSheet("color: #888; padding: 5px; background-color: transparent;")
        layout.addWidget(info_lbl)
        
        layout.addStretch()
        return page

    def _create_info_page(self):
        """创建情報页面"""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(15)

        # 卡片: 软件信息
        card = SettingCard()
        
        # 标题
        title_label = QLabel("ソフトウェア情報")
        title_label.setStyleSheet("font-size: 16px; font-weight: bold; background-color: transparent; color: #333;")
        card.layout.addWidget(title_label)
        
        # 分隔线
        line = QFrame()
        line.setFrameShape(QFrame.Shape.HLine)
        line.setStyleSheet("background-color: #E5E5E5;")
        card.layout.addWidget(line)
        
        # 软件名称和版本
        name_label = QLabel("Jietuba - キャプチャーツール")
        name_label.setStyleSheet("font-size: 14px; font-weight: bold; background-color: transparent; color: #07C160;")
        card.layout.addWidget(name_label)
        
        version_label = QLabel("バージョン: 1.0.0")
        version_label.setStyleSheet("font-size: 12px; background-color: transparent; color: #666;")
        card.layout.addWidget(version_label)
        
        # 说明文本
        desc_label = QLabel(
            "PyQt6フレームワークをベースに開発された高性能スクリーンショットツール。\n"
            "豊富な編集機能、OCR文字認識、長いスクリーンショット、ピン留めなど、\n"
            "多彩な機能を備えています。"
        )
        desc_label.setStyleSheet("font-size: 12px; background-color: transparent; color: #666; line-height: 1.6;")
        desc_label.setWordWrap(True)
        card.layout.addWidget(desc_label)
        
        # 按钮：打开详细说明
        open_btn = QPushButton("📖 詳細情報を表示")
        open_btn.setFixedHeight(36)
        open_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        open_btn.setStyleSheet("""
            QPushButton {
                background-color: #07C160;
                color: white;
                border: none;
                border-radius: 4px;
                font-size: 13px;
                padding: 8px 16px;
            }
            QPushButton:hover {
                background-color: #06AD56;
            }
            QPushButton:pressed {
                background-color: #059048;
            }
        """)
        open_btn.clicked.connect(self._open_about_page)
        card.layout.addWidget(open_btn)
        
        # 作者信息
        card.layout.addSpacing(10)
        author_label = QLabel("👨‍💻 開発者: rijyaaru")
        author_label.setStyleSheet("font-size: 12px; background-color: transparent; color: #666;")
        card.layout.addWidget(author_label)
        
        tech_label = QLabel("🛠️ 技術: Python + Rust + PyQt6 + PaddleOCR")
        tech_label.setStyleSheet("font-size: 12px; background-color: transparent; color: #666;")
        card.layout.addWidget(tech_label)
        
        layout.addWidget(card)
        layout.addStretch()
        return page
    
    def _open_about_page(self):
        """打开关于页面（本地 HTML 文件）"""
        import webbrowser
        from core.resource_manager import ResourceManager
        
        # 获取 ABOUT.html 文件路径
        about_path = ResourceManager.get_resource_path("svg/ABOUT.html")
        
        if os.path.exists(about_path):
            # 使用默认浏览器打开 HTML 文件
            webbrowser.open(f"file:///{about_path.replace(chr(92), '/')}")
        else:
            QMessageBox.warning(
                self,
                "ファイルが見つかりません",
                f"詳細情報ファイルが見つかりませんでした:\n{about_path}"
            )

    # ================= 底部按钮 =================

    def _create_button_area(self):
        layout = QHBoxLayout()
        layout.setSpacing(15)
        
        reset_btn = QPushButton("このページをリセット")
        reset_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        reset_btn.setStyleSheet("""
            QPushButton { color: #FA5151; background: transparent; border: none; font-size: 13px; }
            QPushButton:hover { color: #D00000; }
        """)
        reset_btn.clicked.connect(self._reset_current_page)
        
        cancel_btn = QPushButton("キャンセル")
        cancel_btn.setFixedSize(100, 32)
        cancel_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        cancel_btn.setStyleSheet("""
            QPushButton { background-color: #E5E5E5; border-radius: 4px; border: none; color: #333; }
            QPushButton:hover { background-color: #D6D6D6; }
        """)
        cancel_btn.clicked.connect(self.reject)
        
        ok_btn = QPushButton("適用")
        ok_btn.setFixedSize(100, 32)
        ok_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        ok_btn.setStyleSheet("""
            QPushButton { background-color: #07C160; border-radius: 4px; border: none; color: #FFF; font-weight: bold; }
            QPushButton:hover { background-color: #06AD56; }
        """)
        ok_btn.clicked.connect(self.accept)

        layout.addWidget(reset_btn)
        layout.addStretch()
        layout.addWidget(cancel_btn)
        layout.addWidget(ok_btn)
        
        return layout

    # ================= 逻辑处理 =================

    def _on_nav_changed(self, index):
        title_map = ["ショートカット設定", "長いスクリーンショット", "スマート選択", "スクショ保存設定", "OCR設定", "ログ設定", "その他設定", "ソフトウェア情報"]
        if 0 <= index < len(title_map):
            self.content_title.setText(title_map[index])
            self.content_stack.setCurrentIndex(index)

    def _change_save_dir(self):
        """更改截图保存目录（只更新UI，不立即保存）"""
        new_dir = QFileDialog.getExistingDirectory(self, "スクショ保存フォルダを選択", self.config_manager.get_screenshot_save_path())
        if new_dir:
            # 只更新界面显示，不立即保存到配置
            self.save_path_lbl.setText(new_dir)

    def _open_save_dir(self):
        """打开截图保存目录"""
        path = self.config_manager.get_screenshot_save_path()
        if not os.path.exists(path):
            os.makedirs(path, exist_ok=True)
        if platform.system() == "Windows":
            os.startfile(path)
        elif platform.system() == "Darwin":
            os.system(f"open {path}")
        else:
            os.system(f"xdg-open {path}")

    def _change_log_dir(self):
        """更改日志目录（只更新UI，不立即保存）"""
        new_dir = QFileDialog.getExistingDirectory(self, "ログ保存フォルダを選択", self.config_manager.get_log_dir())
        if new_dir:
            # 只更新界面显示，不立即保存到配置
            self.path_lbl.setText(new_dir)

    def _open_log_dir(self):
        path = self.config_manager.get_log_dir()
        if not os.path.exists(path):
            os.makedirs(path, exist_ok=True)
        if platform.system() == "Windows":
            os.startfile(path)
        elif platform.system() == "Darwin":
            os.system(f"open {path}")
        else:
            os.system(f"xdg-open {path}")

    def _reset_current_page(self):
        """重置当前页面的设置为默认值"""
        current_index = self.content_stack.currentIndex()
        page_names = ["ショートカット設定", "長いスクリーンショット", "スマート選択", "スクショ保存設定", "ログ設定", "その他設定"]
        
        # 根据当前页面重置不同的设置
        if current_index == 0:  # 快捷键设置页面
            self._reset_hotkey_page()
        elif current_index == 1:  # 长截图设置页面
            self._reset_long_screenshot_page()
        elif current_index == 2:  # 智能选择页面
            self._reset_smart_selection_page()
        elif current_index == 3:  # 截图保存设置页面
            self._reset_screenshot_save_page()
        elif current_index == 4:  # 日志设置页面
            self._reset_log_page()
        elif current_index == 5:  # 杂项设置页面
            self._reset_misc_page()
        elif current_index == 6:  # 情報页面
            pass  # 情報页面没有需要重置的设置
        
 
    
    def _reset_hotkey_page(self):
        """重置快捷键设置页面"""
        self.hotkey_input.setText("ctrl+shift+a")
    
    def _reset_long_screenshot_page(self):
        """重置长截图设置页面"""
        self.engine_combo.setCurrentIndex(0)  # rust
        self.debug_toggle.setChecked(False)
        self.cooldown_spinbox.setValue(0.15)  # 默认滚动冷却时间
        # 高级参数已隐藏，无需重置
    
    def _reset_smart_selection_page(self):
        """重置智能选择页面"""
        if hasattr(self, 'smart_toggle'):
            self.smart_toggle.setChecked(False)
    
    def _reset_screenshot_save_page(self):
        """重置截图保存设置页面"""
        if hasattr(self, 'save_toggle'):
            self.save_toggle.setChecked(True)
        # 重置保存路径为默认值
        if hasattr(self, 'save_path_lbl'):
            default_path = os.path.join(os.path.expanduser("~"), "Desktop", "スクショ")
            self.save_path_lbl.setText(default_path)
    
    def _reset_log_page(self):
        """重置日志设置页面"""
        if hasattr(self, 'log_toggle'):
            self.log_toggle.setChecked(True)
        # 重置日志目录为默认值
        if hasattr(self, 'path_lbl'):
            from pathlib import Path
            default = str(Path.home() / ".jietuba" / "logs")
            self.path_lbl.setText(default)
    
    def _reset_misc_page(self):
        """重置杂项设置页面"""
        if hasattr(self, 'show_main_window_toggle'):
            self.show_main_window_toggle.setChecked(True)
        if hasattr(self, 'pin_auto_toolbar_toggle'):
            self.pin_auto_toolbar_toggle.setChecked(True)

    def accept(self):
        """保存所有设置"""
        # 0. 快捷键
        self.config_manager.set_hotkey(self.hotkey_input.text())

        # 1. 基础设置
        # 智能选区
        if hasattr(self, 'smart_toggle'):
            self.config_manager.set_smart_selection(self.smart_toggle.isChecked())
        
        # 日志设置（实时生效）
        if hasattr(self, 'log_toggle'):
            log_enabled = self.log_toggle.isChecked()
            self.config_manager.set_log_enabled(log_enabled)
            
            # 如果日志目录改变，更新日志目录
            if hasattr(self, 'path_lbl'):
                old_log_dir = self.config_manager.get_log_dir()
                new_log_dir = self.path_lbl.text()
                self.config_manager.set_log_dir(new_log_dir)

                # 动态更新日志系统
                from core.logger import get_logger
                logger = get_logger()
                logger.set_enabled(log_enabled)
                # 现有logger实现：初始化完成后不允许切换目录（会warning）。
                # 这里改为在UI层提示“重启生效”。
                if new_log_dir != old_log_dir:
                    logger.set_log_dir(new_log_dir)
                    QMessageBox.information(self, "ログ", "ログ保存場所を変更しました。\n※変更は次回起動時に完全に反映されます。")

                # 更新提示文本
                self._refresh_latest_log_label()
        
        # 2. 截图保存设置
        if hasattr(self, 'save_toggle'):
            self.config_manager.set_screenshot_save_enabled(self.save_toggle.isChecked())
        # 保存路径从标签读取（如果用户修改过）
        if hasattr(self, 'save_path_lbl'):
            self.config_manager.set_screenshot_save_path(self.save_path_lbl.text())
        
        # 3. OCR 设置
        if hasattr(self, 'ocr_enable_toggle'):
            self.config_manager.set_ocr_enabled(self.ocr_enable_toggle.isChecked())
        # 注意: 语言设置已移除,RapidOCR 自动支持多语言混合识别
        
        # OCR 图像预处理设置
        if hasattr(self, 'ocr_grayscale_toggle'):
            self.config_manager.set_ocr_grayscale_enabled(self.ocr_grayscale_toggle.isChecked())
        if hasattr(self, 'ocr_upscale_toggle'):
            self.config_manager.set_ocr_upscale_enabled(self.ocr_upscale_toggle.isChecked())
        if hasattr(self, 'ocr_scale_spinbox'):
            self.config_manager.set_ocr_upscale_factor(self.ocr_scale_spinbox.value())
        
        # 4. 杂项设置
        if hasattr(self, 'show_main_window_toggle'):
            self.config_manager.set_show_main_window(self.show_main_window_toggle.isChecked())
        if hasattr(self, 'pin_auto_toolbar_toggle'):
            self.config_manager.set_pin_auto_toolbar(self.pin_auto_toolbar_toggle.isChecked())
        
        # 5. 引擎和长截图参数
        if hasattr(self, 'engine_combo'):
            self.config_manager.set_long_stitch_engine(self.engine_combo.currentData())
        if hasattr(self, 'debug_toggle'):
            self.config_manager.set_long_stitch_debug(self.debug_toggle.isChecked())
        if hasattr(self, 'cooldown_spinbox'):
            self.config_manager.settings.setValue('screenshot/scroll_cooldown', self.cooldown_spinbox.value())
        
        # 6. Rust 参数
        if hasattr(self, 'spinboxes'):
            for key, spinbox in self.spinboxes.items():
                val = spinbox.value()
                self.config_manager.settings.setValue(f'screenshot/{key}', val)
        
        self.config_manager.settings.setValue('screenshot/rust_try_rollback', self.rollback_toggle.isChecked())

        print("💾 すべての設定を保存しました")
        super().accept()

    def get_hotkey(self):
        return self.hotkey_input.text().strip()
    
    def update_hotkey(self, new_hotkey):
        """更新对话框中显示的快捷键"""
        self.hotkey_input.setText(new_hotkey)


# ==========================================
# 3. 用于测试的 Mock 类
# ==========================================
from PyQt6.QtCore import QSettings

class MockConfig:
    def __init__(self):
        self.settings = QSettings("TestApp", "Settings")
    def get_smart_selection(self): return False
    def set_smart_selection(self, v): pass
    def get_log_enabled(self): return True
    def set_log_enabled(self, v): pass
    def get_log_dir(self): return os.path.expanduser("~")
    def set_log_dir(self, v): pass
    def get_long_stitch_engine(self): return "hash_rust"
    def set_long_stitch_engine(self, v): pass
    def get_long_stitch_debug(self): return False
    def set_long_stitch_debug(self, v): pass
    def get_screenshot_save_enabled(self): return True
    def set_screenshot_save_enabled(self, v): pass
    def get_screenshot_save_path(self): return os.path.join(os.path.expanduser("~"), "Desktop", "スクショ")
    def set_screenshot_save_path(self, v): pass
    def get_show_main_window(self): return True
    def set_show_main_window(self, v): pass
    def get_ocr_enabled(self): return True
    def set_ocr_enabled(self, v): pass
    def get_ocr_grayscale_enabled(self): return False
    def set_ocr_grayscale_enabled(self, v): pass
    def get_ocr_upscale_enabled(self): return False
    def set_ocr_upscale_enabled(self, v): pass
    def get_ocr_upscale_factor(self): return 2.0
    def set_ocr_upscale_factor(self, v): pass
    def get_pin_auto_toolbar(self): return True
    def set_pin_auto_toolbar(self, v): pass

if __name__ == "__main__":
    app = QApplication(sys.argv)
    font = QFont("Microsoft YaHei", 9)
    app.setFont(font)
    
    dlg = SettingsDialog(MockConfig())
    dlg.show()
    sys.exit(app.exec())
