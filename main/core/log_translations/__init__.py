"""
日志中英翻译表

按源码目录拆分成多个子模块（每个子模块对应一个顶层包，如 canvas/、pin/），
分开维护是为了避免所有人都改同一个大字典文件。这里统一合并成一个
TRANSLATIONS 字典供 core.logger.LogMsg.render() 查询。

新增翻译时，把条目加到对应目录的子模块里即可，key 是源码里 T() 调用的
中文模板原文（含 {name} 占位符），value 是对应的英文模板。
"""
from . import (
    canvas,
    capture,
    clipboard,
    core_pkg,
    gif,
    main_app,
    ocr,
    pin,
    settings,
    stitch,
    tools,
    translation,
    ui,
)

_MODULES = (
    canvas,
    capture,
    clipboard,
    core_pkg,
    gif,
    main_app,
    ocr,
    pin,
    settings,
    stitch,
    tools,
    translation,
    ui,
)

TRANSLATIONS: dict[str, str] = {}
for _module in _MODULES:
    TRANSLATIONS.update(_module.TRANSLATIONS)
