# -*- coding: utf-8 -*-
"""设置的导出与导入，范围是设置界面上的选项。

导出：刷新一遍设置界面，记下读过哪些配置键，把这些键的当前值写成 JSON。新加的
设置项只要界面会读它，就自动在导出范围内，不用另外登记。

导入：用 preview_config 把导入后的样子填进设置界面，和手动修改一样，点应用才按
设置窗口原有的保存流程写入并生效。
"""
import json
from contextlib import contextmanager

FILE_TAG = "jietuba-settings"
FORMAT_VERSION = 1

# 只对本机有意义的键，导出时不写、导入时不碰
LOCAL_KEYS = frozenset({
    "app/has_run_before",
    "app/welcome_wizard_done",
    # 自启实际由系统 Run 项决定，只改这个值会和它对不上
    "app/autostart_enabled",
    # 绝对路径换台电脑多半不存在
    "app/screenshot_save_path",
    "app/log_dir",
    "clipboard/db_path",
})


class SettingsFileError(ValueError):
    """文件不是可导入的设置文件。"""


class _ReadRecorder:
    def __init__(self, inner):
        self.inner = inner
        self.keys = set()

    def value(self, key, *args, **kwargs):
        self.keys.add(key)
        return self.inner.value(key, *args, **kwargs)

    def contains(self, key):
        self.keys.add(key)
        return self.inner.contains(key)

    def __getattr__(self, name):
        return getattr(self.inner, name)


@contextmanager
def recording_reads(config):
    """期间经 config 读过的配置键都记进产出的集合。"""
    recorder = _ReadRecorder(config.qsettings)
    config.qsettings = recorder
    try:
        yield recorder.keys
    finally:
        config.qsettings = recorder.inner


def _plain(value):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, (list, tuple)):
        return [_plain(item) for item in value]
    return str(value)


def export_settings(config, keys, path: str, app_version: str = "") -> int:
    """把 keys 里保存过的项写到 path，返回写出的项数。没保存过的项就是默认值，不写。"""
    qsettings = config.qsettings
    qsettings.sync()
    values = {
        key: _plain(qsettings.value(key))
        for key in keys
        if key not in LOCAL_KEYS and qsettings.contains(key)
    }

    payload = {
        "format": FILE_TAG,
        "version": FORMAT_VERSION,
        "app_version": app_version,
        "settings": dict(sorted(values.items())),
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
    return len(values)


def read_settings_file(path: str) -> dict:
    """读取并校验设置文件，返回要导入的键值。任何问题都抛 SettingsFileError。"""
    try:
        with open(path, encoding="utf-8-sig") as f:
            payload = json.load(f)
    except (OSError, ValueError) as e:
        raise SettingsFileError(str(e)) from e

    if (not isinstance(payload, dict) or payload.get("format") != FILE_TAG
            or not isinstance(payload.get("settings"), dict)):
        raise SettingsFileError("not a Jietuba settings file")
    version = payload.get("version")
    if not isinstance(version, int) or version > FORMAT_VERSION:
        raise SettingsFileError(f"unsupported format version: {version!r}")

    return {
        key: value
        for key, value in payload["settings"].items()
        if key and key not in LOCAL_KEYS
        and isinstance(value, (str, int, float, bool, list))
    }


def preview_config(config, values: dict, ini_path: str):
    """返回一个内容等于「导入之后」的临时配置，只供读取。

    文件里没有的项取默认值；本机相关的项沿用 config 里的值。
    """
    from PySide6.QtCore import QSettings
    from settings.tool_settings import ToolSettingsManager

    preview = QSettings(ini_path, QSettings.Format.IniFormat)
    preview.clear()
    for key in LOCAL_KEYS:
        if config.qsettings.contains(key):
            preview.setValue(key, config.qsettings.value(key))
    for key, value in values.items():
        preview.setValue(key, value)
    return ToolSettingsManager(qsettings=preview)
