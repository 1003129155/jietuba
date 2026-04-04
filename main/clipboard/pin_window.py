# -*- coding: utf-8 -*-
"""
剪贴板钉图助手

从剪贴板图片创建钉图窗口的功能模块。
"""

from typing import Optional
from PySide6.QtCore import QPoint
from PySide6.QtGui import QImage, QCursor, QGuiApplication
from PySide6.QtWidgets import QWidget

from core.logger import log_debug, log_error


def create_pin_from_clipboard_item(item_id: int, controller, clipboard_window: Optional[QWidget] = None) -> bool:
    """
    从剪贴板项创建钉图窗口
    
    Args:
        item_id: 剪贴板项 ID
        controller: ClipboardController 实例，用于获取剪贴板项数据
        clipboard_window: 剪贴板窗口实例，用于确定显示屏幕
        
    Returns:
        bool: 是否创建成功
    """
    try:
        # 获取剪贴板项
        clipboard_item = controller.get_item(item_id)
        if not clipboard_item or clipboard_item.content_type != "image":
            log_error(f"无法创建钉图：item_id={item_id} 不是图片", "Clipboard")
            return False
        
        # 检查 image_id
        if not clipboard_item.image_id:
            log_error("无法创建钉图：图片 ID 为空", "Clipboard")
            return False
        
        # 获取剪贴板管理器并加载完整图片数据（不是缩略图）
        from clipboard import ClipboardManager
        manager = ClipboardManager()
        image_data = manager.get_image_data(clipboard_item.image_id)
        
        if not image_data:
            log_error(f"无法创建钉图：图片数据加载失败 (image_id={clipboard_item.image_id})", "Clipboard")
            return False
        
        # Rust 返回的是 list，需要转换为 bytes
        if isinstance(image_data, list):
            image_data = bytes(image_data)
        
        # 从字节数据创建 QImage
        image = QImage.fromData(image_data)
        if image.isNull():
            log_error("无法创建钉图：图片解码失败", "Clipboard")
            return False
        
        # 获取配置管理器
        from settings import get_tool_settings_manager
        config_manager = get_tool_settings_manager()
        
        # 计算钉图窗口的初始位置（在剪贴板窗口所在屏幕的中心）
        position = _calculate_pin_position(image, clipboard_window)
        
        # 创建钉图窗口
        from pin import get_pin_manager
        pin_manager = get_pin_manager()
        pin_manager.create_pin(
            image=image,
            position=position,
            config_manager=config_manager,
            drawing_items=None,  # 从剪贴板创建不带绘制项
            selection_offset=None
        )
        
        log_debug(f"从剪贴板创建钉图窗口成功 (item_id={item_id})", "Clipboard")
        return True
        
    except Exception as e:
        log_error(f"创建钉图窗口失败: {e}", "Clipboard")
        return False


def _calculate_pin_position(image: QImage, clipboard_window: Optional[QWidget] = None) -> QPoint:
    """
    计算钉图窗口的初始位置（在剪贴板窗口所在屏幕的中心）
    
    Args:
        image: 要显示的图片
        clipboard_window: 剪贴板窗口实例，用于确定所在屏幕
        
    Returns:
        QPoint: 钉图窗口的初始位置
    """
    # 确定目标屏幕（剪贴板窗口所在屏幕，如果没有则使用主屏幕）
    if clipboard_window:
        # 获取剪贴板窗口所在的屏幕
        screen = clipboard_window.screen()
    else:
        # 回退：使用主屏幕
        screen = QGuiApplication.primaryScreen()
    
    if not screen:
        # 最终回退：使用默认位置
        return QPoint(100, 100)
    
    # 获取屏幕几何信息
    screen_geometry = screen.geometry()
    
    # 计算屏幕中心位置
    screen_center_x = screen_geometry.x() + screen_geometry.width() // 2
    screen_center_y = screen_geometry.y() + screen_geometry.height() // 2
    
    # 计算钉图窗口位置（图片中心对齐屏幕中心）
    pin_x = screen_center_x - image.width() // 2
    pin_y = screen_center_y - image.height() // 2
    
    # 确保不超出屏幕边界
    pin_x = max(screen_geometry.x(), min(pin_x, screen_geometry.right() - image.width()))
    pin_y = max(screen_geometry.y(), min(pin_y, screen_geometry.bottom() - image.height()))
    
    return QPoint(pin_x, pin_y)
 