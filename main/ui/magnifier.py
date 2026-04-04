"""鼠标放大镜浮层 —— 独立小 widget，跟随鼠标 move()，仅重绘自身。"""

from __future__ import annotations

from typing import Optional

from PySide6.QtCore import QPoint, QPointF, QRect, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPainterPath, QPen, QBrush
from PySide6.QtWidgets import QWidget

from core import log_debug, safe_event


class MagnifierOverlay(QWidget):
	"""显示鼠标附近的放大图和 RGB/HSV 信息。

	架构：固定尺寸 (150×210) 的独立浮层 widget，
	通过 move() 跟随光标，update() 只重绘自身面积，
	不再覆盖整个 ScreenshotWindow。
	"""

	MAG_WIDTH = 150   # 放大镜宽度
	MAG_HEIGHT = 120  # 放大镜高度
	INFO_WIDTH = 150  # 信息框宽度
	INFO_HEIGHT = 90  # 信息框高度
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
		
		# 放大镜倍数（从配置加载，默认值/范围均来自 APP_DEFAULT_SETTINGS）
		from settings.tool_settings import get_tool_settings_manager
		_mgr = get_tool_settings_manager()
		_default_zoom = _mgr.APP_DEFAULT_SETTINGS["magnifier_zoom"]
		_zoom_min     = _mgr.APP_DEFAULT_SETTINGS["magnifier_zoom_min"]
		_zoom_max     = _mgr.APP_DEFAULT_SETTINGS["magnifier_zoom_max"]
		self._zoom_min = _zoom_min
		self._zoom_max = _zoom_max
		if self.config_manager:
			self._zoom_factor = self.config_manager.qsettings.value("app/magnifier_zoom", _default_zoom, type=float)
			self._zoom_factor = max(_zoom_min, min(_zoom_max, self._zoom_factor))
		else:
			self._zoom_factor = _default_zoom

		# 缓存像素采样结果
		self._last_sample_pt: Optional[QPoint] = None
		self._cached_source_rect: Optional[QRect] = None
		self._cached_sample_image = None  # 缓存采样的原始图像
		
		# 预计算的固定字体（首次 paintEvent 中按最大坐标模板一次性确定）
		# POS/RGB/HEX 三行共用一个字号，hint 行单独一个字号
		self._fixed_info_font: Optional[QFont] = None
		self._fixed_hint_font: Optional[QFont] = None
		self._fixed_info_metrics = None   # 缓存 metrics 避免每帧 fontMetrics()
		self._fixed_hint_metrics = None
		
		# 预缓存绘制常量（避免每帧创建临时对象）
		from core.theme import get_theme
		_tc = get_theme().theme_color
		_tc_semi = QColor(_tc)
		_tc_semi.setAlpha(120)
		self._pen_teal_2 = QPen(_tc, 2)
		self._pen_teal_1 = QPen(_tc, 1)
		self._pen_white_2 = QPen(QColor(255, 255, 255), 2)
		self._brush_bg = QBrush(QColor(40, 40, 45, 220))
		self._brush_black_a = QBrush(QColor(0, 0, 0, 180))
		self._brush_crosshair = QBrush(_tc_semi)  # 半透明主题色，用于十字色带
		self._color_pos = QColor(100, 240, 220)
		self._color_rgb = QColor(255, 200, 100)
		self._color_hex = QColor(200, 150, 255)
		self._color_hint = QColor(180, 180, 180)

		# ── 固定尺寸，不再覆盖整个父窗口 ──
		self.setFixedSize(self.INFO_WIDTH, self.COMBINED_HEIGHT)

		self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
		self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
		self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
		self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
		self.setMouseTracking(False)

		self.hide()  # 初始隐藏，等 update_cursor 时再 show
		self.raise_()

	# ------------------------------------------------------------------
	# 外部控制
	# ------------------------------------------------------------------
	def rebind(self, scene, view):
		"""切换到新的 scene/view（窗口复用时调用）。"""
		self.scene = scene
		self.view = view
		self.cursor_scene_pos = None
		self._last_sample_pt = None
		self._cached_source_rect = None
		self._cached_sample_image = None
		# 清除字体缓存，下次 paintEvent 会根据新的 scene_rect 重新计算字号
		self._fixed_info_font = None
		self._fixed_hint_font = None
		self._fixed_info_metrics = None
		self._fixed_hint_metrics = None
		self.hide()

	def update_cursor(self, scene_pos: QPointF):
		"""记录最新的场景坐标，move() 到正确位置并 update() 重绘自身。

		整个 widget 只有 150×210 像素，update() 的脏区就是自身面积，
		不再需要按屏幕裁剪脏区、也不再有跨屏残影问题。
		show/hide 决策在此处完成，paintEvent 只负责绘制。
		"""
		self.cursor_scene_pos = QPointF(scene_pos)

		# ── 控制层决定 show / hide ──
		if not self._should_render():
			if self.isVisible():
				self.hide()
			return

		# ── 计算浮层应该 move 到的位置（父窗口本地坐标） ──
		target = self._calc_widget_pos(scene_pos)
		self.move(target)

		if not self.isVisible():
			self.show()
			self.raise_()

		# update() 只重绘 150×210 自身面积
		self.update()

	def clear_cursor(self):
		"""鼠标移出画布时隐藏放大镜。"""
		if self.cursor_scene_pos is None:
			return
		self.cursor_scene_pos = None
		self.hide()

	def refresh(self):
		"""外部状态变化时触发重绘（如选区确认后工具栏弹出）。

		控制层决定 show/hide，paintEvent 只负责绘制。
		"""
		if self.cursor_scene_pos is None:
			return
		if not self._should_render():
			if self.isVisible():
				self.hide()
			return
		self.update()

	def get_color_info_text(self) -> str:
		"""获取当前放大镜颜色信息文本（简洁格式）。"""
		image = self._background_image()
		color = self._sample_color(image)
		copy_format = "rgb_hex"
		if self.config_manager:
			try:
				copy_format = self.config_manager.get_app_setting("magnifier_color_copy_format", "rgb_hex")
			except Exception:
				copy_format = "rgb_hex"

		if copy_format == "rgb":
			return f"{color.red()}, {color.green()}, {color.blue()}"
		if copy_format == "hex":
			return f"{color.name().upper()}"
		return f"{color.red()}, {color.green()}, {color.blue()}  {color.name().upper()}"

	def copy_color_info(self) -> bool:
		"""复制当前颜色信息到剪贴板。"""
		from PySide6.QtWidgets import QApplication
		if self.cursor_scene_pos is None:
			return False
		text = self.get_color_info_text()
		if not text:
			return False
		QApplication.clipboard().setText(text)
		return True
	
	def adjust_zoom(self, delta: int):
		"""调整放大倍数
		
		Args:
			delta: 调整值，正数增加，负数减少
		"""
		self._zoom_factor += delta * 0.25
		self._zoom_factor = max(self._zoom_min, min(self._zoom_max, self._zoom_factor))
		# 保存到配置
		if self.config_manager:
			self.config_manager.qsettings.setValue("app/magnifier_zoom", self._zoom_factor)
		log_debug(f"调整倍数: {self._zoom_factor:.2f}x", "Magnifier")
		# 清空缓存，因为缩放倍数变了
		self._last_sample_pt = None
		self._cached_source_rect = None
		self._cached_sample_image = None
		self.update()
	
	def get_zoom_factor(self) -> float:
		"""获取当前放大倍数"""
		return self._zoom_factor

	# ------------------------------------------------------------------
	# QWidget 接口
	# ------------------------------------------------------------------
	@safe_event
	def paintEvent(self, event):
		if not self._should_render():
			return

		painter = QPainter(self)
		painter.setRenderHint(QPainter.RenderHint.Antialiasing)

		# 整个 widget 就是放大镜+信息框，从 (0,0) 开始绘制
		combined_rect = QRect(0, 0, self.INFO_WIDTH, self.COMBINED_HEIGHT)
		image = self._background_image()
		color = self._sample_color(image)
		self._update_sample_cache(image)

		self._draw_combined_magnifier(painter, combined_rect, color)

		painter.end()

	# ------------------------------------------------------------------
	# 绘制实现
	# ------------------------------------------------------------------
	def _get_fitted_font(self, painter: QPainter, text: str, max_width: int, 
	                     base_size: int = 11, min_size: int = 7) -> QFont:
		"""计算能在指定宽度内完整显示文字的字体大小（纯计算，无缓存）
		
		仅在 _ensure_fixed_fonts() 中调用一次，运行时不再调用。
		"""
		font = QFont("Microsoft YaHei", base_size)
		font.setBold(True)
		
		painter.setFont(font)
		metrics = painter.fontMetrics()
		text_width = metrics.horizontalAdvance(text)
		
		current_size = base_size
		while text_width > max_width and current_size > min_size:
			current_size -= 1
			font.setPointSize(current_size)
			painter.setFont(font)
			metrics = painter.fontMetrics()
			text_width = metrics.horizontalAdvance(text)
		
		return font

	def _ensure_fixed_fonts(self, painter: QPainter, text_rect_width: int):
		"""首次调用时，用虚拟桌面最大坐标值构造最长模板，一次性确定字号。
		
		之后每帧直接复用 _fixed_info_font / _fixed_hint_font，不再重算。
		多屏不同 DPI 时坐标值更大，模板更长，字号会自动缩小以适配。
		"""
		if self._fixed_info_font is not None:
			return  # 已初始化，跳过
		
		# ── 用实际虚拟桌面尺寸构造最长可能文本 ──
		scene_rect = getattr(self.scene, 'scene_rect', None)
		if scene_rect:
			# 取虚拟桌面右下角坐标（最大值）
			max_x = int(scene_rect.right())
			max_y = int(scene_rect.bottom())
		else:
			# 保守估计：8K 双屏
			max_x = 15360
			max_y = 4320
		
		# 三行中哪行最宽取决于实际像素宽度（不是字符数）
		# 用 base_size 字体测量各模板的像素宽度，取最宽的来决定字号
		worst_pos = f"POS: {max_x}, {max_y}"
		worst_rgb = "RGB: 255, 255, 255"
		worst_hex = "HEX: #FFFFFF"
		
		test_font = QFont("Microsoft YaHei", 11)
		test_font.setBold(True)
		painter.setFont(test_font)
		tm = painter.fontMetrics()
		worst_data = max(
			(worst_pos, worst_rgb, worst_hex),
			key=lambda t: tm.horizontalAdvance(t)
		)
		
		self._fixed_info_font = self._get_fitted_font(
			painter, worst_data, text_rect_width, 13, 5
		)
		
		# hint 行文本固定，单独算一次
		hint_text = self.tr("Press C to copy color info")
		self._fixed_hint_font = self._get_fitted_font(
			painter, hint_text, text_rect_width, 13, 5
		)
		
		# 缓存 metrics，后续绘制不再调 fontMetrics()
		painter.setFont(self._fixed_info_font)
		from PySide6.QtGui import QFontMetrics
		self._fixed_info_metrics = QFontMetrics(self._fixed_info_font)
		self._fixed_hint_metrics = QFontMetrics(self._fixed_hint_font)
	
	def _draw_combined_magnifier(self, painter: QPainter, rect: QRect, color: QColor):
		"""绘制合并的放大镜+信息框"""
		mag_rect = QRect(rect.x(), rect.y(), self.MAG_WIDTH, self.MAG_HEIGHT)
		info_rect = QRect(rect.x(), rect.y() + self.MAG_HEIGHT, self.INFO_WIDTH, self.INFO_HEIGHT)

		# 以宽边（MAG_WIDTH）为基准计算像素格子大小，保证正方形
		# 虚拟正方形边长 = MAG_WIDTH，源图也是正方形，1:1 映射 → 像素格子正方形
		# 显示时只取虚拟正方形的垂直居中 MAG_HEIGHT 行（相当于裁掉上下各一点）
		_ss = max(int(self.SAMPLE_SIZE / self._zoom_factor), 4)
		if _ss % 2 == 0:
			_ss += 1
		_px = self.MAG_WIDTH / _ss   # 每个像素格子的边长（正方形，宽=高）

		# 虚拟正方形中心 = MAG_WIDTH/2，垂直方向也用同样的格子大小
		# 需要从源图（_ss × _ss）中截取垂直居中的 crop_rows 行
		crop_h_px = self.MAG_HEIGHT / _px        # 可视区域对应多少行像素（float）
		crop_top  = (_ss - crop_h_px) / 2.0      # 从源图顶部裁掉多少行（float）

		# ── 1. 背景底色 ──
		painter.setPen(Qt.PenStyle.NoPen)
		painter.setBrush(self._brush_bg)
		painter.drawRoundedRect(rect, 6, 6)

		# ── 2. 放大镜图像（1:1 水平映射，垂直居中裁剪，不拉伸） ──
		painter.setBrush(Qt.BrushStyle.NoBrush)
		if self._cached_sample_image and self._cached_source_rect:
			painter.save()
			clip_path = QPainterPath()
			clip_path.addRoundedRect(rect.x(), rect.y(), rect.width(), rect.height(), 6, 6)
			painter.setClipPath(clip_path)
			painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, False)

			sr = self._cached_source_rect   # 源图矩形（_ss × _ss 个像素）
			src_crop = QRectF(
				sr.x(),
				sr.y() + crop_top,
				sr.width(),
				crop_h_px,
			)
			painter.drawImage(mag_rect, self._cached_sample_image, src_crop.toRect())

			painter.restore()

		# ── 3. 十字线（穿过中心像素中心，加粗显示） ──
		painter.setPen(self._pen_teal_2)
		painter.drawLine(mag_rect.left(), mag_rect.center().y(), mag_rect.right(), mag_rect.center().y())
		painter.drawLine(mag_rect.center().x(), mag_rect.top(), mag_rect.center().x(), mag_rect.bottom())

		# 左上角显示放大倍数（贴在外框左上角）
		zoom_text = f"{self._zoom_factor:.1f}x"
		painter.setFont(self._font)
		painter.setPen(self._pen_white_2)
		painter.setBrush(self._brush_black_a)
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
		
		# 右上角显示取色颜色方块（贴在外框右上角）
		color_box_size = 20
		color_box_rect = QRect(
			mag_rect.right() - color_box_size - 4,
			mag_rect.top() + 4,
			color_box_size,
			color_box_size
		)
		painter.setPen(self._pen_white_2)
		painter.setBrush(QBrush(color))
		painter.drawRect(color_box_rect)
		
		# 绘制分隔线
		painter.setPen(self._pen_teal_1)
		painter.drawLine(info_rect.left(), info_rect.top(), info_rect.right(), info_rect.top())
		
		# 绘制信息文本（字号在首帧由最长模板一次性确定，之后永远复用）
		painter.setBrush(Qt.BrushStyle.NoBrush)

		pos = self.cursor_scene_pos or QPointF(0, 0)
		rgb_text = f"RGB: {color.red()}, {color.green()}, {color.blue()}"
		hex_text = f"HEX: {color.name().upper()}"
		pos_text = f"POS: {int(pos.x())}, {int(pos.y())}"
		hint_text = self.tr("Press C to copy color info")

		# 定义每行文字的固定矩形区域 (宽度锁定)
		text_padding_left = 6
		text_padding_right = 3
		line_height = 20
		text_rect_width = info_rect.width() - text_padding_left - text_padding_right
		
		# 首帧：用虚拟桌面最大坐标模板一次性确定字号
		self._ensure_fixed_fonts(painter, text_rect_width)
		
		text_x = info_rect.x() + text_padding_left
		base_y = info_rect.y() + 4
		
		# POS / RGB / HEX 三行：共用 _fixed_info_font
		info_metrics = self._fixed_info_metrics
		info_text_height = info_metrics.height()
		info_descent = info_metrics.descent()
		painter.setFont(self._fixed_info_font)
		
		for text, text_color, y_pos in (
			(pos_text, self._color_pos, base_y),
			(rgb_text, self._color_rgb, base_y + line_height),
			(hex_text, self._color_hex, base_y + line_height * 2),
		):
			painter.setPen(text_color)
			y_centered = y_pos + (line_height + info_text_height) // 2 - info_descent
			painter.drawText(text_x, y_centered, text)
		
		# hint 行：单独字号
		painter.setFont(self._fixed_hint_font)
		painter.setPen(self._color_hint)
		hint_metrics = self._fixed_hint_metrics
		y_hint = base_y + line_height * 3
		y_centered = y_hint + (line_height + hint_metrics.height()) // 2 - hint_metrics.descent()
		painter.drawText(text_x, y_centered, hint_text)

		# ── 最终外框描边（最后绘制，保证在所有内容之上） ──
		painter.setPen(self._pen_teal_2)
		painter.setBrush(Qt.BrushStyle.NoBrush)
		painter.drawRoundedRect(rect, 6, 6)


	# ------------------------------------------------------------------
	# 数据准备
	# ------------------------------------------------------------------
	def _should_render(self) -> bool:
		if self.cursor_scene_pos is None or not self.scene or not self.view:
			return False
		view = self.view
		if view.is_drawing:
			return False
		if view._text_drag_active:
			return False
		try:
			if view.smart_edit_controller.layer_editor.dragging_handle:
				return False
		except AttributeError:
			pass
		current_tool = self.scene.tool_controller.current_tool
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

	def _update_sample_cache(self, image) -> None:
		"""根据当前光标位置更新采样区域缓存（_cached_source_rect / _cached_sample_image）。"""
		if image is None or self.cursor_scene_pos is None:
			return
		
		# 使用像素中心坐标进行采样，让准星对齐到像素中心
		center_pos = self._scene_to_pixel_center(self.cursor_scene_pos)
		if not center_pos:
			return
		pt = self._scene_to_image_point(center_pos)
		if not pt:
			return
		
		# 像素坐标没变就跳过，复用已有缓存
		if pt == self._last_sample_pt and self._cached_sample_image is not None:
			return
		
		# 根据放大倍数动态调整采样区域大小
		# 确保采样区域为奇数大小，这样中心像素正好在中间
		sample_size = max(int(self.SAMPLE_SIZE / self._zoom_factor), 4)
		if sample_size % 2 == 0:
			sample_size += 1  # 确保是奇数
		half = sample_size // 2
		src = QRect(pt.x() - half, pt.y() - half, sample_size, sample_size)
		src = src.intersected(image.rect())
		if src.width() <= 0 or src.height() <= 0:
			return
		
		# 缓存源图像引用和采样区域，让 drawImage 自动处理缩放
		self._last_sample_pt = QPoint(pt)
		self._cached_source_rect = QRect(src)
		self._cached_sample_image = image

	def _calc_widget_pos(self, scene_pos: QPointF) -> QPoint:
		"""计算浮层在父窗口内的 move() 目标位置。

		场景坐标 == 全局屏幕坐标（CaptureService 虚拟桌面），
		先转成父窗口本地坐标，再按 margin / 边界翻转。
		"""
		parent = self.parentWidget()
		if parent is None:
			return QPoint(0, 0)

		global_pt = QPoint(round(scene_pos.x()), round(scene_pos.y()))
		local = parent.mapFromGlobal(global_pt)

		margin = self.EDGE_MARGIN
		w = self.INFO_WIDTH
		h = self.COMBINED_HEIGHT
		pw = parent.width()
		ph = parent.height()

		# 默认：鼠标右下方
		x = local.x() + margin
		y = local.y() + margin

		# 右侧溢出 → 左侧
		if x + w > pw - margin:
			x = max(margin, local.x() - w - margin)

		# 下方溢出 → 上方
		if y + h > ph - margin:
			y = max(margin, local.y() - h - margin)

		return QPoint(x, y)
 