#[cfg(feature = "python")]
use pyo3::prelude::*;
use serde::{Deserialize, Serialize};

/// 剪贴板项
/// 
/// Attributes:
///     id: 唯一标识
///     content: 主要内容
///     html_content: HTML 富文本内容
///     content_type: 类型 ("text", "file", "image")
///     image_id: 图片文件 ID
///     thumbnail: 缩略图 Base64 (data:image/png;base64,...)
///     is_pinned: 是否置顶
///     paste_count: 粘贴次数
///     source_app: 来源应用
///     char_count: 字符数
///     created_at: 创建时间戳
///     updated_at: 更新时间戳
#[cfg_attr(feature = "python", pyclass(name = "ClipboardItem", get_all, set_all))]
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PyClipboardItem {
    pub id: i64,
    pub title: Option<String>,
    pub content: String,
    pub html_content: Option<String>,
    pub content_type: String,
    pub image_id: Option<String>,
    pub thumbnail: Option<String>,
    pub is_pinned: bool,
    pub paste_count: i64,
    pub source_app: Option<String>,
    pub char_count: Option<i64>,
    pub created_at: i64,
    pub updated_at: i64,
}

impl PyClipboardItem {
    pub fn new(id: i64, content: String, content_type: String) -> Self {
        let now = chrono::Local::now().timestamp();
        Self {
            id,
            title: None,
            content,
            html_content: None,
            content_type,
            image_id: None,
            thumbnail: None,
            is_pinned: false,
            paste_count: 0,
            source_app: None,
            char_count: None,
            created_at: now,
            updated_at: now,
        }
    }
}

/// 分页查询结果
/// 
/// 支持迭代和索引访问。
/// 
/// Attributes:
///     total_count: 总记录数
///     items: 当前页的数据列表
///     offset: 偏移量
///     limit: 每页数量
///     has_more: 是否还有更多数据
/// 
/// Example:
///     >>> result = manager.get_history()
///     >>> print(len(result))  # 当前页数量
///     >>> for item in result:  # 迭代
///     ...     print(item.content)
///     >>> first = result[0]  # 索引访问
#[cfg_attr(feature = "python", pyclass(name = "PaginatedResult", get_all))]
#[derive(Clone, Debug)]
pub struct PyPaginatedResult {
    pub total_count: i64,
    pub items: Vec<PyClipboardItem>,
    pub offset: i64,
    pub limit: i64,
    pub has_more: bool,
}

impl PyPaginatedResult {
    pub fn new(total_count: i64, items: Vec<PyClipboardItem>, offset: i64, limit: i64) -> Self {
        let items_len = items.len() as i64;
        let has_more = offset + items_len < total_count;
        Self {
            total_count,
            items,
            offset,
            limit,
            has_more,
        }
    }
}

/// 分组
/// 
/// Attributes:
///     id: 分组 ID
///     name: 分组名称
///     color: 分组颜色（可选）
///     icon: 分组图标（可选）
///     item_order: 排序顺序
///     created_at: 创建时间戳
///     group_type: 分组类型 (0=普通, 1=快速启动, 2=隐藏, 3/4/5=筛选文本/图片/文件)
#[cfg_attr(feature = "python", pyclass(name = "Group", get_all, set_all))]
#[derive(Clone, Debug)]
pub struct PyGroup {
    pub id: i64,
    pub name: String,
    pub color: Option<String>,
    pub icon: Option<String>,
    pub item_order: i64,
    pub created_at: i64,
    pub group_type: i64,
}
