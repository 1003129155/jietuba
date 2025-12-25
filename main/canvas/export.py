"""
导出服务
统一的图像导出接口
"""

from PyQt6.QtCore import QRectF
from PyQt6.QtGui import QImage, QPainter, QPixmap
from PyQt6.QtWidgets import QApplication


class ExportService:
    """
    导出服务 - 统一处理图像导出
    """
    
    def __init__(self, scene):
        """
        Args:
            scene: CanvasScene 实例
        """
        self.scene = scene
    
    def get_result_pixmap(self) -> QPixmap:
        """
        获取最终结果图像 (选区内容)
        """
        # 获取选区
        selection_rect = self.scene.selection_model.rect()
        if selection_rect.isEmpty():
            # 如果没有选区，导出整个场景
            selection_rect = self.scene.sceneRect()
            
        return QPixmap.fromImage(self.export(selection_rect))

    def export(self, selection_rect: QRectF) -> QImage:
        """
        导出选区图像（包含背景和绘制内容）
        
        Args:
            selection_rect: 选区矩形（场景坐标）
            
        Returns:
            导出的图像
        """
        if selection_rect.isNull() or selection_rect.isEmpty():
            print("⚠️ [导出] 选区为空")
            return QImage()
        
        print(f"🔍 [导出] 接收到选区: {selection_rect}")
        
        # 输出图像大小按选区逻辑像素
        w = max(1, int(selection_rect.width()))
        h = max(1, int(selection_rect.height()))
        
        print(f"🔍 [导出] 目标图像大小: {w}x{h}")
        
        out = QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
        out.fill(0)  # 透明背景
        
        painter = QPainter(out)
        try:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            
            # 渲染场景到图像
            # sourceRect: selection_rect
            # targetRect: (0, 0, w, h)
            
            # 临时隐藏遮罩和选区框，只渲染背景和绘图内容
            self.scene.overlay_mask.setVisible(False)
            self.scene.selection_item.setVisible(False)
            
            self.scene.render(painter, QRectF(0, 0, w, h), selection_rect)
            
            # 恢复显示
            self.scene.overlay_mask.setVisible(True)
            if not self.scene.selection_model.is_confirmed:
                 self.scene.selection_item.setVisible(True)
        finally:
            painter.end()
        
        print(f"📤 [导出] 完成！最终图像: {out.width()}x{out.height()}")
        return out
    
    def export_base_image_only(self, selection_rect: QRectF) -> QImage:
        """
        导出选区的纯净底图（不包含任何绘制内容）
        用于钉图功能，保证钉图可以继续编辑绘制内容
        
        Args:
            selection_rect: 选区矩形（场景坐标）
            
        Returns:
            只包含背景的图像
        """
        if selection_rect.isNull() or selection_rect.isEmpty():
            print("⚠️ [导出底图] 选区为空")
            return QImage()
        
        print(f"🔍 [导出底图] 接收到选区: {selection_rect}")
        
        # 输出图像大小按选区逻辑像素
        w = max(1, int(selection_rect.width()))
        h = max(1, int(selection_rect.height()))
        
        print(f"🔍 [导出底图] 目标图像大小: {w}x{h}")
        
        out = QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
        out.fill(0)  # 透明背景
        
        painter = QPainter(out)
        try:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            
            # 🔥 只渲染背景层，排除所有绘制项目
            # 临时隐藏所有非背景图层
            old_visible_states = []
            for item in self.scene.items():
                if item != self.scene.background:
                    old_visible_states.append((item, item.isVisible()))
                    item.setVisible(False)
            
            # 只渲染背景
            self.scene.render(painter, QRectF(0, 0, w, h), selection_rect)
            
            # 恢复所有图层的可见性
            for item, visible in old_visible_states:
                item.setVisible(visible)
                
        finally:
            painter.end()
        
        print(f"📤 [导出底图] 完成！纯净底图: {out.width()}x{out.height()}")
        return out
    
    def export_full(self) -> QImage:
        """
        导出整个场景
        
        Returns:
            完整场景图像
        """
        rect = self.scene.sceneRect()
        return self.export(rect)
    
    def copy_to_clipboard(self, img: QImage):
        """
        复制图像到剪贴板
        
        Args:
            img: 要复制的图像
        """
        QApplication.clipboard().setImage(img)
        print(f"📋 [导出] 已复制到剪贴板")
    
    def save_to_file(self, img: QImage, path: str, quality: int = 100) -> bool:
        """
        保存图像到文件
        
        Args:
            img: 要保存的图像
            path: 文件路径
            quality: 质量（0-100）
            
        Returns:
            是否成功
        """
        success = img.save(path, quality=quality)
        if success:
            print(f"💾 [导出] 保存成功: {path}")
        else:
            print(f"❌ [导出] 保存失败: {path}")
        return success
    
    def export_and_copy(self, selection_rect: QRectF):
        """
        导出选区并复制到剪贴板（快捷操作）
        
        Args:
            selection_rect: 选区矩形
        """
        if selection_rect.isNull() or selection_rect.isEmpty():
            print("⚠️ [导出] 选区为空")
            return
        
        img = self.export(selection_rect)
        self.copy_to_clipboard(img)
