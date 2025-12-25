"""鼠标放大镜叠加层，复刻老版截图界面的颜色查看逻辑。"""

from __future__ import annotations

from typing import Optional

from PyQt6.QtCore import QPoint, QPointF, QRect, Qt
from PyQt6.QtGui import QColor, QFont, QPainter, QPen, QBrush, QPixmap
from PyQt6.QtWidgets import QWidget


class MagnifierOverlay(QWidget):
	"""显示鼠标附近的放大图和 RGB/HSV 信息。"""

	MAG_WIDTH = 150   # 放大镜宽度
	MAG_HEIGHT = 120  # 放大镜高度
	INFO_WIDTH = 150  # 信息框宽度调整为150，足够显示长数字
	INFO_HEIGHT = 70  # 信息框高度
	SAMPLE_SIZE = 48
	EDGE_MARGIN = 16
	COMBINED_HEIGHT = MAG_HEIGHT + INFO_HEIGHT  # 合并后的总高度

	def __init__(self, parent: QWidget, scene, view, config_manager=None):
		super().__init__(parent)
		self.scene = scene
		self.view = view
		self.config_manager = config_manager
		self.cursor_scene_pos: Optional[QPointF] = None
		# 创建字体时不使用QFont.Weight.Bold，改用setBold避免字体变体问题
		self._font = QFont("Microsoft YaHei", 10)
		self._font.setBold(True)
		
		# 信息框文字使用的字体
		self._info_font = QFont("Microsoft YaHei", 11)
		self._info_font.setBold(True)
		
		# 放大镜倍数（从配置加载，默认为2.5倍，范围1-10）
		if self.config_manager:
			self._zoom_factor = self.config_manager.qsettings.value("app/magnifier_zoom", 2.5, type=float)
			# 确保倍数在有效范围内
			self._zoom_factor = max(1.0, min(10.0, self._zoom_factor))
		else:
			self._zoom_factor = 2.5

		self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
		self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
		self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
		self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
		self.setMouseTracking(False)

		self.show()
		self.raise_()

	# ------------------------------------------------------------------
	# 外部控制
	# ------------------------------------------------------------------
	def update_cursor(self, scene_pos: QPointF):
		"""记录最新的场景坐标并请求重绘。"""
		self.cursor_scene_pos = QPointF(scene_pos)
		self.update()

	def clear_cursor(self):
		"""鼠标移出画布时隐藏放大镜。"""
		if self.cursor_scene_pos is None:
			return
		self.cursor_scene_pos = None
		self.update()

	def refresh(self):
		"""外部状态变化时触发重绘。"""
		self.update()
	
	def adjust_zoom(self, delta: int):
		"""调整放大倍数
		
		Args:
			delta: 调整值，正数增加，负数减少
		"""
		self._zoom_factor += delta * 0.25
		self._zoom_factor = max(1.0, min(10.0, self._zoom_factor))  # 限制范围 1-10
		# 保存到配置
		if self.config_manager:
			self.config_manager.qsettings.setValue("app/magnifier_zoom", self._zoom_factor)
		print(f"🔍 [放大镜] 调整倍数: {self._zoom_factor:.2f}x")
		self.update()
	
	def get_zoom_factor(self) -> float:
		"""获取当前放大倍数"""
		return self._zoom_factor

	# ------------------------------------------------------------------
	# QWidget 接口
	# ------------------------------------------------------------------
	def paintEvent(self, event):
		painter = QPainter(self)
		painter.setRenderHint(QPainter.RenderHint.Antialiasing)

		if not self._should_render():
			painter.end()
			return

		anchor = self._map_scene_to_local(self.cursor_scene_pos)
		if anchor is None:
			painter.end()
			return

		combined_rect = self._layout_combined_rect(anchor)
		image = self._background_image()
		color = self._sample_color(image)
		pixmap = self._build_magnified_pixmap(image)

		self._draw_combined_magnifier(painter, combined_rect, pixmap, color)

		painter.end()

	# ------------------------------------------------------------------
	# 绘制实现
	# ------------------------------------------------------------------
	def _get_fitted_font(self, painter: QPainter, text: str, max_width: int, 
	                     base_size: int = 11, min_size: int = 7) -> QFont:
		"""计算能在指定宽度内完整显示文字的字体大小
		
		Args:
			painter: QPainter对象
			text: 要显示的文字
			max_width: 最大宽度（像素）
			base_size: 基础字体大小
			min_size: 最小字体大小
			
		Returns:
			调整后的QFont对象
		"""
		font = QFont("Microsoft YaHei", base_size)
		font.setBold(True)
		
		# 测试当前字体宽度
		metrics = painter.fontMetrics()
		painter.setFont(font)
		metrics = painter.fontMetrics()
		text_width = metrics.horizontalAdvance(text)
		
		# 如果文字超出宽度，逐步缩小字体
		current_size = base_size
		while text_width > max_width and current_size > min_size:
			current_size -= 1
			font.setPointSize(current_size)
			painter.setFont(font)
			metrics = painter.fontMetrics()
			text_width = metrics.horizontalAdvance(text)
		
		return font
	
	def _draw_combined_magnifier(self, painter: QPainter, rect: QRect, pixmap: Optional[QPixmap], color: QColor):
		"""绘制合并的放大镜+信息框"""
		# 放大镜区域填满整个上半部分(长方形)
		mag_rect = QRect(rect.x(), rect.y(), self.MAG_WIDTH, self.MAG_HEIGHT)
		info_rect = QRect(rect.x(), rect.y() + self.MAG_HEIGHT, self.INFO_WIDTH, self.INFO_HEIGHT)
		
		# 绘制整体背景和边框
		painter.setPen(QPen(QColor(64, 224, 208), 2))
		painter.setBrush(QBrush(QColor(40, 40, 45, 220)))
		painter.drawRoundedRect(rect, 6, 6)
		
		# 绘制放大镜部分(填满整个长方形区域)
		painter.setBrush(Qt.BrushStyle.NoBrush)
		if pixmap:
			# 使用裁剪模式:保持宽高比,缩放到能覆盖整个区域,然后裁剪
			# 计算缩放比例,选择能覆盖整个区域的较大比例
			scale_x = self.MAG_WIDTH / pixmap.width()
			scale_y = self.MAG_HEIGHT / pixmap.height()
			scale = max(scale_x, scale_y)  # 使用较大的比例,确保能覆盖整个区域
			
			# 按比例缩放
			new_width = int(pixmap.width() * scale)
			new_height = int(pixmap.height() * scale)
			scaled_pixmap = pixmap.scaled(
				new_width, 
				new_height,
				Qt.AspectRatioMode.KeepAspectRatio,
				Qt.TransformationMode.FastTransformation
			)
			
			# 居中裁剪:计算绘制起点,使图片居中,超出部分会被裁剪
			offset_x = (new_width - self.MAG_WIDTH) // 2
			offset_y = (new_height - self.MAG_HEIGHT) // 2
			
			# 设置裁剪区域
			painter.save()
			painter.setClipRect(mag_rect)
			
			# 绘制(超出mag_rect的部分会被裁剪)
			painter.drawPixmap(
				mag_rect.x() - offset_x,
				mag_rect.y() - offset_y,
				scaled_pixmap
			)
			
			painter.restore()

		# 十字线
		painter.setPen(QPen(QColor(64, 224, 208), 1))
		center_x = mag_rect.center().x()
		center_y = mag_rect.center().y()
		painter.drawLine(mag_rect.left(), center_y, mag_rect.right(), center_y)
		painter.drawLine(center_x, mag_rect.top(), center_x, mag_rect.bottom())
		
		# 左上角显示放大倍数
		zoom_text = f"{self._zoom_factor:.1f}x"
		painter.setFont(self._font)
		painter.setPen(QPen(QColor(255, 255, 255), 2))
		painter.setBrush(QBrush(QColor(0, 0, 0, 180)))
		text_margin = 4
		metrics = painter.fontMetrics()
		text_width = metrics.horizontalAdvance(zoom_text)
		text_height = metrics.height()
		text_bg_rect = QRect(
			mag_rect.left() + 2,
			mag_rect.top() + 2,
			text_width + text_margin * 2,
			text_height + text_margin
		)
		painter.drawRoundedRect(text_bg_rect, 3, 3)
		painter.setBrush(Qt.BrushStyle.NoBrush)
		painter.drawText(
			text_bg_rect.x() + text_margin,
			text_bg_rect.y() + text_height - 2,
			zoom_text
		)
		
		# 右上角显示取色颜色方块
		color_box_size = 20
		color_box_rect = QRect(
			mag_rect.right() - color_box_size - 4,
			mag_rect.top() + 4,
			color_box_size,
			color_box_size
		)
		painter.setPen(QPen(QColor(255, 255, 255), 2))
		painter.setBrush(QBrush(color))
		painter.drawRect(color_box_rect)
		
		# 绘制分隔线
		painter.setPen(QPen(QColor(64, 224, 208), 1))
		painter.drawLine(info_rect.left(), info_rect.top(), info_rect.right(), info_rect.top())
		
		# 绘制信息文本 (自动缩小字体以适应固定宽度)
		painter.setBrush(Qt.BrushStyle.NoBrush)

		pos = self.cursor_scene_pos or QPointF(0, 0)
		rgb_text = f"RGB: {color.red()}, {color.green()}, {color.blue()}"
		hex_text = f"HEX: {color.name().upper()}"
		pos_text = f"POS: {int(pos.x())}, {int(pos.y())}"

		# 定义每行文字的固定矩形区域 (宽度锁定)
		text_padding_left = 6
		text_padding_right = 3
		line_height = 20
		text_rect_width = info_rect.width() - text_padding_left - text_padding_right
		
		text_x = info_rect.x() + text_padding_left
		base_y = info_rect.y() + 4
		
		# 绘制每行文字，自动调整字体大小以适应宽度
		texts = [
			(pos_text, QColor(100, 240, 220), base_y),
			(rgb_text, QColor(255, 200, 100), base_y + line_height),
			(hex_text, QColor(200, 150, 255), base_y + line_height * 2)
		]
		
		for text, color, y_pos in texts:
			# 计算合适的字体大小
			font = self._get_fitted_font(painter, text, text_rect_width, 11, 7)
			painter.setFont(font)
			painter.setPen(color)
			
			# 计算垂直居中位置
			metrics = painter.fontMetrics()
			text_height = metrics.height()
			y_centered = y_pos + (line_height + text_height) // 2 - metrics.descent()
			
			painter.drawText(text_x, y_centered, text)


	# ------------------------------------------------------------------
	# 数据准备
	# ------------------------------------------------------------------
	def _should_render(self) -> bool:
		if self.cursor_scene_pos is None or not self.scene or not self.view:
			return False

		if getattr(self.view, 'is_drawing', False):
			return False
		if getattr(self.view, '_text_drag_active', False):
			return False

		controller = getattr(self.view, 'smart_edit_controller', None)
		if controller:
			editor = getattr(controller, 'layer_editor', None)
			if editor and getattr(editor, 'dragging_handle', None):
				return False

		current_tool = getattr(self.scene.tool_controller, 'current_tool', None)
		if (self.scene.selection_model.is_confirmed and
				current_tool and current_tool.id != 'cursor'):
			return False

		return True

	def _background_image(self):
		background = getattr(self.scene, 'background', None)
		if background and hasattr(background, 'image'):
			return background.image()
		return None

	def _scene_to_image_point(self, scene_pos: QPointF) -> Optional[QPoint]:
		rect = getattr(self.scene, 'scene_rect', None)
		if rect is None:
			return None
		# 使用 floor 取整，确保取的是鼠标所在的像素块
		x = int(scene_pos.x() - rect.left())
		y = int(scene_pos.y() - rect.top())
		return QPoint(x, y)
	
	def _scene_to_pixel_center(self, scene_pos: QPointF) -> Optional[QPointF]:
		"""将场景坐标转换为对齐到像素中心的场景坐标"""
		rect = getattr(self.scene, 'scene_rect', None)
		if rect is None:
			return None
		# 转换到图像坐标
		x = scene_pos.x() - rect.left()
		y = scene_pos.y() - rect.top()
		# 吸附到像素中心
		x_center = int(x) + 0.5
		y_center = int(y) + 0.5
		# 转换回场景坐标
		return QPointF(x_center + rect.left(), y_center + rect.top())

	def _sample_color(self, image) -> QColor:
		if image is None or self.cursor_scene_pos is None:
			return QColor(255, 255, 255)
		pt = self._scene_to_image_point(self.cursor_scene_pos)
		if pt and image.rect().contains(pt):
			return QColor(image.pixelColor(pt))
		return QColor(255, 255, 255)

	def _build_magnified_pixmap(self, image) -> Optional[QPixmap]:
		if image is None or self.cursor_scene_pos is None:
			return None
		# 使用像素中心坐标进行采样，让准星对齐到像素中心
		center_pos = self._scene_to_pixel_center(self.cursor_scene_pos)
		if not center_pos:
			return None
		pt = self._scene_to_image_point(center_pos)
		if not pt:
			return None
		# 根据放大倍数动态调整采样区域大小
		# 确保采样区域为奇数大小，这样中心像素正好在中间
		sample_size = max(int(self.SAMPLE_SIZE / self._zoom_factor), 4)
		if sample_size % 2 == 0:
			sample_size += 1  # 确保是奇数
		half = sample_size // 2
		src = QRect(pt.x() - half, pt.y() - half, sample_size, sample_size)
		src = src.intersected(image.rect())
		if src.width() <= 0 or src.height() <= 0:
			return None
		sample = image.copy(src)
		# 缩放到长方形放大镜尺寸，使用最近邻算法保持像素边界清晰
		scaled = sample.scaled(
			self.MAG_WIDTH,
			self.MAG_HEIGHT,
			Qt.AspectRatioMode.IgnoreAspectRatio,  # 填满整个区域
			Qt.TransformationMode.FastTransformation,  # 最近邻算法，保持像素清晰
		)
		return QPixmap.fromImage(scaled)

	def _map_scene_to_local(self, scene_pos: QPointF) -> Optional[QPoint]:
		if scene_pos is None or not self.view:
			return None
		viewport_point = self.view.mapFromScene(scene_pos)
		global_point = self.view.viewport().mapToGlobal(viewport_point)
		return self.mapFromGlobal(global_point)

	def _layout_combined_rect(self, anchor: QPoint) -> QRect:
		"""计算合并后的放大镜+信息框的位置"""
		margin = self.EDGE_MARGIN
		width = self.INFO_WIDTH  # 使用信息框宽度作为整体宽度
		height = self.COMBINED_HEIGHT
		
		# 默认在鼠标右下方
		x = anchor.x() + margin
		y = anchor.y() + margin

		# 如果右侧超出边界,移到左侧
		if x + width > self.width() - margin:
			x = max(margin, anchor.x() - width - margin)
		
		# 如果下方超出边界,移到上方
		if y + height > self.height() - margin:
			y = max(margin, anchor.y() - height - margin)

		return QRect(x, y, width, height)
