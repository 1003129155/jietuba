"""
截图 / 钉图工具栏的按钮排布：按钮的顺序，以及每个按钮始终显示、收进「…」还是始终隐藏

排布只决定按钮摆在哪、看不看得见，不碰按钮背后的逻辑：收起或隐藏的按钮照样连着
信号，快捷键走 select_tool 和信号、不经过按钮本身，所以隐藏按钮不会让功能失效。

截图和钉图的用法不同，各存一份排布（LayoutProfile），读写和归一化规则共用。
"""

import json
from dataclasses import dataclass

SHOW = "show"   # 始终显示在工具栏上
MORE = "more"   # 收进「…」弹层
HIDE = "hide"   # 始终隐藏
MODES = (SHOW, MORE, HIDE)


@dataclass(frozen=True)
class LayoutProfile:
    """一个工具栏的排布规则。

    order：可配置按钮的全集，也是默认顺序；default_more：默认收进「…」的按钮，其余默认始终显示；
    locked：固定显示、不出现在调整界面里的按钮；setting_key：存排布的设置键。
    """
    order: tuple
    default_more: frozenset
    locked: frozenset
    setting_key: str

    def default_mode(self, key):
        return MORE if key in self.default_more else SHOW


# 截图工具栏的可配置按钮，也是默认顺序
DEFAULT_ORDER = (
    "long_screenshot", "save", "screenshot_translate", "text_recognize", "scan_code", "gif",
    "pen", "highlighter", "mosaic", "spotlight", "watermark", "arrow", "number", "rect", "ellipse", "text", "eraser",
    "undo", "redo",
    "cancel", "pin", "confirm",
)

# 默认收进「…」的按钮。后加的低频功能放这里，免得把工具栏默认宽度越撑越宽
DEFAULT_MORE = frozenset({"text_recognize", "scan_code", "spotlight", "watermark"})

# 确定按钮固定显示、不可调整，也不出现在工具栏调整界面中：截图必须始终有办法收尾。
# 其余按钮都是普通编辑操作，用户可以自由排序、收进「…」或隐藏。
LOCKED = frozenset({"confirm"})

SETTING_KEY = "screenshot_toolbar_layout"

SCREENSHOT = LayoutProfile(DEFAULT_ORDER, DEFAULT_MORE, LOCKED, SETTING_KEY)

# 钉图没有截图专属的按钮（长截图、录屏、结束、钉图、确定），多一个复制。
# 默认显示的按钮和顺序与截图默认一致，用户在两个窗口里找同一个按钮时位置不变。
PIN = LayoutProfile(
    order=(
        "save", "copy",
        "pen", "highlighter", "mosaic", "spotlight", "watermark", "arrow", "number", "rect", "ellipse", "text",
        "eraser",
        "undo", "redo",
    ),
    default_more=frozenset({"spotlight", "watermark"}),
    locked=frozenset(),
    setting_key="pin_toolbar_layout",
)

SETTING_KEYS = (SCREENSHOT.setting_key, PIN.setting_key)


def default_layout(profile=SCREENSHOT):
    """[(按钮, 显示方式)]，默认顺序、默认显示方式"""
    return [(key, profile.default_mode(key)) for key in profile.order]


def normalize_layout(entries, profile=SCREENSHOT):
    """把任意来源的排布整理成合法、完整的排布；读配置、存配置、对话框回填都只走这里。

    - 未知按钮、重复按钮、格式不对的条目丢掉；不认识的显示方式按这个按钮的默认显示方式
    - 锁定的按钮一律始终显示
    - 缺的按钮（比如升级后新加的）按默认显示方式，插回默认顺序里它前一个按钮的后面。按
      默认顺序逐个补，补到第 i 个时前面的都已在列表里，前一个按钮一定找得到。不直接堆到
      末尾，是因为那样新按钮会出现在「确定」右边
    """
    if not isinstance(entries, (list, tuple)):
        entries = ()

    order = profile.order
    result = []
    seen = set()
    for entry in entries:
        try:
            key, mode = entry
        except (TypeError, ValueError):
            continue
        if key not in order or key in seen:
            continue
        seen.add(key)
        if key in profile.locked:
            mode = SHOW
        elif mode not in MODES:
            mode = profile.default_mode(key)
        result.append((key, mode))

    for index, key in enumerate(order):
        if key in seen:
            continue
        keys = [k for k, _mode in result]
        position = keys.index(order[index - 1]) + 1 if index else 0
        result.insert(position, (key, profile.default_mode(key)))
        seen.add(key)
    return result


def load_layout(profile=SCREENSHOT):
    """读用户配置；没配过或配置损坏时就是默认排布"""
    from settings import get_tool_settings_manager

    raw = get_tool_settings_manager().get_app_setting(profile.setting_key)
    try:
        stored = json.loads(raw) if raw else None
    except ValueError:
        stored = None
    return normalize_layout(stored, profile)


def save_layout(entries, profile=SCREENSHOT):
    """归一化后写入配置，返回实际保存的排布"""
    from settings import get_tool_settings_manager

    layout = normalize_layout(entries, profile)
    get_tool_settings_manager().set_app_setting(profile.setting_key, json.dumps(layout))
    return layout
