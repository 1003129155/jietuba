# -*- coding: utf-8 -*-
"""
剪贴板钉图助手

从剪贴板图片创建钉图窗口的功能模块。
"""

from typing import Optional
from PySide6.QtCore import QPoint
from PySide6.QtGui import QImage, QGuiApplication
from PySide6.QtWidgets import QWidget

from core.logger import T, log_debug, log_error


# 历史里「最新」的那张图不能直接取列表第一条：get_history 的排序是
# is_pinned DESC, item_order DESC（见 rust_libs/pyclipboard/src/database.rs），
# 置顶项永远排在最前，手动拖动过的条目顺序也和入库时间无关。所以取一页图片
# 条目回来按 created_at 自己挑最大的。一页 50 条足以覆盖置顶数量和手动排序
# 造成的偏移，代价只有一次查询——为了这点精度去改 Rust 侧的 SQL 不划算。
_LATEST_IMAGE_SCAN_LIMIT = 50


def create_pin_from_clipboard_item(
    item_id: int,
    controller,
    clipboard_window: Optional[QWidget] = None,
    center: Optional[QPoint] = None,
) -> bool:
    """
    从剪贴板项创建钉图窗口
    
    Args:
        item_id: 剪贴板项 ID
        controller: 提供 get_item(item_id) 的对象。剪贴板窗口传
            ClipboardController；全局热键那条路径没有窗口，直接传
            ClipboardManager——这里只用到 get_item 这一个方法。
        clipboard_window: 剪贴板窗口实例，用于确定显示屏幕
        center: 钉图的对齐中心（全局坐标）。给了就以它为中心，否则回退到屏幕中心
        
    Returns:
        bool: 是否创建成功
    """
    try:
        # 获取剪贴板项
        clipboard_item = controller.get_item(item_id)
        if not clipboard_item or clipboard_item.content_type != "image":
            log_error(T("无法创建钉图：item_id={item_id} 不是图片", item_id=item_id), "Clipboard")
            return False
        
        # 检查 image_id
        if not clipboard_item.image_id:
            log_error(T("无法创建钉图：图片 ID 为空"), "Clipboard")
            return False
        
        # 获取剪贴板管理器并加载完整图片数据（不是缩略图）
        from clipboard import ClipboardManager
        manager = ClipboardManager()
        image_data = manager.get_image_data(clipboard_item.image_id)
        
        if not image_data:
            log_error(T("无法创建钉图：图片数据加载失败 (image_id={image_id})", image_id=clipboard_item.image_id), "Clipboard")
            return False
        
        # 从字节数据创建 QImage
        image = QImage.fromData(image_data)
        if image.isNull():
            log_error(T("无法创建钉图：图片解码失败"), "Clipboard")
            return False
        # fromData 已完成解码，QImage 不依赖输入的压缩数据；尽早放掉 PNG bytes。
        del image_data
        
        if not create_pin_from_image(image, clipboard_window, center):
            return False
        
        log_debug(T("从剪贴板创建钉图窗口成功 (item_id={item_id})", item_id=item_id), "Clipboard")
        return True
        
    except Exception as e:
        log_error(T("创建钉图窗口失败: {e}", e=e), "Clipboard")
        return False


def create_pin_from_image(
    image: QImage,
    clipboard_window: Optional[QWidget] = None,
    center: Optional[QPoint] = None,
) -> bool:
    """把一张已经解码好的图片钉到屏幕上。

    剪贴板项和系统剪贴板两条路径最后都汇到这里，收尾动作（位置计算、
    工作集裁剪）只写一份。
    """
    if image is None or image.isNull():
        return False
    
    # 获取配置管理器
    from settings import get_tool_settings_manager
    config_manager = get_tool_settings_manager()
    
    # 计算钉图窗口的初始位置
    position = _calculate_pin_position(image, clipboard_window, center)
    
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

    # 普通截图钉图会在 ScreenshotWindow.cleanup_and_close() 中做同样的
    # 延迟裁剪。剪贴板路径没有截图窗口负责收尾，因此需要在这里补齐；
    # 延迟执行也确保调用方的 PNG bytes 临时对象已经离开作用域。
    from core.platform_utils import request_trim_working_set
    request_trim_working_set(1500)
    return True


def pin_latest_clipboard_image(center: Optional[QPoint] = None) -> bool:
    """钉住剪贴板里的图片：当前剪贴板是图就钉它，否则找历史里最新入库的那张。

    先看系统剪贴板再查历史，是为了剪贴板历史功能被关掉时这条热键仍然可用——
    那种情况下历史库根本没人往里写，只查历史等于按下去永远没反应。
    """
    try:
        image = _read_system_clipboard_image()
        if image is not None:
            if not create_pin_from_image(image, center=center):
                return False
            log_debug(T("已钉住系统剪贴板中的图片"), "Clipboard")
            return True
        
        found = _find_latest_history_image()
        if found is None:
            log_debug(T("剪贴板和历史中都没有图片，钉图已跳过"), "Clipboard")
            return False
        
        manager, item = found
        return create_pin_from_clipboard_item(item.id, manager, center=center)
    
    except Exception as e:
        log_error(T("创建钉图窗口失败: {e}", e=e), "Clipboard")
        return False


def _read_system_clipboard_image() -> Optional[QImage]:
    """读系统剪贴板里的图片，没有或读不出来都返回 None。

    有文本或文件列表时一律不算图片——这条优先级和 Rust 监听端写历史时用的
    完全一致（见 rust_libs/pyclipboard/src/lib.rs 的 text/file/image 分支）。
    不对齐的话，复制一片 Excel 单元格（同时带文本和位图）按下热键会钉出一张
    单元格截图，而历史里那条记录是文本，用户看到的和想要的对不上。
    """
    clipboard = QGuiApplication.clipboard()
    if clipboard is None:
        return None
    
    mime = clipboard.mimeData()
    if mime is not None and (mime.hasUrls() or (mime.hasText() and mime.text())):
        return None
    
    image = clipboard.image()
    if image is None or image.isNull():
        return None
    return image


def _find_latest_history_image():
    """在剪贴板历史里找入库时间最晚的那张图，返回 (manager, item)。"""
    from settings import get_tool_settings_manager
    if not get_tool_settings_manager().get_clipboard_enabled():
        # 监听关着的时候历史库不再更新，这里也不去碰它：为一次热键把数据库
        # 拉起来，只会让用户觉得「关了还在记录」。
        return None
    
    from clipboard import ClipboardManager
    manager = ClipboardManager()
    if not manager.is_available:
        return None
    
    items = manager.get_history(0, _LATEST_IMAGE_SCAN_LIMIT, content_type="image")
    candidates = [item for item in items if item.image_id]
    if not candidates:
        return None
    
    dated = [item for item in candidates if item.created_at]
    if not dated:
        # created_at 正常不会缺，真缺了就退回列表顺序，至少别什么都不钉。
        return manager, candidates[0]
    return manager, max(dated, key=lambda item: item.created_at)


def _calculate_pin_position(
    image: QImage,
    clipboard_window: Optional[QWidget] = None,
    center: Optional[QPoint] = None,
) -> QPoint:
    """
    计算钉图窗口的初始位置（图片中心对齐 center，默认为所在屏幕的中心）
    
    Args:
        image: 要显示的图片
        clipboard_window: 剪贴板窗口实例，用于确定所在屏幕
        center: 对齐中心（全局坐标）。全局热键传鼠标位置——从菜单钉图时视线
            在剪贴板窗口上，屏幕中心是合理的；按热键时视线在鼠标上，图出现在
            别处会让人先找一下
        
    Returns:
        QPoint: 钉图窗口的初始位置
    """
    # 确定目标屏幕：优先 center 所在屏幕，其次剪贴板窗口所在屏幕，最后主屏幕
    if center is not None:
        screen = QGuiApplication.screenAt(center) or QGuiApplication.primaryScreen()
    elif clipboard_window:
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
    
    if center is None:
        # 没指定中心就用屏幕中心
        center = QPoint(
            screen_geometry.x() + screen_geometry.width() // 2,
            screen_geometry.y() + screen_geometry.height() // 2,
        )
    
    # 计算钉图窗口位置（图片中心对齐 center）
    pin_x = center.x() - image.width() // 2
    pin_y = center.y() - image.height() // 2
    
    # 确保不超出屏幕边界
    pin_x = max(screen_geometry.x(), min(pin_x, screen_geometry.right() - image.width()))
    pin_y = max(screen_geometry.y(), min(pin_y, screen_geometry.bottom() - image.height()))
    
    return QPoint(pin_x, pin_y)
