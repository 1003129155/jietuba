# -*- coding: utf-8 -*-
from PyQt6.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QLabel,
    QTextEdit, QPushButton, QApplication, QWidget,
    QComboBox, QFrame
)
from PyQt6.QtCore import Qt, QPoint, pyqtSignal, QCoreApplication
from core import log_info, log_debug


def _tr(text: str) -> str:
    """翻译辅助函数，明确使用 TranslationDialog 上下文"""
    return QCoreApplication.translate('TranslationDialog', text)


class TranslationDialog(QDialog):
    translate_requested = pyqtSignal(str, str, str)
    
    # 类变量：记住用户选择的目标语言
    _saved_target_lang = None
    # 类变量：记住置顶状态（默认开启）
    _stay_on_top = True

    def __init__(
        self,
        original_text: str = "",
        translated_text: str = "",
        parent: QWidget = None,
        position: QPoint = None,
        source_lang: str = "auto",
        target_lang: str = "ZH"
    ):
        super().__init__(parent)
        self.original_text = original_text
        self.translated_text = translated_text
        # 源语言始终为auto
        self.source_lang = "auto"
        # 如果有保存的目标语言就用保存的，否则用传入的
        if TranslationDialog._saved_target_lang:
            self.target_lang = TranslationDialog._saved_target_lang
        else:
            self.target_lang = target_lang
        self._detected_source_lang = ""
        
        # 根据保存的置顶状态设置窗口标志
        self._is_on_top = TranslationDialog._stay_on_top

        self.setWindowTitle(_tr("Translator"))
        self._update_window_flags()
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        self.setMinimumSize(700, 400)
        self.resize(800, 450)

        if position:
            self.move(position)

        self._setup_ui()
        
        log_debug("Translation window created", "Translation")

    def _get_languages(self):
        """返回支持的语言列表（使用原生语言名称，避免翻译问题）"""
        return {
            "auto": _tr("Auto Detect"),
            "ZH": "中文",
            "EN": "English",
            "JA": "日本語",
            "KO": "한국어",
            "DE": "Deutsch",
            "FR": "Français",
            "ES": "Español",
            "IT": "Italiano",
            "PT": "Português",
            "RU": "Русский",
            "PL": "Polski",
            "NL": "Nederlands",
        }

    def _get_target_languages(self):
        langs = self._get_languages()
        return {k: v for k, v in langs.items() if k != "auto"}

    def _setup_ui(self):
        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(15, 15, 15, 15)
        main_layout.setSpacing(10)

        self._apply_styles()

        # 顶部语言选择栏
        lang_bar = self._create_language_bar()
        main_layout.addLayout(lang_bar)

        content_layout = QHBoxLayout()
        content_layout.setSpacing(15)

        left_panel = self._create_left_panel()
        content_layout.addLayout(left_panel, stretch=1)

        separator = QFrame()
        separator.setFrameShape(QFrame.Shape.VLine)
        separator.setStyleSheet("background-color: #3c3c3c;")
        separator.setFixedWidth(1)
        content_layout.addWidget(separator)

        right_panel = self._create_right_panel()
        content_layout.addLayout(right_panel, stretch=1)

        main_layout.addLayout(content_layout, stretch=1)

        button_layout = self._create_buttons()
        main_layout.addLayout(button_layout)

    def _apply_styles(self):
        self.setStyleSheet('''
            QDialog { background-color: #2b2b2b; color: #e0e0e0; }
            QLabel { color: #e0e0e0; font-size: 13px; }
            QTextEdit {
                background-color: #1e1e1e; color: #e0e0e0;
                border: 1px solid #3c3c3c; border-radius: 6px;
                padding: 10px; font-size: 14px;
            }
            QComboBox {
                background-color: #3c3c3c; color: #e0e0e0;
                border: 1px solid #4a4a4a; border-radius: 4px;
                padding: 5px 10px; min-width: 100px;
            }
            QComboBox:hover { border-color: #0d6efd; }
            QComboBox::drop-down { border: none; width: 20px; }
            QComboBox QAbstractItemView {
                background-color: #2b2b2b; color: #e0e0e0;
                selection-background-color: #0d6efd;
            }
            QPushButton {
                background-color: #3c3c3c; color: #e0e0e0;
                border: none; border-radius: 6px;
                padding: 10px 20px; min-width: 90px;
            }
            QPushButton:hover { background-color: #4a4a4a; }
            QPushButton#translateBtn { background-color: #0d6efd; color: white; font-weight: bold; }
            QPushButton#translateBtn:hover { background-color: #0b5ed7; }
            QPushButton#translateBtn:disabled { background-color: #6c757d; }
            QPushButton#swapBtn { 
                background-color: transparent; color: #e0e0e0;
                font-size: 18px; font-weight: bold;
                min-width: 40px; padding: 5px;
            }
            QPushButton#swapBtn:hover { background-color: #3c3c3c; }
        ''')

    def _create_left_panel(self):
        left_panel = QVBoxLayout()
        left_panel.setSpacing(8)

        self.source_label = QLabel(_tr("Original"))
        self.source_label.setStyleSheet("font-weight: bold; font-size: 14px;")
        left_panel.addWidget(self.source_label)

        self.source_edit = QTextEdit()
        self.source_edit.setPlainText(self.original_text)
        self.source_edit.setPlaceholderText(_tr("Enter text to translate..."))
        left_panel.addWidget(self.source_edit, stretch=1)

        return left_panel

    def _create_right_panel(self):
        right_panel = QVBoxLayout()
        right_panel.setSpacing(8)

        self.target_label = QLabel(_tr("Translation"))
        self.target_label.setStyleSheet("font-weight: bold; font-size: 14px;")
        right_panel.addWidget(self.target_label)

        self.target_edit = QTextEdit()
        self.target_edit.setPlainText(self.translated_text)
        self.target_edit.setPlaceholderText(_tr("Translation will appear here..."))
        right_panel.addWidget(self.target_edit, stretch=1)

        return right_panel

    def _create_language_bar(self):
        """创建顶部语言选择栏"""
        lang_bar = QHBoxLayout()
        lang_bar.setSpacing(10)

        # 源语言选择器
        self.source_lang_combo = QComboBox()
        for code, name in self._get_languages().items():
            self.source_lang_combo.addItem(name, code)
        idx = self.source_lang_combo.findData(self.source_lang)
        if idx >= 0:
            self.source_lang_combo.setCurrentIndex(idx)
        lang_bar.addWidget(self.source_lang_combo)

        # 交换按钮
        self.swap_btn = QPushButton("⇄")
        self.swap_btn.setObjectName("swapBtn")
        self.swap_btn.setToolTip(_tr("Swap languages"))
        self.swap_btn.clicked.connect(self._on_swap_languages)
        lang_bar.addWidget(self.swap_btn)

        # 目标语言选择器
        self.target_lang_combo = QComboBox()
        for code, name in self._get_target_languages().items():
            self.target_lang_combo.addItem(name, code)
        idx = self.target_lang_combo.findData(self.target_lang)
        if idx >= 0:
            self.target_lang_combo.setCurrentIndex(idx)
        self.target_lang_combo.currentIndexChanged.connect(self._on_target_lang_changed)
        lang_bar.addWidget(self.target_lang_combo)

        lang_bar.addStretch()
        
        # 置顶切换按钮
        self.pin_btn = QPushButton("📌")
        self.pin_btn.setObjectName("pinBtn")
        self.pin_btn.setToolTip(_tr("Toggle always on top"))
        self.pin_btn.setCheckable(True)
        self.pin_btn.setChecked(self._is_on_top)
        self.pin_btn.clicked.connect(self._on_toggle_pin)
        self._update_pin_button_style()
        lang_bar.addWidget(self.pin_btn)

        return lang_bar

    def _on_swap_languages(self):
        """交换源语言和目标语言"""
        source_lang = self.source_lang_combo.currentData()
        target_lang = self.target_lang_combo.currentData()
        
        # 如果源语言是auto，使用检测到的语言
        if source_lang == "auto" and self._detected_source_lang:
            source_lang = self._detected_source_lang
        
        # 如果源语言仍然是auto，无法交换
        if source_lang == "auto":
            return
        
        # 设置目标语言为原来的源语言
        idx = self.target_lang_combo.findData(source_lang)
        if idx >= 0:
            self.target_lang_combo.setCurrentIndex(idx)
        
        # 设置源语言为原来的目标语言
        idx = self.source_lang_combo.findData(target_lang)
        if idx >= 0:
            self.source_lang_combo.setCurrentIndex(idx)

    def _update_window_flags(self):
        """更新窗口标志（置顶/非置顶）"""
        flags = (Qt.WindowType.Window | 
                 Qt.WindowType.WindowCloseButtonHint | 
                 Qt.WindowType.WindowMinimizeButtonHint)
        if self._is_on_top:
            flags |= Qt.WindowType.WindowStaysOnTopHint
        self.setWindowFlags(flags)
    
    def _on_toggle_pin(self):
        """切换置顶状态"""
        self._is_on_top = not self._is_on_top
        TranslationDialog._stay_on_top = self._is_on_top
        self._update_pin_button_style()
        
        # 保存当前位置
        pos = self.pos()
        was_visible = self.isVisible()
        
        # 更新窗口标志
        self._update_window_flags()
        
        # 恢复位置和显示状态
        self.move(pos)
        if was_visible:
            self.show()
        
        log_debug(f"Toggle always on top: {self._is_on_top}", "Translation")
    
    def _update_pin_button_style(self):
        """更新置顶按钮样式"""
        if self._is_on_top:
            self.pin_btn.setStyleSheet("""
                QPushButton {
                    background-color: #0d6efd;
                    color: white;
                    border: none;
                    border-radius: 3px;
                    padding: 2px 6px;
                    font-size: 12px;
                    min-width: 24px;
                    max-width: 24px;
                }
                QPushButton:hover { background-color: #0b5ed7; }
            """)
            self.pin_btn.setToolTip(_tr("Always on top: ON"))
        else:
            self.pin_btn.setStyleSheet("""
                QPushButton {
                    background-color: #3c3c3c;
                    color: #888888;
                    border: none;
                    border-radius: 3px;
                    padding: 2px 6px;
                    font-size: 12px;
                    min-width: 24px;
                    max-width: 24px;
                }
                QPushButton:hover { background-color: #4a4a4a; }
            """)
            self.pin_btn.setToolTip(_tr("Always on top: OFF"))

    def _create_buttons(self):
        button_layout = QHBoxLayout()
        button_layout.setSpacing(10)

        self.translate_btn = QPushButton(_tr("Translate"))
        self.translate_btn.setObjectName("translateBtn")
        self.translate_btn.setToolTip(_tr("Translate text"))
        self.translate_btn.clicked.connect(self._on_translate)
        button_layout.addWidget(self.translate_btn)

        button_layout.addStretch()

        copy_source_btn = QPushButton(_tr("Copy Original"))
        copy_source_btn.clicked.connect(self._copy_source)
        button_layout.addWidget(copy_source_btn)

        copy_target_btn = QPushButton(_tr("Copy Translation"))
        copy_target_btn.clicked.connect(self._copy_target)
        button_layout.addWidget(copy_target_btn)

        return button_layout

    def _on_translate(self):
        text = self.source_edit.toPlainText().strip()
        if not text:
            return
        source_lang = self.source_lang_combo.currentData()
        target_lang = self.target_lang_combo.currentData()
        self.set_loading()
        self.translate_requested.emit(text, source_lang, target_lang)

    def _on_target_lang_changed(self, index):
        """当用户改变目标语言时保存选择"""
        target_lang = self.target_lang_combo.currentData()
        if target_lang:
            TranslationDialog._saved_target_lang = target_lang
            log_debug(f"Target language saved: {target_lang}", "Translation")

    def _copy_source(self):
        text = self.source_edit.toPlainText()
        if text:
            QApplication.clipboard().setText(text)
            log_info("Copied original text", "Translation")

    def _copy_target(self):
        text = self.target_edit.toPlainText()
        if text:
            QApplication.clipboard().setText(text)
            log_info("Copied translation", "Translation")

    def set_translation_result(self, translated_text: str, detected_lang: str = ""):
        self.target_edit.setPlainText(translated_text)
        self.target_edit.setStyleSheet("")
        if detected_lang:
            self._detected_source_lang = detected_lang
            # 更新源语言选择器显示检测到的语言
            idx = self.source_lang_combo.findData(detected_lang)
            if idx >= 0:
                self.source_lang_combo.setCurrentIndex(idx)
        self.translate_btn.setEnabled(True)
        self.translate_btn.setText(_tr("Translate"))

    def set_translation_error(self, error_msg: str):
        self.target_edit.setPlainText(_tr("Translation failed: ") + error_msg)
        self.target_edit.setStyleSheet("color: #ff6b6b;")
        self.translate_btn.setEnabled(True)
        self.translate_btn.setText(_tr("Translate"))

    def set_loading(self):
        self.translate_btn.setEnabled(False)
        self.translate_btn.setText(_tr("Translating..."))
        self.target_edit.setPlainText(_tr("Translating..."))
        self.target_edit.setStyleSheet("color: #888888; font-style: italic;")

    def get_source_lang(self) -> str:
        return self.source_lang_combo.currentData()

    def get_target_lang(self) -> str:
        return self.target_lang_combo.currentData()

    def update_content(self, text: str, target_lang: str = None):
        self.source_edit.setPlainText(text)
        if target_lang:
            idx = self.target_lang_combo.findData(target_lang)
            if idx >= 0:
                self.target_lang_combo.setCurrentIndex(idx)


class TranslationLoadingDialog(TranslationDialog):
    def __init__(
        self,
        original_text: str = "",
        parent: QWidget = None,
        position: QPoint = None,
        source_lang: str = "auto",
        target_lang: str = "ZH"
    ):
        super().__init__(
            original_text=original_text,
            translated_text="",
            parent=parent,
            position=position,
            source_lang=source_lang,
            target_lang=target_lang
        )
        if original_text and original_text.strip():
            self.set_loading()

    def on_translation_finished(self, success: bool, translated_text: str, error: str, detected_lang: str = ""):
        if success:
            self.set_translation_result(translated_text, detected_lang)
        else:
            self.set_translation_error(error)
