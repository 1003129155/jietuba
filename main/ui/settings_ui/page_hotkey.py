# -*- coding: utf-8 -*-
"""快捷键设置页 — Fluent Design"""
from PySide6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QScrollArea,
    QStackedWidget,
)
from PySide6.QtCore import Qt

from core.resource_manager import ResourceManager
from core.ui_scale import dialog_scaled
from ui.dialogs import show_confirm_dialog
from ui.fluent_lite import (
    ComboBox, CaptionLabel, FluentIcon, SegmentedWidget,
)
from ui.fluent_lite.theme import ACCENT
from .components import IconBadge, SectionCard, add_separated_row, apply_theme_text_style
from ..hotkey_edit import HotkeyEdit, validate_hotkey_group
from ..inapp_key_edit import InAppKeyEdit
from ..key_chip import CHIP_WIDTH, STATUS_GAP, STATUS_SIZE, format_shortcut_text
from settings import ANNOTATION_TOOL_SHORTCUTS
from core.shortcut_manager import is_reserved_inapp_shortcut


# ── 应用内快捷键定义表（分组）──────────────────────────────
SCREENSHOT_KEYS = [
    ("inapp_confirm",   "Confirm Screenshot",     "ctrl+c"),
    ("inapp_pin",       "Pin Image",              "ctrl+d"),
    ("inapp_undo",      "Undo",                   "ctrl+z"),
    ("inapp_redo",      "Redo",                   "ctrl+y"),
    ("inapp_delete",    "Delete Selected",        "delete"),
    ("inapp_restore_last_region", "Restore Last Region", "l"),
    ("inapp_zoom_in",   "Magnifier Zoom In",      "pageup"),
    ("inapp_zoom_out",  "Magnifier Zoom Out",     "pagedown"),
    ("inapp_translate", "Screenshot Translate",    "shift+c"),
    ("inapp_text_recognize", "Recognize Text",   "shift+t"),
]

PIN_KEYS = [
    ("inapp_copy_pin",        "Copy Pinned Image",      "ctrl+c"),
    ("inapp_copy_pin_text",   "Copy All Text",          "ctrl+shift+c"),
    ("inapp_pin_reset_size",  "Reset Size",             "mousemiddle"),
    ("inapp_thumbnail",       "Toggle Thumbnail",       "r"),
    ("inapp_toggle_toolbar",  "Toggle Toolbar",         "space"),
]

TOOL_KEYS = [
    (cfg_key, label, default)
    for cfg_key, _tool_id, label, default in ANNOTATION_TOOL_SHORTCUTS
]

INAPP_KEYS = SCREENSHOT_KEYS + TOOL_KEYS + PIN_KEYS

# 应用内各项的行图标，尽量沿用工具栏上同一功能的图标。字符串是 svg/ 下的文件名。
# 行图标会被整体着色，工具栏的序号图标是白底圆，着色后只剩实心圆点，所以用线框版。
_INAPP_ICONS = {
    "inapp_confirm": "确定.svg",
    "inapp_pin": "钉图.svg",
    "inapp_undo": "撤回.svg",
    "inapp_redo": "复原.svg",
    "inapp_delete": FluentIcon.DELETE,
    "inapp_restore_last_region": FluentIcon.HISTORY,
    "inapp_zoom_in": FluentIcon.SEARCH,
    "inapp_zoom_out": FluentIcon.SEARCH,
    "inapp_translate": FluentIcon.LANGUAGE,
    "inapp_text_recognize": "文字识别.svg",
    "inapp_tool_cursor": "鼠标.svg",
    "inapp_tool_pen": "画笔.svg",
    "inapp_tool_highlighter": "荧光笔.svg",
    "inapp_tool_mosaic": "马赛克.svg",
    "inapp_tool_arrow": "箭头.svg",
    "inapp_tool_number": "序号线框.svg",
    "inapp_tool_rect": "方框.svg",
    "inapp_tool_ellipse": "圆框.svg",
    "inapp_tool_text": "文字.svg",
    "inapp_tool_eraser": "橡皮.svg",
    "inapp_copy_pin": "复制.svg",
    "inapp_copy_pin_text": "文字识别.svg",
    "inapp_pin_reset_size": "长截图.svg",
    "inapp_thumbnail": FluentIcon.HIDE,
    "inapp_toggle_toolbar": "开发.svg",
}
_CURSOR_MOVE_ICON = "移动窗口.svg"

# 全局快捷键同属一个冲突域；任意两个业务不能占用同一个实际按键。
GLOBAL_HOTKEY_EDIT_ATTRS = (
    "hotkey_input",
    "hotkey_input_2",
    "clipboard_hotkey_edit",
    "clipboard_hotkey_edit_2",
    "translation_hotkey_edit",
    "translation_hotkey_edit_2",
    "pin_clipboard_hotkey_edit",
    "pin_clipboard_hotkey_edit_2",
)

# (主键属性, 备用键属性, 标题, 色调, 图标, 读主键, 读备用键)
_GLOBAL_ROWS = (
    ("hotkey_input", "hotkey_input_2", "Screenshot Hotkey", "capture", FluentIcon.CAMERA,
     lambda d: d.current_hotkey, lambda d: d.config_manager.get_hotkey_2()),
    ("clipboard_hotkey_edit", "clipboard_hotkey_edit_2", "Clipboard Hotkey", "clipboard", FluentIcon.PASTE,
     lambda d: d.config_manager.get_clipboard_hotkey(),
     lambda d: d.config_manager.get_clipboard_hotkey_2()),
    ("pin_clipboard_hotkey_edit", "pin_clipboard_hotkey_edit_2", "Pin Clipboard Image", "pin", FluentIcon.PIN,
     lambda d: d.config_manager.get_pin_clipboard_hotkey(),
     lambda d: d.config_manager.get_pin_clipboard_hotkey_2()),
    ("translation_hotkey_edit", "translation_hotkey_edit_2", "Translation Hotkey", "translate", FluentIcon.LANGUAGE,
     lambda d: d.config_manager.get_translation_hotkey(),
     lambda d: d.config_manager.get_translation_hotkey_2()),
)

_INAPP_ROW_HEIGHT = 44
# 应用内的按键块和全局的按键块右边缘对齐：后者右侧还有一个状态图标。
_STATUS_COLUMN = STATUS_GAP + STATUS_SIZE


def _iter_global_hotkey_edits(dialog):
    for attr in GLOBAL_HOTKEY_EDIT_ATTRS:
        edit = getattr(dialog, attr, None)
        if edit is not None:
            yield edit


def validate_global_hotkey_edits(dialog, *, check_system: bool = False) -> bool:
    """校验设置窗口上的全局快捷键。

    这里只回答「哪几个控件属于同一个冲突域」，判重规则本身在
    ui.hotkey_edit.validate_hotkey_group——欢迎向导复用的是同一份。
    """
    return validate_hotkey_group(
        _iter_global_hotkey_edits(dialog), check_system=check_system
    )


def _icon_ref(ref):
    if isinstance(ref, str):
        return ResourceManager.get_icon_path(ref)
    return ref


def _row_label(parent, text: str) -> QLabel:
    label = QLabel(text, parent)
    apply_theme_text_style(label, 14, extra="font-weight: 500;")
    return label


def _global_row(parent, title, tone, icon, editors) -> QWidget:
    row = QWidget(parent)
    layout = QHBoxLayout(row)
    layout.setContentsMargins(0, dialog_scaled(8), 0, dialog_scaled(8))
    layout.setSpacing(dialog_scaled(14))
    layout.addWidget(IconBadge(icon, tone, row))
    layout.addWidget(_row_label(row, title), 1)

    column = QVBoxLayout()
    column.setSpacing(dialog_scaled(5))
    for edit in editors:
        edit.setFixedWidth(dialog_scaled(CHIP_WIDTH + _STATUS_COLUMN))
        column.addWidget(edit)
    layout.addLayout(column)
    return row


def _inapp_row(parent, icon, title, editor) -> QWidget:
    row = QWidget(parent)
    row.setFixedHeight(dialog_scaled(_INAPP_ROW_HEIGHT))
    layout = QHBoxLayout(row)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(dialog_scaled(14))
    layout.addWidget(IconBadge(_icon_ref(icon) if icon else None, None, row))
    layout.addWidget(_row_label(row, title), 1)

    # 宽度定在外层槽位上：ComboBox 每次重算样式都会重设最小宽度，
    # 直接对它 setFixedWidth 撑不住，会缩回内容宽度。
    slot = QWidget(row)
    slot.setFixedWidth(dialog_scaled(CHIP_WIDTH))
    slot_layout = QHBoxLayout(slot)
    slot_layout.setContentsMargins(0, 0, 0, 0)
    slot_layout.addWidget(editor)
    layout.addWidget(slot, 0, Qt.AlignmentFlag.AlignVCenter)
    # 布局不会在控件和 spacer 之间再插 spacing，这段宽度就是全部间距。
    layout.addSpacing(dialog_scaled(_STATUS_COLUMN))
    return row


def create_hotkey_page(dialog) -> QWidget:
    """创建快捷键设置页面 — Fluent Design"""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    view.setStyleSheet("background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(16))

    # ════ 全局热键 ════
    grp_global = SectionCard(
        ResourceManager.get_icon_path("热键.svg"),
        dialog.tr("Global Hotkeys"),
        dialog.tr("Works anytime"),
        view,
    )
    for attr, attr_2, title, tone, icon, read, read_2 in _GLOBAL_ROWS:
        editors = []
        for name, reader in ((attr, read), (attr_2, read_2)):
            edit = HotkeyEdit()
            edit.setText(reader(dialog))
            setattr(dialog, name, edit)
            editors.append(edit)
        grp_global.addRow(_global_row(grp_global, dialog.tr(title), tone, icon, editors))

    # 全局热键统一判重。连接放在所有输入框创建之后，避免初始化过程中
    # 只看到半组控件；最后主动跑一次，以识别配置文件里遗留的旧冲突。
    for edit in _iter_global_hotkey_edits(dialog):
        edit.textChanged.connect(
            lambda _text, d=dialog: validate_global_hotkey_edits(d)
        )
    validate_global_hotkey_edits(dialog)

    layout.addWidget(grp_global)

    # ════ 应用内快捷键 ════
    grp_inapp = SectionCard(
        FluentIcon.LAYOUT,
        dialog.tr("In-App Shortcuts"),
        dialog.tr("Only in screenshot and pin windows"),
        view,
    )

    dialog._inapp_edits = {}
    dialog._inapp_groups = {}

    tab_switch = SegmentedWidget(grp_inapp)
    tab_switch.setFixedHeight(dialog_scaled(34))
    tab_switch.setIndicatorColor(ACCENT, ACCENT)

    stack = QStackedWidget(grp_inapp)
    stack.setObjectName("InAppShortcutStack")
    stack.setStyleSheet("#InAppShortcutStack { background: transparent; border: none; }")

    def _build_tab(keys_list: list, group_name: str, extra_rows=()) -> QWidget:
        page = QWidget()
        vbox = QVBoxLayout(page)
        vbox.setContentsMargins(0, 0, 0, 0)
        vbox.setSpacing(0)

        for cfg_key, tr_src, _default in keys_list:
            edit = InAppKeyEdit()
            value = dialog.config_manager.get_inapp_shortcut(cfg_key)
            edit.setText("" if is_reserved_inapp_shortcut(value) else value)
            dialog._inapp_edits[cfg_key] = edit
            dialog._inapp_groups[cfg_key] = group_name
            add_separated_row(
                vbox, _inapp_row(page, _INAPP_ICONS.get(cfg_key), dialog.tr(tr_src), edit)
            )

        for build_row in extra_rows:
            add_separated_row(vbox, build_row(page))

        vbox.addStretch(1)
        return page

    # 鼠标微移模式
    dialog.cursor_move_combo = ComboBox()
    dialog.cursor_move_combo.addItem("WASD+↑↓←→", userData="both")
    dialog.cursor_move_combo.addItem("↑↓←→", userData="arrows")
    dialog.cursor_move_combo.addItem("WASD", userData="wasd")

    cur_mode = dialog.config_manager.get_inapp_cursor_move_mode()
    idx = dialog.cursor_move_combo.findData(cur_mode)
    if idx >= 0:
        dialog.cursor_move_combo.setCurrentIndex(idx)

    screenshot_tab = _build_tab(
        SCREENSHOT_KEYS, "screenshot",
        extra_rows=[
            lambda page: _inapp_row(
                page, _CURSOR_MOVE_ICON, dialog.tr("Cursor Move Keys"), dialog.cursor_move_combo
            )
        ],
    )
    tools_tab = _build_tab(TOOL_KEYS, "screenshot")
    pin_tab = _build_tab(PIN_KEYS, "pin")

    stack.addWidget(screenshot_tab)
    stack.addWidget(tools_tab)
    stack.addWidget(pin_tab)

    tab_switch.addItem("screenshot", dialog.tr("Screenshot Shortcuts"), lambda: stack.setCurrentIndex(0))
    tab_switch.addItem("tools", dialog.tr("Annotation Tools"), lambda: stack.setCurrentIndex(1))
    tab_switch.addItem("pin", dialog.tr("Pin Shortcuts"), lambda: stack.setCurrentIndex(2))
    tab_switch.setCurrentItem("screenshot")

    tab_row = QWidget(grp_inapp)
    tab_row_layout = QHBoxLayout(tab_row)
    tab_row_layout.setContentsMargins(0, 0, 0, dialog_scaled(10))
    tab_row_layout.addWidget(tab_switch)
    tab_row_layout.addStretch(1)
    grp_inapp.addWidget(tab_row)
    grp_inapp.addWidget(stack)

    # 冲突检测
    for cfg_key, edit in dialog._inapp_edits.items():
        edit.textChanged.connect(
            lambda text, k=cfg_key: _on_shortcut_changed(dialog, k, text)
        )

    layout.addWidget(grp_inapp)

    # 提示
    hint = CaptionLabel(
        dialog.tr("💡 Configured shortcuts take priority over WASD and C. Arrow keys remain available; Esc is reserved."),
        view,
    )
    hint.setStyleSheet(f"padding: {dialog_scaled(5)}px;")
    layout.addWidget(hint)

    layout.addStretch()
    scroll.setWidget(view)
    return scroll


# ── 输入后冲突检测（交互式弹窗）──────────────────────────

def _on_shortcut_changed(dialog, changed_key: str, new_text: str):
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
    for keys_list in (SCREENSHOT_KEYS, TOOL_KEYS, PIN_KEYS):
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
            .replace('%1', format_shortcut_text(new_text))
            .replace('%2', conflict_label),
    )

    if ret is True:
        # 清空旧的，保留新的
        conflict_edit.setText("")
    else:
        # 撤销本次输入，恢复旧值
        old_val = dialog.config_manager.get_inapp_shortcut(changed_key)
        current_edit.setText("" if is_reserved_inapp_shortcut(old_val) else old_val)

    current_edit.blockSignals(False)
    conflict_edit.blockSignals(False)
