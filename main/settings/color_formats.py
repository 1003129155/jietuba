# -*- coding: utf-8 -*-
"""放大镜取色的颜色格式。

格式只有下面这几种预设，用户能改的只是勾选哪些、按什么顺序排。

顺序有意义：勾选的格式都会显示在放大镜上，而按下取色键复制的是排在最前的
那一个——所以「把某个格式拖到第一位」就是「把它设成主格式」。
"""
from __future__ import annotations

import json
from dataclasses import dataclass, replace

SETTING_KEY = "magnifier_color_formats"
# 迁移用：这个键存的是早先单选下拉选中的那一个格式
LEGACY_SETTING_KEY = "magnifier_color_copy_format"

# 名称是技术写法（RGB、HEX、CSS 函数名），四种语言下都一样，不进翻译。
# 名称同时是存储用的标识，改名会让已保存的勾选和顺序对不上。
BUILTIN_FORMATS = (
    ("RGB + HEX", "{r}, {g}, {b}  #{hex}"),
    ("RGB", "{r}, {g}, {b}"),
    ("CSS rgb()", "rgb({r}, {g}, {b})"),
    ("HEX", "#{hex}"),
    ("HEX without #", "{hex}"),
    ("CSS hsl()", "hsl({h}, {s}%, {l}%)"),
)
_TEMPLATES = dict(BUILTIN_FORMATS)
# RGB 排在 HEX 前面，所以默认按取色键复制的是 RGB
DEFAULT_ENABLED = ("RGB", "HEX")

# 早先那版单选下拉的取值 → 现在对应的格式名
_LEGACY_NAMES = {
    "rgb_hex": "RGB + HEX",
    "rgb": "RGB",
    "rgb_css": "CSS rgb()",
    "hex": "HEX",
    "hex_bare": "HEX without #",
    "hsl_css": "CSS hsl()",
}


@dataclass(frozen=True)
class ColorFormat:
    name: str
    template: str
    enabled: bool = False

    def render(self, color) -> str:
        """把模板渲染成最终复制出去的那串文字。"""
        return self.template.format(
            r=color.red(),
            g=color.green(),
            b=color.blue(),
            hex=f"{color.red():02X}{color.green():02X}{color.blue():02X}",
            # 无彩色的 hslHue() 是 -1，直接写进 hsl() 就成了非法的 CSS
            h=max(color.hslHue(), 0),
            s=round(color.hslSaturationF() * 100),
            l=round(color.lightnessF() * 100),
        )


def default_formats() -> list[ColorFormat]:
    """全部预设按原顺序排，启用 DEFAULT_ENABLED 里的几个。"""
    return [
        ColorFormat(name, template, enabled=(name in DEFAULT_ENABLED))
        for name, template in BUILTIN_FORMATS
    ]


def normalize(formats) -> list[ColorFormat]:
    """只留预设、去重，补齐缺失的预设，并保证至少有一个是启用的。

    模板一律取自预设，不信任传进来的。一个都没勾选时放大镜会没东西可显示，
    所以回落到第一条。
    """
    result = []
    seen = set()
    for fmt in formats:
        if fmt.name in _TEMPLATES and fmt.name not in seen:
            seen.add(fmt.name)
            result.append(ColorFormat(fmt.name, _TEMPLATES[fmt.name], fmt.enabled))
    for name, template in BUILTIN_FORMATS:
        if name not in seen:
            result.append(ColorFormat(name, template, enabled=False))
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
        if not isinstance(items, list):
            items = []
        formats = [
            ColorFormat(
                name=str(item.get("name", "")),
                template="",
                enabled=bool(item.get("enabled", False)),
            )
            for item in items if isinstance(item, dict)
        ]
        if formats:
            return normalize(formats)
    return _migrate_from_legacy(config_manager)


def _migrate_from_legacy(config_manager) -> list[ColorFormat]:
    legacy = config_manager.get_app_setting(LEGACY_SETTING_KEY, "")
    wanted = _LEGACY_NAMES.get(legacy)
    if wanted is None:
        return default_formats()
    formats = [
        ColorFormat(name, template, enabled=(name == wanted))
        for name, template in BUILTIN_FORMATS
    ]
    return normalize(formats)


def save(config_manager, formats) -> None:
    if config_manager is None:
        return
    payload = [{"name": f.name, "enabled": f.enabled} for f in normalize(formats)]
    config_manager.set_app_setting(SETTING_KEY, json.dumps(payload, ensure_ascii=False))
