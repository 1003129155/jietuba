# -*- coding: utf-8 -*-
"""
滚动截图去重算法测试（stitch/scroll_window.py 的 aHash 部分）

scroll_window.py 有 910 条语句、覆盖率约 10%，绝大多数方法依赖真实窗口滚动、
pynput 钩子、QTimer 与 Win32 API，无法离线执行。但其中判断"这一帧和上一帧
是不是同一屏"的 _calculate_image_hash / _images_are_similar 是纯计算，
只吃 PIL Image 和字符串，此前没有任何用例覆盖——一旦阈值或哈希位数被改动，
长截图会静默地丢帧或收进重复帧，而测试全绿。

这里不实例化 ScrollCaptureWindow（它的 __init__ 会建三个 QTimer、挂鼠标钩子、
读配置），而是以未绑定方式调用这两个方法，用 SimpleNamespace 充当 self——
它们只读取 self.duplicate_threshold，不碰任何 Qt 对象。
"""
from types import SimpleNamespace

from PIL import Image

from stitch.scroll_window import ScrollCaptureWindow

# 生产代码里 ScrollCaptureWindow.__init__ 设定的默认阈值
PRODUCTION_THRESHOLD = 0.95

HASH_BITS = 256  # 16x16


def _hash_of(image: Image.Image) -> str:
    """以未绑定方式调用，self 不被真正使用"""
    return ScrollCaptureWindow._calculate_image_hash(SimpleNamespace(), image)


def _similar(hash1, hash2, threshold=PRODUCTION_THRESHOLD) -> bool:
    fake_self = SimpleNamespace(duplicate_threshold=threshold)
    return ScrollCaptureWindow._images_are_similar(fake_self, hash1, hash2)


def _half_split(width=64, height=64) -> Image.Image:
    """上半黑、下半白——缩放后仍然是明确的上暗下亮"""
    img = Image.new("RGB", (width, height), (0, 0, 0))
    for y in range(height // 2, height):
        for x in range(width):
            img.putpixel((x, y), (255, 255, 255))
    return img


def _vertical_gradient(width=64, height=64) -> Image.Image:
    img = Image.new("RGB", (width, height))
    for y in range(height):
        level = int(255 * y / (height - 1))
        for x in range(width):
            img.putpixel((x, y), (level, level, level))
    return img


class TestCalculateImageHash:

    def test_hash_is_256_bits_of_zero_and_one(self):
        digest = _hash_of(_vertical_gradient())
        assert len(digest) == HASH_BITS
        assert set(digest) <= {"0", "1"}

    def test_hash_length_is_independent_of_source_size(self):
        """图片先被缩到 16x16 再算，所以任何输入尺寸都得到同样长度"""
        for size in ((8, 8), (64, 64), (300, 17), (1920, 1080)):
            assert len(_hash_of(Image.new("RGB", size, (60, 90, 120)))) == HASH_BITS

    def test_flat_image_hashes_to_all_zeros(self):
        """
        aHash 的判据是"亮于均值"，纯色图每个像素都等于均值，因此全 0。
        这也意味着纯黑和纯白两张完全不同的图哈希相同——
        见 TestDedupBehaviour 里对这个已知局限的说明。
        """
        for color in ((0, 0, 0), (255, 255, 255), (33, 77, 121)):
            assert _hash_of(Image.new("RGB", (64, 64), color)) == "0" * HASH_BITS

    def test_dark_top_and_bright_bottom_map_to_leading_zeros_and_trailing_ones(self):
        digest = _hash_of(_half_split())
        # 哈希按行优先展开：第一行落在暗部、最后一行落在亮部
        assert digest[:16] == "0" * 16
        assert digest[-16:] == "1" * 16

    def test_same_content_hashes_identically(self):
        assert _hash_of(_half_split()) == _hash_of(_half_split())

    def test_vertically_flipped_content_hashes_differently(self):
        original = _half_split()
        flipped = original.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        assert _hash_of(original) != _hash_of(flipped)

    def test_grayscale_and_rgb_sources_are_both_accepted(self):
        rgb = _vertical_gradient()
        assert _hash_of(rgb) == _hash_of(rgb.convert("L"))


class TestImagesAreSimilar:

    def test_none_hash_is_never_similar(self):
        digest = _hash_of(_half_split())
        for pair in ((None, digest), (digest, None), (None, None)):
            assert _similar(*pair) is False

    def test_identical_hashes_are_similar(self):
        digest = _hash_of(_half_split())
        assert _similar(digest, digest) is True

    def test_threshold_boundary_is_inclusive(self):
        """
        用 100 位的合成哈希把边界算干净：差 5 位 → 相似度正好 0.95，
        按 >= 判定应算重复；差 6 位 → 0.94，应算新帧。
        """
        base = "0" * 100
        assert _similar(base, "1" * 5 + "0" * 95, threshold=0.95) is True
        assert _similar(base, "1" * 6 + "0" * 94, threshold=0.95) is False

    def test_threshold_is_read_from_the_instance(self):
        base = "0" * 100
        differ_40 = "1" * 40 + "0" * 60  # 相似度 0.60
        assert _similar(base, differ_40, threshold=0.5) is True
        assert _similar(base, differ_40, threshold=0.99) is False

    def test_completely_opposite_hashes_are_not_similar(self):
        assert _similar("0" * HASH_BITS, "1" * HASH_BITS) is False


class TestDedupBehaviour:
    """把两个函数串起来，按滚动截图真实的用法验证"""

    def test_identical_frames_are_detected_as_duplicates(self):
        first, second = _half_split(), _half_split()
        assert _similar(_hash_of(first), _hash_of(second)) is True

    def test_scrolled_frame_is_treated_as_new_content(self):
        top = _vertical_gradient()
        scrolled = top.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        assert _similar(_hash_of(top), _hash_of(scrolled)) is False

    def test_two_different_flat_frames_collide_by_design(self):
        """
        已知局限，不是回归：纯黑和纯白都哈希成全 0，会被判为重复帧。
        真实截图极少出现整屏纯色，但如果以后要修这个问题（例如改用 dHash
        或在哈希里带上均值），这条用例会失败，提醒同步更新预期。
        """
        black = _hash_of(Image.new("RGB", (64, 64), (0, 0, 0)))
        white = _hash_of(Image.new("RGB", (64, 64), (255, 255, 255)))
        assert _similar(black, white) is True
