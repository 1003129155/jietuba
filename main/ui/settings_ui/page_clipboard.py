# -*- coding: utf-8 -*-
"""剪贴板设置页 — Fluent Design"""
import os
import shutil
import sys

from core.logger import log_exception
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QScrollArea,
    QFileDialog, QProgressDialog,
)
from PySide6.QtCore import Qt, QThread, Signal, QSize
from ui.dialogs import (
    show_info_dialog, show_warning_dialog, show_confirm_dialog,
    show_confirm_checkbox_dialog,
)
from ui.fluent_lite import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, SpinBox, CaptionLabel,
    PushButton, PrimaryPushButton, TransparentToolButton,
)
from .components import SettingCardGroup, WhiteCard, apply_theme_text_style


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
    db_path = _get_clipboard_db_path(dialog)

    storage_card = FSettingCard(
        FluentIcon.FOLDER,
        dialog.tr("Data Storage Location"),
        db_path,
        parent=grp_data,
    )
    dialog._clipboard_storage_card = storage_card
    open_folder_btn = PushButton(dialog.tr("Open Folder"), storage_card)
    open_folder_btn.clicked.connect(
        lambda: _open_clipboard_data_folder(dialog, _get_clipboard_db_path(dialog))
    )
    change_folder_btn = PushButton(dialog.tr("Change Location"), storage_card)
    change_folder_btn.clicked.connect(
        lambda: _change_clipboard_data_location(dialog)
    )
    storage_card.hBoxLayout.addWidget(
        open_folder_btn, 0, Qt.AlignmentFlag.AlignRight
    )
    storage_card.hBoxLayout.addWidget(
        change_folder_btn, 0, Qt.AlignmentFlag.AlignRight
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
    apply_theme_text_style(cleanup_title, 14)
    cleanup_left.addWidget(cleanup_title)

    dialog._clipboard_size_label = QLabel(dialog.tr("← Get storage size"), cleanup_card)
    apply_theme_text_style(dialog._clipboard_size_label, 12, caption=True)
    dialog._calc_clipboard_storage_size = _calc_clipboard_storage_size

    # 手动刷新按钮 + 大小标签
    size_row = QHBoxLayout()
    size_row.setSpacing(4)
    refresh_btn = TransparentToolButton(FluentIcon.SYNC, cleanup_card)
    refresh_btn.setFixedSize(48, 48)
    refresh_btn.setIconSize(QSize(32, 32))
    refresh_btn.clicked.connect(lambda: _refresh_clipboard_size_async(dialog))
    dialog._clipboard_refresh_btn = refresh_btn
    size_row.addWidget(refresh_btn)
    size_row.addWidget(dialog._clipboard_size_label)

    cleanup_desc = QLabel(dialog.tr("Delete all clipboard history records"), cleanup_card)
    apply_theme_text_style(cleanup_desc, 12, caption=True)
    cleanup_left.addWidget(cleanup_desc)

    cleanup_h.addLayout(cleanup_left, 1)
    cleanup_h.addLayout(size_row)

    clear_btn = PrimaryPushButton(dialog.tr("Clear History"), cleanup_card)
    clear_btn.clicked.connect(lambda: _clear_clipboard_history(dialog))
    cleanup_h.addWidget(clear_btn)
    cleanup_card.setFixedHeight(72)
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


def _get_clipboard_db_path(dialog) -> str:
    """获取当前剪贴板数据库路径，用于设置页显示。"""
    try:
        from clipboard import ClipboardManager
        cm = ClipboardManager()
        if cm.is_available:
            return cm.get_db_path() or dialog.tr("Unknown")
        return dialog.tr("Clipboard module not available")
    except Exception as e:
        log_exception(e, "获取剪贴板数据库路径")
        return dialog.tr("Clipboard module not available")


def _set_clipboard_storage_path_label(dialog, path: str):
    card = getattr(dialog, "_clipboard_storage_card", None)
    if card is None:
        return
    try:
        if hasattr(card, "setContent"):
            card.setContent(path)
        elif hasattr(card, "contentLabel"):
            card.contentLabel.setText(path)
    except RuntimeError:
        pass


def _change_clipboard_data_location(dialog):
    """选择新目录并异步迁移剪贴板数据。"""
    cm = None
    current_db_path = ""
    storage_released = False
    try:
        from clipboard import ClipboardManager
        cm = ClipboardManager()
        if not cm.is_available:
            show_warning_dialog(dialog, dialog.tr("Error"), dialog.tr("Clipboard module not available"))
            return

        current_db_path = cm.get_db_path() or ""
        if not current_db_path:
            show_warning_dialog(dialog, dialog.tr("Error"), dialog.tr("Current clipboard database path is unavailable."))
            return

        current_dir = os.path.dirname(current_db_path)
        target_dir = QFileDialog.getExistingDirectory(
            dialog,
            dialog.tr("Choose Clipboard Data Folder"),
            current_dir,
        )
        if not target_dir:
            return

        current_dir_norm = os.path.normcase(os.path.abspath(current_dir))
        target_dir_norm = os.path.normcase(os.path.abspath(target_dir))
        if current_dir_norm == target_dir_norm:
            show_info_dialog(dialog, dialog.tr("Info"), dialog.tr("The selected folder is already the current location."))
            return

        images_dir = os.path.join(current_dir, "images")
        if os.path.isdir(images_dir):
            images_dir_norm = os.path.normcase(os.path.abspath(images_dir))
            try:
                if os.path.commonpath([images_dir_norm, target_dir_norm]) == images_dir_norm:
                    show_warning_dialog(dialog, dialog.tr("Error"), dialog.tr("Please choose a folder outside the current clipboard images folder."))
                    return
            except ValueError:
                pass

        target_db_path = os.path.join(target_dir, "clipboard.db")
        if _target_has_clipboard_data(target_dir):
            confirmed = show_confirm_dialog(
                dialog,
                dialog.tr("Replace Existing Data"),
                dialog.tr("The selected folder already contains clipboard data. Replace it with the current data?"),
            )
            if not confirmed:
                return

        if not cm.release_storage():
            show_warning_dialog(dialog, dialog.tr("Error"), dialog.tr("Failed to close the current clipboard database."))
            return
        storage_released = True

        progress = QProgressDialog(dialog.tr("Preparing migration..."), "", 0, 100, dialog)
        progress.setWindowTitle(dialog.tr("Moving Clipboard Data"))
        progress.setCancelButton(None)
        progress.setAutoClose(False)
        progress.setAutoReset(False)
        progress.setWindowModality(Qt.WindowModality.WindowModal)
        progress.setMinimumDuration(0)
        progress.show()

        worker = _ClipboardStorageMoveThread(current_db_path, target_dir, dialog)
        dialog._clipboard_move_thread = worker
        dialog._clipboard_move_progress = progress

        worker.progress.connect(lambda value, text: _update_move_progress(progress, value, text))
        worker.completed.connect(lambda new_path, cleanup_error: _on_clipboard_move_completed(dialog, cm, progress, worker, new_path, cleanup_error))
        worker.failed.connect(lambda message: _on_clipboard_move_failed(dialog, cm, progress, worker, current_db_path, message))
        worker.start()
    except Exception as e:
        if storage_released and cm is not None and current_db_path:
            cm.reset_storage(current_db_path)
        show_warning_dialog(dialog, dialog.tr("Error"), str(e))


def _target_has_clipboard_data(target_dir: str) -> bool:
    db_path = os.path.join(target_dir, "clipboard.db")
    if any(os.path.exists(db_path + suffix) for suffix in ("", "-wal", "-shm")):
        return True
    images_dir = os.path.join(target_dir, "images")
    if not os.path.isdir(images_dir):
        return False
    with os.scandir(images_dir) as entries:
        return any(entries)


def _update_move_progress(progress: QProgressDialog, value: int, text: str):
    try:
        progress.setValue(value)
        progress.setLabelText(text)
    except RuntimeError:
        pass


def _on_clipboard_move_completed(dialog, cm, progress, worker, new_db_path: str, cleanup_error: str = ""):
    try:
        progress.setValue(100)
        progress.close()
    except RuntimeError:
        pass

    try:
        dialog.config_manager.set_clipboard_db_path(new_db_path)
        if cm.reset_storage(new_db_path):
            _set_clipboard_storage_path_label(dialog, new_db_path)
            _refresh_clipboard_size_async(dialog)
            _refresh_open_clipboard_windows()
            if cleanup_error:
                show_warning_dialog(
                    dialog,
                    dialog.tr("Success"),
                    dialog.tr("Clipboard data location changed successfully, but the old files are still in use and could not be deleted. You can remove them after restarting the app.\n{error}").format(error=cleanup_error),
                )
        else:
            show_warning_dialog(dialog, dialog.tr("Error"), dialog.tr("Clipboard data moved, but reopening the database failed. Please restart the app."))
    finally:
        worker.deleteLater()
        dialog._clipboard_move_thread = None
        dialog._clipboard_move_progress = None


def _on_clipboard_move_failed(dialog, cm, progress, worker, old_db_path: str, message: str):
    try:
        progress.close()
    except RuntimeError:
        pass
    try:
        cm.reset_storage(old_db_path)
    finally:
        worker.deleteLater()
        dialog._clipboard_move_thread = None
        dialog._clipboard_move_progress = None
    show_warning_dialog(dialog, dialog.tr("Error"), message)


def _refresh_open_clipboard_windows():
    try:
        from PySide6.QtWidgets import QApplication
        for widget in QApplication.topLevelWidgets():
            if hasattr(widget, "request_data_refresh"):
                widget.request_data_refresh("Clipboard storage")
            elif hasattr(widget, "controller") and hasattr(widget.controller, "load_history"):
                widget.controller.load_history()
    except Exception:
        pass


class _ClipboardStorageMoveThread(QThread):
    progress = Signal(int, str)
    completed = Signal(str, str)
    failed = Signal(str)

    def __init__(self, source_db_path: str, target_dir: str, dialog):
        super().__init__(dialog)
        self.source_db_path = os.path.abspath(source_db_path)
        self.target_dir = os.path.abspath(target_dir)
        self._text_finalizing = dialog.tr("Finalizing migration...")
        self._text_complete = dialog.tr("Migration complete.")
        self._text_no_files = dialog.tr("No clipboard data files were found to move.")
        self._text_moving = dialog.tr("Moving clipboard data... {done} / {total}")

    def run(self):
        staging_dir = os.path.join(self.target_dir, ".clipboard_migration_tmp")
        target_db_path = os.path.join(self.target_dir, "clipboard.db")
        try:
            payload = self._collect_payload()
            total_bytes = sum(size for _, _, size in payload)
            copied = 0

            if os.path.exists(staging_dir):
                shutil.rmtree(staging_dir)
            os.makedirs(staging_dir, exist_ok=True)

            for source, relative_target, size in payload:
                destination = os.path.join(staging_dir, relative_target)
                copied += self._copy_file(source, destination, total_bytes, copied, size)

            self.progress.emit(99, self._text_finalizing)
            self._replace_target_from_staging(staging_dir)
            cleanup_error = ""
            try:
                self._remove_source_payload()
            except Exception as cleanup_exc:
                cleanup_error = str(cleanup_exc)
            self.progress.emit(100, self._text_complete)
            self.completed.emit(target_db_path, cleanup_error)
        except Exception as e:
            try:
                if os.path.exists(staging_dir):
                    shutil.rmtree(staging_dir)
            except Exception:
                pass
            self.failed.emit(str(e))

    def _collect_payload(self):
        payload = []
        for suffix in ("", "-wal", "-shm"):
            source = self.source_db_path + suffix
            if os.path.isfile(source):
                payload.append((source, "clipboard.db" + suffix, os.path.getsize(source)))

        images_dir = os.path.join(os.path.dirname(self.source_db_path), "images")
        if os.path.isdir(images_dir):
            for root, _, files in os.walk(images_dir):
                for file_name in files:
                    source = os.path.join(root, file_name)
                    rel = os.path.relpath(source, images_dir)
                    payload.append((source, os.path.join("images", rel), os.path.getsize(source)))

        if not payload:
            raise RuntimeError(self._text_no_files)
        return payload

    def _copy_file(self, source: str, destination: str, total_bytes: int, copied_before: int, file_size: int) -> int:
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        copied_file = 0
        with open(source, "rb") as src, open(destination, "wb") as dst:
            while True:
                chunk = src.read(1024 * 1024)
                if not chunk:
                    break
                dst.write(chunk)
                copied_file += len(chunk)
                done = copied_before + copied_file
                percent = int(done * 98 / total_bytes) if total_bytes else 98
                self.progress.emit(
                    max(0, min(percent, 98)),
                    self._text_moving.format(
                        done=_format_bytes(done),
                        total=_format_bytes(total_bytes),
                    ),
                )
        shutil.copystat(source, destination)
        return file_size

    def _replace_target_from_staging(self, staging_dir: str):
        os.makedirs(self.target_dir, exist_ok=True)
        backup_dir = os.path.join(self.target_dir, ".clipboard_migration_backup")
        target_names = ["clipboard.db", "clipboard.db-wal", "clipboard.db-shm", "images"]
        staging_names = os.listdir(staging_dir)

        if os.path.exists(backup_dir):
            shutil.rmtree(backup_dir)
        os.makedirs(backup_dir, exist_ok=True)

        try:
            for name in target_names:
                path = os.path.join(self.target_dir, name)
                if os.path.exists(path):
                    shutil.move(path, os.path.join(backup_dir, name))

            try:
                for name in staging_names:
                    shutil.move(os.path.join(staging_dir, name), os.path.join(self.target_dir, name))
            except Exception:
                for name in staging_names:
                    _remove_path_if_exists(os.path.join(self.target_dir, name))
                for name in os.listdir(backup_dir):
                    shutil.move(os.path.join(backup_dir, name), os.path.join(self.target_dir, name))
                raise
        finally:
            shutil.rmtree(staging_dir, ignore_errors=True)
            shutil.rmtree(backup_dir, ignore_errors=True)

    def _remove_source_payload(self):
        source_dir = os.path.dirname(self.source_db_path)
        target_dir_norm = os.path.normcase(os.path.abspath(self.target_dir))
        source_dir_norm = os.path.normcase(os.path.abspath(source_dir))
        if target_dir_norm == source_dir_norm:
            return

        for suffix in ("", "-wal", "-shm"):
            path = self.source_db_path + suffix
            if os.path.exists(path):
                os.remove(path)

        images_dir = os.path.join(source_dir, "images")
        if os.path.isdir(images_dir):
            shutil.rmtree(images_dir)


def _remove_path_if_exists(path: str):
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    elif os.path.exists(path):
        os.remove(path)


def _format_bytes(value: int) -> str:
    if value < 1024:
        return f"{value} B"
    if value < 1024 ** 2:
        return f"{value / 1024:.1f} KB"
    if value < 1024 ** 3:
        return f"{value / 1024 ** 2:.1f} MB"
    return f"{value / 1024 ** 3:.2f} GB"


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
    from PySide6.QtCore import QTimer
    try:
        folder = os.path.dirname(path) if os.path.isfile(path) else path
        if os.path.exists(folder):
            if sys.platform == 'win32':
                # 延迟到 Qt 事件循环里执行，避免在 mouseReleaseEvent 栈内调用
                # subprocess + explorer 在这里会触发 COM 线程冲突（access violation）
                QTimer.singleShot(0, lambda f=os.path.normpath(folder): os.startfile(f))
            elif sys.platform == 'darwin':
                subprocess.Popen(['open', folder])
            else:
                subprocess.Popen(['xdg-open', folder])
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
