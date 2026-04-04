"""
钉图图像变换管理器

管理旋转(0/90/180/270)和翻转(水平/垂直)状态，
提供视图变换、窗口尺寸计算、OCR坐标映射、图像导出变换等全部逻辑。

设计原则：
- 所有变换逻辑集中在本类，pin_window.py 只做高层调用
- 不直接持有 PinWindow 引用，通过方法参数接收所需数据
"""

from PySide6.QtCore import Qt, QSize, QPointF, QRectF
from PySide6.QtGui import QTransform, QImage, QPainter, QPixmap
from core import log_debug


class PinImageTransform:
    """
    钉图图像变换管理器

    状态：
        _rotation: 旋转角度（0, 90, 180, 270）
        _flip_h:   水平翻转
        _flip_v:   垂直翻转
    """

    def __init__(self):
        self._rotation = 0       # 0, 90, 180, 270
        self._flip_h = False
        self._flip_v = False

    # ==================================================================
    # 状态查询
    # ==================================================================

    @property
    def rotation(self) -> int:
        return self._rotation

    @property
    def flip_h(self) -> bool:
        return self._flip_h

    @property
    def flip_v(self) -> bool:
        return self._flip_v

    @property
    def has_transform(self) -> bool:
        """是否有任何变换"""
        return self._rotation != 0 or self._flip_h or self._flip_v

    @property
    def is_rotated_90_or_270(self) -> bool:
        """是否旋转了 90° 或 270°（宽高交换）"""
        return self._rotation in (90, 270)

    # ==================================================================
    # 状态修改（纯数据操作，不触碰 UI）
    # ==================================================================

    def rotate_cw(self):
        """顺时针旋转 90°"""
        self._rotation = (self._rotation + 90) % 360

    def rotate_ccw(self):
        """逆时针旋转 90°"""
        self._rotation = (self._rotation + 270) % 360

    def flip_horizontal(self):
        """水平翻转"""
        self._flip_h = not self._flip_h

    def flip_vertical(self):
        """垂直翻转"""
        self._flip_v = not self._flip_v

    def reset(self):
        """重置所有变换"""
        self._rotation = 0
        self._flip_h = False
        self._flip_v = False

    # ==================================================================
    # 尺寸计算
    # ==================================================================

    def display_size(self, orig_size: QSize) -> QSize:
        """
        计算变换后的显示尺寸

        Args:
            orig_size: 原始图像尺寸 (QSize)

        Returns:
            变换后的窗口尺寸（旋转90°/270°时宽高交换）
        """
        if self.is_rotated_90_or_270:
            return QSize(orig_size.height(), orig_size.width())
        return QSize(orig_size.width(), orig_size.height())

    # ==================================================================
    # 视图变换
    # ==================================================================

    def build_view_transform(self, content_w: float, content_h: float,
                             scene_w: float, scene_h: float) -> QTransform:
        """
        构建用于 QGraphicsView 的完整变换矩阵

        将场景坐标(原始图像空间) 映射到 视口坐标(旋转/翻转后的显示空间)。

        思路：
        1. 先在场景中心做旋转和翻转（几何变换）
        2. 再缩放到视口大小

        旋转 90°/270° 后，场景内容的有效区域变成了 scene_h x scene_w，
        需要缩放到 content_w x content_h 的视口。

        Args:
            content_w: 视口/窗口宽度
            content_h: 视口/窗口高度
            scene_w: 场景宽度（原始图像宽度）
            scene_h: 场景高度（原始图像高度）

        Returns:
            QTransform
        """
        # 第一步：在场景空间中做旋转+翻转（围绕场景中心）
        cx = scene_w / 2.0
        cy = scene_h / 2.0

        geo = QTransform()
        geo.translate(cx, cy)

        # 翻转
        fx = -1.0 if self._flip_h else 1.0
        fy = -1.0 if self._flip_v else 1.0
        geo.scale(fx, fy)

        # 旋转
        if self._rotation:
            geo.rotate(self._rotation)

        geo.translate(-cx, -cy)

        # 第二步：计算缩放
        # 旋转后，场景内容的有效包围盒尺寸
        if self.is_rotated_90_or_270:
            # 旋转90°/270°后，有效区域宽=scene_h, 高=scene_w
            sx = content_w / scene_h
            sy = content_h / scene_w
        else:
            sx = content_w / scene_w
            sy = content_h / scene_h

        # 合成最终变换：先做几何变换(geo)，再平移对齐，再缩放到视口
        # Qt 的 A * B 对点 P 的效果是 A(B(P))，即 A 先作用
        # 所以要达到 geo → offset → scale 的顺序，乘法应为 geo * offset * scale
        scale_t = QTransform()
        scale_t.scale(sx, sy)

        # 旋转后中心偏移补偿
        # 旋转 90°/270° 后场景四角的包围盒不再从 (0,0) 开始
        # 需要平移使内容左上角对齐到 (0,0)
        if self.is_rotated_90_or_270:
            # 计算旋转后场景四角的包围盒
            corners = [
                geo.map(QPointF(0, 0)),
                geo.map(QPointF(scene_w, 0)),
                geo.map(QPointF(scene_w, scene_h)),
                geo.map(QPointF(0, scene_h)),
            ]
            min_x = min(p.x() for p in corners)
            min_y = min(p.y() for p in corners)
            offset = QTransform()
            offset.translate(-min_x, -min_y)
            return geo * offset * scale_t
        else:
            return geo * scale_t

    # ==================================================================
    # 应用变换到窗口（核心整合方法）
    # ==================================================================

    def apply_to_window(self, pin_window):
        """
        将当前变换状态应用到 PinWindow：
        1. 计算新窗口尺寸
        2. 调整窗口几何（中心不变）
        3. 重置 scale_factor
        4. 更新视图变换和背景
        5. 更新 OCR 层的变换信息

        Args:
            pin_window: PinWindow 实例
        """
        # 计算目标尺寸
        new_size = self.display_size(pin_window._orig_size)
        new_w = new_size.width()
        new_h = new_size.height()

        # 保持中心点不变
        cx = pin_window.x() + pin_window.width() // 2
        cy = pin_window.y() + pin_window.height() // 2
        new_x = cx - new_w // 2
        new_y = cy - new_h // 2

        pin_window.setGeometry(new_x, new_y, new_w, new_h)
        pin_window.scale_factor = 1.0

        if pin_window.canvas:
            pin_window.canvas.invalidate_cache()

        # 更新视图变换（该方法内部会读取本管理器状态）
        pin_window._update_view_transform()
        pin_window._refresh_background_for_scale()
        pin_window.update_button_positions()

        if pin_window.toolbar and pin_window.toolbar.isVisible():
            pin_window.toolbar.sync_with_pin_window()

        # 通知 OCR 层变换状态变化
        self._update_ocr_layer(pin_window)

        # 重新应用描边
        self._refresh_border(pin_window)

        pin_window.update()
        log_debug(f"变换已应用: rotation={self._rotation}, "
                  f"flip_h={self._flip_h}, flip_v={self._flip_v}", "ImageTransform")

    def _update_ocr_layer(self, pin_window):
        """通知 OCR 文字层变换状态变化"""
        ocr_layer = getattr(pin_window, 'ocr_text_layer', None)
        if ocr_layer is None:
            return
        try:
            ocr_layer.set_image_transform(self)
        except RuntimeError:
            pass  # C++ 对象已销毁

    def _refresh_border(self, pin_window):
        """创建/刺新描边 Overlay（无阴影，仅单圈主题色线）"""
        from .pin_border_overlay import PinBorderOverlay
        if pin_window.halo_enabled:
            if not pin_window.border_overlay:
                pin_window.border_overlay = PinBorderOverlay(
                    pin_window,
                    corner_radius=pin_window.corner,
                    border_color=pin_window.border_color,
                )
            pin_window.border_overlay.setGeometry(
                0, 0, pin_window.width(), pin_window.height())
            pin_window.border_overlay.show()
            pin_window.border_overlay.raise_()
        else:
            if pin_window.border_overlay:
                pin_window.border_overlay.hide()
        if pin_window.view:
            radius = pin_window.corner if pin_window.halo_enabled else 0
            pin_window.view.set_corner_radius(radius)

    # ==================================================================
    # 图像导出变换
    # ==================================================================

    def transform_image(self, image: QImage) -> QImage:
        """
        对导出图像应用当前变换

        Args:
            image: 原始渲染结果（场景坐标空间的图像）

        Returns:
            变换后的 QImage
        """
        if not self.has_transform:
            return image

        t = QTransform()
        w = image.width()
        h = image.height()
        cw = w / 2.0
        ch = h / 2.0

        # 围绕中心做变换
        t.translate(cw, ch)

        # 翻转
        fx = -1.0 if self._flip_h else 1.0
        fy = -1.0 if self._flip_v else 1.0
        t.scale(fx, fy)

        # 旋转
        if self._rotation:
            t.rotate(self._rotation)

        t.translate(-cw, -ch)

        result = image.transformed(t, Qt.TransformationMode.SmoothTransformation)
        result.setDevicePixelRatio(image.devicePixelRatio())
        return result

    # ==================================================================
    # OCR 坐标映射
    # ==================================================================

    def map_ocr_point(self, x: float, y: float,
                      orig_w: float, orig_h: float) -> tuple:
        """
        将原始坐标系中的 OCR 点映射到变换后的坐标系

        变换顺序和 build_view_transform 一致：先翻转，再旋转。
        旋转后输出坐标系的尺寸可能交换（90°/270°时）。

        Args:
            x, y: 原始坐标（像素）
            orig_w: 原始图像宽度
            orig_h: 原始图像高度

        Returns:
            (mapped_x, mapped_y) 映射后坐标
        """
        px, py = float(x), float(y)
        cx = orig_w / 2.0
        cy = orig_h / 2.0

        # 相对于中心
        px -= cx
        py -= cy

        # 1) 翻转
        if self._flip_h:
            px = -px
        if self._flip_v:
            py = -py

        # 2) 旋转
        if self._rotation == 90:
            px, py = -py, px
        elif self._rotation == 180:
            px, py = -px, -py
        elif self._rotation == 270:
            px, py = py, -px

        # 3) 回到变换后坐标系
        if self.is_rotated_90_or_270:
            # 变换后中心是 (orig_h/2, orig_w/2)
            px += orig_h / 2.0
            py += orig_w / 2.0
        else:
            px += cx
            py += cy

        return (px, py)

    def map_ocr_rect(self, rect: QRectF,
                     orig_w: float, orig_h: float) -> QRectF:
        """
        将原始坐标系中的 OCR 矩形映射到变换后的坐标系

        Args:
            rect: 原始矩形 (QRectF，像素坐标)
            orig_w: 原始图像宽度
            orig_h: 原始图像高度

        Returns:
            变换后的 QRectF
        """
        # 映射四个角，取包围盒
        corners = [
            (rect.left(), rect.top()),
            (rect.right(), rect.top()),
            (rect.right(), rect.bottom()),
            (rect.left(), rect.bottom()),
        ]
        mapped = [self.map_ocr_point(cx, cy, orig_w, orig_h) for cx, cy in corners]
        xs = [p[0] for p in mapped]
        ys = [p[1] for p in mapped]
        return QRectF(min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys))

    def mapped_image_size(self, orig_w: float, orig_h: float) -> tuple:
        """
        变换后的图像逻辑尺寸（用于 OCR 层 scale_factors 计算）

        Args:
            orig_w: 原始图像宽度
            orig_h: 原始图像高度

        Returns:
            (mapped_w, mapped_h)
        """
        if self.is_rotated_90_or_270:
            return (orig_h, orig_w)
        return (orig_w, orig_h)
 