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
///     is_pinned: 是否置顶
///     paste_count: 粘贴次数
///     source_app: 来源应用
///     char_count: 字符数
///     created_at: 创建时间戳
///     updated_at: 更新时间戳
#[pyclass]
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct PyClipboardItem {
    #[pyo3(get, set)]
    pub id: i64,
    #[pyo3(get, set)]
    pub content: String,
    #[pyo3(get, set)]
    pub html_content: Option<String>,
    #[pyo3(get, set)]
    pub content_type: String,
    #[pyo3(get, set)]
    pub image_id: Option<String>,
    #[pyo3(get, set)]
    pub is_pinned: bool,
    #[pyo3(get, set)]
    pub paste_count: i64,
    #[pyo3(get, set)]
    pub source_app: Option<String>,
    #[pyo3(get, set)]
    pub char_count: Option<i64>,
    #[pyo3(get, set)]
    pub created_at: i64,
    #[pyo3(get, set)]
    pub updated_at: i64,
}

#[pymethods]
impl PyClipboardItem {
    #[new]
    #[pyo3(signature = (id, content, content_type))]
    pub fn new(id: i64, content: String, content_type: String) -> Self {
        let now = chrono::Local::now().timestamp();
        Self {
            id,
            content,
            html_content: None,
            content_type,
            image_id: None,
            is_pinned: false,
            paste_count: 0,
            source_app: None,
            char_count: None,
            created_at: now,
            updated_at: now,
        }
    }
    
    fn __repr__(&self) -> String {
        let preview = if self.content.len() > 50 { 
            format!("{}...", &self.content.chars().take(50).collect::<String>()) 
        } else { 
            self.content.clone() 
        };
        format!("ClipboardItem(id={}, type='{}', content='{}')", self.id, self.content_type, preview)
    }
    
    fn __str__(&self) -> String {
        self.content.clone()
    }
    
    /// 转换为 Python 字典
    /// 
    /// Returns:
    ///     dict: 包含所有属性的字典
    fn to_dict(&self, py: Python<'_>) -> PyResult<PyObject> {
        let dict = pyo3::types::PyDict::new_bound(py);
        dict.set_item("id", self.id)?;
        dict.set_item("content", &self.content)?;
        dict.set_item("html_content", &self.html_content)?;
        dict.set_item("content_type", &self.content_type)?;
        dict.set_item("image_id", &self.image_id)?;
        dict.set_item("is_pinned", self.is_pinned)?;
        dict.set_item("paste_count", self.paste_count)?;
        dict.set_item("source_app", &self.source_app)?;
        dict.set_item("char_count", self.char_count)?;
        dict.set_item("created_at", self.created_at)?;
        dict.set_item("updated_at", self.updated_at)?;
        Ok(dict.into())
    }
}

/// 查询参数
/// 
/// Attributes:
///     offset: 偏移量，默认 0
///     limit: 每页数量，默认 50
///     search: 搜索关键词
///     content_type: 内容类型过滤
#[pyclass]
#[derive(Clone, Debug)]
pub struct PyQueryParams {
    #[pyo3(get, set)]
    pub offset: i64,
    #[pyo3(get, set)]
    pub limit: i64,
    #[pyo3(get, set)]
    pub search: Option<String>,
    #[pyo3(get, set)]
    pub content_type: Option<String>,
}

#[pymethods]
impl PyQueryParams {
    #[new]
    #[pyo3(signature = (offset=0, limit=50, search=None, content_type=None))]
    fn new(offset: i64, limit: i64, search: Option<String>, content_type: Option<String>) -> Self {
        Self { offset, limit, search, content_type }
    }
    
    fn __repr__(&self) -> String {
        format!("QueryParams(offset={}, limit={}, search={:?})", self.offset, self.limit, self.search)
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
#[pyclass]
#[derive(Clone, Debug)]
pub struct PyPaginatedResult {
    #[pyo3(get)]
    pub total_count: i64,
    #[pyo3(get)]
    pub items: Vec<PyClipboardItem>,
    #[pyo3(get)]
    pub offset: i64,
    #[pyo3(get)]
    pub limit: i64,
    #[pyo3(get)]
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

#[pymethods]
impl PyPaginatedResult {
    fn __repr__(&self) -> String {
        format!("PaginatedResult(total={}, count={}, has_more={})", 
            self.total_count, self.items.len(), self.has_more)
    }
    
    fn __len__(&self) -> usize {
        self.items.len()
    }
    
    fn __getitem__(&self, index: usize) -> PyResult<PyClipboardItem> {
        self.items.get(index)
            .cloned()
            .ok_or_else(|| pyo3::exceptions::PyIndexError::new_err("索引超出范围"))
    }
    
    fn __iter__(slf: PyRef<'_, Self>) -> PyResult<Py<PyPaginatedResultIter>> {
        let items = slf.items.clone();
        let iter = PyPaginatedResultIter { items, index: 0 };
        Py::new(slf.py(), iter)
    }
}

#[pyclass]
pub struct PyPaginatedResultIter {
    items: Vec<PyClipboardItem>,
    index: usize,
}

#[pymethods]
impl PyPaginatedResultIter {
    fn __iter__(slf: PyRef<'_, Self>) -> PyRef<'_, Self> {
        slf
    }
    
    fn __next__(&mut self) -> Option<PyClipboardItem> {
        if self.index < self.items.len() {
            let item = self.items[self.index].clone();
            self.index += 1;
            Some(item)
        } else {
            None
        }
    }
}
