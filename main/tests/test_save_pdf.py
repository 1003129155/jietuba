# -*- coding: utf-8 -*-
"""
SaveService PDF 保存功能测试

测试 77eee29..HEAD 范围内新增的 PDF 输出功能：
- save_qimage_to_path (PDF 格式)
- _normalize_format
- _flatten_for_pdf
- _save_qimage_to_pdf_path
- _save_pil_to_path (PDF 格式)
"""
import os
import pytest
from unittest.mock import MagicMock, patch
from PySide6.QtGui import QImage, QPainter
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture
def save_service(qapp, tmp_path):
    from core.save import SaveService
    mock_config = MagicMock()
    mock_config.get_screenshot_save_path.return_value = str(tmp_path)
    return SaveService(config_manager=mock_config)


# ============================================================================
# _normalize_format 测试
# ============================================================================

class TestNormalizeFormat:
    """_normalize_format 测试"""

    def test_standard_formats(self, save_service):
        assert save_service._normalize_format("PNG") == "PNG"
        assert save_service._normalize_format("jpg") == "JPG"
        assert save_service._normalize_format("BMP") == "BMP"
        assert save_service._normalize_format("webp") == "WEBP"
        assert save_service._normalize_format("pdf") == "PDF"

    def test_jpeg_maps_to_jpg(self, save_service):
        assert save_service._normalize_format("JPEG") == "JPG"
        assert save_service._normalize_format("jpeg") == "JPG"

    def test_dot_prefix_stripped(self, save_service):
        assert save_service._normalize_format(".png") == "PNG"
        assert save_service._normalize_format(".PDF") == "PDF"

    def test_whitespace_stripped(self, save_service):
        assert save_service._normalize_format("  PNG  ") == "PNG"

    def test_none_defaults_to_png(self, save_service):
        assert save_service._normalize_format(None) == "PNG"

    def test_empty_defaults_to_png(self, save_service):
        assert save_service._normalize_format("") == "PNG"


# ============================================================================
# _flatten_for_pdf 测试
# ============================================================================

class TestFlattenForPDF:
    """_flatten_for_pdf 测试"""

    def test_argb32_with_alpha_flattened(self, save_service):
        image = QImage(10, 10, QImage.Format.Format_ARGB32)
        image.fill(0x80FF0000)  # 半透明红色

        result = save_service._flatten_for_pdf(image)
        assert result.format() == QImage.Format.Format_RGB32
        assert result.size() == image.size()

    def test_rgb32_no_alpha_unchanged(self, save_service):
        image = QImage(10, 10, QImage.Format.Format_RGB32)
        image.fill(0xFFFF0000)

        result = save_service._flatten_for_pdf(image)
        # RGB32 无 alpha 通道，直接转换
        assert result.format() == QImage.Format.Format_RGB32

    def test_white_background_for_alpha(self, save_service):
        image = QImage(10, 10, QImage.Format.Format_ARGB32)
        image.fill(0x00000000)  # 全透明

        result = save_service._flatten_for_pdf(image)
        # 应填充白色背景
        pixel = result.pixelColor(5, 5)
        assert pixel.red() == 255
        assert pixel.green() == 255
        assert pixel.blue() == 255


# ============================================================================
# save_qimage_to_path PDF 测试
# ============================================================================

class TestSaveQImageToPathPDF:
    """save_qimage_to_path PDF 输出测试"""

    def test_saves_pdf_file(self, save_service, tmp_path):
        image = QImage(40, 30, QImage.Format.Format_ARGB32)
        image.fill(0xFF336699)
        path = os.path.join(str(tmp_path), "test_output.pdf")

        success = save_service.save_qimage_to_path(
            image, path, image_format="PDF", pdf_dpi=150
        )

        assert success is True
        assert os.path.exists(path)
        assert os.path.getsize(path) > 0

    def test_pdf_low_dpi_still_writes(self, save_service, tmp_path):
        image = QImage(20, 15, QImage.Format.Format_RGB32)
        image.fill(0xFFFF0000)
        path = os.path.join(str(tmp_path), "low_dpi.pdf")

        success = save_service.save_qimage_to_path(
            image, path, image_format="PDF", pdf_dpi=72
        )

        assert success is True
        assert os.path.getsize(path) > 0

    def test_dpi_below_72_clamped(self, save_service, tmp_path):
        image = QImage(10, 10, QImage.Format.Format_RGB32)
        image.fill(0xFF00FF00)
        path = os.path.join(str(tmp_path), "clamped_dpi.pdf")

        # dpi=10 会被 clamp 到 72
        success = save_service.save_qimage_to_path(
            image, path, image_format="PDF", pdf_dpi=10
        )
        assert success is True

    def test_null_image_returns_false(self, save_service, tmp_path):
        path = os.path.join(str(tmp_path), "null.pdf")
        success = save_service.save_qimage_to_path(
            QImage(), path, image_format="PDF"
        )
        assert success is False

    def test_format_detected_from_extension(self, save_service, tmp_path):
        image = QImage(20, 15, QImage.Format.Format_ARGB32)
        image.fill(0xFFFF0000)
        path = os.path.join(str(tmp_path), "auto.pdf")

        # image_format=None，从扩展名检测
        success = save_service.save_qimage_to_path(image, path)
        assert success is True
        assert os.path.exists(path)


# ============================================================================
# save_qimage PDF 测试
# ============================================================================

class TestSaveQImagePDF:
    """save_qimage 同步接口 PDF 测试"""

    def test_sync_save_pdf(self, save_service, tmp_path):
        image = QImage(16, 12, QImage.Format.Format_ARGB32)
        image.fill(0xFF663399)

        success, path = save_service.save_qimage(
            image,
            directory=str(tmp_path),
            prefix="pdf_test",
            image_format="PDF",
        )

        assert success is True
        assert path is not None
        assert path.endswith(".pdf")
        assert os.path.exists(path)

    def test_sync_save_pdf_with_suffix(self, save_service, tmp_path):
        image = QImage(20, 15, QImage.Format.Format_ARGB32)
        image.fill(0xFF996633)

        success, path = save_service.save_qimage(
            image,
            directory=str(tmp_path),
            prefix="img",
            suffix="v2",
            image_format="PDF",
        )

        assert success is True
        assert "img" in path
        assert "v2" in path


# ============================================================================
# _build_filename PDF 测试
# ============================================================================

class TestBuildFilenamePDF:
    """_build_filename PDF 格式测试"""

    def test_pdf_extension(self, save_service):
        filename = save_service._build_filename("截图", "", "PDF")
        assert filename.endswith(".pdf")

    def test_jpg_extension(self, save_service):
        filename = save_service._build_filename("test", "", "JPG")
        assert filename.endswith(".jpg")

    def test_jpeg_maps_to_jpg(self, save_service):
        filename = save_service._build_filename("test", "", "JPEG")
        assert filename.endswith(".jpg")


# ============================================================================
# _compose_path PDF 测试
# ============================================================================

class TestComposePathPDF:
    """_compose_path PDF 格式测试"""

    def test_pdf_path_extension(self, save_service, tmp_path):
        path = save_service._compose_path(str(tmp_path), "pdf", "", "PDF")
        assert path.endswith(".pdf")
