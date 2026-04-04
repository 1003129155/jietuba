# -*- coding: utf-8 -*-
"""日志设置页 — Fluent Design"""
import os
import subprocess
import sys
import glob

from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QScrollArea,
)
from PySide6.QtCore import Qt
from ui.dialogs import show_info_dialog
from qfluentwidgets import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, SpinBox,
    CaptionLabel, PushButton,
)
from .components import SettingCardGroup, WhiteCard, theme_text_style, theme_caption_style


_CARD_TITLE_STYLE = theme_text_style(14)
_CARD_CAPTION_STYLE = theme_caption_style(12)
_CARD_MUTED_STYLE = theme_caption_style(12)


def create_log_page(dialog) -> QWidget:
    """创建日志设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    # ════ 日志设置 ════
    grp_log = SettingCardGroup(dialog.tr("Log Settings"), view)

    # 日志开关
    log_card = SwitchSettingCard(
        FluentIcon.HISTORY,
        dialog.tr("Save Logs"),
        dialog.tr("Saves app activity logs to file."),
        parent=grp_log,
    )
    log_card.setChecked(dialog.config_manager.get_log_enabled())
    dialog.log_toggle = log_card
    grp_log.addSettingCard(log_card)

    # 日志等级
    level_card = FSettingCard(
        FluentIcon.FILTER,
        dialog.tr("Log Level"),
        dialog.tr("Minimum log level to record"),
        parent=grp_log,
    )
    dialog.log_level_combo = ComboBox(level_card)
    dialog.log_level_combo.addItems(["DEBUG", "INFO", "WARNING", "ERROR"])
    dialog.log_level_combo.setItemData(0, "DEBUG")
    dialog.log_level_combo.setItemData(1, "INFO")
    dialog.log_level_combo.setItemData(2, "WARNING")
    dialog.log_level_combo.setItemData(3, "ERROR")
    dialog.log_level_combo.setFixedWidth(130)
    current_level = dialog.config_manager.get_log_level()
    level_idx = {"DEBUG": 0, "INFO": 1, "WARNING": 2, "ERROR": 3}.get(
        current_level, 2
    )
    dialog.log_level_combo.setCurrentIndex(level_idx)
    level_card.hBoxLayout.addWidget(
        dialog.log_level_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    level_card.hBoxLayout.addSpacing(16)
    grp_log.addSettingCard(level_card)

    # 保留天数
    retention_card = FSettingCard(
        FluentIcon.DATE_TIME,
        dialog.tr("Retention Period"),
        dialog.tr("Auto-delete old logs (0=keep forever)"),
        parent=grp_log,
    )
    dialog.log_retention_spinbox = SpinBox(retention_card)
    dialog.log_retention_spinbox.setRange(0, 365)
    dialog.log_retention_spinbox.setSuffix(" " + dialog.tr("days"))
    dialog.log_retention_spinbox.setValue(
        dialog.config_manager.get_log_retention_days()
    )
    dialog.log_retention_spinbox.setFixedWidth(150)
    retention_card.hBoxLayout.addWidget(
        dialog.log_retention_spinbox, 0, Qt.AlignmentFlag.AlignRight
    )
    retention_card.hBoxLayout.addSpacing(16)
    grp_log.addSettingCard(retention_card)

    layout.addWidget(grp_log)

    # ════ 存储位置 ════
    grp_path = SettingCardGroup(dialog.tr("Storage"), view)

    path_card = WhiteCard(grp_path)
    path_v = QVBoxLayout(path_card)
    path_v.setContentsMargins(20, 14, 20, 14)
    path_v.setSpacing(8)

    path_title = QLabel(dialog.tr("Save Location:"), path_card)
    path_title.setStyleSheet(_CARD_TITLE_STYLE)
    path_v.addWidget(path_title)

    dialog.path_lbl = QLabel(dialog.config_manager.get_log_dir(), path_card)
    dialog.path_lbl.setWordWrap(True)
    dialog.path_lbl.setStyleSheet(_CARD_CAPTION_STYLE)
    path_v.addWidget(dialog.path_lbl)

    dialog.latest_log_lbl = QLabel("", path_card)
    dialog.latest_log_lbl.setStyleSheet(_CARD_MUTED_STYLE)
    dialog.latest_log_lbl.setWordWrap(True)
    refresh_latest_log_label(dialog)
    path_v.addWidget(dialog.latest_log_lbl)

    # 按钮行
    btn_row = QHBoxLayout()
    btn_row.setSpacing(8)
    btn_row.addStretch(1)

    btn_change = PushButton(dialog.tr("Change"), path_card)
    btn_change.setFixedHeight(32)
    btn_change.clicked.connect(dialog._change_log_dir)
    btn_row.addWidget(btn_change)

    btn_open = PushButton(dialog.tr("Open"), path_card)
    btn_open.setFixedHeight(32)
    btn_open.clicked.connect(dialog._open_log_dir)
    btn_row.addWidget(btn_open)

    btn_latest = PushButton(dialog.tr("Latest Log"), path_card)
    btn_latest.setFixedHeight(32)
    btn_latest.clicked.connect(lambda: _open_latest_log_file(dialog))
    btn_row.addWidget(btn_latest)

    path_v.addLayout(btn_row)

    path_card.setFixedHeight(146)
    grp_path.addSettingCard(path_card)
    layout.addWidget(grp_path)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll


def refresh_latest_log_label(dialog):
    """刷新当前/最新日志文件路径显示"""
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
            dialog.latest_log_lbl.setText(dialog.tr("Current Log:") + f" {log_path}")
        else:
            dialog.latest_log_lbl.setText(dialog.tr("Current Log: (Not generated)  *Log will be created after app starts"))
    except Exception:
        if hasattr(dialog, "latest_log_lbl"):
            dialog.latest_log_lbl.setText(dialog.tr("Current Log: (Not generated)"))


def _open_latest_log_file(dialog):
    """打开最新日志文件"""
    path = dialog.config_manager.get_log_dir()
    if not os.path.exists(path):
        os.makedirs(path, exist_ok=True)

    pattern = os.path.join(path, "runtime_*.log")
    files = glob.glob(pattern)
    if not files:
        show_info_dialog(dialog, dialog.tr("Log"), dialog.tr("No log files yet. Please start and use the app first."))
        return

    latest = max(files, key=os.path.getmtime)
    if sys.platform == "win32":
        os.startfile(latest)
    elif sys.platform == "darwin":
        subprocess.run(["open", latest], check=False)
    else:
        subprocess.run(["xdg-open", latest], check=False)
 