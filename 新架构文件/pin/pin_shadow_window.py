"""
钉图阴影窗口 - 独立的透明窗口，只负责绘制阴影

架构说明：
- ShadowWindow 是一个独立的透明窗口，位于 PinWindow 下方
- 只负责绘制阴影/光晕效果
- 跟随 PinWindow 移动和调整大小
- 不处理任何鼠标事件（穿透到 PinWindow）
"""

from PyQt6.QtWidgets import QWidget
from PyQt6.QtCore import Qt, QRect, QRectF
from PyQt6.QtGui import QPainter, QImage, QPixmap, QColor, QPainterPath

from core import log_debug, log_info


class PinShadowWindow(QWidget):
    """
    钉图阴影窗口
    
    独立的透明窗口，只负责绘制阴影效果。
    与 PinWindow 分离，避免干扰内容渲染。
    """
    
    def __init__(self, content_window: QWidget):
        """
        Args:
            content_window: 内容窗口（PinWindow），阴影将跟随它
        """
        super().__init__()
        
        self.content_window = content_window
        
        # 阴影样式参数
        self.pad = 20                     # 阴影留白（逻辑像素）
        self.corner = 8                   # 内容圆角
        self.shadow_spread = 18           # 阴影"扩散层数"
        self.shadow_max_alpha = 80        # 阴影最深处 alpha
        self.glow_enable = True           # 外发光开关
        self.glow_spread = 6              # 外发光层数
        self.glow_color = QColor(255, 255, 255)  # 外发光颜色
        self.glow_max_alpha = 35          # 外发光最大alpha
        self.border_enable = True         # 描边开关
        self.border_color = QColor(255, 255, 255, 100)  # 描边颜色
        self.border_width = 1.0           # 描边宽度
        
        # 阴影缓存
        self._shadow_cache: QPixmap | None = None
        self._shadow_key = None
        
        # 设置窗口属性
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint |
            Qt.WindowType.Tool |
            Qt.WindowType.WindowStaysOnTopHint |
            Qt.WindowType.WindowTransparentForInput  # 鼠标事件穿透
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        
        log_info("阴影窗口创建成功", "PinShadowWindow")
    
    def sync_geometry(self, content_rect: QRect):
        """
        同步几何信息（位置和大小）
        
        Args:
            content_rect: 内容窗口的矩形（全局坐标）
        """
        # 阴影窗口比内容窗口大，周围多出 pad 像素
        shadow_rect = QRect(
            content_rect.x() - self.pad,
            content_rect.y() - self.pad,
            content_rect.width() + self.pad * 2,
            content_rect.height() + self.pad * 2
        )
        
        # 如果大小变化，需要清除缓存
        if self.geometry() != shadow_rect:
            self._shadow_cache = None
            self._shadow_key = None
        
        self.setGeometry(shadow_rect)
        self.update()
    
    def content_rect(self) -> QRectF:
        """内容区域（阴影窗口中间的区域，对应 PinWindow 的位置）"""
        return QRectF(
            self.pad,
            self.pad,
            max(1, self.width() - self.pad * 2),
            max(1, self.height() - self.pad * 2)
        )
    
    def _rounded_path(self, rect: QRectF, radius: float) -> QPainterPath:
        """创建圆角矩形路径"""
        path = QPainterPath()
        path.addRoundedRect(rect, radius, radius)
        return path
    
    def _ensure_shadow_cache(self):
        """确保阴影缓存是最新的"""
        dpr = float(self.devicePixelRatioF())
        key = (
            self.width(), self.height(), round(dpr, 6),
            self.pad, self.corner, self.shadow_spread, self.shadow_max_alpha,
            self.glow_enable, self.glow_spread, self.glow_max_alpha,
            self.glow_color.rgba(), self.border_enable,
            self.border_color.rgba(), self.border_width
        )
        
        if self._shadow_cache is not None and self._shadow_key == key:
            return  # 缓存有效
        
        self._shadow_key = key
        self._shadow_cache = self._build_shadow_pixmap()
    
    def _build_shadow_pixmap(self) -> QPixmap:
        """构建阴影/光晕缓存"""
        dpr = float(self.devicePixelRatioF())
        w = max(1, self.width())
        h = max(1, self.height())
        phys_w = max(1, int(w * dpr))
        phys_h = max(1, int(h * dpr))
        
        img = QImage(phys_w, phys_h, QImage.Format.Format_ARGB32_Premultiplied)
        img.fill(Qt.GlobalColor.transparent)
        img.setDevicePixelRatio(dpr)
        
        p = QPainter(img)
        p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        
        cr = self.content_rect()
        
        # 1. 绘制阴影（多层叠加近似高斯模糊）
        for i in range(self.shadow_spread, 0, -1):
            alpha = int(self.shadow_max_alpha * (1 - i / self.shadow_spread) ** 1.5)
            offset = i * 1.2
            shadow_rect = cr.adjusted(-offset, -offset, offset, offset)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(0, 0, 0, alpha))
            p.drawPath(self._rounded_path(shadow_rect, self.corner + offset * 0.5))
        
        # 2. 绘制外发光
        if self.glow_enable:
            for i in range(self.glow_spread, 0, -1):
                alpha = int(self.glow_max_alpha * (1 - i / self.glow_spread))
                offset = i * 0.8
                glow_rect = cr.adjusted(-offset, -offset, offset, offset)
                glow_color = QColor(self.glow_color)
                glow_color.setAlpha(alpha)
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(glow_color)
                p.drawPath(self._rounded_path(glow_rect, self.corner + offset * 0.3))
        
        # 3. 绘制描边
        if self.border_enable:
            from PyQt6.QtGui import QPen
            pen = QPen(self.border_color, self.border_width)
            p.setPen(pen)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawPath(self._rounded_path(cr, self.corner))
        
        p.end()
        return QPixmap.fromImage(img)
    
    def paintEvent(self, event):
        """绘制事件 - 只绘制阴影"""
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        
        self._ensure_shadow_cache()
        if self._shadow_cache is not None:
            painter.drawPixmap(0, 0, self._shadow_cache)
        
        painter.end()
    
    def show_shadow(self):
        """显示阴影窗口"""
        self.show()
        # 确保阴影窗口在内容窗口下方
        # 先显示阴影，然后让内容窗口提升到前面
        if self.content_window:
            self.content_window.raise_()
    
    def hide_shadow(self):
        """隐藏阴影窗口"""
        self.hide()
    
    def close_shadow(self):
        """关闭阴影窗口"""
        self.close()
    
    def invalidate_cache(self):
        """使缓存失效（样式改变时调用）"""
        self._shadow_cache = None
        self._shadow_key = None
        self.update()
