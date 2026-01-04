# -*- coding: utf-8 -*-
"""
剪贴板管理器

封装 pyclipboard 后端，提供 Python 友好的接口。
"""

from typing import Optional, Callable, List
from dataclasses import dataclass
from datetime import datetime
import json

try:
    import pyclipboard
    from pyclipboard import PyClipboardManager, PyClipboardItem, PyGroup
    PYCLIPBOARD_AVAILABLE = True
except ImportError as e:
    PYCLIPBOARD_AVAILABLE = False
    import sys
    print(f"⚠️ pyclipboard 模块未安装: {e}")
    print(f"   Python: {sys.executable}")
    print(f"   sys.path: {sys.path[:3]}...")


@dataclass
class ClipboardItem:
    """剪贴板项数据类"""
    id: int
    content: str
    content_type: str  # "text", "image", "file"
    title: Optional[str] = None  # 标题（用于收藏内容）
    html_content: Optional[str] = None
    image_id: Optional[str] = None
    thumbnail: Optional[str] = None  # 缩略图 Base64 (data:image/png;base64,...)
    is_pinned: bool = False
    paste_count: int = 0
    source_app: Optional[str] = None
    created_at: datetime = None
    updated_at: datetime = None
    
    @classmethod
    def from_py_item(cls, item: 'PyClipboardItem') -> 'ClipboardItem':
        """从 PyClipboardItem 转换"""
        return cls(
            id=item.id,
            content=item.content,
            content_type=item.content_type,
            title=item.title,
            html_content=item.html_content,
            image_id=item.image_id,
            thumbnail=item.thumbnail,
            is_pinned=item.is_pinned,
            paste_count=item.paste_count,
            source_app=item.source_app,
            created_at=datetime.fromtimestamp(item.created_at) if item.created_at else None,
            updated_at=datetime.fromtimestamp(item.updated_at) if item.updated_at else None,
        )
    
    @property
    def display_text(self) -> str:
        """获取用于显示的文本（界面显示用，不影响实际存储内容）"""
        if self.content_type == "text":
            # 截断长文本
            text = self.content.replace('\n', ' ').strip()
            return text[:100] + "..." if len(text) > 100 else text
        elif self.content_type == "image":
            return self.content  # "[图片 WxH]"
        elif self.content_type == "file":
            try:
                import os
                data = json.loads(self.content)
                files = data.get("files", [])
                if len(files) == 1:
                    # 单个文件显示 "file: 文件名"
                    return f"file: {os.path.basename(files[0])}"
                elif len(files) > 1:
                    # 多个文件显示完整路径，用逗号分隔
                    return f"file: {', '.join(files)}"
                else:
                    return "file: 文件"
            except:
                return "file: 文件"
        return self.content[:50]
    
    @property
    def icon(self) -> str:
        """获取图标（仅文件类型显示图标）"""
        if self.content_type == "file":
            return "📁"
        elif self.content_type == "image":
            return "📷"
        return ""  # 文本不显示图标


@dataclass
class Group:
    """分组数据类"""
    id: int
    name: str
    color: Optional[str] = None
    icon: Optional[str] = None
    
    @classmethod
    def from_py_group(cls, group: 'PyGroup') -> 'Group':
        """从 PyGroup 转换"""
        return cls(
            id=group.id,
            name=group.name,
            color=group.color,
            icon=group.icon,
        )


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
                self._manager = PyClipboardManager(db_path)
                print("✅ [Clipboard] 管理器初始化成功")
                
                # 设置历史限制（由 Rust 后端处理清理）
                self._apply_history_limit()
            except Exception as e:
                print(f"❌ [Clipboard] 初始化失败: {e}")
    
    @property
    def is_available(self) -> bool:
        """检查是否可用"""
        return self._manager is not None
    
    def get_db_path(self) -> Optional[str]:
        """获取数据库文件路径"""
        if self.is_available:
            try:
                return self._manager.get_db_path()
            except Exception:
                pass
        return None
    
    def get_images_dir(self) -> Optional[str]:
        """获取图片存储目录路径"""
        if self.is_available:
            try:
                return self._manager.get_images_dir()
            except Exception:
                pass
        return None
    
    def _get_history_limit(self) -> int:
        """获取历史数量限制设置
        
        如果 auto_cleanup 关闭，返回 0 表示不限制
        """
        try:
            from settings import get_tool_settings_manager
            config = get_tool_settings_manager()
            # 如果自动清理关闭，返回 0 表示不限制
            if not config.get_clipboard_auto_cleanup():
                return 0
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
            print(f"📋 [Clipboard] 历史限制设置为: {limit}")
        except Exception as e:
            print(f"⚠️ [Clipboard] 设置历史限制失败: {e}")
    
    def start_monitoring(self, callback: Optional[Callable[[ClipboardItem], None]] = None):
        """
        开始监听剪贴板变化
        
        Args:
            callback: 剪贴板变化时的回调函数
        """
        if not self.is_available:
            print("⚠️ [Clipboard] 管理器不可用")
            return
        
        self._callback = callback
        
        def _on_change(py_item):
            """内部回调，转换类型后调用用户回调"""
            item = ClipboardItem.from_py_item(py_item)
            print(f"[Clipboard] 新内容: {item.icon} {item.display_text[:30]}")
            
            # 清理逻辑已由 Rust 后端处理，这里只需调用用户回调
            if self._callback:
                self._callback(item)
        
        try:
            self._manager.start_monitor(callback=_on_change)
            print("✅ [Clipboard] 开始监听剪贴板")
        except Exception as e:
            print(f"❌ [Clipboard] 启动监听失败: {e}")
    
    def stop_monitoring(self):
        """停止监听"""
        if self.is_available:
            try:
                self._manager.stop_monitor()
                print("🛑 [Clipboard] 停止监听")
            except Exception as e:
                print(f"⚠️ [Clipboard] 停止监听失败: {e}")
    
    def is_monitoring(self) -> bool:
        """检查是否正在监听"""
        if self.is_available:
            return self._manager.is_monitoring()
        return False
    
    def get_history(self, offset: int = 0, limit: int = 50, 
                    search: Optional[str] = None,
                    content_type: Optional[str] = None) -> List[ClipboardItem]:
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
            result = self._manager.get_history(offset, limit, search, content_type)
            return [ClipboardItem.from_py_item(item) for item in result.items]
        except Exception as e:
            print(f"❌ [Clipboard] 获取历史失败: {e}")
            return []
    
    def get_total_count(self) -> int:
        """获取总记录数"""
        if self.is_available:
            try:
                return self._manager.get_count()
            except:
                pass
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
            print(f"❌ [Clipboard] 获取项失败: {e}")
        return None
    
    def delete_item(self, item_id: int) -> bool:
        """删除项"""
        if not self.is_available:
            return False
        
        try:
            self._manager.delete_item(item_id)
            return True
        except Exception as e:
            print(f"❌ [Clipboard] 删除失败: {e}")
            return False
    
    def clear_history(self) -> bool:
        """清空历史"""
        if not self.is_available:
            return False
        
        try:
            self._manager.clear_history()
            print("🗑️ [Clipboard] 历史已清空")
            return True
        except Exception as e:
            print(f"❌ [Clipboard] 清空失败: {e}")
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
            print(f"✅ [Clipboard] 添加内容成功: ID={item_id}")
            return item_id
        except Exception as e:
            print(f"❌ [Clipboard] 添加内容失败: {e}")
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
            print(f"❌ [Clipboard] 更新内容失败: {e}")
            return False
    
    def toggle_pin(self, item_id: int) -> bool:
        """切换置顶状态，返回新状态"""
        if not self.is_available:
            return False
        
        try:
            return self._manager.toggle_pin(item_id)
        except Exception as e:
            print(f"❌ [Clipboard] 置顶失败: {e}")
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
            print(f"❌ [Clipboard] 粘贴失败: {e}")
            return False
    
    def get_image_data(self, image_id: str) -> Optional[bytes]:
        """获取图片数据"""
        if not self.is_available:
            return None
        
        try:
            return self._manager.get_image_data(image_id)
        except Exception as e:
            print(f"❌ [Clipboard] 获取图片失败: {e}")
            return None
    
    # ==================== 分组功能 ====================
    
    def create_group(self, name: str, color: Optional[str] = None, 
                     icon: Optional[str] = None) -> Optional[int]:
        """创建分组，返回分组 ID"""
        if not self.is_available:
            return None
        
        try:
            return self._manager.create_group(name, color, icon)
        except Exception as e:
            print(f"❌ [Clipboard] 创建分组失败: {e}")
            return None
    
    def get_groups(self) -> List[Group]:
        """获取所有分组"""
        if not self.is_available:
            return []
        
        try:
            py_groups = self._manager.get_groups()
            return [Group.from_py_group(g) for g in py_groups]
        except Exception as e:
            print(f"❌ [Clipboard] 获取分组失败: {e}")
            return []
    
    def delete_group(self, group_id: int) -> bool:
        """删除分组"""
        if not self.is_available:
            return False
        
        try:
            self._manager.delete_group(group_id)
            return True
        except Exception as e:
            print(f"❌ [Clipboard] 删除分组失败: {e}")
            return False
    
    def rename_group(self, group_id: int, name: str) -> bool:
        """重命名分组"""
        if not self.is_available:
            return False
        
        try:
            self._manager.rename_group(group_id, name)
            return True
        except Exception as e:
            print(f"❌ [Clipboard] 重命名分组失败: {e}")
            return False
    
    def update_group(self, group_id: int, name: str, 
                     color: Optional[str] = None, icon: Optional[str] = None) -> bool:
        """更新分组（名称、颜色、图标）"""
        if not self.is_available:
            return False
        
        try:
            self._manager.update_group(group_id, name, color, icon)
            return True
        except Exception as e:
            print(f"❌ [Clipboard] 更新分组失败: {e}")
            return False
    
    def move_to_group(self, item_id: int, group_id: Optional[int] = None) -> bool:
        """将项目移动到分组"""
        if not self.is_available:
            return False
        
        try:
            self._manager.move_to_group(item_id, group_id)
            return True
        except Exception as e:
            print(f"❌ [Clipboard] 移动到分组失败: {e}")
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
            print(f"❌ [Clipboard] 按分组查询失败: {e}")
            return []
    
    # ==================== 静态方法 ====================
    
    @staticmethod
    def get_clipboard_text() -> Optional[str]:
        """获取当前剪贴板文本"""
        if PYCLIPBOARD_AVAILABLE:
            return pyclipboard.get_clipboard_text()
        return None
    
    @staticmethod
    def set_clipboard_text(text: str) -> bool:
        """设置剪贴板文本"""
        if PYCLIPBOARD_AVAILABLE:
            try:
                pyclipboard.set_clipboard_text(text)
                return True
            except:
                pass
        return False
    
    @staticmethod
    def get_clipboard_image() -> Optional[bytes]:
        """获取剪贴板图片"""
        if PYCLIPBOARD_AVAILABLE:
            return pyclipboard.get_clipboard_image()
        return None
    
    @staticmethod
    def set_clipboard_image(image_bytes: bytes) -> bool:
        """设置剪贴板图片"""
        if PYCLIPBOARD_AVAILABLE:
            try:
                pyclipboard.set_clipboard_image(image_bytes)
                return True
            except:
                pass
        return False
    
    @staticmethod
    def get_clipboard_owner() -> Optional[str]:
        """获取剪贴板内容来源应用"""
        if PYCLIPBOARD_AVAILABLE:
            return pyclipboard.get_clipboard_owner()
        return None
