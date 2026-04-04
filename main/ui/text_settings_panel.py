"""
文字工具设置面板
提供字体、字号、颜色、描边、阴影等高级设置
"""
import time
from PySide6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QPushButton, QLabel,
    QComboBox, QCheckBox, QFrame, QButtonGroup,
    QSlider, QApplication
)
from PySide6.QtCore import Qt, Signal, QSize, QEvent, QTimer, QPoint
from PySide6.QtGui import QIcon, QColor, QFont, QFontDatabase
from .base_settings_panel import StepperWidget, build_settings_panel_stylesheet, paint_rounded_panel, PANEL_SCALE
from .color_picker_button import ColorPickerButton
from core.constants import CSS_FONT_FAMILY, DEFAULT_FONT_FAMILY
from core import safe_event
from core.logger import log_exception

class TextSettingsPanel(QWidget):
    """文字工具二级菜单"""
    
    # 信号定义
    font_changed = Signal(QFont)
    color_changed = Signal(QColor)
    background_changed = Signal(bool, QColor, int)
    
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        # 使用更安全的默认字体选择
        default_font_family = DEFAULT_FONT_FAMILY
        # 缓存字体列表，避免多次调用 QFontDatabase.families()（首次调用很慢）
        self._cached_fonts = QFontDatabase.families()
        if default_font_family not in self._cached_fonts:
            # fallback到系统可用的字体
            if "Arial" in self._cached_fonts:
                default_font_family = "Arial"
            elif "SimSun" in self._cached_fonts:
                default_font_family = "SimSun"
            elif len(self._cached_fonts) > 0:
                default_font_family = self._cached_fonts[0]
        
        self.current_font = QFont(default_font_family, 16)
        self.current_color = QColor(Qt.GlobalColor.red)
        self.background_enabled = False
        self.background_color = QColor(255, 255, 255)
        self.background_opacity = 255
        self._background_hover_btn = False
        self._background_hover_popup = False
        self._background_hide_timer = QTimer(self)
        self._background_hide_timer.setSingleShot(True)
        self._background_hide_timer.setInterval(150)
        self._background_hide_timer.timeout.connect(self._maybe_hide_background_popup)
        
        self._init_ui()
        self._connect_signals()

    @safe_event
    def paintEvent(self, event):
        paint_rounded_panel(self)

    def _init_ui(self):
        """初始化UI布局"""
        self.setStyleSheet(build_settings_panel_stylesheet(
            combo_enabled=True,
            combo_padding="2px 8px 2px 4px",
            combo_min_width=40,
            combo_max_width=120
        ) + """
            QCheckBox {
                spacing: 5px;
                background-color: transparent;
                border: none;
                font-family: {CSS_FONT_FAMILY};
                font-size: 12px;
                color: #333;
            }
        """)
        
        layout = QHBoxLayout(self)
        layout.setContentsMargins(round(10 * PANEL_SCALE), round(8 * PANEL_SCALE),
                                  round(10 * PANEL_SCALE), round(8 * PANEL_SCALE))
        layout.setSpacing(round(10 * PANEL_SCALE))
        
        # === 1. 基础样式区 ===
        
        # 字体选择 - 使用已缓存的字体列表
        self.font_combo = QComboBox()
        # 过滤出常用中文字体放在前面
        priority_fonts = [DEFAULT_FONT_FAMILY, "SimSun", "SimHei", "KaiTi", "Arial", "Times New Roman"]
        sorted_fonts = []
        # 先添加优先字体
        for f in priority_fonts:
            if f in self._cached_fonts:
                sorted_fonts.append(f)
        # 再添加其他字体
        for f in self._cached_fonts:
            if f not in sorted_fonts:
                sorted_fonts.append(f)
                
        self.font_combo.addItems(sorted_fonts)
        # 设置当前字体，如果不可用则使用第一个可用字体
        if DEFAULT_FONT_FAMILY in sorted_fonts:
            self.font_combo.setCurrentText(DEFAULT_FONT_FAMILY)
        elif len(sorted_fonts) > 0:
            self.font_combo.setCurrentIndex(0)
        self.font_combo.setToolTip(self.tr("Font"))
        
        # 确保字体组合框有合理的最大可见项数
        self.font_combo.setMaxVisibleItems(15)
        # 设置下拉列表的最小宽度，确保字体名称完整显示
        self.font_combo.view().setMinimumWidth(200)
        
        layout.addWidget(self.font_combo)
        
        # 字号选择
        self.size_spin = StepperWidget(16, 8, 144)
        self.size_spin.setFixedWidth(round(60 * PANEL_SCALE))
        self.size_spin.setToolTip(self.tr("Font Size"))
        layout.addWidget(self.size_spin)
        
        # 样式按钮组 (粗体/斜体/下划线)
        _btn_sz = round(28 * PANEL_SCALE)
        self.bold_btn = QPushButton("B")
        self.bold_btn.setCheckable(True)
        self.bold_btn.setFixedSize(_btn_sz, _btn_sz)
        self.bold_btn.setToolTip(self.tr("Bold"))
        # 使用CSS确保粗体效果，不依赖字体变体
        self.bold_btn.setStyleSheet("""
            QPushButton {
                font-weight: bold;
                font-size: 14px;
                font-family: Arial, sans-serif;
            }
        """)
        
        self.italic_btn = QPushButton("I")
        self.italic_btn.setCheckable(True)
        self.italic_btn.setFixedSize(_btn_sz, _btn_sz)
        self.italic_btn.setToolTip(self.tr("Italic"))
        # 使用CSS确保斜体效果
        self.italic_btn.setStyleSheet("""
            QPushButton {
                font-style: italic;
                font-size: 14px;
                font-family: Arial, sans-serif;
            }
        """)
        
        self.underline_btn = QPushButton("U")
        self.underline_btn.setCheckable(True)
        self.underline_btn.setFixedSize(_btn_sz, _btn_sz)
        self.underline_btn.setToolTip(self.tr("Underline"))
        # 使用CSS确保下划线效果
        self.underline_btn.setStyleSheet("""
            QPushButton {
                text-decoration: underline;
                font-size: 14px;
                font-family: Arial, sans-serif;
            }
        """)
        
        layout.addWidget(self.bold_btn)
        layout.addWidget(self.italic_btn)
        layout.addWidget(self.underline_btn)

        # 背景按钮
        self.background_btn = QPushButton("BG")
        self.background_btn.setCheckable(True)
        self.background_btn.setFixedSize(_btn_sz, _btn_sz)
        self.background_btn.setToolTip(self.tr("Text Background"))
        self._update_background_btn_style()
        layout.addWidget(self.background_btn)
        
        # 分隔线
        line1 = QFrame()
        line1.setObjectName("separator")
        line1.setFrameShape(QFrame.Shape.VLine)
        line1.setFixedWidth(1)
        layout.addWidget(line1)
        
        # === 2. 颜色预设区 ===
        
        # 颜色选择按钮
        self.color_btn = ColorPickerButton(
            self.current_color, size=_btn_sz, show_alpha=True
        )
        self.color_btn.setToolTip(self.tr("Custom Color"))
        layout.addWidget(self.color_btn)
        
        # 预设颜色按钮
        preset_colors = [
            "#FF0000", # 红色
            "#FFFF00", # 黄色
            "#00FF00", # 绿色
            "#0000FF", # 蓝色
            "#000000", # 黑色
            "#FFFFFF", # 白色
        ]
        
        _preset_sz = round(24 * PANEL_SCALE)
        for color_str in preset_colors:
            btn = QPushButton()
            btn.setFixedSize(_preset_sz, _preset_sz)
            btn.setToolTip(color_str)
            
            # 设置样式
            border_color = "#888888" if color_str == "#FFFFFF" else "#333333"
            btn.setStyleSheet(f"""
                QPushButton {{
                    background-color: {color_str};
                    border: 1px solid {border_color};
                    border-radius: 6px;
                }}
                QPushButton:hover {{
                    border: 2px solid #000;
                }}
            """)
            
            # 连接点击事件
            # 注意：在循环中使用 lambda 需要捕获变量
            btn.clicked.connect(lambda checked, c=color_str: self._on_preset_color_clicked(c))
            layout.addWidget(btn)
        
        layout.addStretch()

        self._init_background_popup()
        
    def _connect_signals(self):
        """连接内部信号"""
        self.font_combo.currentTextChanged.connect(self._on_font_changed)
        self.size_spin.valueChanged.connect(self._on_font_changed)
        self.bold_btn.toggled.connect(self._on_font_changed)
        self.italic_btn.toggled.connect(self._on_font_changed)
        self.underline_btn.toggled.connect(self._on_font_changed)
        
        self.color_btn.color_changed.connect(self._on_color_picked)
        self.background_btn.toggled.connect(self._on_background_toggled)

        self.background_btn.installEventFilter(self)
        if self._background_popup:
            self._background_popup.installEventFilter(self)
        
    def _on_color_picked(self, color: QColor):
        """颜色选择器回调"""
        self.current_color = color
        self.color_changed.emit(color)

    def _on_preset_color_clicked(self, color_str):
        """点击预设颜色"""
        color = QColor(color_str)
        self.current_color = color
        self.color_btn.set_color(color)
        self.color_changed.emit(color)

    def _init_background_popup(self):
        """初始化背景设置弹窗"""
        self._background_popup = QFrame(self, Qt.WindowType.ToolTip)
        self._background_popup.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self._background_popup.setFrameShape(QFrame.Shape.Box)
        
        # 防止闪烁：记录上次隐藏时间
        self._popup_last_hide_time = 0
        self._background_popup.setFrameShadow(QFrame.Shadow.Plain)
        self._background_popup.setStyleSheet("""
            QFrame {
                background-color: white;
                border: 1px solid #cccccc;
                border-radius: 4px;
            }
            QPushButton {
                background-color: white;
                border: 1px solid #ddd;
                border-radius: 3px;
                padding: 2px;
                font-family: {CSS_FONT_FAMILY};
                font-size: 11px;
            }
            QPushButton:hover {
                background-color: #f0f0f0;
                border: 1px solid #bbb;
            }
            QSlider {
                background-color: transparent;
            }
        """)

        popup_layout = QVBoxLayout(self._background_popup)
        popup_layout.setContentsMargins(8, 6, 8, 6)
        popup_layout.setSpacing(6)

        # 第一行：颜色按钮
        colors_layout = QHBoxLayout()
        colors_layout.setSpacing(6)

        self.background_color_btn = QPushButton()
        self.background_color_btn.setFixedSize(24, 24)
        self.background_color_btn.setToolTip(self.tr("Custom Background Color"))
        self._update_background_color_btn()
        colors_layout.addWidget(self.background_color_btn)

        preset_colors = [
            ("#FFFFFF", self.tr("White")),
            ("#000000", self.tr("Black")),
            ("#FFFF00", self.tr("Yellow")),
        ]

        for color_hex, tooltip in preset_colors:
            btn = QPushButton()
            btn.setFixedSize(22, 22)
            btn.setToolTip(tooltip)
            border_color = "#888888" if color_hex == "#FFFFFF" else "#333333"
            btn.setStyleSheet(f"""
                QPushButton {{
                    background-color: {color_hex};
                    border: 1px solid {border_color};
                    border-radius: 6px;
                }}
                QPushButton:hover {{
                    border: 2px solid #000;
                }}
            """)
            btn.clicked.connect(lambda checked, c=color_hex: self._on_background_preset_color(c))
            colors_layout.addWidget(btn)

        popup_layout.addLayout(colors_layout)

        # 第二行：透明度滑动条
        self.background_opacity_slider = QSlider(Qt.Orientation.Horizontal)
        self.background_opacity_slider.setRange(0, 255)
        self.background_opacity_slider.setValue(self.background_opacity)
        self.background_opacity_slider.setToolTip(self.tr("Background Opacity"))
        popup_layout.addWidget(self.background_opacity_slider)

        self.background_color_btn.clicked.connect(self._pick_background_color)
        self.background_opacity_slider.valueChanged.connect(self._on_background_opacity_changed)

        self._background_popup.hide()

    def _update_background_btn_style(self):
        """更新背景按钮样式"""
        if self.background_btn.isChecked():
            self.background_btn.setStyleSheet("""
                QPushButton {
                    background-color: #e0e0e0;
                    border: 1px solid #999;
                    font-size: 12px;
                }
            """)
        else:
            self.background_btn.setStyleSheet("""
                QPushButton {
                    background-color: white;
                    border: 1px solid #ddd;
                    font-size: 12px;
                }
                QPushButton:hover {
                    background-color: #f0f0f0;
                    border: 1px solid #bbb;
                }
            """)

    def _update_background_color_btn(self):
        """更新背景颜色按钮显示"""
        color = QColor(self.background_color)
        color.setAlpha(self.background_opacity)
        self.background_color_btn.setStyleSheet(f"""
            background-color: {color.name()};
            border: 1px solid #999;
            border-radius: 3px;
        """)

    def _on_background_toggled(self, checked: bool):
        self.background_enabled = checked
        self._update_background_btn_style()
        if not checked:
            self._background_hover_btn = False
            self._background_hover_popup = False
            self._background_hide_timer.stop()
            self._background_popup.hide()
            self._popup_last_hide_time = time.time()  # 记录隐藏时间防止闪烁
        else:
            # 开启时，如果鼠标在按钮上，立即显示popup
            if self.background_btn.underMouse():
                self._background_hover_btn = True
                self._background_hide_timer.stop()
                # 延迟一点点再显示，确保toggle动画完成
                QTimer.singleShot(50, self._show_background_popup)
        self._emit_background_changed()

    def _pick_background_color(self):
        from PySide6.QtWidgets import QColorDialog
        dlg = QColorDialog(self.background_color, None)
        dlg.setWindowTitle(self.tr("Background Color"))
        dlg.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
        if dlg.exec():
            self.background_color = dlg.selectedColor()
            self._update_background_color_btn()
            self._emit_background_changed()

    def _on_background_preset_color(self, color_hex: str):
        self.background_color = QColor(color_hex)
        self._update_background_color_btn()
        self._emit_background_changed()

    def _on_background_opacity_changed(self, value: int):
        self.background_opacity = value
        self._update_background_color_btn()
        self._emit_background_changed()

    def _emit_background_changed(self):
        color = QColor(self.background_color)
        color.setAlpha(self.background_opacity)
        self.background_changed.emit(self.background_enabled, color, self.background_opacity)

    def _show_background_popup(self):
        if not self.background_btn.isChecked():
            return
        if self._background_popup.isVisible():
            return
        # 防止闪烁：如果刚隐藏不久，不立即显示
        if time.time() - self._popup_last_hide_time < 0.2:
            return
        self._background_popup.adjustSize()
        anchor = self.background_btn.mapToGlobal(QPoint(0, self.background_btn.height() + 4))
        popup_pos = self._resolve_popup_position(anchor)
        if self._background_popup.isWindow():
            self._background_popup.move(popup_pos)
        else:
            self._background_popup.move(self.mapFromGlobal(popup_pos))
        self._background_popup.show()
        self._background_popup.raise_()

    def _resolve_popup_position(self, anchor: QPoint) -> QPoint:
        popup_size = self._background_popup.sizeHint()
        x = anchor.x()
        y = anchor.y()

        screen = QApplication.screenAt(anchor)
        if screen is None:
            screen = QApplication.primaryScreen()
        if screen is None:
            return QPoint(x, y)

        available = screen.availableGeometry()
        max_x = available.right() - popup_size.width() + 1
        max_y = available.bottom() - popup_size.height() + 1
        min_x = available.left()
        min_y = available.top()

        if x > max_x:
            x = max_x
        if x < min_x:
            x = min_x

        if y > max_y:
            y = anchor.y() - popup_size.height() - 4
            if y < min_y:
                y = max_y

        return QPoint(x, y)

    def _maybe_hide_background_popup(self):
        if not (self._background_hover_btn or self._background_hover_popup):
            self._background_popup.hide()
            self._popup_last_hide_time = time.time()

    @safe_event
    def eventFilter(self, obj, event):
        if obj == self.background_btn:
            if event.type() == QEvent.Type.Enter:
                self._background_hover_btn = True
                self._background_hide_timer.stop()
                if self.background_btn.isChecked():
                    self._show_background_popup()
                else:
                    self._background_popup.hide()
            elif event.type() == QEvent.Type.Leave:
                self._background_hover_btn = False
                if self.background_btn.isChecked():
                    self._background_hide_timer.start()
                else:
                    self._background_popup.hide()
        elif obj == self._background_popup:
            if event.type() == QEvent.Type.Enter:
                self._background_hover_popup = True
                self._background_hide_timer.stop()
            elif event.type() == QEvent.Type.Leave:
                self._background_hover_popup = False
                if self.background_btn.isChecked():
                    self._background_hide_timer.start()
                else:
                    self._background_popup.hide()

        return super().eventFilter(obj, event)
            
    def _on_font_changed(self):
        """字体属性改变"""
        font = QFont(self.font_combo.currentText())
        font.setPointSize(max(1, self.size_spin.value()))
        font.setBold(self.bold_btn.isChecked())
        font.setItalic(self.italic_btn.isChecked())
        font.setUnderline(self.underline_btn.isChecked())
        
        self.current_font = font
        self.font_changed.emit(font)
        
    # 移除特效改变处理函数
    # def _on_effect_changed(self): ...

    def set_state_from_item(self, item):
        """根据选中的 TextItem 更新面板状态"""
        if not item: return
        
        # 阻断信号防止循环触发
        self.blockSignals(True)
        
        # 字体
        font = item.font()
        self.font_combo.setCurrentText(font.family())
        point_size = font.pointSize()
        if point_size <= 0:
            point_size = int(round(font.pointSizeF())) if font.pointSizeF() > 0 else 16
        self.size_spin.setValue(max(1, point_size))
        self.bold_btn.setChecked(font.bold())
        self.italic_btn.setChecked(font.italic())
        self.underline_btn.setChecked(font.underline())

        # 背景
        self.background_enabled = getattr(item, "has_background", False)
        bg_color = getattr(item, "background_color", None)
        if isinstance(bg_color, QColor):
            self.background_color = QColor(bg_color)
            self.background_opacity = self.background_color.alpha()
            self.background_color.setAlpha(255)
        else:
            self.background_color = QColor(255, 255, 255)
            self.background_opacity = 255
        self.background_btn.setChecked(self.background_enabled)
        self._update_background_btn_style()
        self._update_background_color_btn()
        self.background_opacity_slider.setValue(self.background_opacity)
        
        # 颜色
        self.current_color = item.defaultTextColor()
        self.color_btn.set_color(self.current_color)
        
        # 移除特效状态同步
            
        self.blockSignals(False)

    def set_background_settings(self, enabled: bool, color: QColor, opacity: int):
        """设置背景配置（不触发信号）"""
        self.background_enabled = bool(enabled)
        self.background_color = QColor(color)
        self.background_opacity = int(opacity)
        self.background_btn.blockSignals(True)
        self.background_btn.setChecked(self.background_enabled)
        self.background_btn.blockSignals(False)
        self._update_background_btn_style()
        self._update_background_color_btn()
        if self.background_opacity_slider:
            self.background_opacity_slider.blockSignals(True)
            self.background_opacity_slider.setValue(self.background_opacity)
            self.background_opacity_slider.blockSignals(False)

    # ── 配置读写 ──────────────────────────────────────────

    def load_from_config(self):
        """从 ToolSettingsManager 加载文字工具配置到面板控件。"""
        try:
            from settings import get_tool_settings_manager
            manager = get_tool_settings_manager()
            text_settings = manager.get_tool_settings("text")
            if not text_settings:
                return

            font = QFont(text_settings.get("font_family", DEFAULT_FONT_FAMILY))
            font.setPointSize(text_settings.get("font_size", 16))
            font.setBold(text_settings.get("font_bold", False))
            font.setItalic(text_settings.get("font_italic", False))
            font.setUnderline(text_settings.get("font_underline", False))
            color = QColor(text_settings.get("color", "#FF0000"))

            self.font_combo.setCurrentText(font.family())
            point_size = font.pointSize()
            if point_size <= 0:
                point_size = int(round(font.pointSizeF())) if font.pointSizeF() > 0 else 16
            self.size_spin.setValue(max(1, point_size))
            self.bold_btn.setChecked(font.bold())
            self.italic_btn.setChecked(font.italic())
            self.underline_btn.setChecked(font.underline())
            self.current_color = color
            self.color_btn.set_color(color)

            bg_enabled = text_settings.get("background_enabled", False)
            bg_color = QColor(text_settings.get("background_color", "#FFFFFF"))
            bg_opacity = text_settings.get("background_opacity", 255)
            self.set_background_settings(bg_enabled, bg_color, bg_opacity)
        except Exception as e:
            log_exception(e, "加载文字设置")

    @staticmethod
    def save_font_to_config(font: QFont):
        """保存字体设置到 ToolSettingsManager。"""
        try:
            from settings import get_tool_settings_manager
            get_tool_settings_manager().update_settings(
                "text",
                font_family=font.family(),
                font_size=font.pointSize(),
                font_bold=font.bold(),
                font_italic=font.italic(),
                font_underline=font.underline(),
            )
        except Exception as e:
            log_exception(e, "保存字体设置")

    @staticmethod
    def save_background_to_config(enabled: bool, color: QColor, opacity: int):
        """保存文字背景设置到 ToolSettingsManager。"""
        try:
            from settings import get_tool_settings_manager
            get_tool_settings_manager().update_settings(
                "text",
                background_enabled=bool(enabled),
                background_color=color.name() if hasattr(color, 'name') else str(color),
                background_opacity=int(opacity),
            )
        except Exception as e:
            log_exception(e, "保存文字背景设置")
 