# -*- coding: utf-8 -*-
"""放大镜取色的颜色格式。

一个格式就是一条模板串，渲染时把 {r} {g} {b} {hex} {h} {s} {l} 换成取到的颜色。
内置格式和用户自己加的格式共用这一套模型，所以列表里两者能混排、一起勾选、
一起排序，管理界面也不必分两块。

顺序有意义：勾选的格式都会显示在放大镜上，而按下取色键复制的是排在最前的
那一个——所以「把某个格式拖到第一位」就是「把它设成主格式」。
"""
from __future__ import annotations

import json
from dataclasses import dataclass, replace

SETTING_KEY = "magnifier_color_formats"
# 迁移用：这个键存的是早先单选下拉选中的那一个格式
LEGACY_SETTING_KEY = "magnifier_color_copy_format"

# 内置格式。名称是技术写法（RGB、HEX、CSS 函数名），四种语言下都一样，不进翻译。
BUILTIN_FORMATS = (
    ("RGB + HEX", "{r}, {g}, {b}  #{hex}"),
    ("RGB", "{r}, {g}, {b}"),
    ("CSS rgb()", "rgb({r}, {g}, {b})"),
    ("HEX", "#{hex}"),
    ("HEX without #", "{hex}"),
    ("CSS hsl()", "hsl({h}, {s}%, {l}%)"),
)

# 早先那版单选下拉的取值 → 现在对应的内置格式名
_LEGACY_NAMES = {
    "rgb_hex": "RGB + HEX",
    "rgb": "RGB",
    "rgb_css": "CSS rgb()",
    "hex": "HEX",
    "hex_bare": "HEX without #",
    "hsl_css": "CSS hsl()",
}

PLACEHOLDERS = ("r", "g", "b", "hex", "h", "s", "l")


@dataclass(frozen=True)
class ColorFormat:
    name: str
    template: str
    enabled: bool = False
    builtin: bool = False

    def render(self, color) -> str:
        """把模板渲染成最终复制出去的那串文字。

        模板是用户可以自己写的，认不出的占位符只会让这一条显示成原样，
        不至于让整个放大镜画不出来。
        """
        values = {
            "r": color.red(),
            "g": color.green(),
            "b": color.blue(),
            "hex": f"{color.red():02X}{color.green():02X}{color.blue():02X}",
            # 无彩色的 hslHue() 是 -1，直接写进 hsl() 就成了非法的 CSS
            "h": max(color.hslHue(), 0),
            "s": round(color.hslSaturationF() * 100),
            "l": round(color.lightnessF() * 100),
        }
        try:
            return self.template.format(**values)
        except (KeyError, IndexError, ValueError):
            return self.template


def default_formats() -> list[ColorFormat]:
    """内置格式，默认只启用第一个。"""
    return [
        ColorFormat(name, template, enabled=(index == 0), builtin=True)
        for index, (name, template) in enumerate(BUILTIN_FORMATS)
    ]


def normalize(formats) -> list[ColorFormat]:
    """补齐缺失的内置格式，并保证至少有一个是启用的。

    内置格式不允许删除，只允许取消勾选——列表里少了一条的话，用户在管理界面
    里就再也找不回来了。一个都没勾选时放大镜会没东西可显示，所以回落到第一条。
    """
    result = [f for f in formats if f.name or f.template]
    present = {f.name for f in result}
    for name, template in BUILTIN_FORMATS:
        if name not in present:
            result.append(ColorFormat(name, template, enabled=False, builtin=True))
    if not any(f.enabled for f in result):
        result[0] = replace(result[0], enabled=True)
    return result


def enabled_formats(formats) -> list[ColorFormat]:
    return [f for f in formats if f.enabled]


def load(config_manager) -> list[ColorFormat]:
    """从配置读出格式列表，没有就按早先的单选设置迁移一份。"""
    if config_manager is None:
        return default_formats()
    raw = config_manager.get_app_setting(SETTING_KEY, "")
    if raw:
        try:
            items = json.loads(raw)
        except (TypeError, ValueError):
            items = []
        formats = [
            ColorFormat(
                name=str(item.get("name", "")),
                template=str(item.get("template", "")),
                enabled=bool(item.get("enabled", False)),
                builtin=bool(item.get("builtin", False)),
            )
            for item in items if isinstance(item, dict)
        ]
        if formats:
            return normalize(formats)
    return _migrate_from_legacy(config_manager)


def _migrate_from_legacy(config_manager) -> list[ColorFormat]:
    legacy = config_manager.get_app_setting(LEGACY_SETTING_KEY, "")
    wanted = _LEGACY_NAMES.get(legacy)
    formats = [
        ColorFormat(name, template, enabled=(name == wanted), builtin=True)
        for name, template in BUILTIN_FORMATS
    ]
    return normalize(formats)


def save(config_manager, formats) -> None:
    if config_manager is None:
        return
    payload = [
        {"name": f.name, "template": f.template,
         "enabled": f.enabled, "builtin": f.builtin}
        for f in normalize(formats)
    ]
    config_manager.set_app_setting(SETTING_KEY, json.dumps(payload, ensure_ascii=False))
