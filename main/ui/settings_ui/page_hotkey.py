# -*- coding: utf-8 -*-
"""快捷键设置页 — Fluent Design"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QScrollArea,
    QStackedWidget,
)
from PySide6.QtCore import Qt

from ui.dialogs import show_confirm_dialog
from qfluentwidgets import (
    ComboBox, CaptionLabel, SegmentedWidget,
)
from .components import SettingCardGroup, WhiteCard, LBL_STYLE, theme_text_style
from ..hotkey_edit import HotkeyEdit
from ..inapp_key_edit import InAppKeyEdit


# ── 应用内快捷键定义表（分组）──────────────────────────────
SCREENSHOT_KEYS = [
    ("inapp_confirm",   "Confirm Screenshot",     "ctrl+c"),
    ("inapp_pin",       "Pin Image",              "ctrl+d"),
    ("inapp_undo",      "Undo",                   "ctrl+z"),
    ("inapp_redo",      "Redo",                   "ctrl+y"),
    ("inapp_delete",    "Delete Selected",        "delete"),
    ("inapp_zoom_in",   "Magnifier Zoom In",      "pageup"),
    ("inapp_zoom_out",  "Magnifier Zoom Out",     "pagedown"),
    ("inapp_translate", "Screenshot Translate",    "shift+c"),
]

PIN_KEYS = [
    ("inapp_copy_pin",        "Copy Pinned Image",      "ctrl+c"),
    ("inapp_thumbnail",       "Toggle Thumbnail",       "r"),
    ("inapp_toggle_toolbar",  "Toggle Toolbar",         "space"),
]

INAPP_KEYS = SCREENSHOT_KEYS + PIN_KEYS

_EDIT_W = 140
_EDIT_H = 28
_CARD_LABEL_STYLE = theme_text_style(14)
_ROW_TITLE_STYLE = theme_text_style(13)
_SEGMENT_HINT_STYLE = "font-size: 12px; background: transparent;"


def _build_shortcut_row(dialog, parent, title: str, editor: QWidget) -> QWidget:
    row_card = WhiteCard(parent)
    row_card.setFixedHeight(46)

    row_layout = QHBoxLayout(row_card)
    row_layout.setContentsMargins(16, 0, 14, 0)
    row_layout.setSpacing(12)
    row_layout.setAlignment(Qt.AlignmentFlag.AlignVCenter)

    title_label = QLabel(title, row_card)
    title_label.setStyleSheet(_ROW_TITLE_STYLE)
    row_layout.addWidget(title_label, 1)
    row_layout.addWidget(editor, 0, Qt.AlignmentFlag.AlignRight)
    return row_card


def _stack_page_height(row_count: int) -> int:
    return row_count * 46 + max(0, row_count - 1) * 8


def create_hotkey_page(dialog) -> QWidget:
    """创建快捷键设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, 10, 0)
    layout.setSpacing(20)

    input_style = dialog._get_input_style()

    # ════ 全局热键 ════
    grp_global = SettingCardGroup(dialog.tr("Global Hotkeys"), view)

    # 截图热键（主 + 备用）
    ss_card = WhiteCard(grp_global)
    ss_h = QHBoxLayout(ss_card)
    ss_h.setContentsMargins(20, 8, 20, 8)
    ss_h.setSpacing(12)

    ss_lbl = QLabel(dialog.tr("Screenshot Hotkey"), ss_card)
    ss_lbl.setStyleSheet(_CARD_LABEL_STYLE)
    ss_h.addWidget(ss_lbl)
    ss_h.addStretch()

    ss_v = QVBoxLayout()
    ss_v.setSpacing(5)
    dialog.hotkey_input = HotkeyEdit()
    dialog.hotkey_input.setText(dialog.current_hotkey)
    dialog.hotkey_input.setPlaceholderText(dialog.tr("e.g.: ctrl+shift+a"))
    dialog.hotkey_input.setFixedWidth(200)
    dialog.hotkey_input.setStyleSheet(input_style)
    ss_v.addWidget(dialog.hotkey_input)

    dialog.hotkey_input_2 = HotkeyEdit()
    dialog.hotkey_input_2.setText(dialog.config_manager.get_hotkey_2())
    dialog.hotkey_input_2.setPlaceholderText(dialog.tr("e.g.: ctrl+shift+a"))
    dialog.hotkey_input_2.setFixedWidth(200)
    dialog.hotkey_input_2.setStyleSheet(input_style)
    ss_v.addWidget(dialog.hotkey_input_2)
    ss_h.addLayout(ss_v)
    ss_card.setFixedHeight(80)
    grp_global.addSettingCard(ss_card)

    # 剪贴板热键（主 + 备用）
    cb_card = WhiteCard(grp_global)
    cb_h = QHBoxLayout(cb_card)
    cb_h.setContentsMargins(20, 8, 20, 8)
    cb_h.setSpacing(12)

    cb_lbl = QLabel(dialog.tr("Clipboard Hotkey"), cb_card)
    cb_lbl.setStyleSheet(_CARD_LABEL_STYLE)
    cb_h.addWidget(cb_lbl)
    cb_h.addStretch()

    cb_v = QVBoxLayout()
    cb_v.setSpacing(5)
    dialog.clipboard_hotkey_edit = HotkeyEdit()
    dialog.clipboard_hotkey_edit.setText(dialog.config_manager.get_clipboard_hotkey())
    dialog.clipboard_hotkey_edit.setPlaceholderText(dialog.tr("e.g.: ctrl+shift+a"))
    dialog.clipboard_hotkey_edit.setFixedWidth(200)
    dialog.clipboard_hotkey_edit.setStyleSheet(input_style)
    cb_v.addWidget(dialog.clipboard_hotkey_edit)

    dialog.clipboard_hotkey_edit_2 = HotkeyEdit()
    dialog.clipboard_hotkey_edit_2.setText(dialog.config_manager.get_clipboard_hotkey_2())
    dialog.clipboard_hotkey_edit_2.setPlaceholderText(dialog.tr("e.g.: ctrl+shift+a"))
    dialog.clipboard_hotkey_edit_2.setFixedWidth(200)
    dialog.clipboard_hotkey_edit_2.setStyleSheet(input_style)
    cb_v.addWidget(dialog.clipboard_hotkey_edit_2)
    cb_h.addLayout(cb_v)
    cb_card.setFixedHeight(80)
    grp_global.addSettingCard(cb_card)

    layout.addWidget(grp_global)

    # ════ 应用内快捷键 ════
    grp_inapp = SettingCardGroup(dialog.tr("In-App Shortcuts"), view)

    dialog._inapp_edits = {}
    dialog._inapp_groups = {}

    tab_card = WhiteCard(grp_inapp)
    tab_layout = QVBoxLayout(tab_card)
    tab_layout.setContentsMargins(16, 16, 16, 16)
    tab_layout.setSpacing(12)

    tab_switch = SegmentedWidget(tab_card)
    tab_switch.setFixedHeight(34)
    tab_switch.setIndicatorColor("#07C160", "#07C160")

    stack = QStackedWidget(tab_card)
    stack.setObjectName("InAppShortcutStack")
    stack.setStyleSheet("#InAppShortcutStack { background: transparent; border: none; }")

    def _build_tab(keys_list: list, group_name: str, extra_widgets=None) -> QWidget:
        page = QWidget()
        vbox = QVBoxLayout(page)
        vbox.setContentsMargins(0, 0, 0, 0)
        vbox.setSpacing(8)

        for cfg_key, tr_src, default in keys_list:
            edit = InAppKeyEdit()
            edit.setFixedSize(_EDIT_W, _EDIT_H)
            edit.setStyleSheet(input_style)
            edit.setText(
                dialog.config_manager.get_inapp_shortcut(cfg_key) or default
            )
            dialog._inapp_edits[cfg_key] = edit
            dialog._inapp_groups[cfg_key] = group_name

            vbox.addWidget(_build_shortcut_row(dialog, page, dialog.tr(tr_src), edit))

        if extra_widgets:
            for w in extra_widgets:
                vbox.addWidget(w)

        vbox.addStretch(1)
        return page

    # 鼠标微移模式
    dialog.cursor_move_combo = ComboBox()
    dialog.cursor_move_combo.setFixedSize(_EDIT_W, _EDIT_H)
    dialog.cursor_move_combo.addItem("WASD + ↑↓←→", userData="both")
    dialog.cursor_move_combo.addItem("↑↓←→", userData="arrows")
    dialog.cursor_move_combo.addItem("WASD", userData="wasd")

    cur_mode = dialog.config_manager.get_inapp_cursor_move_mode()
    idx = dialog.cursor_move_combo.findData(cur_mode)
    if idx >= 0:
        dialog.cursor_move_combo.setCurrentIndex(idx)

    move_row = _build_shortcut_row(
        dialog, tab_card, dialog.tr("Cursor Move Keys"), dialog.cursor_move_combo
    )

    screenshot_tab = _build_tab(
        SCREENSHOT_KEYS, "screenshot", extra_widgets=[move_row]
    )
    pin_tab = _build_tab(PIN_KEYS, "pin")

    stack.addWidget(screenshot_tab)
    stack.addWidget(pin_tab)

    tab_switch.addItem("screenshot", dialog.tr("Screenshot Shortcuts"), lambda: stack.setCurrentIndex(0))
    tab_switch.addItem("pin", dialog.tr("Pin Shortcuts"), lambda: stack.setCurrentIndex(1))
    tab_switch.setCurrentItem("screenshot")

    tab_layout.addWidget(tab_switch, 0, Qt.AlignmentFlag.AlignLeft)
    tab_layout.addWidget(stack)

    screenshot_h = _stack_page_height(len(SCREENSHOT_KEYS) + 1)
    pin_h = _stack_page_height(len(PIN_KEYS))
    stack.setMinimumHeight(max(screenshot_h, pin_h))
    tab_card.setFixedHeight(max(screenshot_h, pin_h) + 80)
    grp_inapp.addSettingCard(tab_card)

    # 冲突检测
    for cfg_key, edit in dialog._inapp_edits.items():
        edit.textChanged.connect(
            lambda text, k=cfg_key: _on_shortcut_changed(
                dialog, k, text, input_style
            )
        )

    layout.addWidget(grp_inapp)

    # 提示
    hint = CaptionLabel(
        dialog.tr("💡 Hint: Click the input box and press the desired key combination."),
        view,
    )
    hint.setStyleSheet("padding: 5px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll


# ── 输入后冲突检测（交互式弹窗）──────────────────────────

def _on_shortcut_changed(dialog, changed_key: str, new_text: str, base_style: str):
    """某个输入框值变化时，检查同组内是否冲突，弹窗询问是否替换"""
    new_text = new_text.strip().lower()
    # 忽略空值、未完成的中间态（如 "ctrl+"）
    if not new_text or new_text.endswith("+"):
        return

    my_group = dialog._inapp_groups.get(changed_key, "")

    # 找同组内与新值相同的其他 edit
    conflict_key = None
    for cfg_key, edit in dialog._inapp_edits.items():
        if cfg_key == changed_key:
            continue
        if dialog._inapp_groups.get(cfg_key, "") != my_group:
            continue
        if edit.text().strip().lower() == new_text:
            conflict_key = cfg_key
            break

    if conflict_key is None:
        return  # 无冲突

    # 找到冲突项的显示名
    conflict_label = conflict_key
    for keys_list in (SCREENSHOT_KEYS, PIN_KEYS):
        for cfg, tr_src, _default in keys_list:
            if cfg == conflict_key:
                conflict_label = dialog.tr(tr_src)
                break

    # 弹窗询问
    current_edit = dialog._inapp_edits[changed_key]
    conflict_edit = dialog._inapp_edits[conflict_key]

    # 阻塞信号防止递归
    current_edit.blockSignals(True)
    conflict_edit.blockSignals(True)

    ret = show_confirm_dialog(
        dialog,
        dialog.tr("Shortcut Conflict"),
        dialog.tr('"%1" is already used by "%2".\nReplace it?')
            .replace('%1', new_text.upper())
            .replace('%2', conflict_label),
    )

    if ret is True:
        # 清空旧的，保留新的
        conflict_edit.setText("")
    else:
        # 撤销本次输入，恢复旧值
        old_val = dialog.config_manager.get_inapp_shortcut(changed_key)
        if not old_val:
            # 找默认值
            for keys_list in (SCREENSHOT_KEYS, PIN_KEYS):
                for cfg, _tr, default in keys_list:
                    if cfg == changed_key:
                        old_val = default
                        break
        current_edit.setText(old_val or "")

    current_edit.blockSignals(False)
    conflict_edit.blockSignals(False)
 