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

生命周期：
  登记表用弱引用持有被 patch 的对象，调用方漏掉 unregister 也不会让窗口泄漏。
"""

from __future__ import annotations
import inspect
import weakref
from typing import Callable


def _weak_callable(obj, original: Callable) -> Callable:
    """把原始方法包装成不持有 obj 强引用的调用器。

    绑定方法内部持有 obj。若把它直接存进登记表的值里，
    WeakKeyDictionary 的值就会反向引用自己的键，弱引用退化成强引用，
    对象照样无法回收——弱引用表最容易踩的坑就在这里。
    非绑定方法（普通函数、可调用对象）本来就不引用 obj，原样返回即可。
    """
    if inspect.ismethod(original) and original.__self__ is obj:
        ref = weakref.WeakMethod(original)

        def call(*args, **kwargs):
            method = ref()
            if method is None:      # 对象已销毁，分发器不该再被调用
                return None
            return method(*args, **kwargs)

        return call
    return original


class HookManager:
    """管理对某个对象方法的 monkey-patch，支持多个回调安全注册/注销。"""

    def __init__(self):
        # obj → { attr_name: { 'call': 原方法调用器, 'own_attr': bool, 'callbacks': [...] } }
        #
        # 用弱引用表而不是以 id(obj) 为键的普通字典：后者必须同时持有 obj 的强引用
        # 才能保证 id 不失效，于是只要调用方漏掉一次 unregister，被 patch 的窗口
        # 就永远不会被回收。弱引用表让对象该走就走，表项随之自动消失。
        self._hooks: "weakref.WeakKeyDictionary[object, dict]" = weakref.WeakKeyDictionary()

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
        attrs = self._hooks.get(obj)
        if attrs is None:
            attrs = {}
            self._hooks[obj] = attrs

        entry = attrs.get(attr)
        if entry is None:
            # 第一次注册：保存原始方法，安装分发器
            entry = {
                'call': _weak_callable(obj, getattr(obj, attr)),
                # 记录原本是否为实例属性：若不是（通常是类方法），恢复时应当
                # delattr 让查找回落到类上，否则会在实例上留下一个指向自身的
                # 绑定方法，凭空造出一个引用环。
                'own_attr': attr in getattr(obj, '__dict__', {}),
                'callbacks': [],
                'wrap_mode': wrap_mode,
            }
            attrs[attr] = entry

            # 安装分发器（闭包只捕获 entry，不持有 obj 的强引用）
            if wrap_mode == "after":
                def dispatcher(*args, **kwargs):
                    entry['call'](*args, **kwargs)
                    for cb in list(entry['callbacks']):
                        cb(*args, **kwargs)
                setattr(obj, attr, dispatcher)

            elif wrap_mode == "chain":
                def dispatcher(*args, **kwargs):
                    result = entry['call'](*args, **kwargs)
                    for cb in list(entry['callbacks']):
                        result = cb(result, *args, **kwargs)
                    return result
                setattr(obj, attr, dispatcher)

        entry['callbacks'].append(callback)

    def unregister(self, obj, attr: str, callback: Callable) -> None:
        """注销一个回调。如果该方法上没有回调了，恢复原始方法。"""
        attrs = self._hooks.get(obj)
        if not attrs:
            return
        entry = attrs.get(attr)
        if not entry:
            return

        try:
            entry['callbacks'].remove(callback)
        except ValueError:
            pass

        # 没有回调了 → 恢复原始方法
        if not entry['callbacks']:
            self._restore(obj, attr, entry)
            attrs.pop(attr, None)
            if not attrs:
                self._hooks.pop(obj, None)

    def unregister_all(self) -> None:
        """注销所有回调，恢复所有原始方法。"""
        for obj, attrs in list(self._hooks.items()):
            for attr, entry in list(attrs.items()):
                self._restore(obj, attr, entry)
        self._hooks.clear()

    @staticmethod
    def _restore(obj, attr: str, entry: dict) -> None:
        """把被 patch 的方法恢复成原样。"""
        try:
            if entry['own_attr']:
                setattr(obj, attr, entry['call'])
            else:
                # 原本是类方法：删掉实例属性，让查找重新回落到类上
                delattr(obj, attr)
        except Exception:
            # 对象可能已在析构中；退回到直接赋值，仍失败就放弃
            try:
                setattr(obj, attr, entry['call'])
            except Exception:
                pass
