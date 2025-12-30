"""
选区数据模型
管理选区矩形和状态
"""

from PyQt6.QtCore import QObject, pyqtSignal, QRectF, QSizeF


class SelectionModel(QObject):
    """
    选区模型
    管理选区矩形，支持信号通知
    """
    
    # 信号：选区改变
    rectChanged = pyqtSignal(QRectF)
    
    # 信号：选区确认
    confirmed = pyqtSignal(QRectF)
    
    def __init__(self):
        super().__init__()
        self._rect = QRectF()
        self.min_size = QSizeF(8, 8)  # 最小选区尺寸
        self._is_active = False       # 是否正在选择
        self._is_confirmed = False    # 是否已确认（进入绘图模式）
        
    def rect(self) -> QRectF:
        """获取当前选区矩形（副本）"""
        return QRectF(self._rect)
    
    def set_rect(self, r: QRectF):
        """
        设置选区矩形
        
        Args:
            r: 新的选区矩形
        """
        if r == self._rect:
            return
        
        # 确保最小尺寸
        if r.width() < self.min_size.width():
            r.setWidth(self.min_size.width())
        if r.height() < self.min_size.height():
            r.setHeight(self.min_size.height())
        
        self._rect = QRectF(r)
        self.rectChanged.emit(QRectF(self._rect))
    
    def is_active(self) -> bool:
        """是否有活动选区"""
        return self._is_active and not self._rect.isNull()
    
    def activate(self):
        """激活选区"""
        self._is_active = True
    
    def deactivate(self):
        """取消选区"""
        self._is_active = False
        self._rect = QRectF()
        self.rectChanged.emit(QRectF())
    
    def confirm(self):
        """确认选区"""
        if self.is_active():
            self._is_confirmed = True
            self.confirmed.emit(QRectF(self._rect))
    
    @property
    def is_confirmed(self) -> bool:
        """选区是否已确认（进入绘图模式）"""
        return self._is_confirmed

    def initialize_confirmed_rect(self, rect: QRectF):
        """直接设置一个已确认的选区（用于预定义画布）"""
        self.set_rect(rect)
        self._is_active = False
        self._is_confirmed = True
        self.confirmed.emit(QRectF(self._rect))
    
    def is_empty(self) -> bool:
        """选区是否为空"""
        return self._rect.isNull() or self._rect.isEmpty()
    
    def normalize(self):
        """规范化选区（确保宽高为正）"""
        if not self._rect.isNull():
            self._rect = self._rect.normalized()
            self.rectChanged.emit(QRectF(self._rect))
