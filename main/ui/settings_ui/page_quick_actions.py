# -*- coding: utf-8 -*-
"""快捷行为页 — 跳过确认或结果窗口的开关

识别结果窗口里的勾选框和这里写的是同一个配置键：窗口里勾上后就不会再弹窗，
想关回来只能到这一页，所以每一项的说明都要写清楚开着时会发生什么。
"""
from PySide6.QtWidgets import QWidget, QVBoxLayout
from core.ui_scale import dialog_scaled
from ui.fluent_lite import FluentIcon, SwitchSettingCard
from .components import SettingCardGroup, page_scroll_area
from core.ui_theme import set_own_style


def create_quick_actions_page(dialog) -> QWidget:
    scroll = page_scroll_area(dialog)
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
    crosshair_card = SwitchSettingCard(
        FluentIcon.LAYOUT,
        dialog.tr("Fullscreen Crosshair"),
        dialog.tr(
            "Show horizontal and vertical guide lines across the screen while capturing."
        ),
        parent=grp_capture,
    )
    crosshair_card.setChecked(
        dialog.config_manager.get_app_setting("capture_fullscreen_crosshair", False)
    )
    dialog._behavior_controls["capture_fullscreen_crosshair"] = crosshair_card
    grp_capture.addSettingCard(crosshair_card)

    cursor_card = SwitchSettingCard(
        FluentIcon.CAMERA,
        dialog.tr("Include Mouse Pointer"),
        dialog.tr(
            "Draw the mouse pointer into the screenshot where it was when capturing started."
        ),
        parent=grp_capture,
    )
    cursor_card.setChecked(
        dialog.config_manager.get_app_setting("capture_include_cursor", False)
    )
    dialog._behavior_controls["capture_include_cursor"] = cursor_card
    grp_capture.addSettingCard(cursor_card)

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

    ocr_card = SwitchSettingCard(
        FluentIcon.DOCUMENT,
        dialog.tr("Copy Recognized Text Directly"),
        dialog.tr(
            "Copy the recognized text straight to the clipboard instead of opening the result window."
        ),
        parent=grp_capture,
    )
    ocr_card.setChecked(dialog.config_manager.get_ocr_copy_directly_enabled())
    dialog.ocr_copy_directly_toggle = ocr_card
    grp_capture.addSettingCard(ocr_card)

    barcode_card = SwitchSettingCard(
        FluentIcon.SEARCH,
        dialog.tr("Copy a Single Code Directly"),
        dialog.tr(
            "When only one QR code or barcode is found, copy its content instead of opening "
            "the result window. Multiple codes still open it."
        ),
        parent=grp_capture,
    )
    barcode_card.setChecked(dialog.config_manager.get_barcode_copy_single_enabled())
    dialog.barcode_copy_single_toggle = barcode_card
    grp_capture.addSettingCard(barcode_card)

    layout.addWidget(grp_capture)

    # ── 剪贴板 ────────────────────────────────────────
    grp_clipboard = SettingCardGroup(dialog.tr("Clipboard"), view)

    win_v_card = SwitchSettingCard(
        FluentIcon.PASTE,
        dialog.tr("Open Clipboard with Win+V"),
        dialog.tr(
            "Win+V opens this app's clipboard instead of the Windows clipboard history. "
            "Turn it off to give Win+V back to Windows."
        ),
        parent=grp_clipboard,
    )
    win_v_card.setChecked(dialog.config_manager.get_app_setting("clipboard_take_over_win_v"))
    dialog._behavior_controls["clipboard_take_over_win_v"] = win_v_card
    grp_clipboard.addSettingCard(win_v_card)

    layout.addWidget(grp_clipboard)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll
