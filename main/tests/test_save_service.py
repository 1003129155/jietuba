# -*- coding: utf-8 -*-
"""
SaveService 路径生成逻辑单元测试

测试 _build_filename / _reserve_unique_path / _compose_path 等纯逻辑。
"""
import pytest
import os
from unittest.mock import MagicMock
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture
def save_service(qapp, tmp_path):
    """创建 SaveService，使用临时目录作为默认保存路径"""
    from core.save import SaveService
    mock_config = MagicMock()
    mock_config.get_screenshot_save_path.return_value = str(tmp_path)
    return SaveService(config_manager=mock_config)


class TestBuildFilename:
    """_build_filename 测试"""

    def test_basic_filename(self, save_service):
        """基本文件名格式"""
        filename = save_service._build_filename("截图", "", "PNG")
        assert filename.startswith("截图_")
        assert filename.endswith(".png")

    def test_with_suffix(self, save_service):
        """带后缀的文件名"""
        filename = save_service._build_filename("截图", "标注", "PNG")
        assert "截图" in filename
        assert "标注" in filename
        assert filename.endswith(".png")

    def test_format_lowercase(self, save_service):
        """扩展名应为小写"""
        filename = save_service._build_filename("test", "", "JPG")
        assert filename.endswith(".jpg")

    def test_empty_prefix(self, save_service):
        """空前缀"""
        filename = save_service._build_filename("", "", "PNG")
        assert filename.endswith(".png")
        assert len(filename) > 4  # 至少有时间戳


class TestReserveUniquePath:
    """_reserve_unique_path 测试"""

    def test_creates_file(self, save_service, tmp_path):
        """应在目标目录创建占位文件"""
        path = save_service._reserve_unique_path(str(tmp_path), "test", "", "png")
        assert os.path.exists(path)
        assert path.endswith(".png")

    def test_unique_names(self, save_service, tmp_path):
        """多次调用应生成不同文件名"""
        paths = set()
        for _ in range(5):
            path = save_service._reserve_unique_path(str(tmp_path), "test", "", "png")
            paths.add(path)
        assert len(paths) == 5

    def test_path_in_target_dir(self, save_service, tmp_path):
        """路径应在目标目录下"""
        path = save_service._reserve_unique_path(str(tmp_path), "img", "", "jpg")
        assert str(tmp_path) in path


class TestComposePath:
    """_compose_path 测试"""

    def test_default_directory(self, save_service, tmp_path):
        """无指定目录时使用默认目录"""
        path = save_service._compose_path(None, "截图", "", "PNG")
        assert str(tmp_path) in path

    def test_custom_directory(self, save_service, tmp_path):
        """指定自定义目录"""
        custom_dir = str(tmp_path / "custom")
        path = save_service._compose_path(custom_dir, "截图", "", "PNG")
        assert custom_dir in path
        assert os.path.isdir(custom_dir)  # 自动创建目录

    def test_creates_directory(self, save_service, tmp_path):
        """不存在的目录应被自动创建"""
        new_dir = str(tmp_path / "a" / "b" / "c")
        path = save_service._compose_path(new_dir, "test", "", "png")
        assert os.path.isdir(new_dir)


class TestGetDefaultDirectory:
    """get_default_directory 测试"""

    def test_returns_string(self, save_service):
        """应返回字符串"""
        result = save_service.get_default_directory()
        assert isinstance(result, str)
 