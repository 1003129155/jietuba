# -*- coding: utf-8 -*-
"""翻译服务商设置项的读写：把 Field 声明接到配置管理器和控件上。

只管两件事——「字段 ↔ 配置」和「字段 ↔ 控件取值」。**不管布局。**

分界线划在这里是因为两个页面的视觉本来就该不同：设置页是分组卡片、欢迎页是
QStackedWidget 分页。共享布局会做出一个谁都不好用的万能组件；而「有哪些字段、
怎么存取」这件事两边完全一样，各写一份就是在等着漏。

漏的代价不是崩溃而是静默：以前设置页手写保存，加百度时漏了两行，界面照常显示、
照常收输入、点保存什么也没存，用户只看到「未配置」。这个模块存在就是为了让那种
漏法在结构上不可能发生——界面不认服务商名字，只认注册表里的声明。
"""

from translation.provider import ToggleField

from core.logger import log_exception, T


def read_config(config, field):
    """从配置管理器读一个字段的值。读不到时返回该类型的空值。"""
    getter = getattr(config, "get_" + field.config_key, None)
    if getter is None:
        return False if isinstance(field, ToggleField) else ""
    try:
        value = getter()
    except Exception as e:
        log_exception(e, T("读取翻译设置 {config_key}",
                           config_key=field.config_key))
        return False if isinstance(field, ToggleField) else ""
    if isinstance(field, ToggleField):
        return bool(value)
    return value or ""


def write_config(config, field, value):
    """把一个字段的值写回配置管理器。没有对应 setter 时静默跳过。"""
    setter = getattr(config, "set_" + field.config_key, None)
    if setter is None:
        return
    try:
        setter(value)
    except Exception as e:
        log_exception(e, T("保存翻译设置 {config_key}",
                           config_key=field.config_key))


def widget_value(field, widget):
    """从控件取值，类型由 Field 决定。"""
    if isinstance(field, ToggleField):
        return bool(widget.isChecked())
    # 文本一律去掉首尾空白：凭据里混进空格是高频事故，且肉眼看不出来
    return widget.text().strip()


def apply_widget_value(field, widget, value):
    """把值写进控件。"""
    if isinstance(field, ToggleField):
        widget.setChecked(bool(value))
    else:
        widget.setText(str(value or ""))


def load_into(config, fields, widgets):
    """配置 -> 控件。widgets 是 {config_key: 控件}，缺的键跳过。"""
    for f in fields:
        widget = widgets.get(f.config_key)
        if widget is not None:
            apply_widget_value(f, widget, read_config(config, f))


def save_from(config, fields, widgets):
    """控件 -> 配置。widgets 是 {config_key: 控件}，缺的键跳过。"""
    for f in fields:
        widget = widgets.get(f.config_key)
        if widget is not None:
            write_config(config, f, widget_value(f, widget))


def reset_to_defaults(defaults, fields, widgets):
    """把控件恢复成 APP_DEFAULT_SETTINGS 里的默认值。

    defaults 里没有这个键时跳过，而不是清空：清空会把用户填好的凭据抹掉，
    而「默认值表漏登记一个键」是比「恢复默认少恢复一项」严重得多的事故。
    """
    for f in fields:
        widget = widgets.get(f.config_key)
        if widget is None or f.config_key not in defaults:
            continue
        apply_widget_value(f, widget, defaults[f.config_key])


def provider_fields(registry, provider_id):
    """某一家的全部设置项（凭据 + 独有选项）。"""
    meta = registry.metadata(provider_id)
    return tuple(meta.credentials) + tuple(meta.options)


def all_fields(registry):
    """所有已注册服务商的全部设置项，按注册顺序。

    保存和恢复默认都遍历这个，所以新增一家不需要改任何界面代码。
    """
    fields = []
    for meta in registry.available_providers():
        fields.extend(meta.credentials)
        fields.extend(meta.options)
    return tuple(fields)
