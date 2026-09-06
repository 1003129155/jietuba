# -*- coding: utf-8 -*-
"""
测试共享 fixtures

提供 QApplication 实例等公共 fixture。
"""
import pytest
import sys
import os

# 确保 main/ 在 sys.path（从 tests/ 向上两级）
main_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if main_dir not in sys.path:
    sys.path.insert(0, main_dir)

# 项目根目录也加入（用于 Rust 库等）
project_root = os.path.dirname(main_dir)
if project_root not in sys.path:
    sys.path.insert(0, project_root)


@pytest.fixture(scope="session")
def qapp():
    """提供一个全局 QApplication 实例（整个测试会话复用）"""
    from PySide6.QtWidgets import QApplication
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture(scope="session", autouse=True)
def isolated_tool_settings(tmp_path_factory):
    """让全局工具设置单例落在临时文件上，而不是开发机真实的配置。

    工具设置里有些键会改变行为本身，而不只是外观——马赛克的 draw_mode 就决定了
    同一串鼠标事件画出的是自由涂抹还是框选。只要测试读的是真实配置，用过一次
    "框选"的机器上整个马赛克测试文件就会挂，而 CI 的干净环境却全绿：这种失败
    最贵，因为它把"环境不同"伪装成"代码坏了"。

    这里用的是 get_tool_settings_manager 已有的注入口子（qsettings 参数只在首次
    创建单例时生效），所以必须抢在任何测试触碰单例之前把它建出来。
    """
    from PySide6.QtCore import QSettings
    from settings import tool_settings

    settings_file = str(tmp_path_factory.mktemp("settings") / "tool_settings.ini")
    previous = tool_settings._tool_settings_manager
    tool_settings._tool_settings_manager = None
    tool_settings.get_tool_settings_manager(
        QSettings(settings_file, QSettings.Format.IniFormat)
    )
    yield
    tool_settings._tool_settings_manager = previous


@pytest.fixture
def tmp_settings(tmp_path):
    """提供一个临时的 QSettings，避免污染真正的配置"""
    from PySide6.QtCore import QSettings
    settings_file = str(tmp_path / "test_settings.ini")
    return QSettings(settings_file, QSettings.Format.IniFormat)
 