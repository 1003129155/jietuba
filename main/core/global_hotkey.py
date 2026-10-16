"""全局热键的键名表与注册方式判定。

录入框、保存前校验、注册、旧设置迁移都按这里的结果走，别处不另写规则。
不依赖 Qt，C++ 版可以直接拿这里的结果逐条对照。
"""

from typing import Dict, Iterable, Optional, Tuple

MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_WIN = 0x0008
MOD_NOREPEAT = 0x4000

VK_SNAPSHOT = 0x2C
VK_DELETE = 0x2E
VK_L = ord("L")

# 鼠标侧键用独立的名字，和键盘组合键分开解析、分开登记
MOUSE_BUTTON_BACK = "mouseback"
MOUSE_BUTTON_FORWARD = "mouseforward"
MOUSE_BUTTONS = frozenset({MOUSE_BUTTON_BACK, MOUSE_BUTTON_FORWARD})

# 判定结果
UNSET = "unset"          # 留空
INVALID = "invalid"      # 解析不了，或不带修饰键的普通字符键
RESERVED = "reserved"    # 系统在钩子之前处理，谁也拦不住
MOUSE = "mouse"          # 鼠标侧键，走钩子
REGISTER = "register"    # 只走系统注册，被别的程序占着就是冲突
HOOK = "hook"            # 钩子接管，同时系统注册兜底

# 写出时的顺序，和录入框、输入中心一致
_MODIFIERS = (("ctrl", MOD_CONTROL), ("shift", MOD_SHIFT), ("alt", MOD_ALT), ("win", MOD_WIN))
_MODIFIER_ALIASES = {
    "ctrl": MOD_CONTROL, "control": MOD_CONTROL,
    "shift": MOD_SHIFT,
    "alt": MOD_ALT,
    "win": MOD_WIN, "meta": MOD_WIN, "super": MOD_WIN,
}

# (虚拟键码, 写出的名字, 读入时也认的别名…)；写出的名字就是录入框记录的那个
_NAMED_KEYS = (
    (0x08, "backspace"),
    (0x09, "tab", "backtab"),
    (0x0C, "clear"),
    (0x0D, "enter", "return"),
    (0x13, "pause"),
    (0x1B, "esc", "escape"),
    (0x20, "space"),
    (0x21, "pageup"),
    (0x22, "pagedown"),
    (0x23, "end"),
    (0x24, "home"),
    (0x25, "left"),
    (0x26, "up"),
    (0x27, "right"),
    (0x28, "down"),
    (VK_SNAPSHOT, "print", "printscreen", "prtsc", "sysreq"),
    (0x2D, "insert"),
    (VK_DELETE, "delete", "del"),
    (0xBA, ";", "semicolon"),
    (0xBB, "=", "equals", "equal"),
    (0xBC, ",", "comma"),
    (0xBD, "-", "minus"),
    (0xBE, ".", "period"),
    (0xBF, "/", "slash"),
    (0xC0, "`", "oem3", "backquote", "grave"),
    (0xDB, "[", "lbracket"),
    (0xDC, "\\", "backslash"),
    (0xDD, "]", "rbracket"),
    (0xDE, "'", "quote"),
)

# 不带修饰键也能单独绑定的键：功能键和这些不会打出字符的键
_BARE_KEYS = frozenset(
    [0x13, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, VK_SNAPSHOT, 0x2D]
    + list(range(0x70, 0x88))
)


def _build_tables() -> Tuple[Dict[str, int], Dict[int, str]]:
    name_to_vk: Dict[str, int] = {}
    vk_to_name: Dict[int, str] = {}
    for vk, name, *aliases in _NAMED_KEYS:
        vk_to_name[vk] = name
        for alias in (name, *aliases):
            name_to_vk[alias] = vk
    for vk in range(ord("A"), ord("Z") + 1):
        vk_to_name[vk] = chr(vk).lower()
        name_to_vk[chr(vk).lower()] = vk
    for vk in range(ord("0"), ord("9") + 1):
        vk_to_name[vk] = chr(vk)
        name_to_vk[chr(vk)] = vk
    for n in range(1, 25):
        vk_to_name[0x6F + n] = f"f{n}"
        name_to_vk[f"f{n}"] = 0x6F + n
    return name_to_vk, vk_to_name


_NAME_TO_VK, _VK_TO_NAME = _build_tables()


def parse_hotkey(text: str) -> Tuple[int, int]:
    """'ctrl+shift+a' → (修饰键位掩码, 虚拟键码)，不含 MOD_NOREPEAT。解析不了抛 ValueError。"""
    if not isinstance(text, str):
        raise ValueError("hotkey must be a string")
    parts = [p.strip().lower() for p in text.split("+")]
    # "ctrl++" 这类以 + 结尾的是没录完的前缀
    if not parts or any(not p for p in parts):
        raise ValueError(f"incomplete hotkey: {text!r}")
    mods = 0
    keys = []
    for part in parts:
        if part in _MODIFIER_ALIASES:
            mods |= _MODIFIER_ALIASES[part]
        else:
            keys.append(part)
    if len(keys) != 1:
        raise ValueError(f"expected exactly one main key: {text!r}")
    vk = _NAME_TO_VK.get(keys[0])
    if vk is None:
        raise ValueError(f"unsupported key: {keys[0]!r}")
    return mods, vk


def format_hotkey(mods: int, vk: int) -> Optional[str]:
    """(修饰键位掩码, 虚拟键码) → 录入框记录的写法；键不在表里返回 None。"""
    name = _VK_TO_NAME.get(vk)
    if name is None:
        return None
    return "+".join([n for n, bit in _MODIFIERS if mods & bit] + [name])


def modifier_mask(names: Iterable[str]) -> int:
    """输入中心报告的修饰键名 → 位掩码。"""
    mask = 0
    for name in names:
        mask |= _MODIFIER_ALIASES.get(name, 0)
    return mask


def modifier_names(mods: int):
    """位掩码 → 输入中心认的修饰键名。"""
    return [name for name, bit in _MODIFIERS if mods & bit]


def route(text: str) -> str:
    """这个值该怎么注册：UNSET / INVALID / RESERVED / MOUSE / REGISTER / HOOK。"""
    normalized = (text or "").strip().lower()
    if not normalized:
        return UNSET
    if normalized in MOUSE_BUTTONS:
        return MOUSE
    try:
        mods, vk = parse_hotkey(normalized)
    except ValueError:
        return INVALID
    return route_of(mods, vk)


def route_of(mods: int, vk: int) -> str:
    """route() 的解析后版本。"""
    mods &= ~MOD_NOREPEAT
    # Win+L 锁屏、Ctrl+Alt+Del 安全序列都在钩子之前由系统处理；多按几个修饰键也一样
    if mods & MOD_WIN and vk == VK_L:
        return RESERVED
    if mods & MOD_CONTROL and mods & MOD_ALT and vk == VK_DELETE:
        return RESERVED
    if not mods and vk not in _BARE_KEYS:
        return INVALID
    # 系统自己占着的组合多是 Win 开头，PrtSc 则可能注册成功却仍弹出系统截图
    if mods & MOD_WIN or vk == VK_SNAPSHOT:
        return HOOK
    return REGISTER

