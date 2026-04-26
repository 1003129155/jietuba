"""通用对话框组件模块

提供标准化、模态化的对话框基类和便捷函数，
替代 QMessageBox 以解决焦点抢夺和跨窗口置顶问题。
"""

from PySide6.QtWidgets import QApplication, QDialog, QVBoxLayout, QLabel, QDialogButtonBox, QCheckBox
from PySide6.QtWidgets import QScrollArea, QWidget
from PySide6.QtCore import Qt
from core.i18n import make_tr


_translate_dialog_text = make_tr("StandardDialog")

class StandardDialog(QDialog):
    """
    统一的带有统一样式和模态行为的对话框基类，
    用于替代默认的 QMessageBox，防止其抢夺全局焦点和跨窗口置顶的问题。
    """
    def __init__(self, parent, title, message):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.setObjectName("standardDialog")
        
        # 强制自身为对话框，且不需要全局置顶，绑定至父窗口的模态
        self.setWindowFlags(
            Qt.WindowType.Dialog
            | Qt.WindowType.WindowTitleHint
            | Qt.WindowType.WindowCloseButtonHint
        )
        self.setWindowModality(Qt.WindowModality.WindowModal)
        self.setStyleSheet("""
            QDialog#standardDialog {
                background: #FFFFFF;
                color: #202124;
            }
            QLabel {
                background: transparent;
                color: #202124;
                font-size: 13px;
            }
            QCheckBox {
                background: transparent;
                color: #202124;
                font-size: 13px;
            }
            QPushButton {
                min-width: 84px;
                min-height: 30px;
                padding: 4px 14px;
                background: #FFFFFF;
                color: #202124;
                border: 1px solid #D0D7DE;
                border-radius: 6px;
            }
            QPushButton:hover {
                background: #F5F7FA;
            }
            QPushButton:pressed {
                background: #EBEEF2;
            }
        """)
        
        layout = QVBoxLayout(self)
        layout.setSpacing(12)
        layout.setContentsMargins(20, 16, 20, 16)
        
        self.label = QLabel(message, self)
        self.label.setWordWrap(True)
        layout.addWidget(self.label)
        
        self.button_box = QDialogButtonBox(self)
        layout.addWidget(self.button_box)
        
        self.result_action = None


def _track_modeless_dialog(dialog):
    """跟踪非模态对话框，避免 show() 后被 Python 提前回收。"""
    app = QApplication.instance()
    if app is None:
        return dialog

    dialogs = getattr(app, "_modeless_dialogs", None)
    if dialogs is None:
        dialogs = []
        setattr(app, "_modeless_dialogs", dialogs)

    dialogs.append(dialog)

    def _cleanup(*_args):
        current_dialogs = getattr(app, "_modeless_dialogs", None)
        if current_dialogs is not None and dialog in current_dialogs:
            current_dialogs.remove(dialog)

    dialog.destroyed.connect(_cleanup)
    return dialog

def show_custom_confirm_dialog(parent, title, message, buttons_config) -> str:
    """
    显示自定义多按钮确认对话框，返回被点击按钮的 action_id
    buttons_config 格式:
      [
        {"id": "save", "text": "Save", "role": QDialogButtonBox.ButtonRole.AcceptRole},
        {"id": "discard", "text": "Don't Save", "role": QDialogButtonBox.ButtonRole.DestructiveRole},
        {"id": "cancel", "text": "Cancel", "role": QDialogButtonBox.ButtonRole.RejectRole, "default": True},
      ]
    """
    dialog = StandardDialog(parent, title, message)
    dialog.result_action = "cancel" # Default fallback
    
    for cfg in buttons_config:
        btn = dialog.button_box.addButton(cfg["text"], cfg["role"])
        if cfg.get("default"):
            btn.setDefault(True)
        
        # Capture the id for the callback using default arg in lambda
        def make_callback(action_id):
            return lambda: setattr(dialog, "result_action", action_id)
            
        btn.clicked.connect(make_callback(cfg["id"]))
        
        # Bind accept/reject appropriately to close the dialog
        if cfg["role"] in (QDialogButtonBox.ButtonRole.AcceptRole, QDialogButtonBox.ButtonRole.YesRole):
            btn.clicked.connect(dialog.accept)
        elif cfg["role"] in (QDialogButtonBox.ButtonRole.RejectRole, QDialogButtonBox.ButtonRole.NoRole):
            btn.clicked.connect(dialog.reject)
        else:
            # For destructive/action roles, we also just accept/close
            btn.clicked.connect(dialog.accept)

    dialog.exec()
    return dialog.result_action

def show_confirm_dialog(parent, title, message) -> bool:
    """显示确认对话框 (Yes / No)，返回 bool"""
    yes_text = _translate_dialog_text("Yes")
    no_text = _translate_dialog_text("No")
    
    config = [
        {"id": True, "text": yes_text, "role": QDialogButtonBox.ButtonRole.YesRole},
        {"id": False, "text": no_text, "role": QDialogButtonBox.ButtonRole.NoRole, "default": True}
    ]
    return show_custom_confirm_dialog(parent, title, message, config)

def show_confirm_checkbox_dialog(parent, title, message, checkbox_text, checkbox_checked=False):
    """显示带复选框的确认对话框，返回 (confirmed, checkbox_checked)"""
    yes_text = _translate_dialog_text("Yes")
    no_text = _translate_dialog_text("No")

    dialog = StandardDialog(parent, title, message)
    checkbox = QCheckBox(checkbox_text, dialog)
    checkbox.setChecked(checkbox_checked)
    dialog.layout().insertWidget(dialog.layout().count() - 1, checkbox)

    yes_button = dialog.button_box.addButton(yes_text, QDialogButtonBox.ButtonRole.YesRole)
    no_button = dialog.button_box.addButton(no_text, QDialogButtonBox.ButtonRole.NoRole)
    no_button.setDefault(True)

    yes_button.clicked.connect(dialog.accept)
    no_button.clicked.connect(dialog.reject)

    confirmed = dialog.exec() == QDialog.DialogCode.Accepted
    return confirmed, checkbox.isChecked()

def show_info_dialog(parent, title, message):
    """显示信息提示框 (OK)"""
    ok_text = _translate_dialog_text("OK")
    config = [
        {"id": "ok", "text": ok_text, "role": QDialogButtonBox.ButtonRole.AcceptRole, "default": True}
    ]
    show_custom_confirm_dialog(parent, title, message, config)

def show_warning_dialog(parent, title, message):
    """显示警告提示框 (OK)"""
    ok_text = _translate_dialog_text("OK")
    config = [
        {"id": "ok", "text": ok_text, "role": QDialogButtonBox.ButtonRole.AcceptRole, "default": True}
    ]
    show_custom_confirm_dialog(parent, title, message, config)


def show_text_dialog(parent, title, content):
    """显示带可滚动正文的文本对话框。"""
    ok_text = _translate_dialog_text("OK")

    dialog = StandardDialog(parent, title, "")
    dialog.resize(620, 430)
    dialog.label.hide()
    dialog.layout().setContentsMargins(18, 14, 18, 14)
    dialog.layout().setSpacing(10)

    scroll = QScrollArea(dialog)
    scroll.setWidgetResizable(True)
    scroll.setFrameShape(QScrollArea.Shape.NoFrame)
    scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }")

    content_widget = QWidget(scroll)
    content_layout = QVBoxLayout(content_widget)
    content_layout.setContentsMargins(0, 0, 0, 0)

    content_label = QLabel(content, content_widget)
    content_label.setWordWrap(True)
    content_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
    content_label.setStyleSheet("font-size: 13px; line-height: 1.45; background: transparent;")
    content_layout.addWidget(content_label)
    content_layout.addStretch(1)

    scroll.setWidget(content_widget)
    dialog.layout().insertWidget(dialog.layout().count() - 1, scroll, 1)

    ok_button = dialog.button_box.addButton(ok_text, QDialogButtonBox.ButtonRole.AcceptRole)
    ok_button.setDefault(True)
    ok_button.clicked.connect(dialog.accept)

    dialog.exec()


def show_modeless_warning_dialog(parent, title, message):
    """显示非模态警告提示框，适用于截图等不能阻塞交互的场景。"""
    ok_text = _translate_dialog_text("OK")

    dialog = StandardDialog(parent, title, message)
    dialog.setWindowModality(Qt.WindowModality.NonModal)
    dialog.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
    dialog.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)

    ok_button = dialog.button_box.addButton(ok_text, QDialogButtonBox.ButtonRole.AcceptRole)
    ok_button.setDefault(True)
    ok_button.clicked.connect(dialog.accept)

    _track_modeless_dialog(dialog)
    dialog.show()
    return dialog

def show_error_dialog(parent, title, message):
    """显示错误提示框 (OK)"""
    ok_text = _translate_dialog_text("OK")
    config = [
        {"id": "ok", "text": ok_text, "role": QDialogButtonBox.ButtonRole.AcceptRole, "default": True}
    ]
    show_custom_confirm_dialog(parent, title, message, config)
 