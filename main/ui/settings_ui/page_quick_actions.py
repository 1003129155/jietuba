# -*- coding: utf-8 -*-
"""快捷行为页 — 跳过确认或结果窗口的开关

识别结果窗口里的勾选框和这里写的是同一个配置键：窗口里勾上后就不会再弹窗，
想关回来只能到这一页，所以每一项的说明都要写清楚开着时会发生什么。
"""
from PySide6.QtWidgets import QWidget, QVBoxLayout, QScrollArea
from core.ui_scale import dialog_scaled
from ui.fluent_lite import SwitchSettingCard, FluentIcon
from .components import SettingCardGroup
from core.ui_theme import set_own_style


def create_quick_actions_page(dialog) -> QWidget:
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    set_own_style(view, "background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(20))

    # ── 截图 ──────────────────────────────────────────
    grp_capture = SettingCardGroup(dialog.tr("Screenshot"), view)

    double_click_card = SwitchSettingCard(
        FluentIcon.CAMERA,
        dialog.tr("Double-click to Copy and Close"),
        dialog.tr(
            "Double-click the selected screenshot to copy it to the clipboard and close the capture."
        ),
        parent=grp_capture,
    )
    double_click_card.setChecked(
        dialog.config_manager.get_double_click_copy_close_enabled()
    )
    dialog.double_click_copy_close_toggle = double_click_card
    grp_capture.addSettingCard(double_click_card)

    cross_tool_card = SwitchSettingCard(
        FluentIcon.EDIT,
        dialog.tr("Enable Ctrl Cross-Tool Selection"),
        dialog.tr(
            "Hold Ctrl and click any editable annotation to adjust it without switching tools."
        ),
        parent=grp_capture,
    )
    cross_tool_card.setChecked(
        dialog.config_manager.get_cross_tool_selection_enabled()
    )
    dialog.cross_tool_selection_toggle = cross_tool_card
    grp_capture.addSettingCard(cross_tool_card)

    text_top_card = SwitchSettingCard(
        FluentIcon.FONT,
        dialog.tr("Keep Text Annotations on Top"),
        dialog.tr(
            "Keep text above other annotations, including ones drawn later."
        ),
        parent=grp_capture,
    )
    text_top_card.setChecked(
        dialog.config_manager.get_text_always_on_top_enabled()
    )
    dialog.text_always_on_top_toggle = text_top_card
    grp_capture.addSettingCard(text_top_card)

    layout.addWidget(grp_capture)

    # ── 文字识别与扫码 ────────────────────────────────
    grp_recognition = SettingCardGroup(dialog.tr("Text Recognition and Scanning"), view)

    ocr_card = SwitchSettingCard(
        FluentIcon.DOCUMENT,
        dialog.tr("Copy Recognized Text Directly"),
        dialog.tr(
            "Copy the recognized text straight to the clipboard instead of opening the result window."
        ),
        parent=grp_recognition,
    )
    ocr_card.setChecked(dialog.config_manager.get_ocr_copy_directly_enabled())
    dialog.ocr_copy_directly_toggle = ocr_card
    grp_recognition.addSettingCard(ocr_card)

    barcode_card = SwitchSettingCard(
        FluentIcon.SEARCH,
        dialog.tr("Copy a Single Code Directly"),
        dialog.tr(
            "When only one QR code or barcode is found, copy its content instead of opening "
            "the result window. Multiple codes still open it."
        ),
        parent=grp_recognition,
    )
    barcode_card.setChecked(dialog.config_manager.get_barcode_copy_single_enabled())
    dialog.barcode_copy_single_toggle = barcode_card
    grp_recognition.addSettingCard(barcode_card)

    layout.addWidget(grp_recognition)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll
