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
from settings import ANNOTATION_TOOL_SHORTCUTS, clipboard_pick_keys
from core.shortcut_manager import is_reserved_inapp_shortcut, is_inapp_mouse_shortcut
from core.ui_theme import set_own_style
from settings.tool_settings import (
    CAPTURE_MOUSE_ACTIONS,
    PIN_MOUSE_ACTIONS,
    get_capture_mouse_binding,
    get_pin_mouse_binding,
)


class MouseBindingEditor(QWidget):
    """A modifier combination and a mouse gesture, with no global mouse hooks."""

    def __init__(self, dialog, kind, parent=None):
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(dialog_scaled(8))
        self.modifiers = ComboBox(self)
        for value in ("", "ctrl", "shift", "alt", "ctrl+shift", "ctrl+alt", "shift+alt", "ctrl+shift+alt"):
            self.modifiers.addItem(value.title() if value else dialog.tr("No Modifier"), userData=value)
        self.gesture = ComboBox(self)
        gestures = {
            "wheel": (("wheel", "Mouse Wheel"),),
            "drag": (("dragleft", "Left Drag"), ("dragmiddle", "Middle Drag"), ("dragright", "Right Drag")),
            "click": (("left", "Left Click"), ("middle", "Middle Click"), ("right", "Right Click"),
                      ("doubleleft", "Left Double-click"), ("doublemiddle", "Middle Double-click"),
                      ("doubleright", "Right Double-click")),
            # 截图画布的左键单击负责选区/标注，不开放；右键不设双击，
            # 否则单击要等双击超时才能响应。
            "capture": (("middle", "Middle Click"), ("right", "Right Click"),
                        ("doubleleft", "Left Double-click")),
        }[kind]
        for value, label in (("", "No Action"),) + gestures:
            self.gesture.addItem(dialog.tr(label), userData=value)
        plus = QLabel("+", self)
        plus.setAlignment(Qt.AlignmentFlag.AlignCenter)
        apply_theme_text_style(plus, 14, caption=True)
        # 两侧等分剩余宽度，加号才能在每一行都落在同一列上
        layout.addWidget(self.modifiers, 1)
        layout.addWidget(plus)
        layout.addWidget(self.gesture, 1)

    def currentData(self):
        gesture = self.gesture.currentData()
        modifiers = self.modifiers.currentData()
        return f"{modifiers}+{gesture}" if gesture and modifiers else gesture

    def setBinding(self, binding):
        parts = str(binding or "").lower().split("+")
        modifiers = "+".join(key for key in ("ctrl", "shift", "alt") if key in parts[:-1])
        self.modifiers.setCurrentIndex(max(0, self.modifiers.findData(modifiers)))
        self.gesture.setCurrentIndex(max(0, self.gesture.findData(parts[-1])))


def _build_mouse_shortcut_tab(
    dialog, *, actions, key_prefix, action_icons, binding_getter
):
    page = QWidget()
    page_layout = QVBoxLayout(page)
    page_layout.setContentsMargins(0, 0, 0, 0)
    page_layout.setSpacing(0)
    if not hasattr(dialog, '_behavior_controls'):
        dialog._behavior_controls = {}
    for action, label, _default, kind in actions:
        card = QWidget(page)
        row = QHBoxLayout(card)
        row.setContentsMargins(0, dialog_scaled(8), 0, dialog_scaled(8))
        row.setSpacing(dialog_scaled(14))
        # 和上方“应用内快捷键”一致：图标表达这一行的具体动作，
        # 不再给整个标签页重复使用相机/图钉分类图标。
        icon = action_icons.get(action)
        row.addWidget(IconBadge(_icon_ref(icon) if icon else None, None, card))
        title = _row_label(card, dialog.tr(label))
        title.setWordWrap(True)
        row.addWidget(title, 1)
        editor = MouseBindingEditor(dialog, kind, card)
        editor.setFixedWidth(dialog_scaled(320))
        editor.setBinding(binding_getter(dialog.config_manager, action))
        dialog._behavior_controls[f"{key_prefix}{action}"] = editor
        row.addWidget(editor)
        add_separated_row(page_layout, card)
    page_layout.addStretch(1)
    return page


def _create_mouse_shortcuts(dialog, parent):
    group = SectionCard(
        ResourceManager.get_icon_path("鼠标.svg"),
        dialog.tr("Mouse Shortcuts"),
        parent=parent,
    )

    tab_switch = SegmentedWidget(group)
    tab_switch.setFixedHeight(dialog_scaled(34))
    tab_switch.setIndicatorColor(ACCENT, ACCENT)

    stack = QStackedWidget(group)
    stack.setObjectName("MouseShortcutStack")
    stack.setStyleSheet("#MouseShortcutStack { background: transparent; border: none; }")

    stack.addWidget(_build_mouse_shortcut_tab(
        dialog,
        actions=CAPTURE_MOUSE_ACTIONS,
        key_prefix="mouse_capture_",
        action_icons=_CAPTURE_MOUSE_ICONS,
        binding_getter=get_capture_mouse_binding,
    ))
    stack.addWidget(_build_mouse_shortcut_tab(
        dialog,
        actions=PIN_MOUSE_ACTIONS,
        key_prefix="mouse_pin_",
        action_icons=_PIN_MOUSE_ICONS,
        binding_getter=get_pin_mouse_binding,
    ))

    tab_switch.addItem(
        "screenshot", dialog.tr("Screenshot Shortcuts"),
        lambda: stack.setCurrentIndex(0),
    )
    tab_switch.addItem(
        "pin", dialog.tr("Pin Shortcuts"),
        lambda: stack.setCurrentIndex(1),
    )
    tab_switch.setCurrentItem("screenshot")

    tab_row = QWidget(group)
    tab_row_layout = QHBoxLayout(tab_row)
    tab_row_layout.setContentsMargins(0, 0, 0, dialog_scaled(10))
    tab_row_layout.addWidget(tab_switch)
    tab_row_layout.addStretch(1)
    group.addWidget(tab_row)
    group.addWidget(stack)
    return group


def validate_mouse_bindings(dialog) -> bool:
    """Mouse gestures must be unique inside each active window context."""
    return not mouse_binding_conflicts(dialog)


def _normalized_mouse_binding(binding: str) -> str:
    parts = [part for part in str(binding or "").lower().split("+") if part]
    return "+".join("middle" if part == "mousemiddle" else part for part in parts)


def _mouse_binding_text(dialog, binding: str) -> str:
    """Return the same friendly mouse names used by the binding editors."""
    parts = _normalized_mouse_binding(binding).split("+")
    gesture = parts[-1]
    gesture_source = {
        "wheel": "Mouse Wheel",
        "dragleft": "Left Drag",
        "dragmiddle": "Middle Drag",
        "dragright": "Right Drag",
        "left": "Left Click",
        "middle": "Middle Click",
        "right": "Right Click",
        "doubleleft": "Left Double-click",
        "doublemiddle": "Middle Double-click",
        "doubleright": "Right Double-click",
    }.get(gesture, gesture)
    modifier_text = format_shortcut_text("+".join(parts[:-1]))
    gesture_text = dialog.tr(gesture_source)
    return f"{modifier_text} + {gesture_text}" if modifier_text else gesture_text


def mouse_binding_conflicts(dialog) -> list[tuple[str, str, tuple[str, ...]]]:
    """List every duplicated mouse binding with its context and owners.

    The old save-time validator returned only a boolean, which left the warning
    dialog unable to tell the user what to fix.  Keeping the detailed result
    here also ensures validation and the displayed explanation cannot drift.
    """
    controls = getattr(dialog, '_behavior_controls', {})
    inapp_edits = getattr(dialog, '_inapp_edits', {})
    inapp_groups = getattr(dialog, '_inapp_groups', {})
    conflicts = []

    domains = (
        ("mouse_capture_", "screenshot", "Screenshot Shortcuts", CAPTURE_MOUSE_ACTIONS),
        ("mouse_pin_", "pin", "Pin Shortcuts", PIN_MOUSE_ACTIONS),
    )
    for prefix, inapp_group, context_source, actions in domains:
        owners_by_binding = {}

        # Follow the UI row order so both the conflict list and its owners are
        # stable and easy to find on the settings page.
        for action, label_source, _default, _kind in actions:
            control = controls.get(f"{prefix}{action}")
            if control is None:
                continue
            binding = _normalized_mouse_binding(control.currentData())
            if binding:
                owners_by_binding.setdefault(binding, []).append(
                    f"{dialog.tr(label_source)} ({dialog.tr('Mouse Shortcuts')})"
                )

        for key, edit in inapp_edits.items():
            if inapp_groups.get(key) != inapp_group:
                continue
            raw_binding = edit.text()
            binding = _normalized_mouse_binding(raw_binding)
            if is_inapp_mouse_shortcut(raw_binding) and binding:
                owners_by_binding.setdefault(binding, []).append(
                    f"{_inapp_label(dialog, key)} ({dialog.tr('In-App Shortcuts')})"
                )

        for binding, owners in owners_by_binding.items():
            if len(owners) > 1:
                conflicts.append((
                    dialog.tr(context_source),
                    _mouse_binding_text(dialog, binding),
                    tuple(owners),
                ))

    return conflicts


def mouse_binding_conflict_message(dialog, conflicts=None) -> str:
    """Build a concise, actionable save-time warning for mouse conflicts."""
    if conflicts is None:
        conflicts = mouse_binding_conflicts(dialog)
    lines = [dialog.tr("The following shortcuts conflict:"), ""]
    lines.extend(
        f"• {context} · {binding}: {' / '.join(owners)}"
        for context, binding, owners in conflicts
    )
    lines.extend(("", dialog.tr("Change one shortcut in each row before applying.")))
    return "\n".join(lines)


# ── 应用内快捷键定义表（分组）──────────────────────────────
SCREENSHOT_KEYS = [
    ("inapp_confirm",   "Copy to Clipboard",      "ctrl+c"),
    ("inapp_pin",       "Pin to Screen",           "mousemiddle"),
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
    ("inapp_pin_reset_size",  "Reset Size",             ""),
    ("inapp_thumbnail",       "Toggle Thumbnail",       "r"),
    ("inapp_toggle_toolbar",  "Toggle Toolbar",         "space"),
]

TOOL_KEYS = [
    (cfg_key, label, default)
    for cfg_key, _tool_id, label, default in ANNOTATION_TOOL_SHORTCUTS
]

CLIPBOARD_KEYS = [
    ("inapp_clipboard_quick_edit", "Quick Edit", "tab"),
]

# 在快速编辑框的文本框里生效，不能占用会输入字符的键
CLIPBOARD_EDITOR_KEYS = [
    ("inapp_clipboard_edit_save", "Save Edit", "ctrl+enter"),
    ("inapp_clipboard_edit_save_paste", "Save and Paste", "ctrl+shift+enter"),
]

INAPP_KEYS = SCREENSHOT_KEYS + TOOL_KEYS + PIN_KEYS + CLIPBOARD_KEYS + CLIPBOARD_EDITOR_KEYS

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
    "inapp_clipboard_quick_edit": FluentIcon.EDIT,
    "inapp_clipboard_edit_save": FluentIcon.SAVE,
    "inapp_clipboard_edit_save_paste": FluentIcon.SEND,
}

# 鼠标动作与上方应用内快捷键沿用同一套具体功能图标；只有鼠标手势的
# 录入方式不同，不应退化成每行重复相机/图钉的分类图标。
_CAPTURE_MOUSE_ICONS = {
    "copy": _INAPP_ICONS["inapp_confirm"],
    "pin": _INAPP_ICONS["inapp_pin"],
    "save": "保存.svg",
    "quick_save": FluentIcon.DOWNLOAD,
    "close": FluentIcon.CLOSE,
}

_PIN_MOUSE_ICONS = {
    "zoom": FluentIcon.SEARCH,
    "opacity": FluentIcon.TRANSPARENT,
    # 工具栏的“关闭.svg”自带红色圆形底；作为可着色的行图标时，
    # 底和白色叉号会合并成一整块蒙版，因此这里使用纯叉号图标。
    "close": FluentIcon.CLOSE,
    "reset": _INAPP_ICONS["inapp_pin_reset_size"],
    "thumbnail": _INAPP_ICONS["inapp_thumbnail"],
    "region": "选择.svg",
    "copy_text": _INAPP_ICONS["inapp_copy_pin_text"],
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


def _global_hotkey_values(dialog) -> set:
    """当前各全局热键录入框里的有效值；留空和未录完的前缀（如 "ctrl+"）不算。"""
    return {
        text
        for edit in _iter_global_hotkey_edits(dialog)
        for text in (edit.text().strip().lower(),)
        if text and not text.endswith("+")
    }


def _refresh_inapp_shadow_states(dialog):
    """标出和某个全局热键撞了键的应用内快捷键。

    全局热键靠系统 RegisterHotKey 在按键到达窗口前就拦下了它，撞键的应用内
    快捷键实际收不到按键、形同虚设；这里只负责提示，不改动任何已保存的值。
    """
    inapp_edits = getattr(dialog, "_inapp_edits", None)
    if not inapp_edits:
        return
    globals_ = _global_hotkey_values(dialog)
    tip = dialog.tr(
        "This is the same as a global hotkey — the system intercepts it "
        "first, so it won't work here."
    )
    for edit in inapp_edits.values():
        text = edit.text().strip().lower()
        shadowed = bool(text) and text in globals_
        edit.setErrorState(shadowed)
        edit.setToolTip(tip if shadowed else "")


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
    set_own_style(view, "background: transparent;")
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
        # 全局热键一变，应用内快捷键里和它撞了键的那些也要跟着更新提示。
        edit.textChanged.connect(
            lambda _text, d=dialog: _refresh_inapp_shadow_states(d)
        )
    validate_global_hotkey_edits(dialog)

    layout.addWidget(grp_global)

    # ════ 应用内快捷键 ════
    grp_inapp = SectionCard(
        FluentIcon.LAYOUT,
        dialog.tr("In-App Shortcuts"),
        dialog.tr("Only in screenshot, pin and clipboard windows"),
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

    def _build_tab(keys_list: list, group_name: str, extra_rows=(), allow_mouse=True, text_input_keys=()) -> QWidget:
        page = QWidget()
        vbox = QVBoxLayout(page)
        vbox.setContentsMargins(0, 0, 0, 0)
        vbox.setSpacing(0)

        for cfg_key, tr_src, _default in keys_list:
            edit = InAppKeyEdit(
                allow_mouse=allow_mouse and cfg_key != "inapp_pin_reset_size",
                allow_text_keys=cfg_key not in text_input_keys,
            )
            value = dialog.config_manager.get_inapp_shortcut(cfg_key)
            if cfg_key == "inapp_pin_reset_size" and is_inapp_mouse_shortcut(value):
                # The mouse binding is edited in the dedicated mouse section below.
                value = ""
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
    # 直选粘贴键不单独配置，只选用哪一套；判重时算进剪贴板组
    dialog.clipboard_pick_combo = ComboBox()
    for mode, label in (("both", "Digits + Letters"), ("letters", "Letters"), ("digits", "Digits")):
        dialog.clipboard_pick_combo.addItem(dialog.tr(label), userData=mode)
    idx = dialog.clipboard_pick_combo.findData(dialog.config_manager.get_inapp_clipboard_pick_mode())
    if idx >= 0:
        dialog.clipboard_pick_combo.setCurrentIndex(idx)

    # 剪贴板窗口的快捷键处理器只接键盘
    clipboard_tab = _build_tab(
        CLIPBOARD_KEYS + CLIPBOARD_EDITOR_KEYS, "clipboard", allow_mouse=False,
        text_input_keys={cfg_key for cfg_key, _label, _default in CLIPBOARD_EDITOR_KEYS},
        extra_rows=[
            lambda page: _inapp_row(
                page, FluentIcon.PASTE, dialog.tr("Direct Pick Keys"), dialog.clipboard_pick_combo
            )
        ],
    )

    stack.addWidget(screenshot_tab)
    stack.addWidget(tools_tab)
    stack.addWidget(pin_tab)
    stack.addWidget(clipboard_tab)

    tab_switch.addItem("screenshot", dialog.tr("Screenshot Shortcuts"), lambda: stack.setCurrentIndex(0))
    tab_switch.addItem("tools", dialog.tr("Annotation Tools"), lambda: stack.setCurrentIndex(1))
    tab_switch.addItem("pin", dialog.tr("Pin Shortcuts"), lambda: stack.setCurrentIndex(2))
    tab_switch.addItem("clipboard", dialog.tr("Clipboard Shortcuts"), lambda: stack.setCurrentIndex(3))
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
        edit.textChanged.connect(
            lambda _text, d=dialog: _refresh_inapp_shadow_states(d)
        )
    # 主动跑一次，标出配置文件里已经和全局热键撞键的项。
    _refresh_inapp_shadow_states(dialog)
    dialog._clipboard_pick_mode = dialog.clipboard_pick_combo.currentData()
    dialog.clipboard_pick_combo.currentIndexChanged.connect(
        lambda _index: _on_pick_mode_changed(dialog)
    )

    layout.addWidget(grp_inapp)

    layout.addWidget(_create_mouse_shortcuts(dialog, view))
    layout.addWidget(CaptionLabel(
        dialog.tr("Pin mouse actions apply outside annotation mode. Conflicts use the first matching action. "
                  "Copy text uses the selected OCR text; otherwise right-click opens the menu."), view,
    ))

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
        if my_group == "clipboard":
            _check_pick_conflict(dialog, changed_key, new_text)
        return

    conflict_label = _inapp_label(dialog, conflict_key)

    # 弹窗询问
    current_edit = dialog._inapp_edits[changed_key]
    conflict_edit = dialog._inapp_edits[conflict_key]

    # 阻塞信号防止递归
    current_edit.blockSignals(True)
    conflict_edit.blockSignals(True)

    if _ask_replace(dialog, new_text, conflict_label):
        # 清空旧的，保留新的
        conflict_edit.setText("")
    else:
        # 撤销本次输入，恢复旧值
        old_val = dialog.config_manager.get_inapp_shortcut(changed_key)
        current_edit.setText("" if is_reserved_inapp_shortcut(old_val) else old_val)

    current_edit.blockSignals(False)
    conflict_edit.blockSignals(False)


def _inapp_label(dialog, cfg_key: str) -> str:
    for cfg, tr_src, _default in INAPP_KEYS:
        if cfg == cfg_key:
            return dialog.tr(tr_src)
    return cfg_key


def _pick_char_of(text: str) -> str:
    """会被剪贴板直选吃掉的键（"e"、"shift+e"、"3"）对应的字符，否则空串。

    直选只挡 Ctrl/Alt/Win，按住 Shift 的字母同样会被直选接走。
    """
    *mods, main = text.strip().lower().split("+")
    if set(mods) - {"shift"}:
        return ""
    return main if len(main) == 1 and main in clipboard_pick_keys("both") else ""


def _ask_replace(dialog, key_text: str, owner_label: str) -> bool:
    return show_confirm_dialog(
        dialog,
        dialog.tr("Shortcut Conflict"),
        dialog.tr('"%1" is already used by "%2".\nReplace it?')
            .replace('%1', format_shortcut_text(key_text))
            .replace('%2', owner_label),
    ) is True


def _set_pick_mode(dialog, mode: str):
    combo = dialog.clipboard_pick_combo
    combo.blockSignals(True)
    combo.setCurrentIndex(combo.findData(mode))
    combo.blockSignals(False)
    dialog._clipboard_pick_mode = mode


def _check_pick_conflict(dialog, changed_key: str, new_text: str):
    """剪贴板快捷键占用了当前直选键：替换则直选切到不含它的那一套，否则撤销输入。"""
    char = _pick_char_of(new_text)
    if not char or char not in clipboard_pick_keys(dialog._clipboard_pick_mode):
        return
    if _ask_replace(dialog, new_text, dialog.tr("Direct Pick Keys")):
        _set_pick_mode(dialog, "letters" if char.isdigit() else "digits")
        return
    edit = dialog._inapp_edits[changed_key]
    old_val = dialog.config_manager.get_inapp_shortcut(changed_key)
    edit.blockSignals(True)
    edit.setText("" if is_reserved_inapp_shortcut(old_val) else old_val)
    edit.blockSignals(False)


def _on_pick_mode_changed(dialog):
    """换一套直选键后，逐个检查剪贴板快捷键有没有被它占用。"""
    mode = dialog.clipboard_pick_combo.currentData()
    keys = clipboard_pick_keys(mode)
    for cfg_key, edit in dialog._inapp_edits.items():
        if dialog._inapp_groups.get(cfg_key) != "clipboard":
            continue
        char = _pick_char_of(edit.text())
        if not char or char not in keys:
            continue
        if not _ask_replace(dialog, edit.text(), _inapp_label(dialog, cfg_key)):
            _set_pick_mode(dialog, dialog._clipboard_pick_mode)
            return
        edit.blockSignals(True)
        edit.setText("")
        edit.blockSignals(False)
    dialog._clipboard_pick_mode = mode
