# -*- coding: utf-8 -*-
"""
HookManager —— 统一的 monkey-patch 管理器
原理：
  不再让每个模块各自保存/恢复原始方法（形成 A→B→C 链），
  而是由统一的 HookManager 持有原始方法 + 一个回调列表。
  monkey-patch 只做一次（第一次 register 时），之后所有模块
  只是往列表里 append / remove 回调。

  调用链：
    被 patch 的方法 → HookManager 分发器 → 原始方法 → 回调1 → 回调2 → ...

  任何模块卸载时只需 unregister 自己的回调，不影响其他模块。
"""

from __future__ import annotations
from typing import Callable


class HookManager:
    """管理对某个对象方法的 monkey-patch，支持多个回调安全注册/注销。"""

    def __init__(self):
        # key: (obj_id, attr_name)
        # value: { 'original': 原始方法, 'callbacks': [回调列表], 'obj': obj, 'attr': attr }
        self._hooks: dict[tuple[int, str], dict] = {}

    def register(self, obj, attr: str, callback: Callable, *,
                 wrap_mode: str = "after") -> None:
        """
        注册一个回调到指定对象的方法上。

        Args:
            obj:        被 patch 的对象（如 mask_overlay, export_service）
            attr:       被 patch 的方法名（如 'paintEvent', 'export'）
            callback:   回调函数，签名需与 wrap_mode 匹配：
                        - "after":  callback(*args, **kwargs)  在原始方法之后调用
                        - "chain":  result = callback(result, *extra_args)  链式处理返回值
            wrap_mode:  "after" = 原始方法执行后依次调用回调（paintEvent 场景）
                        "chain" = 原始方法的返回值依次传给每个回调处理（export 场景）
        """
        key = (id(obj), attr)

        if key not in self._hooks:
            # 第一次注册：保存原始方法，安装分发器
            original = getattr(obj, attr)
            entry = {
                'original': original,
                'callbacks': [],
                'obj': obj,
                'attr': attr,
                'wrap_mode': wrap_mode,
            }
            self._hooks[key] = entry

            # 安装分发器
            if wrap_mode == "after":
                def dispatcher(*args, **kwargs):
                    entry['original'](*args, **kwargs)
                    for cb in list(entry['callbacks']):
                        cb(*args, **kwargs)
                setattr(obj, attr, dispatcher)

            elif wrap_mode == "chain":
                def dispatcher(*args, **kwargs):
                    result = entry['original'](*args, **kwargs)
                    for cb in list(entry['callbacks']):
                        result = cb(result, *args, **kwargs)
                    return result
                setattr(obj, attr, dispatcher)

        self._hooks[key]['callbacks'].append(callback)

    def unregister(self, obj, attr: str, callback: Callable) -> None:
        """注销一个回调。如果该方法上没有回调了，恢复原始方法。"""
        key = (id(obj), attr)
        entry = self._hooks.get(key)
        if not entry:
            return

        try:
            entry['callbacks'].remove(callback)
        except ValueError:
            pass

        # 没有回调了 → 恢复原始方法
        if not entry['callbacks']:
            setattr(entry['obj'], entry['attr'], entry['original'])
            del self._hooks[key]

    def unregister_all(self) -> None:
        """注销所有回调，恢复所有原始方法。"""
        for entry in self._hooks.values():
            setattr(entry['obj'], entry['attr'], entry['original'])
        self._hooks.clear()
 