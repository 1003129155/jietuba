# -*- coding: utf-8 -*-
"""截图设置页 — Fluent Design"""
import importlib.util

from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QScrollArea, QLabel,
)
from PySide6.QtCore import Qt
from ui.fluent_lite import (
    SwitchSettingCard, SettingCard as FSettingCard,
    FluentIcon, ComboBox, CaptionLabel,
    PushButton,
)
from .components import SettingCardGroup, WhiteCard, apply_theme_text_style


def create_capture_page(dialog) -> QWidget:
    """截图設定 ─ 交互行为 + 智能选区 + 保存设置 + OCR"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    # ── 截图交互 ──────────────────────────────────────
    grp_behavior = SettingCardGroup(dialog.tr("Capture Behavior"), view)

    double_click_card = SwitchSettingCard(
        FluentIcon.CAMERA,
        dialog.tr("Double-click to Copy and Close"),
        dialog.tr(
            "Double-click the selected screenshot to copy it to the clipboard and close the capture."
        ),
        parent=grp_behavior,
    )
    double_click_card.setChecked(
        dialog.config_manager.get_double_click_copy_close_enabled()
    )
    dialog.double_click_copy_close_toggle = double_click_card
    grp_behavior.addSettingCard(double_click_card)

    cross_tool_card = SwitchSettingCard(
        FluentIcon.EDIT,
        dialog.tr("Enable Ctrl Cross-Tool Selection"),
        dialog.tr(
            "Hold Ctrl and click any editable annotation to adjust it without switching tools."
        ),
        parent=grp_behavior,
    )
    cross_tool_card.setChecked(
        dialog.config_manager.get_cross_tool_selection_enabled()
    )
    dialog.cross_tool_selection_toggle = cross_tool_card
    grp_behavior.addSettingCard(cross_tool_card)

    text_top_card = SwitchSettingCard(
        FluentIcon.FONT,
        dialog.tr("Keep Text Annotations on Top"),
        dialog.tr(
            "Keep text above other annotations, including ones drawn later."
        ),
        parent=grp_behavior,
    )
    text_top_card.setChecked(
        dialog.config_manager.get_text_always_on_top_enabled()
    )
    dialog.text_always_on_top_toggle = text_top_card
    grp_behavior.addSettingCard(text_top_card)

    layout.addWidget(grp_behavior)

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
    apply_theme_text_style(path_icon_lbl, 14)
    dialog.save_path_lbl = QLabel(dialog.config_manager.get_screenshot_save_path(), path_card)
    dialog.save_path_lbl.setWordWrap(True)
    dialog.save_path_lbl.setCursor(Qt.CursorShape.PointingHandCursor)
    apply_theme_text_style(dialog.save_path_lbl, 12, caption=True)

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
    dialog.screenshot_format_combo.addItem("PDF", userData="PDF")
    dialog.screenshot_format_combo.setFixedWidth(110)
    _fmt_idx = {"PNG": 0, "JPG": 1, "BMP": 2, "WEBP": 3, "PDF": 4}.get(
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

    # OCR 可用性检测：走 ocr 模块的官方多引擎检测（含 ppocr_rust / windows_media_ocr），
    # 而不是只看 windows_media_ocr —— 否则装了 ppocr_rust 也会误报“无 OCR 版本”。
    try:
        from ocr import is_ocr_available
        ocr_available = bool(is_ocr_available())
    except Exception:
        # 兜底：ocr 模块不可导入时，退回到最基础的探测
        ocr_available = (
            importlib.util.find_spec("ppocr_rust") is not None
            or importlib.util.find_spec("windows_media_ocr") is not None
        )
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
 
