# -*- coding: utf-8 -*-
"""
PinImageTransform 钉图变换单元测试

覆盖旋转/翻转状态机、显示尺寸换算、OCR 坐标映射与导出图像变换。

重点验证数学不变量而非方法存在性：
- 旋转状态机的周期性（转 4 次回到原点）
- 坐标映射把原图四角映射到变换后图像的四角（不越界、不丢角）
- map_ocr_point 与 build_view_transform 的一致性
  （pin_image_transform 的文档声明两者变换顺序一致，这里把该声明固化成测试）
"""
import pytest
from PySide6.QtWidgets import QApplication
from PySide6.QtCore import QSize, QRectF, QPointF
from PySide6.QtGui import QImage


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


@pytest.fixture
def tf(qapp):
    from pin.pin_image_transform import PinImageTransform
    return PinImageTransform()


# ============================================================================
# 旋转 / 翻转状态机
# ============================================================================

class TestRotationState:
    """旋转状态机"""

    def test_initial_state_has_no_transform(self, tf):
        assert tf.rotation == 0
        assert tf.flip_h is False
        assert tf.flip_v is False
        assert tf.has_transform is False

    def test_rotate_cw_cycles_through_four_steps(self, tf):
        """顺时针转 4 次应回到 0，且中间经过 90/180/270"""
        expected = [90, 180, 270, 0]
        for want in expected:
            tf.rotate_cw()
            assert tf.rotation == want

    def test_rotate_ccw_cycles_backwards(self, tf):
        """逆时针转 4 次应回到 0，且中间经过 270/180/90"""
        expected = [270, 180, 90, 0]
        for want in expected:
            tf.rotate_ccw()
            assert tf.rotation == want

    def test_cw_then_ccw_returns_to_origin(self, tf):
        tf.rotate_cw()
        tf.rotate_ccw()
        assert tf.rotation == 0
        assert tf.has_transform is False

    def test_is_rotated_90_or_270_only_for_quarter_turns(self, tf):
        assert tf.is_rotated_90_or_270 is False
        tf.rotate_cw()                       # 90
        assert tf.is_rotated_90_or_270 is True
        tf.rotate_cw()                       # 180
        assert tf.is_rotated_90_or_270 is False
        tf.rotate_cw()                       # 270
        assert tf.is_rotated_90_or_270 is True

    def test_flips_toggle_independently(self, tf):
        tf.flip_horizontal()
        assert tf.flip_h is True and tf.flip_v is False
        tf.flip_vertical()
        assert tf.flip_h is True and tf.flip_v is True
        tf.flip_horizontal()
        assert tf.flip_h is False and tf.flip_v is True

    def test_flip_alone_counts_as_transform(self, tf):
        """只翻转不旋转也应视为有变换（影响导出与视图刷新）"""
        tf.flip_horizontal()
        assert tf.has_transform is True

    def test_reset_clears_every_axis(self, tf):
        tf.rotate_cw()
        tf.flip_horizontal()
        tf.flip_vertical()
        tf.reset()
        assert tf.rotation == 0
        assert tf.flip_h is False
        assert tf.flip_v is False
        assert tf.has_transform is False


# ============================================================================
# 显示尺寸
# ============================================================================

class TestDisplaySize:
    """窗口尺寸换算"""

    def test_no_rotation_keeps_size(self, tf):
        assert tf.display_size(QSize(200, 100)) == QSize(200, 100)

    def test_quarter_turns_swap_width_and_height(self, tf):
        tf.rotate_cw()
        assert tf.display_size(QSize(200, 100)) == QSize(100, 200)
        tf.rotate_cw()                       # 180
        assert tf.display_size(QSize(200, 100)) == QSize(200, 100)
        tf.rotate_cw()                       # 270
        assert tf.display_size(QSize(200, 100)) == QSize(100, 200)

    def test_flips_never_change_size(self, tf):
        tf.flip_horizontal()
        tf.flip_vertical()
        assert tf.display_size(QSize(200, 100)) == QSize(200, 100)

    def test_mapped_image_size_matches_display_size(self, tf):
        """mapped_image_size 与 display_size 必须给出同一套宽高，否则 OCR 层缩放会错位"""
        for _ in range(4):
            size = tf.display_size(QSize(200, 100))
            assert tf.mapped_image_size(200, 100) == (size.width(), size.height())
            tf.rotate_cw()


# ============================================================================
# OCR 坐标映射
# ============================================================================

ORIG_W, ORIG_H = 200.0, 100.0


def _corners(w, h):
    return [(0.0, 0.0), (w, 0.0), (w, h), (0.0, h)]


class TestOcrPointMapping:
    """OCR 点坐标映射"""

    def test_identity_when_untransformed(self, tf):
        assert tf.map_ocr_point(37, 21, ORIG_W, ORIG_H) == (37.0, 21.0)

    def test_center_is_a_fixed_point_under_every_transform(self, tf):
        """图像中心在任意旋转/翻转下都应映射到新图像的中心"""
        for _ in range(4):
            for flip_h in (False, True):
                for flip_v in (False, True):
                    tf.reset()
                    tf._flip_h, tf._flip_v = flip_h, flip_v
                    mapped_w, mapped_h = tf.mapped_image_size(ORIG_W, ORIG_H)
                    x, y = tf.map_ocr_point(ORIG_W / 2, ORIG_H / 2, ORIG_W, ORIG_H)
                    assert x == pytest.approx(mapped_w / 2)
                    assert y == pytest.approx(mapped_h / 2)
            tf.rotate_cw()

    def test_rotate_90_sends_top_left_to_top_right(self, tf):
        """顺时针 90°：原图左上角落到新图右上角"""
        tf.rotate_cw()
        x, y = tf.map_ocr_point(0, 0, ORIG_W, ORIG_H)
        assert (x, y) == pytest.approx((ORIG_H, 0.0))

    def test_rotate_180_is_point_reflection(self, tf):
        tf.rotate_cw()
        tf.rotate_cw()
        x, y = tf.map_ocr_point(30, 20, ORIG_W, ORIG_H)
        assert (x, y) == pytest.approx((ORIG_W - 30, ORIG_H - 20))

    def test_rotate_270_sends_top_left_to_bottom_left(self, tf):
        tf.rotate_ccw()
        x, y = tf.map_ocr_point(0, 0, ORIG_W, ORIG_H)
        assert (x, y) == pytest.approx((0.0, ORIG_W))

    def test_flip_h_mirrors_x_only(self, tf):
        tf.flip_horizontal()
        x, y = tf.map_ocr_point(30, 20, ORIG_W, ORIG_H)
        assert (x, y) == pytest.approx((ORIG_W - 30, 20.0))

    def test_flip_v_mirrors_y_only(self, tf):
        tf.flip_vertical()
        x, y = tf.map_ocr_point(30, 20, ORIG_W, ORIG_H)
        assert (x, y) == pytest.approx((30.0, ORIG_H - 20))

    def test_corners_map_onto_corners_for_every_state(self, tf):
        """
        任意旋转/翻转组合下，原图四角必须恰好映射到新图四角
        （既不越界，也不塌缩）——这是坐标映射正确性的强约束。
        """
        for _ in range(4):
            for flip_h in (False, True):
                for flip_v in (False, True):
                    state_rotation = tf.rotation
                    tf.reset()
                    tf._rotation = state_rotation
                    tf._flip_h, tf._flip_v = flip_h, flip_v

                    mapped_w, mapped_h = tf.mapped_image_size(ORIG_W, ORIG_H)
                    got = {
                        tuple(round(v, 6) for v in tf.map_ocr_point(cx, cy, ORIG_W, ORIG_H))
                        for cx, cy in _corners(ORIG_W, ORIG_H)
                    }
                    want = {
                        tuple(round(v, 6) for v in c)
                        for c in _corners(mapped_w, mapped_h)
                    }
                    assert got == want, (
                        f"rotation={state_rotation} flip_h={flip_h} flip_v={flip_v}"
                    )
            tf.rotate_cw()


class TestOcrRectMapping:
    """OCR 矩形映射"""

    def test_identity_rect_when_untransformed(self, tf):
        rect = tf.map_ocr_rect(QRectF(10, 20, 50, 30), ORIG_W, ORIG_H)
        assert rect.left() == pytest.approx(10)
        assert rect.top() == pytest.approx(20)
        assert rect.width() == pytest.approx(50)
        assert rect.height() == pytest.approx(30)

    def test_quarter_turn_swaps_rect_dimensions(self, tf):
        tf.rotate_cw()
        rect = tf.map_ocr_rect(QRectF(10, 20, 50, 30), ORIG_W, ORIG_H)
        assert rect.width() == pytest.approx(30)
        assert rect.height() == pytest.approx(50)

    def test_mapped_rect_stays_inside_mapped_image(self, tf):
        """映射后的文字框不能跑到图像外面去（OCR 文字层选区错位的常见根因）"""
        for _ in range(4):
            for flip_h in (False, True):
                for flip_v in (False, True):
                    rotation = tf.rotation
                    tf.reset()
                    tf._rotation = rotation
                    tf._flip_h, tf._flip_v = flip_h, flip_v

                    mapped_w, mapped_h = tf.mapped_image_size(ORIG_W, ORIG_H)
                    rect = tf.map_ocr_rect(QRectF(10, 20, 50, 30), ORIG_W, ORIG_H)
                    assert rect.left() >= -1e-6
                    assert rect.top() >= -1e-6
                    assert rect.right() <= mapped_w + 1e-6
                    assert rect.bottom() <= mapped_h + 1e-6
            tf.rotate_cw()

    def test_180_rotation_is_self_inverse_on_rects(self, tf):
        """转 180° 两次应把矩形还原到原位"""
        tf._rotation = 180
        once = tf.map_ocr_rect(QRectF(10, 20, 50, 30), ORIG_W, ORIG_H)
        twice = tf.map_ocr_rect(once, ORIG_W, ORIG_H)
        assert twice.left() == pytest.approx(10)
        assert twice.top() == pytest.approx(20)
        assert twice.width() == pytest.approx(50)
        assert twice.height() == pytest.approx(30)


# ============================================================================
# map_ocr_point 与 build_view_transform 的一致性
# ============================================================================

class TestMappingMatchesViewTransform:
    """
    pin_image_transform 的文档声明 map_ocr_point 与 build_view_transform
    使用同样的变换顺序（先翻转再旋转）。若两者走偏，OCR 文字层会和画面错位，
    这里把该声明固化为测试。
    """

    @pytest.mark.parametrize("rotation", [0, 90, 180, 270])
    @pytest.mark.parametrize("flip_h,flip_v", [(False, False), (True, False),
                                               (False, True), (True, True)])
    def test_view_transform_agrees_with_ocr_mapping(self, tf, rotation, flip_h, flip_v):
        tf._rotation = rotation
        tf._flip_h, tf._flip_v = flip_h, flip_v

        mapped_w, mapped_h = tf.mapped_image_size(ORIG_W, ORIG_H)
        # 视口尺寸取变换后的 1:1 尺寸，此时视图变换应与 OCR 映射完全重合
        view_t = tf.build_view_transform(mapped_w, mapped_h, ORIG_W, ORIG_H)

        for px, py in [(0, 0), (ORIG_W, 0), (ORIG_W, ORIG_H), (0, ORIG_H),
                       (ORIG_W / 2, ORIG_H / 2), (37, 21)]:
            via_view = view_t.map(QPointF(px, py))
            via_ocr = tf.map_ocr_point(px, py, ORIG_W, ORIG_H)
            assert via_view.x() == pytest.approx(via_ocr[0], abs=1e-6)
            assert via_view.y() == pytest.approx(via_ocr[1], abs=1e-6)


# ============================================================================
# 导出图像变换
# ============================================================================

class TestTransformImage:
    """导出图像变换"""

    @staticmethod
    def _image(w=8, h=4):
        img = QImage(w, h, QImage.Format.Format_ARGB32)
        img.fill(0xFF000000)
        # 左上角标记一个白点，用来追踪它变换后落到哪里
        img.setPixel(0, 0, 0xFFFFFFFF)
        return img

    def test_untransformed_image_is_returned_as_is(self, tf):
        img = self._image()
        assert tf.transform_image(img) is img

    def test_quarter_turn_swaps_image_dimensions(self, tf):
        tf.rotate_cw()
        out = tf.transform_image(self._image(8, 4))
        assert (out.width(), out.height()) == (4, 8)

    def test_180_keeps_dimensions(self, tf):
        tf._rotation = 180
        out = tf.transform_image(self._image(8, 4))
        assert (out.width(), out.height()) == (8, 4)

    def test_rotate_90_moves_marker_to_top_right(self, tf):
        """左上角的标记点顺时针 90° 后应出现在右上角"""
        tf.rotate_cw()
        out = tf.transform_image(self._image(8, 4))
        assert out.pixel(out.width() - 1, 0) == 0xFFFFFFFF

    def test_flip_h_moves_marker_to_top_right(self, tf):
        tf.flip_horizontal()
        out = tf.transform_image(self._image(8, 4))
        assert out.pixel(out.width() - 1, 0) == 0xFFFFFFFF

    def test_flip_v_moves_marker_to_bottom_left(self, tf):
        tf.flip_vertical()
        out = tf.transform_image(self._image(8, 4))
        assert out.pixel(0, out.height() - 1) == 0xFFFFFFFF

    def test_device_pixel_ratio_is_preserved(self, tf):
        """高 DPI 屏上导出若丢掉 DPR，保存出来的图会变成两倍大小"""
        img = self._image()
        img.setDevicePixelRatio(2.0)
        tf.rotate_cw()
        out = tf.transform_image(img)
        assert out.devicePixelRatio() == 2.0
