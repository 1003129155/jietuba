# -*- coding: utf-8 -*-
"""剪贴板设置页 — Fluent Design"""
import os
import sys

from core.logger import log_exception
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QScrollArea,
)
from PySide6.QtCore import Qt
from ui.dialogs import show_info_dialog, show_warning_dialog, show_confirm_checkbox_dialog
from qfluentwidgets import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, SpinBox, CaptionLabel,
    PushButton, PrimaryPushButton,
)
from .components import SettingCardGroup, WhiteCard, theme_text_style, theme_caption_style


_CARD_TITLE_STYLE = theme_text_style(14)
_CARD_CAPTION_STYLE = theme_caption_style(12)


def create_clipboard_page(dialog) -> QWidget:
    """创建剪贴板设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    # ════ 基本设置 ════
    grp_basic = SettingCardGroup(dialog.tr("Basic Settings"), view)

    enabled_card = SwitchSettingCard(
        FluentIcon.PASTE,
        dialog.tr("Enable Clipboard Manager"),
        dialog.tr("Monitor and manage clipboard history"),
        parent=grp_basic,
    )
    enabled_card.setChecked(dialog.config_manager.get_clipboard_enabled())
    dialog.clipboard_enabled_toggle = enabled_card
    grp_basic.addSettingCard(enabled_card)

    layout.addWidget(grp_basic)

    # ════ 历史管理 ════
    grp_history = SettingCardGroup(dialog.tr("History"), view)

    limit_card = FSettingCard(
        FluentIcon.HISTORY,
        dialog.tr("History Limit"),
        dialog.tr("Maximum number of items to keep (0 = unlimited)"),
        parent=grp_history,
    )
    dialog.clipboard_history_limit_spin = SpinBox(limit_card)
    dialog.clipboard_history_limit_spin.setRange(0, 10000)
    dialog.clipboard_history_limit_spin.setValue(
        dialog.config_manager.get_clipboard_history_limit()
    )
    dialog.clipboard_history_limit_spin.setFixedWidth(150)
    limit_card.hBoxLayout.addWidget(
        dialog.clipboard_history_limit_spin, 0, Qt.AlignmentFlag.AlignRight
    )
    limit_card.hBoxLayout.addSpacing(16)
    grp_history.addSettingCard(limit_card)

    layout.addWidget(grp_history)

    # ════ 数据管理 ════
    grp_data = SettingCardGroup(dialog.tr("Data"), view)

    # 存储位置
    try:
        from clipboard import ClipboardManager
        cm = ClipboardManager()
        if cm.is_available:
            db_path = cm.get_db_path() or dialog.tr("Unknown")
        else:
            db_path = dialog.tr("Clipboard module not available")
    except Exception as e:
        log_exception(e, "获取剪贴板数据库路径")
        db_path = dialog.tr("Clipboard module not available")

    storage_card = FSettingCard(
        FluentIcon.FOLDER,
        dialog.tr("Data Storage Location"),
        db_path,
        parent=grp_data,
    )
    open_folder_btn = PushButton(dialog.tr("Open Folder"), storage_card)
    open_folder_btn.clicked.connect(
        lambda: _open_clipboard_data_folder(dialog, db_path)
    )
    storage_card.hBoxLayout.addWidget(
        open_folder_btn, 0, Qt.AlignmentFlag.AlignRight
    )
    storage_card.hBoxLayout.addSpacing(16)
    grp_data.addSettingCard(storage_card)

    # 清理
    cleanup_card = WhiteCard(grp_data)
    cleanup_h = QHBoxLayout(cleanup_card)
    cleanup_h.setContentsMargins(20, 12, 20, 12)
    cleanup_h.setSpacing(12)

    cleanup_left = QVBoxLayout()
    cleanup_left.setSpacing(2)
    cleanup_title = QLabel(dialog.tr("Clear Clipboard History"), cleanup_card)
    cleanup_title.setStyleSheet(_CARD_TITLE_STYLE)
    cleanup_left.addWidget(cleanup_title)

    dialog._clipboard_size_label = QLabel("…", cleanup_card)
    dialog._clipboard_size_label.setStyleSheet(_CARD_CAPTION_STYLE)
    dialog._calc_clipboard_storage_size = _calc_clipboard_storage_size
    _refresh_clipboard_size_async(dialog)

    cleanup_desc = QLabel(dialog.tr("Delete all clipboard history records"), cleanup_card)
    cleanup_desc.setStyleSheet(_CARD_CAPTION_STYLE)
    cleanup_left.addWidget(cleanup_desc)

    cleanup_h.addLayout(cleanup_left, 1)
    cleanup_h.addWidget(dialog._clipboard_size_label)

    clear_btn = PrimaryPushButton(dialog.tr("Clear History"), cleanup_card)
    clear_btn.clicked.connect(lambda: _clear_clipboard_history(dialog))
    cleanup_h.addWidget(clear_btn)
    cleanup_card.setFixedHeight(60)
    grp_data.addSettingCard(cleanup_card)

    layout.addWidget(grp_data)

    # 提示
    hint = CaptionLabel(
        dialog.tr("💡 Hint: Set clipboard hotkey in Shortcuts settings."), view
    )
    hint.setStyleSheet("padding: 5px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll


def _refresh_clipboard_size_async(dialog):
    """在子线程中计算剪贴板体积，完成后更新 UI 标签"""
    from PySide6.QtCore import QThread, Signal

    class _SizeThread(QThread):
        done = Signal(str)

        def run(self):
            self.done.emit(_calc_clipboard_storage_size())

    label = getattr(dialog, '_clipboard_size_label', None)
    state = {'alive': True}

    def _mark_dead(*_args):
        state['alive'] = False

    if dialog is not None:
        try:
            dialog.destroyed.connect(_mark_dead)
        except RuntimeError:
            state['alive'] = False
    if label is not None:
        try:
            label.destroyed.connect(_mark_dead)
        except RuntimeError:
            state['alive'] = False

    thread = _SizeThread()
    thread.done.connect(lambda s: _apply_size_to_label(label, s, state))
    thread.finished.connect(thread.deleteLater)
    # 持有引用防止被 GC
    dialog._clipboard_size_thread = thread
    thread.start()


def _apply_size_to_label(label, size_str: str, state: dict | None = None):
    """将计算结果更新到 label（仅在控件仍存活时）"""
    if state is not None and not state.get('alive', True):
        return
    if label is not None:
        try:
            label.setText(size_str if size_str else "—")
        except RuntimeError:
            pass  # 控件已销毁


def _calc_clipboard_storage_size() -> str:
    """计算剪贴板数据存储大小"""
    try:
        from clipboard import ClipboardManager
        cm = ClipboardManager()
        if not cm.is_available:
            return ""
        total = 0
        db_path = cm.get_db_path() or ""
        if db_path:
            db_dir = os.path.dirname(db_path)
            db_base = os.path.basename(db_path)
            for suffix in ("", "-wal", "-shm"):
                p = os.path.join(db_dir, db_base + suffix)
                if os.path.isfile(p):
                    total += os.path.getsize(p)
        try:
            img_dir = cm.get_images_dir() or ""
            if img_dir and os.path.isdir(img_dir):
                for fname in os.listdir(img_dir):
                    fp = os.path.join(img_dir, fname)
                    if os.path.isfile(fp):
                        total += os.path.getsize(fp)
        except Exception as e:
            log_exception(e, "计算图片目录大小")
        if total < 1024:
            return f"{total} B"
        elif total < 1024 ** 2:
            return f"{total / 1024:.1f} KB"
        elif total < 1024 ** 3:
            return f"{total / 1024 ** 2:.1f} MB"
        else:
            return f"{total / 1024 ** 3:.2f} GB"
    except Exception as e:
        log_exception(e, "计算剪贴板存储大小")
        return ""


def _open_clipboard_data_folder(dialog, path: str):
    """打开剪贴板数据文件夹"""
    import subprocess
    try:
        folder = os.path.dirname(path) if os.path.isfile(path) else path
        if os.path.exists(folder):
            if sys.platform == 'win32':
                subprocess.run(['explorer', folder])
            elif sys.platform == 'darwin':
                subprocess.run(['open', folder])
            else:
                subprocess.run(['xdg-open', folder])
        else:
            show_warning_dialog(dialog, dialog.tr("Warning"), dialog.tr("Folder does not exist"))
    except Exception as e:
        show_warning_dialog(dialog, dialog.tr("Error"), str(e))


def _clear_clipboard_history(dialog):
    """清空剪贴板历史"""
    confirmed, delete_grouped = show_confirm_checkbox_dialog(
        dialog,
        dialog.tr("Confirm Clear"),
        dialog.tr("Are you sure you want to clear all clipboard history?\nThis action cannot be undone."),
        dialog.tr("Also delete saved content in groups"),
    )

    if not confirmed:
        return

    keep_grouped = not delete_grouped

    try:
        from clipboard import ClipboardManager
        cm = ClipboardManager()
        if cm.is_available and cm.clear_history(keep_grouped=keep_grouped):
            show_info_dialog(dialog, dialog.tr("Success"), dialog.tr("Clipboard history cleared successfully"))
            dialog._refresh_clipboard_size(delay_ms=800)
        else:
            show_warning_dialog(dialog, dialog.tr("Error"), dialog.tr("Failed to clear clipboard history"))
    except Exception as e:
        show_warning_dialog(dialog, dialog.tr("Error"), str(e))
 