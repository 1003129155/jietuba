# -*- coding: utf-8 -*-
"""截图设置页 — Fluent Design"""
import importlib.util

from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QScrollArea, QLabel,
)
from PySide6.QtCore import Qt
from qfluentwidgets import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, CaptionLabel,
    PushButton,
)
from .components import SettingCardGroup, WhiteCard, theme_text_style, theme_caption_style


_CARD_TITLE_STYLE = theme_text_style(14)
_CARD_CAPTION_STYLE = theme_caption_style(12)


def create_capture_page(dialog) -> QWidget:
    """截图設定 ─ 智能选区 + 保存设置 + OCR"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    # ── 智能选区 ──────────────────────────────────────
    grp_smart = SettingCardGroup(dialog.tr("Smart Selection"), view)

    smart_card = SwitchSettingCard(
        FluentIcon.CAMERA,
        dialog.tr("Enable Smart Selection"),
        dialog.tr("Automatically recognizes UI elements at mouse cursor position."),
        parent=grp_smart,
    )
    smart_card.setChecked(dialog.config_manager.get_smart_selection())
    dialog.smart_toggle = smart_card
    grp_smart.addSettingCard(smart_card)

    layout.addWidget(grp_smart)

    # ── 截图保存 ──────────────────────────────────────
    grp_save = SettingCardGroup(dialog.tr("Save Settings"), view)

    save_card = SwitchSettingCard(
        FluentIcon.SAVE,
        dialog.tr("Auto-save Screenshots"),
        dialog.tr("Automatically saves as file when capturing."),
        parent=grp_save,
    )
    save_card.setChecked(dialog.config_manager.get_screenshot_save_enabled())
    dialog.save_toggle = save_card
    grp_save.addSettingCard(save_card)

    # 保存路径（卡片）
    path_card = WhiteCard(grp_save)
    path_h = QHBoxLayout(path_card)
    path_h.setContentsMargins(20, 12, 20, 12)
    path_h.setSpacing(12)

    path_icon_lbl = QLabel(dialog.tr("Save Folder:"), path_card)
    path_icon_lbl.setStyleSheet(_CARD_TITLE_STYLE)
    dialog.save_path_lbl = QLabel(dialog.config_manager.get_screenshot_save_path(), path_card)
    dialog.save_path_lbl.setWordWrap(True)
    dialog.save_path_lbl.setCursor(Qt.CursorShape.PointingHandCursor)
    dialog.save_path_lbl.setStyleSheet(_CARD_CAPTION_STYLE)

    btn_change = PushButton(dialog.tr("Change"), path_card)
    btn_change.setFixedHeight(32)
    btn_change.clicked.connect(dialog._change_save_dir)
    btn_open = PushButton(dialog.tr("Open"), path_card)
    btn_open.setFixedHeight(32)
    btn_open.clicked.connect(dialog._open_save_dir)

    path_h.addWidget(path_icon_lbl)
    path_h.addWidget(dialog.save_path_lbl, 1)
    path_h.addWidget(btn_change)
    path_h.addWidget(btn_open)
    path_card.setFixedHeight(58)
    grp_save.addSettingCard(path_card)

    # 保存格式
    fmt_card = FSettingCard(
        FluentIcon.DOCUMENT,
        dialog.tr("Save Format"),
        dialog.tr("File format for auto-saved screenshots."),
        parent=grp_save,
    )
    dialog.screenshot_format_combo = ComboBox(fmt_card)
    dialog.screenshot_format_combo.addItem("PNG", userData="PNG")
    dialog.screenshot_format_combo.addItem("JPG", userData="JPG")
    dialog.screenshot_format_combo.addItem("BMP", userData="BMP")
    dialog.screenshot_format_combo.addItem("WebP", userData="WEBP")
    dialog.screenshot_format_combo.setFixedWidth(110)
    _fmt_idx = {"PNG": 0, "JPG": 1, "BMP": 2, "WEBP": 3}.get(
        dialog.config_manager.get_screenshot_format().upper(), 0
    )
    dialog.screenshot_format_combo.setCurrentIndex(_fmt_idx)
    fmt_card.hBoxLayout.addWidget(
        dialog.screenshot_format_combo, 0, Qt.AlignmentFlag.AlignRight
    )
    fmt_card.hBoxLayout.addSpacing(16)
    grp_save.addSettingCard(fmt_card)

    layout.addWidget(grp_save)

    # ── OCR ───────────────────────────────────────────
    grp_ocr = SettingCardGroup(dialog.tr("OCR"), view)

    ocr_available = importlib.util.find_spec("windows_media_ocr") is not None
    ocr_card = SwitchSettingCard(
        FluentIcon.SEARCH,
        dialog.tr("Enable OCR"),
        dialog.tr("Enables text recognition and selection in pinned windows."),
        parent=grp_ocr,
    )
    ocr_card.setChecked(
        dialog.config_manager.get_ocr_enabled() if ocr_available else False
    )
    if not ocr_available:
        ocr_card.setEnabled(False)
        ocr_card.setChecked(False)
    dialog.ocr_enable_toggle = ocr_card
    grp_ocr.addSettingCard(ocr_card)

    if not ocr_available:
        no_ocr_card = FSettingCard(
            FluentIcon.INFO,
            dialog.tr("No OCR Version / OCR module not found"),
            parent=grp_ocr,
        )
        grp_ocr.addSettingCard(no_ocr_card)

    layout.addWidget(grp_ocr)

    # 提示
    hint = CaptionLabel(
        dialog.tr("💡 Hint: Even with auto-save off, it will be copied to clipboard."),
        view,
    )
    hint.setStyleSheet("padding: 5px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll
 