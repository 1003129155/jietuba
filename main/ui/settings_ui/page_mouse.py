# -*- coding: utf-8 -*-
"""鼠标快捷键设置页：全局鼠标快捷键，以及截图、钉图、剪贴板窗口里的鼠标动作"""
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QHBoxLayout, QLabel, QStackedWidget, QVBoxLayout, QWidget

from core.resource_manager import ResourceManager
from core.shortcut_manager import is_inapp_mouse_shortcut
from core.ui_scale import dialog_scaled
from core.ui_theme import set_own_style
from settings.tool_settings import (
    CAPTURE_MOUSE_ACTIONS,
    CLIPBOARD_MOUSE_ACTIONS,
    PIN_MOUSE_ACTIONS,
    QUICK_CAPTURE_ACTIONS,
    QUICK_CAPTURE_MODIFIERS,
    get_capture_mouse_binding,
    get_clipboard_mouse_binding,
    get_pin_mouse_binding,
    get_quick_capture_binding,
)
from ui.fluent_lite import ComboBox, FluentIcon, SegmentedWidget
from ui.fluent_lite.theme import ACCENT
from .components import IconBadge, SectionCard, add_separated_row, apply_theme_text_style, icon_ref, row_label, page_scroll_area
from .page_hotkey import INAPP_ICONS, inapp_label
from ..key_chip import format_shortcut_text


# 行图标与键盘页「应用内快捷键」共用，表示这一行的具体动作
_QUICK_CAPTURE_ICONS = {
    "pin": INAPP_ICONS["inapp_pin"],
    "copy": INAPP_ICONS["inapp_confirm"],
    "copy_pin": INAPP_ICONS["inapp_copy_pin"],
    "ocr": INAPP_ICONS["inapp_text_recognize"],
    "translate": INAPP_ICONS["inapp_translate"],
    "edit": FluentIcon.EDIT,
}

_CAPTURE_MOUSE_ICONS = {
    "copy": INAPP_ICONS["inapp_confirm"],
    "pin": INAPP_ICONS["inapp_pin"],
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
    "reset": INAPP_ICONS["inapp_pin_reset_size"],
    "thumbnail": INAPP_ICONS["inapp_thumbnail"],
    "region": "选择.svg",
    "copy_text": INAPP_ICONS["inapp_copy_pin_text"],
}

_CLIPBOARD_MOUSE_ICONS = {
    "paste": FluentIcon.PASTE,
    "pin": FluentIcon.PIN,
    "quick_edit": FluentIcon.EDIT,
    "menu": FluentIcon.LAYOUT,
}

_INAPP_MODIFIERS = ("", "ctrl", "shift", "alt", "ctrl+shift", "ctrl+alt", "shift+alt", "ctrl+shift+alt")
# 全局手势必须带修饰键。Win 组合排在前面：单独 Ctrl / Shift / Alt 加拖动是常见操作，容易误触
_GLOBAL_MODIFIERS = (
    "", "win", "ctrl+win", "shift+win", "alt+win", "ctrl+alt", "ctrl+shift", "shift+alt", "ctrl", "shift", "alt",
)
_MOUSE_GESTURES = {
    "wheel": (("wheel", "Mouse Wheel"),),
    "drag": (("dragleft", "Left Drag"), ("dragmiddle", "Middle Drag"), ("dragright", "Right Drag")),
    "click": (("left", "Left Click"), ("middle", "Middle Click"), ("right", "Right Click"),
              ("doubleleft", "Left Double-click"), ("doublemiddle", "Middle Double-click"),
              ("doubleright", "Right Double-click")),
    # 截图画布的左键单击负责选区/标注，不开放；右键不设双击，
    # 否则单击要等双击超时才能响应。
    "capture": (("middle", "Middle Click"), ("right", "Right Click"),
                ("doubleleft", "Left Double-click")),
    "global": (("dragleft", "Left Drag"), ("dragmiddle", "Middle Drag"), ("dragright", "Right Drag"),
               ("dragx1", "Back Button Drag"), ("dragx2", "Forward Button Drag")),
}
_GESTURE_LABELS = {value: label for gestures in _MOUSE_GESTURES.values() for value, label in gestures}


class MouseBindingEditor(QWidget):
    """修饰键组合 + 鼠标手势，currentData() 形如 "ctrl+dragright"。"""

    def __init__(self, dialog, kind, parent=None):
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(dialog_scaled(8))
        self.modifiers = ComboBox(self)
        for value in _GLOBAL_MODIFIERS if kind == "global" else _INAPP_MODIFIERS:
            label = value.title() if value else ("" if kind == "global" else dialog.tr("No Modifier"))
            self.modifiers.addItem(label, userData=value)
        self.gesture = ComboBox(self)
        for value, label in (("", "No Action"),) + _MOUSE_GESTURES[kind]:
            self.gesture.addItem(dialog.tr(label), userData=value)
        plus = QLabel("+", self)
        plus.setAlignment(Qt.AlignmentFlag.AlignCenter)
        apply_theme_text_style(plus, 14, caption=True)
        # 两侧等分剩余宽度，加号才能在每一行都落在同一列上
        layout.addWidget(self.modifiers, 1)
        layout.addWidget(plus)
        layout.addWidget(self.gesture, 1)
        self._modifier_required = kind == "global"
        if self._modifier_required:
            self.gesture.currentIndexChanged.connect(self._pair_modifiers)
            self.modifiers.currentIndexChanged.connect(self._pair_gesture)

    def _pair_modifiers(self):
        # 选了鼠标键就补上 Win，清掉鼠标键时修饰键一起清掉
        index = 1 if self.gesture.currentData() else 0
        if bool(self.modifiers.currentData()) != bool(index):
            self.modifiers.setCurrentIndex(index)

    def _pair_gesture(self):
        if not self.modifiers.currentData() and self.gesture.currentIndex():
            self.gesture.setCurrentIndex(0)

    def currentData(self):
        gesture = self.gesture.currentData()
        modifiers = self.modifiers.currentData()
        return f"{modifiers}+{gesture}" if gesture and modifiers else gesture

    def setBinding(self, binding):
        parts = str(binding or "").lower().split("+")
        modifiers = self.modifiers.findData("+".join(key for key in QUICK_CAPTURE_MODIFIERS if key in parts[:-1]))
        gesture = self.gesture.findData(parts[-1])
        # 选项里没有的写法按未设置显示，不能被补成另一个手势
        if modifiers < 0 or gesture < 0 or (self._modifier_required and bool(modifiers) != bool(gesture)):
            modifiers = gesture = 0
        self.modifiers.setCurrentIndex(modifiers)
        self.gesture.setCurrentIndex(gesture)


def _build_mouse_rows(dialog, *, actions, key_prefix, action_icons, binding_getter):
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
        icon = action_icons.get(action)
        row.addWidget(IconBadge(icon_ref(icon) if icon else None, None, card))
        title = row_label(card, dialog.tr(label))
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


def _create_global_section(dialog, parent):
    group = SectionCard(
        ResourceManager.get_icon_path("鼠标.svg"),
        dialog.tr("Global Mouse Shortcuts"),
        dialog.tr("Hold modifier keys and drag the mouse"),
        parent=parent,
    )
    group.addWidget(_build_mouse_rows(
        dialog, actions=QUICK_CAPTURE_ACTIONS, key_prefix="quick_capture_",
        action_icons=_QUICK_CAPTURE_ICONS, binding_getter=get_quick_capture_binding,
    ))
    return group


def _create_inapp_section(dialog, parent):
    group = SectionCard(
        FluentIcon.LAYOUT,
        dialog.tr("In-App Mouse Shortcuts"),
        dialog.tr("Only in screenshot, pin and clipboard windows"),
        parent=parent,
    )

    tab_switch = SegmentedWidget(group)
    tab_switch.setFixedHeight(dialog_scaled(34))
    tab_switch.setIndicatorColor(ACCENT, ACCENT)

    stack = QStackedWidget(group)
    stack.setObjectName("MouseShortcutStack")
    stack.setStyleSheet("#MouseShortcutStack { background: transparent; border: none; }")
    tabs = (
        ("screenshot", "Screenshot Shortcuts", CAPTURE_MOUSE_ACTIONS, "mouse_capture_",
         _CAPTURE_MOUSE_ICONS, get_capture_mouse_binding),
        ("pin", "Pin Shortcuts", PIN_MOUSE_ACTIONS, "mouse_pin_",
         _PIN_MOUSE_ICONS, get_pin_mouse_binding),
        ("clipboard", "Clipboard Shortcuts", CLIPBOARD_MOUSE_ACTIONS, "mouse_clipboard_",
         _CLIPBOARD_MOUSE_ICONS, get_clipboard_mouse_binding),
    )
    routes = [route for route, *_ in tabs]
    for route, title, actions, prefix, icons, getter in tabs:
        stack.addWidget(_build_mouse_rows(
            dialog, actions=actions, key_prefix=prefix, action_icons=icons, binding_getter=getter,
        ))
        tab_switch.addItem(route, dialog.tr(title))
    tab_switch.currentItemChanged.connect(lambda route: stack.setCurrentIndex(routes.index(route)))
    tab_switch.setCurrentItem("screenshot")

    tab_row = QWidget(group)
    tab_row_layout = QHBoxLayout(tab_row)
    tab_row_layout.setContentsMargins(0, 0, 0, dialog_scaled(10))
    tab_row_layout.addWidget(tab_switch)
    tab_row_layout.addStretch(1)
    group.addWidget(tab_row)
    group.addWidget(stack)
    return group


def create_mouse_page(dialog) -> QWidget:
    scroll = page_scroll_area(dialog)
    scroll.setWidgetResizable(True)
    scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

    view = QWidget()
    set_own_style(view, "background: transparent;")
    layout = QVBoxLayout(view)
    layout.setContentsMargins(0, 0, dialog_scaled(10), 0)
    layout.setSpacing(dialog_scaled(16))
    layout.addWidget(_create_global_section(dialog, view))
    layout.addWidget(_create_inapp_section(dialog, view))
    layout.addStretch()
    scroll.setWidget(view)
    return scroll


# ════ 保存前的冲突检查 ════

def _normalized_mouse_binding(binding: str) -> str:
    parts = [part for part in str(binding or "").lower().split("+") if part]
    return "+".join("middle" if part == "mousemiddle" else part for part in parts)


def _mouse_binding_text(dialog, binding: str) -> str:
    """按设置页上的叫法显示鼠标绑定。"""
    parts = _normalized_mouse_binding(binding).split("+")
    gesture = parts[-1]
    modifier_text = format_shortcut_text("+".join(parts[:-1]))
    gesture_label = dialog.tr(_GESTURE_LABELS.get(gesture, gesture))
    return f"{modifier_text} + {gesture_label}" if modifier_text else gesture_label


def mouse_binding_conflicts(dialog) -> list[tuple[str, str, tuple[str, ...]]]:
    """列出所有重复的鼠标绑定，每项是 (所在范围, 绑定, 占用者)。"""
    controls = getattr(dialog, '_behavior_controls', {})
    inapp_edits = getattr(dialog, '_inapp_edits', {})
    inapp_groups = getattr(dialog, '_inapp_groups', {})
    conflicts = []

    domains = (
        ("quick_capture_", None, "Global Mouse Shortcuts", QUICK_CAPTURE_ACTIONS),
        ("mouse_capture_", "screenshot", "Screenshot Shortcuts", CAPTURE_MOUSE_ACTIONS),
        ("mouse_pin_", "pin", "Pin Shortcuts", PIN_MOUSE_ACTIONS),
        ("mouse_clipboard_", "clipboard", "Clipboard Shortcuts", CLIPBOARD_MOUSE_ACTIONS),
    )
    for prefix, inapp_group, context_source, actions in domains:
        owners_by_binding = {}
        actions_by_binding = {}

        # 按设置页的行序遍历，冲突和占用者的顺序与界面一致
        for action, label_source, _default, _kind in actions:
            control = controls.get(f"{prefix}{action}")
            if control is None:
                continue
            binding = _normalized_mouse_binding(control.currentData())
            if binding:
                actions_by_binding.setdefault(binding, set()).add(action)
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
                    f"{inapp_label(dialog, key)} ({dialog.tr('In-App Shortcuts')})"
                )

        for binding, owners in owners_by_binding.items():
            # 钉图只对图片、快速编辑只对文字生效，两者可以共用一个手势
            if (prefix == "mouse_clipboard_" and len(owners) == 2
                    and actions_by_binding.get(binding) == {"pin", "quick_edit"}):
                continue
            if len(owners) > 1:
                conflicts.append((
                    dialog.tr(context_source),
                    _mouse_binding_text(dialog, binding),
                    tuple(owners),
                ))

    return conflicts


def mouse_binding_conflict_message(dialog, conflicts=None) -> str:
    """保存时的冲突提示，逐条列出要改的绑定。"""
    if conflicts is None:
        conflicts = mouse_binding_conflicts(dialog)
    lines = [dialog.tr("The following shortcuts conflict:"), ""]
    lines.extend(
        f"• {context} · {binding}: {' / '.join(owners)}"
        for context, binding, owners in conflicts
    )
    lines.extend(("", dialog.tr("Change one shortcut in each row before applying.")))
    return "\n".join(lines)
