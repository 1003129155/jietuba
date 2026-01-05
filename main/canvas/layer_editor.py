"""
图层编辑器 - LayerEditor
基于“图层数据模型 / QGraphicsItem”的 8 控制点编辑系统

功能：
1) 8 个调整控制点（四角 + 四边）
2) 命中测试 / 悬停状态 / 光标样式
3) 拖拽调整几何（默认实现）
4) 支持撤销：end_drag 返回 old_state / new_state（dict）
5) render() 直接用 QPainter 画控制点（轻量）
"""

from __future__ import annotations

import math
import os
from dataclasses import dataclass
from enum import Enum
from typing import Optional, List, Tuple, Dict, Any, Union

from PyQt6.QtCore import QPointF, QRectF, Qt
from PyQt6.QtGui import QPainter, QPen, QBrush, QColor, QTransform, QPixmap, QCursor
from PyQt6.QtWidgets import QGraphicsTextItem
from PyQt6.QtSvg import QSvgRenderer

from core import log_debug, log_warning
from core.logger import log_exception

try:
    # 你项目里的撤销命令
    from .undo import EditItemCommand
except Exception:
    EditItemCommand = None  # 允许单文件测试

try:
    # 导入资源管理器
    from core.resource_manager import ResourceManager
except Exception:
    ResourceManager = None


class HandleType(Enum):
    """控制点类型（8点）"""
    CORNER_TL = "corner_tl"  # 左上
    CORNER_TR = "corner_tr"  # 右上
    CORNER_BR = "corner_br"  # 右下
    CORNER_BL = "corner_bl"  # 左下

    EDGE_T = "edge_t"        # 上边
    EDGE_R = "edge_r"        # 右边
    EDGE_B = "edge_b"        # 下边
    EDGE_L = "edge_l"        # 左边
    ROTATE = "rotate"        # 旋转手柄
    ARROW_START = "arrow_start"  # 箭头起点
    ARROW_END = "arrow_end"      # 箭头终点


@dataclass
class EditHandle:
    """编辑控制点（坐标系由调用方保证一致：推荐 scene 坐标）"""
    id: int
    handle_type: HandleType
    position: QPointF
    cursor: Union[Qt.CursorShape, QCursor]  # 支持内置光标和自定义光标
    size: int = 8
    hit_area_padding: int = 8  # 命中判定扩展区域（增加可点击范围）

    def get_rect(self) -> QRectF:
        """获取显示区域（实际绘制大小）"""
        half = self.size / 2
        return QRectF(
            self.position.x() - half,
            self.position.y() - half,
            self.size,
            self.size,
        )
    
    def get_hit_rect(self) -> QRectF:
        """获取判定区域（比显示区域大，更容易点击）"""
        half = (self.size + self.hit_area_padding * 2) / 2
        return QRectF(
            self.position.x() - half,
            self.position.y() - half,
            self.size + self.hit_area_padding * 2,
            self.size + self.hit_area_padding * 2,
        )

    def contains(self, pos: QPointF) -> bool:
        """命中检测（使用扩大的判定区域）"""
        return self.get_hit_rect().contains(pos)


class LayerEditor:
    """
    图层编辑器 - 统一的图层编辑控制点管理

    典型用法（推荐在 QGraphicsView.drawForeground 中 render）：
    - start_edit(layer)
    - update_hover(scene_pos) / get_cursor(scene_pos)
    - hit = hit_test(scene_pos)
    - start_drag(hit, scene_pos)
    - drag_to(scene_pos, keep_ratio=shift_pressed)
    - old_state, new_state = end_drag(undo_stack)
    - render(painter)  # painter 需在 scene 坐标系
    """

    # 视觉配置
    HANDLE_SIZE = 10                        # 控制点大小（增大到 10px）
    HANDLE_BORDER_WIDTH = 2                 # 边框宽度
    HANDLE_COLOR = QColor(0, 123, 255)      # 蓝色描边（更现代的蓝色）
    HANDLE_FILL = QColor(255, 255, 255)     # 白色填充
    HOVER_COLOR = QColor(0, 160, 255)       # 浅蓝色（hover）
    HOVER_FILL = QColor(0, 123, 255)        # hover时填充蓝色
    ROTATE_HANDLE_COLOR = QColor(0, 200, 83)  # 旋转手柄颜色（绿色，匹配SVG）
    ROTATE_HANDLE_SIZE = 23                 # 旋转手柄大小（与SVG相匹配）
    ROTATE_HANDLE_OFFSET = 35               # 旋转手柄离顶部距离（不再使用）
    ROTATE_LINE_COLOR = QColor(0, 123, 255, 100)  # 连接线颜色（半透明）
    
    # 旋转光标（类变量，延迟加载）
    _rotate_cursor: Optional[QCursor] = None

    def __init__(self):
        self.active_layer: Optional[Any] = None
        self.handles: List[EditHandle] = []

        # 特殊模式：标号(NumberItem) 使用独立编辑样式
        self._number_item_mode = False

        self.hovered_handle: Optional[EditHandle] = None
        self.dragging_handle: Optional[EditHandle] = None
        self.drag_start_pos: Optional[QPointF] = None

        # 撤销/基准状态
        self.initial_layer_state: Optional[Dict[str, Any]] = None

        # 用于“每次拖拽都从起始几何计算”，避免累计误差
        self._base_scene_rect: Optional[QRectF] = None  # 起始的 scene 包围盒
        self._base_local_rect: Optional[QRectF] = None  # 起始的 local rect（若支持）
        self._base_pos: Optional[QPointF] = None        # 起始 pos（若支持）
        self._base_transform: Optional[QTransform] = None  # 起始 transform（若支持）
        self._base_rotation = None
        self._rotation_origin_local = None
        self._arrow_base_start_local: Optional[QPointF] = None
        self._arrow_base_end_local: Optional[QPointF] = None
        self._arrow_base_start_scene: Optional[QPointF] = None
        self._arrow_base_end_scene: Optional[QPointF] = None
        
        # 初始化旋转光标
        self._ensure_rotate_cursor()

    # =========================================================================
    #  旋转光标
    # =========================================================================
    
    @classmethod
    def _ensure_rotate_cursor(cls):
        """确保旋转光标已加载（类方法，所有实例共享）"""
        if cls._rotate_cursor is not None:
            return
        
        # 尝试加载旋转SVG图标
        if ResourceManager:
            svg_path = ResourceManager.get_resource_path("svg/旋转.svg")
        else:
            # 回退方式
            svg_path = os.path.join(os.path.dirname(__file__), '..', '..', 'svg', '旋转.svg')
        
        if not os.path.exists(svg_path):
            # 回退到默认光标
            cls._rotate_cursor = QCursor(Qt.CursorShape.OpenHandCursor)
            log_warning(f"旋转光标SVG未找到: {svg_path}，使用默认光标", "LayerEditor")
            return
        
        try:
            # 使用QSvgRenderer加载SVG
            renderer = QSvgRenderer(svg_path)
            if not renderer.isValid():
                cls._rotate_cursor = QCursor(Qt.CursorShape.OpenHandCursor)
                log_warning("旋转光标SVG无效，使用默认光标", "LayerEditor")
                return
            
            # 创建pixmap（23x23像素，与ROTATE_HANDLE_SIZE匹配）
            size = 23
            pixmap = QPixmap(size, size)
            pixmap.fill(Qt.GlobalColor.transparent)
            
            # 渲染SVG到pixmap
            painter = QPainter(pixmap)
            renderer.render(painter)
            painter.end()
            
            # 添加白色描边
            outlined_pixmap = QPixmap(pixmap.size())
            outlined_pixmap.fill(Qt.GlobalColor.transparent)
            
            outline_painter = QPainter(outlined_pixmap)
            outline_painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            
            # 绘制白色描边（通过在周围8个方向绘制白色版本）
            outline_offset = 2  # 描边宽度
            white_color = QColor(255, 255, 255, 255)
            
            for dx in [-outline_offset, 0, outline_offset]:
                for dy in [-outline_offset, 0, outline_offset]:
                    if dx == 0 and dy == 0:
                        continue  # 跳过中心位置
                    # 创建白色版本的图标
                    white_pixmap = QPixmap(pixmap.size())
                    white_pixmap.fill(Qt.GlobalColor.transparent)
                    white_painter = QPainter(white_pixmap)
                    white_painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
                    white_painter.drawPixmap(0, 0, pixmap)
                    white_painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceIn)
                    white_painter.fillRect(white_pixmap.rect(), white_color)
                    white_painter.end()
                    
                    outline_painter.drawPixmap(dx, dy, white_pixmap)
            
            # 最后绘制原始图标在中心
            outline_painter.drawPixmap(0, 0, pixmap)
            outline_painter.end()
            
            # 创建光标（热点在中心）
            cls._rotate_cursor = QCursor(outlined_pixmap, size // 2, size // 2)
            log_debug(f"旋转光标已加载: {svg_path}", "LayerEditor")
        except Exception as e:
            cls._rotate_cursor = QCursor(Qt.CursorShape.OpenHandCursor)
            log_warning(f"加载旋转光标失败: {e}，使用默认光标", "LayerEditor")
    
    @classmethod
    def get_rotate_cursor(cls) -> QCursor:
        """获取旋转光标"""
        if cls._rotate_cursor is None:
            cls._ensure_rotate_cursor()
        return cls._rotate_cursor

    # =========================================================================
    #  编辑会话
    # =========================================================================

    def start_edit(self, layer: Any) -> bool:
        """开始编辑某个图层"""
        if not layer:
            self.stop_edit()
            return False

        self.active_layer = layer
        self._number_item_mode = self._is_number_item(layer)
        self.handles = self._generate_handles(layer)

        if not self.handles and not self._number_item_mode:
            self.stop_edit()
            return False

        return True

    def stop_edit(self):
        """停止编辑"""
        self.active_layer = None
        self.handles = []
        self._number_item_mode = False
        self.hovered_handle = None
        self.dragging_handle = None
        self.drag_start_pos = None
        self.initial_layer_state = None

        self._base_scene_rect = None
        self._base_local_rect = None
        self._base_pos = None
        self._base_transform = None
        self._base_rotation = None
        self._rotation_origin_local = None
        self._arrow_base_start_local = None
        self._arrow_base_end_local = None
        self._arrow_base_start_scene = None
        self._arrow_base_end_scene = None
        self._arrow_base_start_local = None
        self._arrow_base_end_local = None
        self._arrow_base_start_scene = None
        self._arrow_base_end_scene = None

    def is_editing(self) -> bool:
        return self.active_layer is not None

    # =========================================================================
    #  控制点生成
    # =========================================================================

    def _generate_handles(self, layer: Any) -> List[EditHandle]:
        """
        为图层生成控制点：
        - 若 layer 实现 get_edit_handles()，优先调用它（返回 List[EditHandle]）
        - 否则：基于“scene 包围盒”生成 8 控制点（最稳，适配 QGraphicsItem）
        """
        if hasattr(layer, "get_edit_handles") and not self._number_item_mode:
            handles = layer.get_edit_handles()
            return handles or []

        if self._is_arrow_item(layer):
            arrow_handles = self._generate_arrow_handles(layer)
            if arrow_handles:
                return arrow_handles

        rect = self._get_scene_rect(layer)
        if isinstance(rect, QRectF) and rect.isValid():
            if self._number_item_mode:
                return self._generate_number_handles(rect)
            return self._generate_rect_handles(rect)

        # 文本图元自带编辑体验，这里不生成控制点
        if isinstance(layer, QGraphicsTextItem):
            return []
        return []

    def _generate_number_handles(self, rect: QRectF) -> List[EditHandle]:
        """序号工具：仅保留左上角旋转手柄"""
        rotate_cursor = self.get_rotate_cursor()
        return [EditHandle(0, HandleType.ROTATE, rect.topLeft(), rotate_cursor, self.ROTATE_HANDLE_SIZE)]

    def _is_number_item(self, layer: Any) -> bool:
        """检测当前图层是否为序号图元"""
        try:
            from .items import NumberItem
        except Exception:
            NumberItem = None
        return bool(NumberItem and isinstance(layer, NumberItem))

    def _is_arrow_item(self, layer: Any) -> bool:
        """检测是否为箭头图元"""
        try:
            from .items import ArrowItem
        except Exception:
            ArrowItem = None
        return bool(ArrowItem and isinstance(layer, ArrowItem))

    def _generate_arrow_handles(self, layer: Any) -> List[EditHandle]:
        """为箭头生成起点/终点控制柄"""
        start = self._map_arrow_point_to_scene(layer, getattr(layer, "start_pos", None))
        end = self._map_arrow_point_to_scene(layer, getattr(layer, "end_pos", None))
        if not isinstance(start, QPointF) or not isinstance(end, QPointF):
            return []

        size = self.HANDLE_SIZE + 2
        return [
            EditHandle(100, HandleType.ARROW_START, start, Qt.CursorShape.CrossCursor, size),
            EditHandle(101, HandleType.ARROW_END, end, Qt.CursorShape.CrossCursor, size),
        ]

    def _map_arrow_point_to_scene(self, layer: Any, point: Optional[QPointF]) -> Optional[QPointF]:
        if not isinstance(point, QPointF):
            return None
        if hasattr(layer, "mapToScene"):
            try:
                mapped = layer.mapToScene(QPointF(point))
                return QPointF(mapped)
            except Exception as e:
                log_exception(e, "映射箭头点到场景坐标")
        return QPointF(point)

    def _get_scene_rect(self, layer: Any) -> Optional[QRectF]:
        """
        获取 layer 的 scene 包围盒（强烈推荐在 Pin/Scene 架构里使用 sceneBoundingRect）

        支持：
        - QGraphicsItem：sceneBoundingRect()
        - 数据层：layer.rect（你保证其坐标系与调用方一致）
        """
        if layer is None:
            return None

        # QGraphicsItem：最稳（包含 pos/transform/scale 后的包围盒）
        if hasattr(layer, "sceneBoundingRect") and callable(getattr(layer, "sceneBoundingRect")):
            try:
                return QRectF(layer.sceneBoundingRect())
            except Exception as e:
                log_exception(e, "获取sceneBoundingRect")

        # 数据层 rect（RectLayer 等）
        rect_attr = getattr(layer, "rect", None)
        if isinstance(rect_attr, QRectF):
            return QRectF(rect_attr)

        return None

    def _generate_rect_handles(self, rect: QRectF) -> List[EditHandle]:
        """为矩形（scene 包围盒）生成 8 个控制点"""
        hs = self.HANDLE_SIZE
        handles: List[EditHandle] = []

        # 左上角改为旋转手柄（使用更大的尺寸和自定义光标）
        rotate_cursor = self.get_rotate_cursor()
        handles.append(EditHandle(0, HandleType.ROTATE, rect.topLeft(), rotate_cursor, self.ROTATE_HANDLE_SIZE))
        
        # 其他三个角
        handles.append(EditHandle(1, HandleType.CORNER_TR, rect.topRight(), Qt.CursorShape.SizeBDiagCursor, hs))
        handles.append(EditHandle(2, HandleType.CORNER_BR, rect.bottomRight(), Qt.CursorShape.SizeFDiagCursor, hs))
        handles.append(EditHandle(3, HandleType.CORNER_BL, rect.bottomLeft(), Qt.CursorShape.SizeBDiagCursor, hs))

        # 四边
        handles.append(EditHandle(4, HandleType.EDGE_T, QPointF(rect.center().x(), rect.top()), Qt.CursorShape.SizeVerCursor, hs))
        handles.append(EditHandle(5, HandleType.EDGE_R, QPointF(rect.right(), rect.center().y()), Qt.CursorShape.SizeHorCursor, hs))
        handles.append(EditHandle(6, HandleType.EDGE_B, QPointF(rect.center().x(), rect.bottom()), Qt.CursorShape.SizeVerCursor, hs))
        handles.append(EditHandle(7, HandleType.EDGE_L, QPointF(rect.left(), rect.center().y()), Qt.CursorShape.SizeHorCursor, hs))

        return handles

    # =========================================================================
    #  命中/悬停/光标
    # =========================================================================

    def hit_test(self, pos: QPointF) -> Optional[EditHandle]:
        """命中测试：鼠标是否点到某个控制点（pos 需与 handle.position 同坐标系，推荐 scene）"""
        for h in self.handles:
            if h.contains(pos):
                return h
        return None

    def update_hover(self, pos: QPointF):
        self.hovered_handle = self.hit_test(pos)

    def get_cursor(self, pos: QPointF) -> Union[Qt.CursorShape, QCursor]:
        """获取鼠标光标（支持内置和自定义光标）"""
        h = self.hit_test(pos)
        if h:
            return h.cursor
        return Qt.CursorShape.ArrowCursor

    # =========================================================================
    #  拖拽
    # =========================================================================

    def start_drag(self, handle: EditHandle, pos: QPointF):
        """开始拖拽控制点（pos 推荐 scene 坐标）"""
        if not self.is_editing():
            return

        self.dragging_handle = handle
        self.drag_start_pos = pos

        # 保存撤销用“旧状态”
        self.initial_layer_state = self._copy_layer_state(self.active_layer)

        # 保存基准几何：scene 包围盒（所有类型通用）
        self._base_scene_rect = self._get_scene_rect(self.active_layer)

        # 保存 QGraphicsItem 的基础状态（若存在）
        if hasattr(self.active_layer, "pos") and callable(getattr(self.active_layer, "pos")):
            try:
                p = self.active_layer.pos()
                self._base_pos = QPointF(p.x(), p.y())
            except Exception:
                self._base_pos = None

        if hasattr(self.active_layer, "transform") and callable(getattr(self.active_layer, "transform")):
            try:
                self._base_transform = QTransform(self.active_layer.transform())
            except Exception:
                self._base_transform = None

        # 保存 local rect（若是 rect()/setRect() 体系）
        self._base_local_rect = self._get_local_rect(self.active_layer)

        self._base_rotation = None
        self._rotation_origin_local = None
        if hasattr(self.active_layer, "rotation") and callable(getattr(self.active_layer, "rotation")):
            try:
                self._base_rotation = float(self.active_layer.rotation())
            except Exception:
                self._base_rotation = None

        if handle.handle_type == HandleType.ROTATE and self._base_scene_rect is not None:
            if hasattr(self.active_layer, "mapFromScene") and callable(getattr(self.active_layer, "mapFromScene")):
                try:
                    local_center = self.active_layer.mapFromScene(self._base_scene_rect.center())
                    self._rotation_origin_local = QPointF(local_center.x(), local_center.y())
                    if hasattr(self.active_layer, "setTransformOriginPoint"):
                        self.active_layer.setTransformOriginPoint(self._rotation_origin_local)
                except Exception:
                    self._rotation_origin_local = None

        if self._is_arrow_item(self.active_layer):
            self._capture_arrow_base_geometry(self.active_layer)

    def drag_to(self, pos: QPointF, keep_ratio: bool = False):
        """拖拽到新位置（pos 推荐 scene 坐标）"""
        if not self.dragging_handle or not self.drag_start_pos or not self.is_editing():
            return

        delta_scene = pos - self.drag_start_pos

        # 每次拖拽先恢复到基准状态，避免累计误差
        self._restore_base_state(self.active_layer)

        # 应用拖拽
        self._apply_handle_drag(self.active_layer, self.dragging_handle, delta_scene, keep_ratio)

        # 更新控制点
        self.handles = self._generate_handles(self.active_layer)

    def end_drag(
        self,
        undo_stack: Optional[Any] = None,
    ) -> Tuple[Optional[Dict[str, Any]], Optional[Dict[str, Any]]]:
        """结束拖拽：返回 (old_state, new_state)，可选自动推入撤销栈"""
        if not self.is_editing():
            return None, None

        old_state = self.initial_layer_state
        new_state = self._copy_layer_state(self.active_layer)

        self.dragging_handle = None
        self.drag_start_pos = None
        self.initial_layer_state = None

        self._base_scene_rect = None
        self._base_local_rect = None
        self._base_pos = None
        self._base_transform = None
        self._base_rotation = None
        self._rotation_origin_local = None

        if (
            undo_stack is not None
            and EditItemCommand is not None
            and old_state is not None
            and new_state is not None
            and self.active_layer is not None
        ):
            try:
                if old_state != new_state:
                    cmd = EditItemCommand(self.active_layer, old_state, new_state)
                    if hasattr(undo_stack, "push_command"):
                        undo_stack.push_command(cmd)
                    elif hasattr(undo_stack, "push"):
                        undo_stack.push(cmd)
            except Exception as exc:
                log_warning(f"push undo failed: {exc}", "LayerEditor")

        return old_state, new_state

    # =========================================================================
    #  拖拽算法（默认实现）
    # =========================================================================

    def _apply_handle_drag(self, layer: Any, handle: EditHandle, delta_scene: QPointF, keep_ratio: bool):
        """
        应用控制点拖拽到图层

        优先级：
        1) layer.apply_handle_drag(handle_id, delta, keep_ratio)
        2) 旋转手柄：直接旋转图层
        3) StrokeItem（路径类）：缩放 transform
        4) rect()/setRect()：修改 local rect（推荐 RectItem/EllipseItem）
        5) 数据层 rect：直接改 rect
        """
        if hasattr(layer, "apply_handle_drag"):
            layer.apply_handle_drag(handle.id, delta_scene, keep_ratio)
            return

        if handle.handle_type == HandleType.ROTATE:
            self._apply_rotation(layer, delta_scene)
            return

        # ---- 2) StrokeItem（路径类） ----
        # 你项目里是 canvas.items.StrokeItem
        try:
            from .items import StrokeItem  # 改为相对导入
            if isinstance(layer, StrokeItem):
                self._apply_stroke_item_drag(layer, handle, delta_scene, keep_ratio)
                return
        except Exception as e:
            log_exception(e, "导入StrokeItem")

        if self._is_arrow_item(layer) and handle.handle_type in (HandleType.ARROW_START, HandleType.ARROW_END):
            self._apply_arrow_item_drag(layer, handle.handle_type, delta_scene, keep_ratio)
            return

        # ---- 3) rect()/setRect() ----
        local_rect = self._get_local_rect(layer)
        if isinstance(local_rect, QRectF) and self._base_local_rect is not None and hasattr(layer, "mapFromScene"):
            # 将 scene delta 映射到 local delta（更靠谱）
            try:
                p0 = layer.mapFromScene(self.drag_start_pos)  # type: ignore
                p1 = layer.mapFromScene(self.drag_start_pos + delta_scene)  # type: ignore
                delta_local = QPointF(p1.x() - p0.x(), p1.y() - p0.y())
            except Exception as e:
                log_exception(e, "映射scene delta到local")
                delta_local = delta_scene

            new_rect = QRectF(self._base_local_rect)
            self._apply_rect_delta(new_rect, handle.handle_type, delta_local, keep_ratio)
            self._set_local_rect(layer, new_rect.normalized())
            return

        # ---- 4) 数据层 rect ----
        scene_rect = self._get_scene_rect(layer)
        if isinstance(scene_rect, QRectF) and self._base_scene_rect is not None:
            new_scene = QRectF(self._base_scene_rect)
            self._apply_rect_delta(new_scene, handle.handle_type, delta_scene, keep_ratio)
            # 数据层 rect 直接写回（你保证坐标系一致）
            if hasattr(layer, "rect"):
                try:
                    layer.rect = new_scene.normalized()
                except Exception as e:
                    log_exception(e, "设置数据层rect")

    def _apply_rect_delta(self, rect: QRectF, handle_type: HandleType, delta: QPointF, keep_ratio: bool):
        """对一个 QRectF 应用拖拽 delta（delta 与 rect 同坐标系）"""
        if handle_type == HandleType.CORNER_TL:
            rect.setTopLeft(rect.topLeft() + delta)
        elif handle_type == HandleType.CORNER_TR:
            rect.setTopRight(rect.topRight() + delta)
        elif handle_type == HandleType.CORNER_BR:
            rect.setBottomRight(rect.bottomRight() + delta)
        elif handle_type == HandleType.CORNER_BL:
            rect.setBottomLeft(rect.bottomLeft() + delta)
        elif handle_type == HandleType.EDGE_T:
            rect.setTop(rect.top() + delta.y())
        elif handle_type == HandleType.EDGE_R:
            rect.setRight(rect.right() + delta.x())
        elif handle_type == HandleType.EDGE_B:
            rect.setBottom(rect.bottom() + delta.y())
        elif handle_type == HandleType.EDGE_L:
            rect.setLeft(rect.left() + delta.x())

        # keep_ratio（Shift）你后续如果要做：
        # - 仅对 CORNER_* 生效
        # - 以 base 宽高比为目标，调整 x/y 中较小变化 or 以主轴为准
        # 这里先不强行加，避免引入新 bug

    def _apply_rotation(self, layer: Any, delta_scene: QPointF):
        if self._base_scene_rect is None or self.drag_start_pos is None:
            return

        center = self._base_scene_rect.center()
        start_vec = QPointF(self.drag_start_pos.x() - center.x(), self.drag_start_pos.y() - center.y())
        current_pos = self.drag_start_pos + delta_scene
        end_vec = QPointF(current_pos.x() - center.x(), current_pos.y() - center.y())

        if math.isclose(start_vec.x(), 0.0, abs_tol=1e-4) and math.isclose(start_vec.y(), 0.0, abs_tol=1e-4):
            return

        angle_start = math.degrees(math.atan2(start_vec.y(), start_vec.x()))
        angle_end = math.degrees(math.atan2(end_vec.y(), end_vec.x()))
        delta_angle = angle_end - angle_start
        if hasattr(layer, "setRotation") and hasattr(layer, "rotation"):
            base_rot = self._base_rotation if self._base_rotation is not None else float(layer.rotation())
            try:
                layer.setRotation(base_rot + delta_angle)
            except Exception as e:
                log_exception(e, "设置旋转角度")
            if self._rotation_origin_local is not None and hasattr(layer, "setTransformOriginPoint"):
                try:
                    layer.setTransformOriginPoint(self._rotation_origin_local)
                except Exception as e:
                    log_exception(e, "设置旋转原点")
            if hasattr(layer, "update"):
                layer.update()
            return

        base_transform = QTransform(self._base_transform) if self._base_transform is not None else QTransform()
        t = QTransform()
        t.translate(center.x(), center.y())
        t.rotate(delta_angle)
        t.translate(-center.x(), -center.y())
        if hasattr(layer, "setTransform"):
            try:
                layer.setTransform(t * base_transform)
            except Exception as e:
                log_exception(e, "设置变换矩阵")
        if hasattr(layer, "update"):
            layer.update()

    def _apply_stroke_item_drag(self, layer: Any, handle: EditHandle, delta_scene: QPointF, keep_ratio: bool):
        """
        对 StrokeItem 应用控制点拖拽：通过 setTransform() 缩放路径

        用 base_scene_rect -> new_scene_rect 计算缩放比，
        再把缩放中心从 scene 映射到 local 做 transform（稳定且不依赖 path 重建）
        """
        if self._base_scene_rect is None or not self._base_scene_rect.isValid():
            return

        new_scene = QRectF(self._base_scene_rect)
        self._apply_rect_delta(new_scene, handle.handle_type, delta_scene, keep_ratio)
        new_scene = new_scene.normalized()

        w0, h0 = self._base_scene_rect.width(), self._base_scene_rect.height()
        w1, h1 = new_scene.width(), new_scene.height()
        if w0 <= 0 or h0 <= 0 or w1 <= 0 or h1 <= 0:
            return

        sx = w1 / w0
        sy = h1 / h0
        if keep_ratio:
            s = min(sx, sy)
            sx = sy = s

        # 将缩放中心从 scene 转到 item local
        try:
            c0_local = layer.mapFromScene(self._base_scene_rect.center())
            c1_local = layer.mapFromScene(new_scene.center())
        except Exception:
            # 兜底：用 scene delta
            c0_local = QPointF(0, 0)
            c1_local = QPointF(delta_scene.x(), delta_scene.y())

        # 基于起始 transform 做变换（每次先 restore_base_state，所以这里可直接 setTransform）
        t = QTransform()
        t.translate(c1_local.x(), c1_local.y())
        t.scale(sx, sy)
        t.translate(-c0_local.x(), -c0_local.y())

        layer.setTransform(t)
        if hasattr(layer, "update"):
            layer.update()

    def _capture_arrow_base_geometry(self, layer: Any):
        start_local = getattr(layer, "start_pos", None)
        end_local = getattr(layer, "end_pos", None)

        if isinstance(start_local, QPointF):
            self._arrow_base_start_local = QPointF(start_local)
            if hasattr(layer, "mapToScene"):
                try:
                    mapped = layer.mapToScene(QPointF(start_local))
                    self._arrow_base_start_scene = QPointF(mapped)
                except Exception:
                    self._arrow_base_start_scene = QPointF(start_local)
            else:
                self._arrow_base_start_scene = QPointF(start_local)
        else:
            self._arrow_base_start_local = None
            self._arrow_base_start_scene = None

        if isinstance(end_local, QPointF):
            self._arrow_base_end_local = QPointF(end_local)
            if hasattr(layer, "mapToScene"):
                try:
                    mapped = layer.mapToScene(QPointF(end_local))
                    self._arrow_base_end_scene = QPointF(mapped)
                except Exception:
                    self._arrow_base_end_scene = QPointF(end_local)
            else:
                self._arrow_base_end_scene = QPointF(end_local)
        else:
            self._arrow_base_end_local = None
            self._arrow_base_end_scene = None

    def _apply_arrow_item_drag(self, layer: Any, handle_type: HandleType, delta_scene: QPointF, keep_ratio: bool):
        if not hasattr(layer, "set_positions"):
            return

        start_local = self._arrow_base_start_local or getattr(layer, "start_pos", None)
        end_local = self._arrow_base_end_local or getattr(layer, "end_pos", None)
        if not isinstance(start_local, QPointF) or not isinstance(end_local, QPointF):
            return

        delta_scene = QPointF(delta_scene)

        def _scene_sum(base_scene: Optional[QPointF]) -> Optional[QPointF]:
            if not isinstance(base_scene, QPointF):
                return None
            return QPointF(base_scene.x() + delta_scene.x(), base_scene.y() + delta_scene.y())

        if handle_type == HandleType.ARROW_START:
            target_scene = _scene_sum(self._arrow_base_start_scene)
            if isinstance(target_scene, QPointF) and hasattr(layer, "mapFromScene"):
                try:
                    new_start = layer.mapFromScene(target_scene)
                except Exception:
                    new_start = QPointF(start_local.x() + delta_scene.x(), start_local.y() + delta_scene.y())
            else:
                new_start = QPointF(start_local.x() + delta_scene.x(), start_local.y() + delta_scene.y())

            layer.set_positions(QPointF(new_start), QPointF(end_local))
        elif handle_type == HandleType.ARROW_END:
            target_scene = _scene_sum(self._arrow_base_end_scene)
            if isinstance(target_scene, QPointF) and hasattr(layer, "mapFromScene"):
                try:
                    new_end = layer.mapFromScene(target_scene)
                except Exception:
                    new_end = QPointF(end_local.x() + delta_scene.x(), end_local.y() + delta_scene.y())
            else:
                new_end = QPointF(end_local.x() + delta_scene.x(), end_local.y() + delta_scene.y())

            layer.set_positions(QPointF(start_local), QPointF(new_end))

        # keep_ratio 暂不对箭头做额外处理，避免误缩放

        if hasattr(layer, "update"):
            layer.update()

    # =========================================================================
    #  渲染
    # =========================================================================

    def render(self, painter: QPainter):
        """
        渲染编辑控制点

        [WARN] painter 必须与 handle.position 使用同一坐标系：
        - 推荐：在 QGraphicsView.drawForeground(painter, rect) 中调用，
          这时 painter 在 scene 坐标系
        """
        if not self.is_editing():
            return

        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        # 序号工具：额外绘制虚线圈
        if self._number_item_mode and self.active_layer is not None:
            rect = self._get_scene_rect(self.active_layer)
            if isinstance(rect, QRectF) and rect.isValid():
                dash_pen = QPen(QColor(0, 160, 255), 2, Qt.PenStyle.DashLine)
                dash_pen.setDashPattern([6, 4])
                dash_pen.setCosmetic(True)
                painter.setPen(dash_pen)
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawEllipse(rect)

        # 分离旋转手柄和普通手柄
        rotate_handle = None
        normal_handles = []
        
        for h in self.handles:
            if h.handle_type == HandleType.ROTATE:
                rotate_handle = h
            else:
                normal_handles.append(h)
        
        # 旋转手柄不再需要连接线（因为它在左上角）
        
        # 绘制普通控制点（方形）
        for h in normal_handles:
            is_hovered = self.hovered_handle is not None and self.hovered_handle.id == h.id
            
            if is_hovered:
                # hover 状态：蓝色填充
                painter.setPen(QPen(self.HOVER_COLOR, self.HANDLE_BORDER_WIDTH))
                painter.setBrush(QBrush(self.HOVER_FILL))
            else:
                # 正常状态：白色填充，蓝色边框
                painter.setPen(QPen(self.HANDLE_COLOR, self.HANDLE_BORDER_WIDTH))
                painter.setBrush(QBrush(self.HANDLE_FILL))
            
            painter.drawRect(h.get_rect())
        
        # 绘制旋转手柄（使用SVG图标样式）
        if rotate_handle:
            self._render_rotate_handle(painter, rotate_handle)

        painter.restore()
    
    def _render_rotate_handle(self, painter: QPainter, handle: EditHandle):
        """渲染旋转手柄（SVG样式）"""
        is_hovered = self.hovered_handle is not None and self.hovered_handle.id == handle.id
        center = handle.position

        if self._number_item_mode:
            self._render_number_rotate_handle(painter, center, is_hovered)
            return
        
        # 尝试加载并渲染SVG
        if ResourceManager:
            svg_path = ResourceManager.get_resource_path("svg/旋转.svg")
        else:
            svg_path = os.path.join(os.path.dirname(__file__), '..', '..', 'svg', '旋转.svg')
        
        if os.path.exists(svg_path):
            try:
                renderer = QSvgRenderer(svg_path)
                if renderer.isValid():
                    # SVG视图框大小 (从SVG的viewBox="4.5 4.5 23 23"得知)
                    size = self.ROTATE_HANDLE_SIZE
                    
                    # 计算渲染矩形（中心对齐）
                    half_size = size / 2
                    render_rect = QRectF(
                        center.x() - half_size,
                        center.y() - half_size,
                        size,
                        size
                    )
                    
                    # 如果悬停，绘制高亮背景
                    if is_hovered:
                        painter.setPen(Qt.PenStyle.NoPen)
                        painter.setBrush(QBrush(QColor(0, 160, 255, 80)))  # 半透明蓝色背景
                        painter.drawEllipse(center, half_size + 2, half_size + 2)
                    
                    # 渲染SVG
                    renderer.render(painter, render_rect)
                    return
            except Exception as e:
                log_warning(f"渲染旋转SVG失败: {e}", "LayerEditor")
        
        # 回退：绘制简单的旋转图标
        self._render_rotate_handle_fallback(painter, center, is_hovered)
    
    def _render_rotate_handle_fallback(self, painter: QPainter, center: QPointF, is_hovered: bool):
        """旋转手柄的回退渲染（简单圆形+箭头）"""
        color = self.HOVER_COLOR if is_hovered else self.ROTATE_HANDLE_COLOR
        radius = self.ROTATE_HANDLE_SIZE / 2
        
        # 绘制外圆
        painter.setPen(QPen(color, self.HANDLE_BORDER_WIDTH))
        painter.setBrush(QBrush(self.HANDLE_FILL if not is_hovered else self.HOVER_FILL))
        painter.drawEllipse(center, radius, radius)
        
        # 绘制旋转箭头图案（简化版）
        painter.setPen(QPen(color, 1.5))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        
        # 绘制圆弧箭头
        inner_radius = radius * 0.5
        painter.drawEllipse(center, inner_radius, inner_radius)

    def _render_number_rotate_handle(self, painter: QPainter, center: QPointF, is_hovered: bool):
        """序号工具使用绿色方块旋转手柄"""
        size = 8
        half = size / 2
        rect = QRectF(center.x() - half, center.y() - half, size, size)

        base_color = QColor(76, 175, 80)
        fill = base_color.lighter(130) if is_hovered else base_color
        border = base_color.darker(120)

        painter.setPen(QPen(border, 1.6))
        painter.setBrush(QBrush(fill))
        painter.drawRect(rect)

    # =========================================================================
    #  状态拷贝 / 恢复（撤销用 + 避免累计误差）
    # =========================================================================

    def capture_state(self, layer: Optional[Any] = None) -> Optional[Dict[str, Any]]:
        """对外暴露的状态快照，默认针对当前激活图层"""
        target = layer or self.active_layer
        if target is None:
            return None
        snapshot = self._copy_layer_state(target)
        return snapshot.copy() if snapshot is not None else None

    def _copy_layer_state(self, layer: Any) -> Optional[Dict[str, Any]]:
        """拷贝图层关键状态（用于撤销/重做）"""
        if not layer:
            return None
        state: Dict[str, Any] = {}

        # local rect
        r = self._get_local_rect(layer)
        if isinstance(r, QRectF):
            state["rect"] = QRectF(r)

        # pos / transform（QGraphicsItem）
        if hasattr(layer, "pos") and callable(getattr(layer, "pos")):
            try:
                p = layer.pos()
                state["pos"] = QPointF(p.x(), p.y())
            except Exception as e:
                log_exception(e, "捕获layer pos")

        if hasattr(layer, "transform") and callable(getattr(layer, "transform")):
            try:
                state["transform"] = QTransform(layer.transform())
            except Exception as e:
                log_exception(e, "捕获layer transform")
        
        # 旋转角度（重要！用于旋转手柄的撤销）
        if hasattr(layer, "rotation") and callable(getattr(layer, "rotation")):
            try:
                state["rotation"] = float(layer.rotation())
            except Exception as e:
                log_exception(e, "捕获layer rotation")
        
        # 旋转中心点
        if hasattr(layer, "transformOriginPoint") and callable(getattr(layer, "transformOriginPoint")):
            try:
                origin = layer.transformOriginPoint()
                state["transformOriginPoint"] = QPointF(origin.x(), origin.y())
            except Exception as e:
                log_exception(e, "捕获layer transformOriginPoint")

        start = getattr(layer, "start_pos", None)
        if isinstance(start, QPointF):
            state["start"] = QPointF(start)

        end = getattr(layer, "end_pos", None)
        if isinstance(end, QPointF):
            state["end"] = QPointF(end)

        return state

    def _restore_base_state(self, layer: Any):
        """每次 drag_to 前恢复到起始状态，避免累计误差"""
        if not layer:
            return
        # transform
        if self._base_transform is not None and hasattr(layer, "setTransform"):
            try:
                layer.setTransform(QTransform(self._base_transform))
            except Exception as e:
                log_exception(e, "恢复layer transform")

        # pos
        if self._base_pos is not None and hasattr(layer, "setPos"):
            try:
                layer.setPos(self._base_pos)
            except Exception as e:
                log_exception(e, "恢复layer pos")

        # local rect
        if self._base_local_rect is not None:
            self._set_local_rect(layer, QRectF(self._base_local_rect))

        if (
            self._is_arrow_item(layer)
            and self._arrow_base_start_local is not None
            and self._arrow_base_end_local is not None
            and hasattr(layer, "set_positions")
        ):
            try:
                layer.set_positions(
                    QPointF(self._arrow_base_start_local),
                    QPointF(self._arrow_base_end_local),
                )
            except Exception as e:
                log_exception(e, "恢复arrow位置")

    # =========================================================================
    #  rect 读写（兼容 QGraphicsItem / 数据层）
    # =========================================================================

    def _get_local_rect(self, layer: Any) -> Optional[QRectF]:
        """获取“local rect”（适用于 RectItem/EllipseItem 等）"""
        if not layer:
            return None

        # rect() 方法
        if hasattr(layer, "rect") and callable(getattr(layer, "rect")):
            try:
                r = layer.rect()
                return QRectF(r) if isinstance(r, QRectF) else None
            except Exception as e:
                log_exception(e, "获取layer rect")

        # rect 属性（数据层）
        r = getattr(layer, "rect", None)
        return QRectF(r) if isinstance(r, QRectF) else None

    def _set_local_rect(self, layer: Any, rect: QRectF):
        """设置 local rect（setRect 优先，否则写 rect 属性）"""
        if not layer or not isinstance(rect, QRectF):
            return
        if hasattr(layer, "setRect") and callable(getattr(layer, "setRect")):
            try:
                layer.setRect(rect)
                if hasattr(layer, "update"):
                    layer.update()
                return
            except Exception as e:
                log_exception(e, "设置layer rect")

        # 数据层属性
        if hasattr(layer, "rect"):
            try:
                layer.rect = rect
            except Exception as e:
                log_exception(e, "设置layer rect属性")
