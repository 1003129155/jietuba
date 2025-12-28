"""
Mock QGraphicsScene - 用于钉图编辑模式
拦截工具创建的图元，转换为命令字典
"""

from PyQt6.QtCore import QObject, pyqtSignal
from core import log_debug, log_warning


class MockUndoStack:
    """Mock QUndoStack - 钉图不需要撤销功能"""
    
    def push(self, command):
        """假装执行命令（实际不做任何事）"""
        pass


class PinMockScene(QObject):
    """
    Mock QGraphicsScene - 钉图专用
    
    职责：
    - 拦截工具创建的图元（StrokeItem, RectItem等）
    - 提取图元属性，转换为命令字典
    - 通过信号通知画布添加命令
    """
    
    # 信号：图元添加时发出命令字典
    item_added = pyqtSignal(dict)  # 发送命令字典
    
    def __init__(self):
        super().__init__()
        self._items = []
    
    def addItem(self, item):
        """
        拦截工具添加图元
        
        Args:
            item: QGraphicsItem（StrokeItem, RectItem等）
        """
        log_debug(f"拦截图元: {item.__class__.__name__}", "MockScene")
        
        # 🔥 提取图元属性，转换为命令字典
        cmd = self._item_to_command(item)
        if cmd:
            log_debug(f"转换命令: {cmd.get('type', 'unknown')}", "MockScene")
            self.item_added.emit(cmd)
        else:
            log_warning(f"无法转换图元: {item.__class__.__name__}", "MockScene")
        
        # 保存图元引用（虽然不渲染，但保留以防工具需要访问）
        self._items.append(item)
    
    def removeItem(self, item):
        """
        移除图元（工具撤销时调用）
        """
        if item in self._items:
            self._items.remove(item)
            log_debug(f"移除图元: {item.__class__.__name__}", "MockScene")
    
    def _item_to_command(self, item):
        """
        将图元转换为命令字典
        
        Returns:
            dict: 命令字典，格式与渲染器兼容
        """
        class_name = item.__class__.__name__
        
        # 🔥 根据图元类型提取属性
        if class_name == "StrokeItem":
            return self._stroke_item_to_command(item)
        elif class_name == "RectItem":
            return self._rect_item_to_command(item)
        elif class_name == "EllipseItem":
            return self._ellipse_item_to_command(item)
        elif class_name == "ArrowItem":
            return self._arrow_item_to_command(item)
        elif class_name == "NumberItem":
            return self._number_item_to_command(item)
        elif class_name == "TextItem":
            return self._text_item_to_command(item)
        else:
            return None
    
    def _stroke_item_to_command(self, item):
        """画笔/荧光笔 → 命令字典"""
        path = item.path()
        pen = item.pen()
        
        # 提取路径点
        points = []
        for i in range(path.elementCount()):
            elem = path.elementAt(i)
            points.append((elem.x, elem.y))
        
        cmd_type = "highlighter" if getattr(item, 'is_highlighter', False) else "pen"
        
        return {
            "type": cmd_type,
            "points": points,
            "color": pen.color().getRgb()[:3],  # (R, G, B)
            "width": pen.width()
        }
    
    def _rect_item_to_command(self, item):
        """矩形 → 命令字典"""
        rect = item.rect()
        pen = item.pen()
        
        return {
            "type": "rect",
            "x": rect.x(),
            "y": rect.y(),
            "width": rect.width(),
            "height": rect.height(),
            "color": pen.color().getRgb()[:3],
            "line_width": pen.width()
        }
    
    def _ellipse_item_to_command(self, item):
        """椭圆 → 命令字典"""
        rect = item.rect()
        pen = item.pen()
        
        return {
            "type": "ellipse",
            "x": rect.x(),
            "y": rect.y(),
            "width": rect.width(),
            "height": rect.height(),
            "color": pen.color().getRgb()[:3],
            "line_width": pen.width()
        }
    
    def _arrow_item_to_command(self, item):
        """箭头 → 命令字典"""
        # 🔥 ArrowItem 实际属性是 start_pos 和 end_pos
        start = item.start_pos if hasattr(item, 'start_pos') else None
        end = item.end_pos if hasattr(item, 'end_pos') else None
        
        if not start or not end:
            log_warning("ArrowItem 缺少起始点或结束点", "MockScene")
            return None
        
        # 从 brush 获取颜色（ArrowItem 使用填充而非轮廓）
        color = item.brush().color().getRgb()[:3] if item.brush() else (255, 0, 0)
        
        return {
            "type": "arrow",
            "x1": start.x(),
            "y1": start.y(),
            "x2": end.x(),
            "y2": end.y(),
            "color": color,
            "line_width": item.base_width if hasattr(item, 'base_width') else 3
        }
    
    def _number_item_to_command(self, item):
        """序号 → 命令字典"""
        # 🔥 NumberItem 实际属性：number, radius, color, pos()
        number = item.number if hasattr(item, 'number') else 1
        radius = item.radius if hasattr(item, 'radius') else 15
        color = item.color.getRgb()[:3] if hasattr(item, 'color') else (255, 0, 0)
        pos = item.pos()  # 序号的中心位置
        
        return {
            "type": "number",
            "x": pos.x(),
            "y": pos.y(),
            "number": number,
            "color": color,
            "radius": radius
        }
    
    def _text_item_to_command(self, item):
        """文字 → 命令字典"""
        # 🔥 TextItem 是 QGraphicsTextItem
        text = item.toPlainText() if hasattr(item, 'toPlainText') else ""
        pos = item.pos()
        
        # 文字颜色和字体
        color = (0, 0, 0)  # 默认黑色
        font_size = 16  # 默认字号
        
        if hasattr(item, 'defaultTextColor'):
            color = item.defaultTextColor().getRgb()[:3]
        
        if hasattr(item, 'font'):
            font = item.font()
            font_size = font.pointSize() if font.pointSize() > 0 else 16
        
        return {
            "type": "text",
            "x": pos.x(),
            "y": pos.y(),
            "text": text,
            "color": color,
            "font_size": font_size
        }
