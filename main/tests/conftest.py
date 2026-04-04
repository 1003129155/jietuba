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


@pytest.fixture
def tmp_settings(tmp_path):
    """提供一个临时的 QSettings，避免污染真正的配置"""
    from PySide6.QtCore import QSettings
    settings_file = str(tmp_path / "test_settings.ini")
    return QSettings(settings_file, QSettings.Format.IniFormat)
 