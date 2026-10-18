use std::sync::Arc;

use pyo3::exceptions::PyRuntimeError;
use pyo3::prelude::*;
use pyo3::types::PyBytes;

use crate::group_kind;
use crate::manager::{self, ClipboardManager, ItemCallback, MultiPasteOptions};
use crate::multi_paste;
use crate::types::{PyClipboardItem, PyGroup, PyPaginatedResult};

/// pyclipboard - Python 剪贴板管理库
#[pymodule]
fn pyclipboard(m: &Bound<'_, PyModule>) -> PyResult<()> {
    // 注册类
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;
    m.add("MAX_STITCHED_PIXELS", multi_paste::MAX_STITCHED_PIXELS)?;
    m.add_class::<PyClipboardManager>()?;
    m.add_class::<PyClipboardItem>()?;
    m.add_class::<PyQueryParams>()?;
    m.add_class::<PyPaginatedResult>()?;
    m.add_class::<PyGroup>()?;
    
    // 注册函数
    m.add_function(wrap_pyfunction!(get_clipboard_text, m)?)?;
    m.add_function(wrap_pyfunction!(set_clipboard_text, m)?)?;
    m.add_function(wrap_pyfunction!(get_clipboard_image, m)?)?;
    m.add_function(wrap_pyfunction!(set_clipboard_image, m)?)?;
    m.add_function(wrap_pyfunction!(get_clipboard_html, m)?)?;
    m.add_function(wrap_pyfunction!(get_clipboard_rtf, m)?)?;
    m.add_function(wrap_pyfunction!(get_clipboard_files, m)?)?;
    m.add_function(wrap_pyfunction!(set_clipboard_files, m)?)?;
    m.add_function(wrap_pyfunction!(get_available_formats, m)?)?;
    m.add_function(wrap_pyfunction!(get_clipboard_owner, m)?)?;
    m.add_function(wrap_pyfunction!(filter_content_type, m)?)?;
    m.add_function(wrap_pyfunction!(group_accepts, m)?)?;
    
    Ok(())
}

/// 监听线程拿到 GIL 再取回调：stop_monitor 持着 GIL 清掉的回调不会再被调用
fn gil_gate(f: &mut dyn FnMut()) {
    Python::with_gil(|_| f());
}

/// 获取剪贴板文本
#[pyfunction]
fn get_clipboard_text() -> PyResult<Option<String>> {
    manager::get_clipboard_text().map_err(PyRuntimeError::new_err)
}

/// 设置剪贴板文本
#[pyfunction]
fn set_clipboard_text(text: String) -> PyResult<()> {
    manager::set_clipboard_text(&text).map_err(PyRuntimeError::new_err)
}

/// 获取剪贴板图片（返回 PNG 字节）
#[pyfunction]
fn get_clipboard_image() -> PyResult<Option<Vec<u8>>> {
    manager::get_clipboard_image().map_err(PyRuntimeError::new_err)
}

/// 设置剪贴板图片（从 PNG 字节）
#[pyfunction]
fn set_clipboard_image(image_bytes: Vec<u8>) -> PyResult<()> {
    manager::set_clipboard_image(&image_bytes).map_err(PyRuntimeError::new_err)
}

/// 获取剪贴板 HTML 内容
#[pyfunction]
fn get_clipboard_html() -> PyResult<Option<String>> {
    manager::get_clipboard_html().map_err(PyRuntimeError::new_err)
}

/// 获取剪贴板 RTF 富文本内容
#[pyfunction]
fn get_clipboard_rtf() -> PyResult<Option<String>> {
    manager::get_clipboard_rtf().map_err(PyRuntimeError::new_err)
}

/// 获取剪贴板文件路径列表
#[pyfunction]
fn get_clipboard_files() -> PyResult<Vec<String>> {
    manager::get_clipboard_files().map_err(PyRuntimeError::new_err)
}

/// 设置剪贴板文件
#[pyfunction]
fn set_clipboard_files(files: Vec<String>) -> PyResult<()> {
    manager::set_clipboard_files(files).map_err(PyRuntimeError::new_err)
}

/// 获取剪贴板可用格式列表
#[pyfunction]
fn get_available_formats() -> PyResult<Vec<String>> {
    manager::get_available_formats().map_err(PyRuntimeError::new_err)
}

/// 获取剪贴板内容的来源应用（仅 Windows）
#[pyfunction]
fn get_clipboard_owner() -> PyResult<Option<String>> {
    Ok(manager::get_clipboard_owner())
}

/// 筛选分组筛的内容类型（"text" / "image" / "file"）；不是筛选分组返回 None
#[pyfunction]
fn filter_content_type(group_type: i64) -> Option<&'static str> {
    group_kind::filter_content_type(group_type)
}

/// 分组类型能不能存这类内容：快速启动只收文件，筛选分组不收内容
#[pyfunction]
fn group_accepts(group_type: i64, content_type: &str) -> bool {
    group_kind::accepts(group_type, content_type)
}

/// 剪贴板历史管理器
/// 
/// 用于管理剪贴板历史记录，支持监听、查询、搜索等功能。
/// 数据存储在 SQLite 数据库中。
/// 
/// Args:
///     db_path: 数据库文件路径，默认存储在用户数据目录
/// 
/// Example:
///     >>> manager = ClipboardManager()
///     >>> manager.add_item("Hello World")
///     >>> result = manager.get_history()
///     >>> for item in result:
///     ...     print(item.content)
#[pyclass(name = "ClipboardManager")]
pub struct PyClipboardManager {
    inner: ClipboardManager,
}

#[pymethods]
impl PyClipboardManager {
    #[new]
    #[pyo3(signature = (db_path=None))]
    fn new(db_path: Option<String>) -> PyResult<Self> {
        ClipboardManager::new(db_path).map(|inner| Self { inner }).map_err(PyRuntimeError::new_err)
    }

    /// 获取数据库文件路径
    #[getter]
    fn get_db_path(&self) -> String {
        self.inner.db_path()
    }

    /// 获取图片存储目录路径
    /// 
    /// Returns:
    ///     str: 图片存储目录的完整路径
    #[pyo3(name = "get_images_dir")]
    fn get_images_dir_path(&self) -> String {
        self.inner.get_images_dir()
    }

    /// 设置历史记录数量限制
    /// 
    /// Args:
    ///     limit: 最大记录数，0 表示不限制
    /// 
    /// 设置后，插入新记录时会自动清理超出限制的旧记录（保留置顶项）
    #[pyo3(name = "set_history_limit")]
    fn set_history_limit(&self, limit: i64) {
        self.inner.set_history_limit(limit)
    }

    /// 获取当前历史记录数量限制
    #[pyo3(name = "get_history_limit")]
    fn get_history_limit(&self) -> i64 {
        self.inner.get_history_limit()
    }

    /// 启动剪贴板监听
    /// 
    /// Args:
    ///     callback: 可选的回调函数，当剪贴板内容变化时调用
    /// 
    /// Example:
    ///     >>> def on_change(item):
    ///     ...     print(f"New: {item.content}")
    ///     >>> manager.start_monitor(callback=on_change)
    #[pyo3(signature = (callback=None))]
    fn start_monitor(&self, callback: Option<PyObject>) -> PyResult<()> {
        let callback = callback.map(|callback| -> ItemCallback {
            Arc::new(move |item: &PyClipboardItem| {
                Python::with_gil(|py| {
                    let _ = callback.call1(py, (item.clone(),));
                })
            })
        });
        self.inner.start_monitor(callback, gil_gate).map_err(PyRuntimeError::new_err)
    }

    /// 获取图片数据（通过 image_id）
    #[pyo3(signature = (image_id))]
    fn get_image_data<'py>(
        &self,
        py: Python<'py>,
        image_id: String,
    ) -> PyResult<Option<Bound<'py, PyBytes>>> {
        // Vec<u8> 的默认 Python 转换是逐元素的 list。图片稍大时，仅 list
        // 指针数组就会占用约原始字节数的 8 倍，并把进程工作集推到很高。
        // 直接构造 bytes，既符合 Python 包装层声明，也避免这份临时大列表。
        let data = self.inner.get_image_data(&image_id).map_err(PyRuntimeError::new_err)?;
        Ok(data.map(|data| PyBytes::new_bound(py, &data)))
    }

    /// 获取某条记录保存的所有原始剪贴板格式（Ditto 风格）
    /// 
    /// Returns:
    ///     List[Tuple[int, str, bytes]]: [(format_id, format_name, raw_data), ...]
    fn get_raw_formats(&self, id: i64) -> PyResult<Vec<(u32, String, Vec<u8>)>> {
        self.inner.get_raw_formats(id).map_err(PyRuntimeError::new_err)
    }

    /// 手动保存一批原始剪贴板格式数据（主要用于测试或外部调用）
    ///
    /// Args:
    ///     event_id: 关联的 clipboard.id
    ///     formats: List[Tuple[int, str, bytes]]，每项为 (format_id, format_name, raw_data)
    fn insert_formats(&self, event_id: i64, formats: Vec<(u32, String, Vec<u8>)>) -> PyResult<()> {
        self.inner.insert_formats(event_id, formats).map_err(PyRuntimeError::new_err)
    }

    /// 停止剪贴板监听
    fn stop_monitor(&self, py: Python<'_>) -> PyResult<()> {
        self.inner.stop_monitor(|join| py.allow_threads(join));
        Ok(())
    }

    /// 检查监听器是否运行中
    /// 
    /// Returns:
    ///     bool: 是否正在监听
    fn is_monitoring(&self) -> bool {
        self.inner.is_monitoring()
    }

    /// Return without waiting; the GUI keeps the download for a later retry when busy.
    fn pause_for_update(&self) -> bool {
        self.inner.pause_for_update()
    }


    fn resume_after_update(&self) {
        self.inner.resume_after_update()
    }


    fn pending_writes(&self) -> usize {
        self.inner.pending_writes()
    }

    /// 查询剪贴板历史
    /// 
    /// Args:
    ///     offset: 偏移量，默认 0
    ///     limit: 每页数量，
    ///     search: 搜索关键词
    ///     content_type: 内容类型过滤 ("text", "file", "image", "all")
    /// 
    /// Returns:
    ///     PyPaginatedResult: 分页结果
    #[pyo3(signature = (offset=0, limit=50, search=None, content_type=None, start_time=None, end_time=None))]
    fn get_history(
        &self,
        offset: i64,
        limit: i64,
        search: Option<String>,
        content_type: Option<String>,
        start_time: Option<i64>,
        end_time: Option<i64>,
    ) -> PyResult<PyPaginatedResult> {
        self.inner.get_history(offset, limit, search, content_type, start_time, end_time).map_err(PyRuntimeError::new_err)
    }

    /// 获取总记录数
    /// 
    /// Returns:
    ///     int: 总记录数
    fn get_count(&self) -> PyResult<i64> {
        self.inner.get_count().map_err(PyRuntimeError::new_err)
    }

    /// 根据 ID 获取项
    /// 
    /// Args:
    ///     id: 记录 ID
    /// 
    /// Returns:
    ///     Optional[PyClipboardItem]: 剪贴板项，不存在则返回 None
    fn get_item(&self, id: i64) -> PyResult<Option<PyClipboardItem>> {
        self.inner.get_item(id).map_err(PyRuntimeError::new_err)
    }

    /// 删除指定项
    /// 
    /// Args:
    ///     id: 要删除的记录 ID
    fn delete_item(&self, id: i64) -> PyResult<()> {
        self.inner.delete_item(id).map_err(PyRuntimeError::new_err)
    }

    /// 清空历史记录
    ///
    /// Args:
    ///     keep_grouped: True = 保留已加入分组的条目，只删历史区；False = 删除全部（默认）
    #[pyo3(signature = (keep_grouped=false))]
    fn clear_history(&self, keep_grouped: bool) -> PyResult<()> {
        self.inner.clear_history(keep_grouped).map_err(PyRuntimeError::new_err)
    }

    /// 清理没有置顶、也没有加入分组的条目；置顶和分组里的内容都保留
    ///
    /// Args:
    ///     vacuum: True = 删完整理数据库文件、立即回收磁盘空间（默认）；
    ///             False = 只删行，空间留在库里复用，适合只想让条目不再显示的场合
    #[pyo3(signature = (vacuum=true))]
    fn clear_unpinned_ungrouped(&self, vacuum: bool) -> PyResult<()> {
        self.inner.clear_unpinned_ungrouped(vacuum).map_err(PyRuntimeError::new_err)
    }

    /// 切换置顶状态
    /// 
    /// Args:
    ///     id: 记录 ID
    /// 
    /// Returns:
    ///     bool: 新的置顶状态
    fn toggle_pin(&self, id: i64) -> PyResult<bool> {
        self.inner.toggle_pin(id).map_err(PyRuntimeError::new_err)
    }

    /// 搜索内容
    /// 
    /// Args:
    ///     keyword: 搜索关键词
    ///     limit: 返回数量限制，默认 50
    /// 
    /// Returns:
    ///     List[PyClipboardItem]: 匹配的记录列表
    #[pyo3(signature = (keyword, limit=50))]
    fn search(&self, keyword: String, limit: i64) -> PyResult<Vec<PyClipboardItem>> {
        self.inner.search(keyword, limit).map_err(PyRuntimeError::new_err)
    }

    /// 手动添加内容到历史
    /// 
    /// Args:
    ///     content: 内容文本
    ///     content_type: 内容类型，默认 "text"
    ///     title: 标题（可选，用于收藏内容）
    /// 
    /// Returns:
    ///     int: 新记录的 ID
    #[pyo3(signature = (content, content_type=None, title=None))]
    fn add_item(&self, content: String, content_type: Option<String>, title: Option<String>) -> PyResult<i64> {
        self.inner.add_item(content, content_type, title).map_err(PyRuntimeError::new_err)
    }

    /// 更新内容项
    /// 
    /// Args:
    ///     id: 内容 ID
    ///     title: 标题（可选）
    ///     content: 内容文本
    #[pyo3(signature = (id, content, title=None))]
    fn update_item(&self, id: i64, content: String, title: Option<String>) -> PyResult<()> {
        self.inner.update_item(id, content, title).map_err(PyRuntimeError::new_err)
    }

    /// 移动剪贴板内容到指定位置（拖拽排序）
    /// 
    /// Args:
    ///     id: 要移动的项 ID
    ///     before_id: 它前面的项 ID（None = 移到最前）
    ///     after_id: 它后面的项 ID（None = 移到最后）
    /// 
    /// Example:
    ///     # 将 item_3 移到 item_1 和 item_2 之间
    ///     manager.move_item_between(3, before_id=1, after_id=2)
    ///     
    ///     # 移到最前面
    ///     manager.move_item_between(3, before_id=None, after_id=1)
    ///     
    ///     # 移到最后面
    ///     manager.move_item_between(3, before_id=5, after_id=None)
    #[pyo3(signature = (id, before_id=None, after_id=None))]
    fn move_item_between(&self, id: i64, before_id: Option<i64>, after_id: Option<i64>) -> PyResult<()> {
        self.inner.move_item_between(id, before_id, after_id).map_err(PyRuntimeError::new_err)
    }

    /// 创建分组
    /// 
    /// Args:
    ///     name: 分组名称
    ///     color: 分组颜色（可选，如 "#FF0000"）
    ///     icon: 分组图标（可选）
    ///     group_type: 分组类型 (0=普通, 1=快速启动, 2=隐藏, 3/4/5=筛选文本/图片/文件)。默认 0
    /// 
    /// Returns:
    ///     int: 新分组的 ID
    #[pyo3(signature = (name, color=None, icon=None, group_type=0))]
    fn create_group(&self, name: String, color: Option<String>, icon: Option<String>, group_type: i64) -> PyResult<i64> {
        self.inner.create_group(name, color, icon, group_type).map_err(PyRuntimeError::new_err)
    }

    /// 获取所有分组
    /// 
    /// Returns:
    ///     List[PyGroup]: 分组列表
    fn get_groups(&self) -> PyResult<Vec<PyGroup>> {
        self.inner.get_groups().map_err(PyRuntimeError::new_err)
    }

    /// 删除分组
    /// 
    /// Args:
    ///     id: 分组 ID
    fn delete_group(&self, id: i64) -> PyResult<()> {
        self.inner.delete_group(id).map_err(PyRuntimeError::new_err)
    }

    /// 重命名分组
    /// 
    /// Args:
    ///     id: 分组 ID
    ///     name: 新名称
    fn rename_group(&self, id: i64, name: String) -> PyResult<()> {
        self.inner.rename_group(id, name).map_err(PyRuntimeError::new_err)
    }

    /// 更新分组
    /// 
    /// Args:
    ///     id: 分组 ID
    ///     name: 名称
    ///     color: 颜色（可选）
    ///     icon: 图标（可选）
    ///     group_type: 分组类型 (0=普通, 1=快速启动, 2=隐藏, 3/4/5=筛选文本/图片/文件)。默认 0
    #[pyo3(signature = (id, name, color=None, icon=None, group_type=0))]
    fn update_group(&self, id: i64, name: String, color: Option<String>, icon: Option<String>, group_type: i64) -> PyResult<()> {
        self.inner.update_group(id, name, color, icon, group_type).map_err(PyRuntimeError::new_err)
    }

    /// 将项目移动到分组
    /// 
    /// Args:
    ///     item_id: 剪贴板项 ID
    ///     group_id: 目标分组 ID（None 表示移出分组）
    #[pyo3(signature = (item_id, group_id=None))]
    fn move_to_group(&self, item_id: i64, group_id: Option<i64>) -> PyResult<()> {
        self.inner.move_to_group(item_id, group_id).map_err(PyRuntimeError::new_err)
    }

    /// 移动分组到指定位置（拖拽排序）
    /// 
    /// Args:
    ///     id: 要移动的分组 ID
    ///     before_id: 它前面的分组 ID（None = 移到最前）
    ///     after_id: 它后面的分组 ID（None = 移到最后）
    /// 
    /// Example:
    ///     # 将分组 3 移到分组 1 和分组 2 之间
    ///     manager.move_group_between(3, before_id=1, after_id=2)
    #[pyo3(signature = (id, before_id=None, after_id=None))]
    fn move_group_between(&self, id: i64, before_id: Option<i64>, after_id: Option<i64>) -> PyResult<()> {
        self.inner.move_group_between(id, before_id, after_id).map_err(PyRuntimeError::new_err)
    }

    /// 按分组查询
    /// 
    /// Args:
    ///     group_id: 分组 ID（None 表示查询未分组的项目）
    ///     offset: 偏移量，默认 0
    ///     limit: 每页数量，默认 50
    /// 
    /// Returns:
    ///     PyPaginatedResult: 分页结果
    #[pyo3(signature = (group_id=None, offset=0, limit=50))]
    fn get_by_group(&self, group_id: Option<i64>, offset: i64, limit: i64) -> PyResult<PyPaginatedResult> {
        self.inner.get_by_group(group_id, offset, limit).map_err(PyRuntimeError::new_err)
    }

    /// 增加粘贴次数（当用户粘贴某项时调用）
    /// 
    /// Args:
    ///     id: 剪贴板项 ID
    /// 
    /// Returns:
    ///     int: 新的粘贴次数
    fn increment_paste_count(&self, id: i64) -> PyResult<i64> {
        self.inner.increment_paste_count(id).map_err(PyRuntimeError::new_err)
    }

    /// 将项目内容设置到剪贴板（用于粘贴）
    ///
    /// Args:
    ///     id: 剪贴板项 ID
    ///     with_html: 是否包含 HTML 格式（默认 true）
    ///
    /// Returns:
    ///     bool: 是否成功
    #[pyo3(signature = (id, with_html=true, move_to_top=true))]
    fn paste_item(&self, id: i64, with_html: bool, move_to_top: bool) -> PyResult<bool> {
        self.inner.paste_item(id, with_html, move_to_top).map_err(PyRuntimeError::new_err)
    }

    /// 把几条记录合成一份内容写进剪贴板（多选粘贴）
    ///
    /// 文本按分隔符拼接，HTML、RTF 各合成一份，文件取并集；选中的全是图片时拼成
    /// 一张，否则图片不参与。
    ///
    /// Args:
    ///     ids: 按粘贴顺序排好的记录 ID
    ///     with_html: 带上合并后的 HTML、RTF
    ///     move_to_top: 用到的记录移到最前，彼此的先后不变
    ///     separator: 文本之间的分隔符，默认换行
    ///     plain_text: 只写纯文本，文件记录按路径写成文本
    ///     layout: 全是图片时的拼接方向，"vertical"（默认）或 "horizontal"
    ///     keep_in_history: 合并结果作为一条新记录进历史
    ///
    /// Returns:
    ///     list[int]: 实际用上的记录 ID；什么都没写进剪贴板时为空
    #[pyo3(signature = (ids, with_html=true, move_to_top=false, separator=None, plain_text=false, layout=None, keep_in_history=false))]
    #[allow(clippy::too_many_arguments)]
    fn paste_items(
        &self,
        py: Python<'_>,
        ids: Vec<i64>,
        with_html: bool,
        move_to_top: bool,
        separator: Option<String>,
        plain_text: bool,
        layout: Option<String>,
        keep_in_history: bool,
    ) -> PyResult<Vec<i64>> {
        let options = MultiPasteOptions::new(with_html, move_to_top, separator, plain_text, layout.as_deref(), keep_in_history)
            .map_err(pyo3::exceptions::PyValueError::new_err)?;
        let inner = &self.inner;
        // 拼图、编码可能要几百毫秒，期间放开 GIL
        py.allow_threads(move || inner.paste_items(&ids, &options))
            .map_err(PyRuntimeError::new_err)
    }
}

// ============== 数据类型的 Python 接口 ==============

#[pymethods]
impl PyClipboardItem {
    #[new]
    #[pyo3(signature = (id, content, content_type))]
    fn py_new(id: i64, content: String, content_type: String) -> Self {
        Self::new(id, content, content_type)
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
        dict.set_item("thumbnail", &self.thumbnail)?;
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
#[pyclass(name = "QueryParams")]
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
    #[pyo3(get, set)]
    pub start_time: Option<i64>,
    #[pyo3(get, set)]
    pub end_time: Option<i64>,
}

#[pymethods]
impl PyQueryParams {
    #[new]
    #[pyo3(signature = (offset=0, limit=50, search=None, content_type=None, start_time=None, end_time=None))]
    fn new(
        offset: i64,
        limit: i64,
        search: Option<String>,
        content_type: Option<String>,
        start_time: Option<i64>,
        end_time: Option<i64>,
    ) -> Self {
        Self { offset, limit, search, content_type, start_time, end_time }
    }
    
    fn __repr__(&self) -> String {
        format!("QueryParams(offset={}, limit={}, search={:?})", self.offset, self.limit, self.search)
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

#[pyclass(name = "PaginatedResultIter")]
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

#[pymethods]
impl PyGroup {
    #[new]
    #[pyo3(signature = (id, name, color=None, icon=None, group_type=0))]
    fn py_new(id: i64, name: String, color: Option<String>, icon: Option<String>, group_type: i64) -> Self {
        Self {
            id,
            name,
            color,
            icon,
            item_order: 0,
            created_at: chrono::Local::now().timestamp(),
            group_type,
        }
    }
    
    fn __repr__(&self) -> String {
        format!("Group(id={}, name='{}', group_type={})", self.id, self.name, self.group_type)
    }
    
    fn __str__(&self) -> String {
        self.name.clone()
    }
}
