# -*- coding: utf-8 -*-
"""快捷行为页 — 跳过确认或结果窗口的开关

识别结果窗口里的勾选框和这里写的是同一个配置键：窗口里勾上后就不会再弹窗，
想关回来只能到这一页，所以每一项的说明都要写清楚开着时会发生什么。
"""
from PySide6.QtWidgets import QWidget, QVBoxLayout, QScrollArea
from PySide6.QtCore import Qt
from core.ui_scale import dialog_scaled
from ui.fluent_lite import (
    CaptionLabel,
    FluentIcon,
    SettingCard as FSettingCard,
    SwitchButton,
    SwitchSettingCard,
)
from settings.tool_settings import CAPTURE_ACTIONS, CAPTURE_TRIGGERS, get_capture_action
from .components import SettingCardGroup
from .page_capture import CaptureActionComboBox
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

    if not hasattr(dialog, '_behavior_controls'):
        dialog._behavior_controls = {}

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

    for trigger, label, _default in CAPTURE_TRIGGERS:
        card = FSettingCard(FluentIcon.CAMERA, dialog.tr(label), parent=grp_capture)
        combo = CaptureActionComboBox(card)
        combo.setFixedWidth(dialog_scaled(150))
        for action, title in CAPTURE_ACTIONS:
            combo.addItem(dialog.tr(title), userData=action)
        combo.setCurrentIndex(
            combo.findData(get_capture_action(dialog.config_manager, trigger))
        )

        exit_label = CaptionLabel(dialog.tr("Exit capture after action"), card)
        exit_label.setWordWrap(True)
        exit_check = SwitchButton(card)
        exit_check.setAccessibleName(dialog.tr("Exit capture after action"))
        exit_check.setChecked(
            dialog.config_manager.get_app_setting(f"capture_{trigger}_exit", True)
        )
        exit_check.setEnabled(combo.currentData() != "none")
        combo.currentIndexChanged.connect(
            lambda _index, c=combo, check=exit_check: check.setEnabled(
                c.currentData() != "none"
            )
        )

        card.hBoxLayout.insertWidget(
            card.hBoxLayout.indexOf(card.controlContainer), combo
        )
        card.addControl(exit_label)
        card.addControl(exit_check, align=Qt.AlignmentFlag.AlignRight)
        dialog._behavior_controls[f"capture_{trigger}_action"] = combo
        dialog._behavior_controls[f"capture_{trigger}_exit"] = exit_check
        grp_capture.addSettingCard(card)

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
