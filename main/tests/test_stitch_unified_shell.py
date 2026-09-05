# -*- coding: utf-8 -*-
"""
长截图拼接入口 jietuba_long_stitch_unified 的 Python 壳层测试

已有的 test_stitch_integration.py 顶部是 pytest.importorskip("longstitch")，
所以在没有装 Rust 扩展的机器（含 CI）上整个文件被跳过，
_stitch_with_hash_rust / stitch_images_auto 这两层的 Python 壳
——参数拼装、多图串联、失败与异常回退、方向返回值——一条也没被执行过。

本文件用 monkeypatch.setitem(sys.modules, "longstitch", 假模块) 注入一个
可控的替身，只验证 Python 这一侧的契约：传给 Rust 的关键字参数对不对、
拼接是否真的把上一轮结果接着往下拼、Rust 返回 None / 抛异常 / 扩展缺失时
是否按约定回退。真实的重叠匹配质量仍由 test_stitch_integration.py 在装了
扩展的机器上负责，两者互补而不重叠。
"""
import io
import sys

import pytest
from PIL import Image

import stitch.jietuba_long_stitch_unified as unified


# ============================================================================
# 辅助构造
# ============================================================================

def _solid(width: int, height: int, color=(10, 20, 30)) -> Image.Image:
    """一张纯色图，内容不重要——本文件不验证匹配质量，只验证参数与流程"""
    return Image.new("RGB", (width, height), color)


def _png_bytes(image: Image.Image) -> bytes:
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


class _FakeLongStitch:
    """
    冒充 Rust 扩展模块。

    生产代码是 `import longstitch` 后取模块属性调用，所以一个普通对象
    塞进 sys.modules 就能顶替，不需要真的构造 ModuleType。
    """

    def __init__(self):
        self.smart_calls = []
        self.auto_calls = []
        self.auto_debug_calls = []
        # 默认成功：返回一张尺寸与输入不同的图，方便断言"结果被解码并串联"
        self.smart_result = _png_bytes(_solid(8, 30))
        self.smart_error = None
        self.auto_result = (_png_bytes(_solid(8, 40)), "forward")
        self.auto_error = None

    def stitch_two_images_rust_smart(self, bytes1, bytes2, **kwargs):
        self.smart_calls.append((bytes1, bytes2, kwargs))
        if self.smart_error is not None:
            raise self.smart_error
        return self.smart_result

    def stitch_two_images_rust_smart_auto(self, bytes1, bytes2, **kwargs):
        self.auto_calls.append((bytes1, bytes2, kwargs))
        if self.auto_error is not None:
            raise self.auto_error
        return self.auto_result

    def stitch_two_images_rust_smart_auto_debug(self, bytes1, bytes2, **kwargs):
        self.auto_debug_calls.append((bytes1, bytes2, kwargs))
        if self.auto_error is not None:
            raise self.auto_error
        return self.auto_result


@pytest.fixture
def fake_rust(monkeypatch):
    fake = _FakeLongStitch()
    monkeypatch.setitem(sys.modules, "longstitch", fake)
    return fake


@pytest.fixture
def no_rust(monkeypatch):
    """
    sys.modules 里放 None 会让 `import longstitch` 抛 ImportError，
    这是模拟"扩展没装"最贴近真实的方式。
    """
    monkeypatch.setitem(sys.modules, "longstitch", None)


@pytest.fixture
def reset_config():
    """拼接配置是模块级单例，逐个用例保存并还原，避免用例之间互相污染"""
    saved = (
        unified.config.engine,
        unified.config.verbose,
        unified.config.ignore_right_pixels,
        unified.config.ignore_top_pixels,
    )
    yield unified.config
    (
        unified.config.engine,
        unified.config.verbose,
        unified.config.ignore_right_pixels,
        unified.config.ignore_top_pixels,
    ) = saved


# ============================================================================
# 入口守卫
# ============================================================================

class TestStitchImagesGuards:

    def test_empty_or_none_input_returns_none(self):
        for empty in ([], (), None):
            assert unified.stitch_images(empty) is None

    def test_single_image_is_returned_unchanged(self):
        only = _solid(4, 4)
        assert unified.stitch_images([only]) is only

    def test_single_image_never_reaches_rust(self, fake_rust):
        unified.stitch_images([_solid(4, 4)])
        assert fake_rust.smart_calls == []


# ============================================================================
# 多图串联与失败回退
# ============================================================================

class TestStitchWithFakeRust:

    def test_two_images_produce_one_call_and_a_decoded_image(self, fake_rust):
        result = unified.stitch_images([_solid(8, 10), _solid(8, 12)])
        assert len(fake_rust.smart_calls) == 1
        assert isinstance(result, Image.Image)
        # 返回的是 Rust 给的 PNG 解码结果，而不是任一输入图
        assert result.size == (8, 30)

    def test_three_images_chain_the_intermediate_result(self, fake_rust):
        result = unified.stitch_images([_solid(8, 10), _solid(8, 12), _solid(8, 14)])
        assert len(fake_rust.smart_calls) == 2
        # 第二次拼接的左图必须是第一次的产物（8x30），不是原始的 8x10
        second_left = Image.open(io.BytesIO(fake_rust.smart_calls[1][0]))
        assert second_left.size == (8, 30)
        assert result.size == (8, 30)

    def test_rust_returning_none_aborts_immediately(self, fake_rust):
        fake_rust.smart_result = None
        result = unified.stitch_images([_solid(8, 10), _solid(8, 12), _solid(8, 14)])
        assert result is None
        # 第一次就失败，不应该继续尝试后面的图
        assert len(fake_rust.smart_calls) == 1

    def test_rust_exception_is_swallowed_and_yields_none(self, fake_rust):
        fake_rust.smart_error = RuntimeError("rust 内部炸了")
        assert unified.stitch_images([_solid(8, 10), _solid(8, 12)]) is None

    def test_missing_extension_yields_none(self, no_rust):
        assert unified.stitch_images([_solid(8, 10), _solid(8, 12)]) is None


# ============================================================================
# 传给 Rust 的参数
# ============================================================================

class TestRustCallArguments:

    def test_ignore_pixels_from_config_are_forwarded(self, fake_rust, reset_config):
        reset_config.ignore_right_pixels = 7
        reset_config.ignore_top_pixels = 3
        unified.stitch_images([_solid(8, 10), _solid(8, 12)])
        kwargs = fake_rust.smart_calls[0][2]
        assert kwargs["ignore_right_pixels"] == 7
        assert kwargs["ignore_top_pixels"] == 3

    def test_zero_right_pixels_becomes_none_but_zero_top_pixels_stays(
            self, fake_rust, reset_config):
        """
        生产代码对两个参数的写法不对称：ignore_right_pixels 用了 `or None`，
        ignore_top_pixels 直接传。这条用例把该差异钉住，避免以后无意改动。
        """
        reset_config.ignore_right_pixels = 0
        reset_config.ignore_top_pixels = 0
        unified.stitch_images([_solid(8, 10), _solid(8, 12)])
        kwargs = fake_rust.smart_calls[0][2]
        assert kwargs["ignore_right_pixels"] is None
        assert kwargs["ignore_top_pixels"] == 0

    def test_zero_img1_ratios_are_sent_as_none(self, fake_rust):
        unified.stitch_images([_solid(8, 10), _solid(8, 12)])
        kwargs = fake_rust.smart_calls[0][2]
        assert kwargs["ignore_img1_top_ratio"] is None
        assert kwargs["ignore_img1_bottom_ratio"] is None

    def test_nonzero_img1_ratios_are_forwarded(self, fake_rust):
        unified.stitch_images(
            [_solid(8, 10), _solid(8, 12)],
            ignore_img1_top_ratio=0.25,
            ignore_img1_bottom_ratio=0.5,
        )
        kwargs = fake_rust.smart_calls[0][2]
        assert kwargs["ignore_img1_top_ratio"] == 0.25
        assert kwargs["ignore_img1_bottom_ratio"] == 0.5


# ============================================================================
# 配置
# ============================================================================

class TestConfigure:

    def test_engine_is_always_normalised_to_hash_rust(self, reset_config):
        for value in ("opencv", "hash", "", 123, object()):
            assert unified.normalize_engine_value(value) == "hash_rust"
            unified.configure(engine=value)
            assert reset_config.engine == "hash_rust"

    def test_none_arguments_leave_existing_values_untouched(self, reset_config):
        reset_config.verbose = True
        reset_config.ignore_right_pixels = 11
        reset_config.ignore_top_pixels = 22
        unified.configure()
        assert reset_config.verbose is True
        assert reset_config.ignore_right_pixels == 11
        assert reset_config.ignore_top_pixels == 22

    def test_unknown_keyword_arguments_are_tolerated(self, reset_config):
        unified.configure(verbose=True, some_future_option=1)
        assert reset_config.verbose is True

    def test_verbose_path_still_returns_the_image(self, fake_rust, reset_config):
        """verbose 分支会走一段额外的日志代码，确认它不会把结果吃掉"""
        reset_config.verbose = True
        result = unified.stitch_images([_solid(8, 10), _solid(8, 12)])
        assert isinstance(result, Image.Image)


# ============================================================================
# 自动方向检测
# ============================================================================

class TestStitchImagesAuto:

    def test_direction_from_rust_is_passed_through(self, fake_rust):
        for direction in ("forward", "reverse"):
            fake_rust.auto_result = (_png_bytes(_solid(8, 40)), direction)
            result, got = unified.stitch_images_auto(_solid(8, 10), _solid(8, 12))
            assert got == direction
            assert isinstance(result, Image.Image)
            assert result.size == (8, 40)

    def test_debug_flag_selects_the_debug_entry_point(self, fake_rust):
        unified.stitch_images_auto(_solid(8, 10), _solid(8, 12), debug=True)
        assert len(fake_rust.auto_debug_calls) == 1
        assert fake_rust.auto_calls == []

    def test_non_debug_uses_the_plain_entry_point(self, fake_rust):
        unified.stitch_images_auto(_solid(8, 10), _solid(8, 12))
        assert len(fake_rust.auto_calls) == 1
        assert fake_rust.auto_debug_calls == []

    def test_none_from_rust_falls_back_to_forward(self, fake_rust):
        fake_rust.auto_result = None
        assert unified.stitch_images_auto(_solid(8, 10), _solid(8, 12)) == (None, "forward")

    def test_exception_falls_back_to_forward(self, fake_rust):
        fake_rust.auto_error = RuntimeError("rust 内部炸了")
        assert unified.stitch_images_auto(_solid(8, 10), _solid(8, 12)) == (None, "forward")

    def test_missing_extension_falls_back_to_forward(self, no_rust):
        assert unified.stitch_images_auto(_solid(8, 10), _solid(8, 12)) == (None, "forward")

    def test_config_ignore_pixels_are_forwarded(self, fake_rust, reset_config):
        reset_config.ignore_right_pixels = 5
        reset_config.ignore_top_pixels = 9
        unified.stitch_images_auto(_solid(8, 10), _solid(8, 12))
        kwargs = fake_rust.auto_calls[0][2]
        assert kwargs["ignore_right_pixels"] == 5
        assert kwargs["ignore_top_pixels"] == 9
