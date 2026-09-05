# -*- coding: utf-8 -*-
"""
剪贴板管理器

封装 pyclipboard 后端，提供 Python 友好的接口。
"""

from typing import Optional, Callable, List
import json

try:
    import pyclipboard
    from pyclipboard import PyClipboardManager, PyClipboardItem, PyGroup
    PYCLIPBOARD_AVAILABLE = True
except ImportError as e:
    PYCLIPBOARD_AVAILABLE = False
    import sys
    from core.logger import T, log_debug, log_warning
    log_warning(T("pyclipboard 模块未安装: {e}", e=e), "Clipboard")
    log_debug(f"Python: {sys.executable}", "Clipboard")
    log_debug(f"sys.path: {sys.path[:3]}...", "Clipboard")

from core.logger import T, log_debug, log_info, log_error, log_exception
from .enums import GroupType
from .models import ClipboardItem, Group


class ClipboardManager:
    """
    剪贴板管理器
    
    提供剪贴板历史管理功能。
    
    使用示例:
        manager = ClipboardManager()
        manager.start_monitoring(callback=on_change)
        
        # 获取历史
        items = manager.get_history(limit=20)
        
        # 粘贴某项
        manager.paste_item(item.id)
        
        # 停止监听
        manager.stop_monitoring()
    """
    
    _instance = None
    
    def __new__(cls, *args, **kwargs):
        """单例模式"""
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance
    
    def __init__(self, db_path: Optional[str] = None):
        """
        初始化管理器
        
        Args:
            db_path: 数据库路径，默认使用系统数据目录
        """
        if hasattr(self, '_initialized') and self._initialized:
            return
            
        self._initialized = True
        self._callback = None
        self._manager = None
        
        if PYCLIPBOARD_AVAILABLE:
            try:
                self._manager = PyClipboardManager(self._resolve_db_path(db_path))
                log_info(T("管理器初始化成功"), "Clipboard")

                # 设置历史限制（由 Rust 后端处理清理）
                self._apply_history_limit()
            except Exception as e:
                log_error(T("初始化失败: {e}", e=e), "Clipboard")
    
    @property
    def is_available(self) -> bool:
        """检查是否可用"""
        return self._manager is not None

    def _resolve_db_path(self, db_path: Optional[str] = None) -> Optional[str]:
        """Return an explicit db path, or the saved clipboard db path."""
        if db_path:
            return db_path
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            if hasattr(config, "get_clipboard_db_path"):
                saved_path = config.get_clipboard_db_path()
            else:
                saved_path = config.get_app_setting("clipboard_db_path", "")
            return saved_path or None
        except Exception:
            return None

    def reset_storage(self, db_path: Optional[str] = None) -> bool:
        """Reopen the Rust backend on a new database path and restore monitoring."""
        if not PYCLIPBOARD_AVAILABLE:
            return False

        import gc

        was_monitoring = self.is_monitoring() or getattr(self, "_resume_monitoring_after_reset", False)
        self._resume_monitoring_after_reset = False
        try:
            if self._manager is not None:
                try:
                    self._manager.stop_monitor()
                except Exception:
                    pass
            self._manager = None
            gc.collect()
            self._manager = PyClipboardManager(self._resolve_db_path(db_path))
            self._apply_history_limit()
            if was_monitoring:
                self.start_monitoring(callback=self._callback)
            return True
        except Exception as e:
            log_exception(e, T("重新打开剪贴板数据库"))
            self._manager = None
            return False

    def release_storage(self) -> bool:
        """Stop monitoring and drop the current Rust backend before file migration."""
        import gc

        was_monitoring = self.is_monitoring()
        self._resume_monitoring_after_reset = was_monitoring
        try:
            if self._manager is not None:
                try:
                    self._manager.stop_monitor()
                except Exception:
                    pass
            self._manager = None
            gc.collect()
            return True
        except Exception as e:
            log_exception(e, T("释放剪贴板数据库"))
            return False
    
    def get_db_path(self) -> Optional[str]:
        """获取数据库文件路径"""
        if self.is_available:
            try:
                if hasattr(self._manager, 'db_path'):
                    return self._manager.db_path
                elif hasattr(self._manager, 'get_db_path'):
                    return self._manager.get_db_path()
            except Exception as e:
                log_exception(e, T("获取数据库路径"))
        return None
    
    def get_images_dir(self) -> Optional[str]:
        """获取图片存储目录路径"""
        if self.is_available:
            try:
                if hasattr(self._manager, 'get_images_dir'):
                    return self._manager.get_images_dir()
                elif hasattr(self._manager, 'images_dir'):
                    return self._manager.images_dir
            except Exception as e:
                log_exception(e, T("获取图片目录"))
        return None
    
    def _get_history_limit(self) -> int:
        """获取历史数量限制设置
        
        返回 0 表示不限制
        """
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            return config.get_clipboard_history_limit()
        except Exception:
            return 500  # 默认限制
    
    def _apply_history_limit(self):
        """
        将历史限制设置传递给 Rust 后端
        
        Rust 后端会在插入新记录时自动清理超出限制的旧记录
        """
        if not self.is_available:
            return
        
        try:
            limit = self._get_history_limit()
            self._manager.set_history_limit(limit)
            log_info(T("历史限制设置为: {limit}", limit=limit), "Clipboard")
        except Exception as e:
            log_error(T("设置历史限制失败: {e}", e=e), "Clipboard")
    
    def start_monitoring(self, callback: Optional[Callable[[ClipboardItem], None]] = None):
        """
        开始监听剪贴板变化
        
        Args:
            callback: 剪贴板变化时的回调函数
        """
        if not self.is_available:
            log_error(T("管理器不可用"), "Clipboard")
            return
        
        self._callback = callback
        
        def _on_change(py_item):
            """内部回调，转换类型后调用用户回调"""
            item = ClipboardItem.from_py_item(py_item)
            # 预处理显示文本：去掉换行符，避免日志行被切断
            preview = item.display_text[:50].replace('\r\n', ' ').replace('\n', ' ').replace('\r', ' ').strip()
            # char_count 编码了格式数据字节统计：raw * 10_000_000 + compressed
            encoded = py_item.char_count if py_item.char_count is not None else 0
            if encoded > 0:
                raw_bytes = encoded // 10_000_000
                comp_bytes = encoded % 10_000_000
                def _fmt(n):
                    if n < 1024: return f"{n} B"
                    if n < 1024*1024: return f"{n/1024:.1f} KB"
                    return f"{n/1024/1024:.2f} MB"
                if raw_bytes != comp_bytes and raw_bytes > 0:
                    ratio = raw_bytes / comp_bytes if comp_bytes > 0 else 0
                    size_info = f"{_fmt(raw_bytes)} → {_fmt(comp_bytes)} ({ratio:.1f}x)"
                else:
                    size_info = _fmt(raw_bytes)
                log_debug(T("新内容: {icon} {preview}  [{size_info}]", icon=item.icon, preview=preview, size_info=size_info), "Clipboard")
            else:
                log_debug(T("新内容: {icon} {preview}", icon=item.icon, preview=preview), "Clipboard")
            
            # 清理逻辑已由 Rust 后端处理，这里只需调用用户回调
            if self._callback:
                self._callback(item)
        
        try:
            self._manager.start_monitor(callback=_on_change)
            log_info(T("开始监听剪贴板"), "Clipboard")
        except Exception as e:
            log_error(T("启动监听失败: {e}", e=e), "Clipboard")
    
    def stop_monitoring(self):
        """停止监听"""
        if self.is_available:
            try:
                self._manager.stop_monitor()
                log_info(T("🛑 停止监听"), "Clipboard")
            except Exception as e:
                log_error(T("停止监听失败: {e}", e=e), "Clipboard")
    
    def is_monitoring(self) -> bool:
        """检查是否正在监听"""
        if self.is_available:
            return self._manager.is_monitoring()
        return False
    
    def get_history(self, offset: int = 0, limit: int = 50,
                    search: Optional[str] = None,
                    content_type: Optional[str] = None,
                    start_time: Optional[int] = None,
                    end_time: Optional[int] = None) -> List[ClipboardItem]:
        """
        获取剪贴板历史
        
        Args:
            offset: 偏移量
            limit: 数量限制
            search: 搜索关键词
            content_type: 内容类型过滤 ("text", "image", "file", "all")
        
        Returns:
            剪贴板项列表
        """
        if not self.is_available:
            return []
        
        try:
            result = self._manager.get_history(offset, limit, search, content_type, start_time, end_time)
            return [ClipboardItem.from_py_item(item) for item in result.items]
        except Exception as e:
            log_error(T("获取历史失败: {e}", e=e), "Clipboard")
            return []
    
    def get_total_count(self) -> int:
        """获取总记录数"""
        if self.is_available:
            try:
                return self._manager.get_count()
            except Exception as e:
                log_exception(e, T("获取总记录数"))
        return 0
    
    def search(self, keyword: str, limit: int = 50) -> List[ClipboardItem]:
        """搜索历史"""
        return self.get_history(search=keyword, limit=limit)
    
    def get_item(self, item_id: int) -> Optional[ClipboardItem]:
        """根据 ID 获取项"""
        if not self.is_available:
            return None
        
        try:
            py_item = self._manager.get_item(item_id)
            if py_item:
                return ClipboardItem.from_py_item(py_item)
        except Exception as e:
            log_error(T("获取项失败: {e}", e=e), "Clipboard")
        return None
    
    def delete_item(self, item_id: int) -> bool:
        """删除项"""
        if not self.is_available:
            return False
        
        try:
            self._manager.delete_item(item_id)
            return True
        except Exception as e:
            log_error(T("删除失败: {e}", e=e), "Clipboard")
            return False
    
    def clear_history(self, keep_grouped: bool = False) -> bool:
        """清空历史
        
        Args:
            keep_grouped: True=保留分组内容，只删未分组的历史；False=全部删除（含分组）
        """
        if not self.is_available:
            return False
        
        try:
            self._manager.clear_history(keep_grouped)
            log_info(T("🗑️ 历史已清空 (keep_grouped={keep_grouped})", keep_grouped=keep_grouped), "Clipboard")
            return True
        except Exception as e:
            log_error(T("清空失败: {e}", e=e), "Clipboard")
            return False
    
    def add_item(self, content: str, content_type: str = "text", 
                 title: Optional[str] = None) -> Optional[int]:
        """
        直接添加内容到数据库
        
        Args:
            content: 内容文本
            content_type: 内容类型，默认 "text"
            title: 标题（可选，用于收藏内容）
        
        Returns:
            新记录的 ID，失败返回 None
        """
        if not self.is_available:
            return None
        
        try:
            item_id = self._manager.add_item(content, content_type, title)
            log_info(T("添加内容成功: ID={item_id}", item_id=item_id), "Clipboard")
            return item_id
        except Exception as e:
            log_error(T("添加内容失败: {e}", e=e), "Clipboard")
            return None
    
    def update_item(self, item_id: int, content: str, 
                    title: Optional[str] = None) -> bool:
        """
        更新内容项
        
        Args:
            item_id: 内容 ID
            content: 新内容
            title: 新标题（可选）
        
        Returns:
            是否成功
        """
        if not self.is_available:
            return False
        
        try:
            self._manager.update_item(item_id, content, title)
            return True
        except Exception as e:
            log_error(T("更新内容失败: {e}", e=e), "Clipboard")
            return False
    
    def toggle_pin(self, item_id: int) -> bool:
        """切换置顶状态，返回新状态"""
        if not self.is_available:
            return False
        
        try:
            return self._manager.toggle_pin(item_id)
        except Exception as e:
            log_error(T("置顶失败: {e}", e=e), "Clipboard")
            return False
    
    def paste_item(self, item_id: int, with_html: bool = True, move_to_top: bool = False) -> bool:
        """
        粘贴某项到剪贴板
        
        会自动设置到系统剪贴板并增加粘贴次数。
        
        Args:
            item_id: 剪贴板项 ID
            with_html: 是否包含 HTML 格式（默认 True）
            move_to_top: 是否将该项移到最前（更新 item_order，默认 False）
        """
        if not self.is_available:
            return False
        
        try:
            return self._manager.paste_item(item_id, with_html, move_to_top)
        except Exception as e:
            log_error(T("粘贴失败: {e}", e=e), "Clipboard")
            return False
    
    def get_image_data(self, image_id: str) -> Optional[bytes]:
        """获取图片数据"""
        if not self.is_available:
            return None
        
        try:
            return self._manager.get_image_data(image_id)
        except Exception as e:
            log_error(T("获取图片失败: {e}", e=e), "Clipboard")
            return None
    
    # ==================== 分组功能 ====================
    
    def create_group(self, name: str, color: Optional[str] = None, 
                     icon: Optional[str] = None,
                     group_type: int = 0) -> Optional[int]:
        """创建分组，返回分组 ID"""
        if not self.is_available:
            return None
        
        try:
            return self._manager.create_group(name, color, icon, group_type)
        except Exception as e:
            log_error(T("创建分组失败: {e}", e=e), "Clipboard")
            return None
    
    def get_groups(self) -> List[Group]:
        """获取所有分组"""
        if not self.is_available:
            return []
        
        try:
            py_groups = self._manager.get_groups()
            return [Group.from_py_group(g) for g in py_groups]
        except Exception as e:
            log_error(T("获取分组失败: {e}", e=e), "Clipboard")
            return []
    
    def delete_group(self, group_id: int) -> bool:
        """删除分组"""
        if not self.is_available:
            return False
        
        try:
            self._manager.delete_group(group_id)
            return True
        except Exception as e:
            log_error(T("删除分组失败: {e}", e=e), "Clipboard")
            return False
    
    def rename_group(self, group_id: int, name: str) -> bool:
        """重命名分组"""
        if not self.is_available:
            return False
        
        try:
            self._manager.rename_group(group_id, name)
            return True
        except Exception as e:
            log_error(T("重命名分组失败: {e}", e=e), "Clipboard")
            return False
    
    def update_group(self, group_id: int, name: str, 
                     color: Optional[str] = None, icon: Optional[str] = None,
                     group_type: int = 0) -> bool:
        """更新分组（名称、颜色、图标、类型）"""
        if not self.is_available:
            return False
        
        try:
            self._manager.update_group(group_id, name, color, icon, group_type)
            return True
        except Exception as e:
            log_error(T("更新分组失败: {e}", e=e), "Clipboard")
            return False
    
    def move_to_group(self, item_id: int, group_id: Optional[int] = None) -> bool:
        """将项目移动到分组。若目标是文件分组，只允许 content_type='file' 的条目进入。"""
        if not self.is_available:
            return False
        
        try:
            # 文件分组校验：目标分组存在且为文件分组时，检查条目类型
            if group_id is not None:
                groups = self._manager.get_groups()
                target = next((g for g in groups if g.id == group_id), None)
                if target is not None and target.group_type == GroupType.FILE:
                    item = self._manager.get_item(item_id)
                    if item is None or item.content_type != "file":
                        log_error(T("文件分组只允许文件类型条目，item_id={item_id} 被拒绝", item_id=item_id), "Clipboard")
                        return False
            self._manager.move_to_group(item_id, group_id)
            return True
        except Exception as e:
            log_error(T("移动到分组失败: {e}", e=e), "Clipboard")
            return False

    def move_group_between(self, group_id: int, before_id: Optional[int] = None,
                           after_id: Optional[int] = None) -> bool:
        """调整分组顺序（移动到指定分组之间）"""
        if not self.is_available:
            return False

        try:
            self._manager.move_group_between(group_id, before_id=before_id, after_id=after_id)
            return True
        except Exception as e:
            log_error(T("调整分组顺序失败: {e}", e=e), "Clipboard")
            return False

    def move_item_between(self, item_id: int, before_id: Optional[int] = None,
                          after_id: Optional[int] = None) -> bool:
        """调整分组内内容顺序（移动到指定内容之间）"""
        if not self.is_available:
            return False

        try:
            self._manager.move_item_between(item_id, before_id=before_id, after_id=after_id)
            return True
        except Exception as e:
            log_error(T("调整内容顺序失败: {e}", e=e), "Clipboard")
            return False
    
    def get_by_group(self, group_id: Optional[int] = None, 
                     offset: int = 0, limit: int = 50) -> List[ClipboardItem]:
        """按分组查询"""
        if not self.is_available:
            return []
        
        try:
            result = self._manager.get_by_group(group_id, offset, limit)
            return [ClipboardItem.from_py_item(item) for item in result.items]
        except Exception as e:
            log_error(T("按分组查询失败: {e}", e=e), "Clipboard")
            return []
