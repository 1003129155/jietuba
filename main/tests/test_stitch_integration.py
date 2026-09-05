# -*- coding: utf-8 -*-
"""
长截图拼接统一接口的集成测试

已有的 test_stitch_algorithm.py 只覆盖了纯 Python 的子串匹配算法，
本文件补上 jietuba_long_stitch_unified 这一层——它是滚动截图真正调用的入口，
内部会把 PIL Image 编码成 PNG 交给 Rust 的 longstitch 做重叠匹配。

用构造出来的、带已知重叠区域的合成图片走真实的 Rust 链路，
验证拼接高度、方向判定与失败路径，而不是 mock 掉 Rust 只测 Python 壳子。
"""
import pytest
from PIL import Image

longstitch = pytest.importorskip(
    "longstitch", reason="需要安装自制的 longstitch Rust 扩展包"
)


# ============================================================================
# 合成测试图片
# ============================================================================

WIDTH = 120


def _row_color(row_id: int):
    """由行号生成一个稳定且彼此差异明显的颜色，保证重叠匹配有足够特征"""
    return ((row_id * 37) % 256, (row_id * 91 + 40) % 256, (row_id * 13 + 90) % 256)


def _strip(start_row: int, rows: int, row_h: int = 4) -> Image.Image:
    """
    生成一张"内容条带"图片。

    第 i 条横带的颜色由它的全局行号决定，因此两张图只要行号区间有交集，
    像素内容就真的重合，Rust 匹配器可以找到重叠。

    注意 _row_color 对行号是模 256 周期的，构造"不重叠"的用例时不能靠
    把行号拉远来实现（行号相差 256 的倍数会撞色），要用 _noise 那种
    结构上就不同的图。
    """
    img = Image.new("RGB", (WIDTH, rows * row_h))
    for i in range(rows):
        row_id = start_row + i
        for y in range(i * row_h, (i + 1) * row_h):
            for x in range(WIDTH):
                img.putpixel((x, y), _row_color(row_id))
    return img


def _noise(rows: int, row_h: int = 4, seed: int = 12345) -> Image.Image:
    """
    生成一张逐像素随机的图片，用于构造"和条带图毫无重叠"的场景。

    条带图每行像素恒定，噪声图每行像素都在变，行内容不可能相等，
    因此匹配器找不到任何重叠。
    """
    import random
    rnd = random.Random(seed)
    img = Image.new("RGB", (WIDTH, rows * row_h))
    for y in range(rows * row_h):
        for x in range(WIDTH):
            img.putpixel((x, y), (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
    return img


@pytest.fixture
def reset_config():
    """每个用例跑在干净的全局配置上（模块级 config 是单例）"""
    from stitch import jietuba_long_stitch_unified as u
    old = (u.config.engine, u.config.verbose,
           u.config.ignore_right_pixels, u.config.ignore_top_pixels)
    u.config.engine = "hash_rust"
    u.config.verbose = False
    u.config.ignore_right_pixels = 0
    u.config.ignore_top_pixels = 0
    yield u
    (u.config.engine, u.config.verbose,
     u.config.ignore_right_pixels, u.config.ignore_top_pixels) = old


# ============================================================================
# 配置
# ============================================================================

class TestConfigure:
    """配置入口"""

    def test_engine_value_is_always_normalized_to_rust(self, reset_config):
        u = reset_config
        assert u.normalize_engine_value("anything") == "hash_rust"
        assert u.normalize_engine_value(None) == "hash_rust"

    def test_configure_updates_only_given_fields(self, reset_config):
        u = reset_config
        u.configure(ignore_top_pixels=12)
        assert u.config.ignore_top_pixels == 12
        assert u.config.ignore_right_pixels == 0      # 未传入的字段保持原值

        u.configure(ignore_right_pixels=8)
        assert u.config.ignore_top_pixels == 12       # 上一次的设置不被覆盖
        assert u.config.ignore_right_pixels == 8

    def test_configure_tolerates_unknown_kwargs(self, reset_config):
        """调用方传了老参数名也不应炸掉滚动截图流程"""
        u = reset_config
        u.configure(some_removed_option=True)
        assert u.config.engine == "hash_rust"


# ============================================================================
# 入口边界
# ============================================================================

class TestStitchImagesGuards:
    """stitch_images 的边界输入"""

    def test_empty_list_returns_none(self, reset_config):
        assert reset_config.stitch_images([]) is None

    def test_none_returns_none(self, reset_config):
        assert reset_config.stitch_images(None) is None

    def test_single_image_is_passed_through_untouched(self, reset_config):
        img = _strip(0, 5)
        assert reset_config.stitch_images([img]) is img


# ============================================================================
# 真实拼接（走 Rust）
# ============================================================================

class TestStitchWithRust:
    """经由 Rust longstitch 的真实拼接"""

    def test_two_overlapping_strips_merge_without_duplicating_overlap(self, reset_config):
        """
        img1 = 行 0..19，img2 = 行 12..31（重叠 8 行）
        正确拼接后总行数应是 32 行，而不是 40 行——重叠部分不能被重复堆叠。
        """
        img1 = _strip(0, 20)
        img2 = _strip(12, 20)

        out = reset_config.stitch_images([img1, img2])

        assert out is not None, "构造了明确重叠的两张图，拼接不应失败"
        assert out.width == WIDTH
        assert out.height == 32 * 4

    def test_three_strips_chain_in_order(self, reset_config):
        """连续拼接三张：0..19 / 12..31 / 24..43，最终应为 44 行"""
        imgs = [_strip(0, 20), _strip(12, 20), _strip(24, 20)]

        out = reset_config.stitch_images(imgs)

        assert out is not None
        assert out.height == 44 * 4

    def test_identical_images_collapse_into_one(self, reset_config):
        """完全相同的两张图重叠 100%，结果高度应保持不变"""
        img = _strip(0, 20)

        out = reset_config.stitch_images([img, img.copy()])

        assert out is not None
        assert out.height == img.height

    def test_stitched_content_preserves_first_and_last_rows(self, reset_config):
        """拼接结果的首尾内容应分别来自 img1 的开头和 img2 的结尾"""
        img1 = _strip(0, 20)
        img2 = _strip(12, 20)

        out = reset_config.stitch_images([img1, img2]).convert("RGB")

        assert out.getpixel((0, 0)) == _row_color(0)
        assert out.getpixel((0, out.height - 1)) == _row_color(31)

    def test_non_overlapping_images_report_failure(self, reset_config):
        """
        两张毫无重叠的图应返回 None，让滚动截图流程知道匹配失败，
        而不是硬拼出一张错误的长图。
        """
        img1 = _strip(0, 20)
        img2 = _noise(20)

        assert reset_config.stitch_images([img1, img2]) is None


class TestAutoDirection:
    """首次拼接的方向自动检测"""

    def test_detects_forward_scrolling(self, reset_config):
        """img2 在 img1 下方（向下滚动）应判为 forward"""
        img1 = _strip(0, 20)
        img2 = _strip(12, 20)

        out, direction = reset_config.stitch_images_auto(img1, img2)

        assert out is not None
        assert direction == "forward"
        assert out.height == 32 * 4

    def test_detects_reverse_scrolling(self, reset_config):
        """img2 在 img1 上方（向上滚动）应判为 reverse"""
        img1 = _strip(12, 20)
        img2 = _strip(0, 20)

        out, direction = reset_config.stitch_images_auto(img1, img2)

        assert out is not None
        assert direction == "reverse"
        assert out.height == 32 * 4

    def test_failure_falls_back_to_forward_direction(self, reset_config):
        """匹配不上时应返回 (None, 'forward')，调用方据此走失败分支"""
        out, direction = reset_config.stitch_images_auto(_strip(0, 20), _noise(20))

        assert out is None
        assert direction == "forward"
