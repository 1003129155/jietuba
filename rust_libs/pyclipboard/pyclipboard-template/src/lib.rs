use pyo3::prelude::*;
use pyo3::exceptions::PyRuntimeError;

mod database;
mod types;

use database::Database;
use types::{PyClipboardItem, PyQueryParams, PyPaginatedResult};

use std::sync::Arc;
use parking_lot::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use once_cell::sync::Lazy;
use std::thread;

// ============== 全局状态 ==============

static IS_RUNNING: AtomicBool = AtomicBool::new(false);
static CALLBACK: Lazy<Arc<Mutex<Option<PyObject>>>> = Lazy::new(|| Arc::new(Mutex::new(None)));

// ============== Python 模块 ==============

/// pyclipboard - Python 剪贴板管理库
#[pymodule]
fn pyclipboard(m: &Bound<'_, PyModule>) -> PyResult<()> {
    // 注册类
    m.add_class::<PyClipboardManager>()?;
    m.add_class::<PyClipboardItem>()?;
    m.add_class::<PyQueryParams>()?;
    m.add_class::<PyPaginatedResult>()?;
    
    // 注册函数
    m.add_function(wrap_pyfunction!(get_clipboard_text, m)?)?;
    m.add_function(wrap_pyfunction!(set_clipboard_text, m)?)?;
    m.add_function(wrap_pyfunction!(get_clipboard_image, m)?)?;
    m.add_function(wrap_pyfunction!(get_clipboard_files, m)?)?;
    m.add_function(wrap_pyfunction!(set_clipboard_files, m)?)?;
    
    Ok(())
}

// ============== 简单函数 ==============

/// 获取剪贴板文本
/// 
/// Returns:
///     Optional[str]: 剪贴板中的文本，如果没有文本则返回 None
#[pyfunction]
fn get_clipboard_text() -> PyResult<Option<String>> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    match ctx.get_text() {
        Ok(text) => Ok(Some(text)),
        Err(_) => Ok(None),
    }
}

/// 设置剪贴板文本
/// 
/// Args:
///     text: 要设置到剪贴板的文本
#[pyfunction]
fn set_clipboard_text(text: String) -> PyResult<()> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    ctx.set_text(text)
        .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板失败: {}", e)))
}

/// 获取剪贴板图片（返回 PNG 字节）
/// 
/// Returns:
///     Optional[bytes]: PNG 格式的图片数据，如果没有图片则返回 None
#[pyfunction]
fn get_clipboard_image() -> PyResult<Option<Vec<u8>>> {
    use clipboard_rs::{Clipboard, ClipboardContext, common::RustImage};
    use image::codecs::png::PngEncoder;
    use image::ImageEncoder;
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    match ctx.get_image() {
        Ok(rust_image) => {
            let rgba = rust_image.to_rgba8()
                .map_err(|e| PyRuntimeError::new_err(e.to_string()))?;
            
            let mut png_data = Vec::new();
            let encoder = PngEncoder::new(&mut png_data);
            encoder.write_image(
                rgba.as_raw(),
                rgba.width(),
                rgba.height(),
                image::ExtendedColorType::Rgba8,
            ).map_err(|e| PyRuntimeError::new_err(e.to_string()))?;
            
            Ok(Some(png_data))
        }
        Err(_) => Ok(None),
    }
}

/// 获取剪贴板文件路径列表
/// 
/// Returns:
///     List[str]: 文件路径列表
#[pyfunction]
fn get_clipboard_files() -> PyResult<Vec<String>> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    match ctx.get_files() {
        Ok(files) => Ok(files),
        Err(_) => Ok(vec![]),
    }
}

/// 设置剪贴板文件
/// 
/// Args:
///     files: 文件路径列表
#[pyfunction]
fn set_clipboard_files(files: Vec<String>) -> PyResult<()> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    ctx.set_files(files)
        .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板文件失败: {}", e)))
}

// ============== 剪贴板管理器 ==============

/// 剪贴板历史管理器
/// 
/// 用于管理剪贴板历史记录，支持监听、查询、搜索等功能。
/// 数据存储在 SQLite 数据库中。
/// 
/// Args:
///     db_path: 数据库文件路径，默认存储在用户数据目录
/// 
/// Example:
///     >>> manager = PyClipboardManager()
///     >>> manager.add_item("Hello World")
///     >>> result = manager.get_history()
///     >>> for item in result:
///     ...     print(item.content)
#[pyclass]
pub struct PyClipboardManager {
    db: Arc<Mutex<Database>>,
}

#[pymethods]
impl PyClipboardManager {
    #[new]
    #[pyo3(signature = (db_path=None))]
    fn new(db_path: Option<String>) -> PyResult<Self> {
        let path = db_path.unwrap_or_else(|| {
            dirs::data_dir()
                .unwrap_or_else(|| std::path::PathBuf::from("."))
                .join("pyclipboard")
                .join("clipboard.db")
                .to_string_lossy()
                .to_string()
        });
        
        // 确保目录存在
        if let Some(parent) = std::path::Path::new(&path).parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| PyRuntimeError::new_err(format!("创建目录失败: {}", e)))?;
        }
        
        let db = Database::new(&path)
            .map_err(|e| PyRuntimeError::new_err(e))?;
        
        Ok(Self {
            db: Arc::new(Mutex::new(db)),
        })
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
        use clipboard_rs::{ClipboardHandler, ClipboardWatcher, ClipboardWatcherContext, Clipboard, ClipboardContext};
        
        if IS_RUNNING.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst).is_err() {
            return Err(PyRuntimeError::new_err("监听器已在运行"));
        }
        
        // 保存回调
        if let Some(cb) = callback {
            *CALLBACK.lock() = Some(cb);
        }
        
        let db = self.db.clone();
        
        thread::spawn(move || {
            struct Handler {
                db: Arc<Mutex<Database>>,
            }
            
            impl ClipboardHandler for Handler {
                fn on_clipboard_change(&mut self) {
                    if !IS_RUNNING.load(Ordering::Relaxed) {
                        return;
                    }
                    
                    if let Ok(ctx) = ClipboardContext::new() {
                        let mut item: Option<PyClipboardItem> = None;
                        
                        // 尝试获取文本
                        if let Ok(text) = ctx.get_text() {
                            if !text.trim().is_empty() {
                                item = Some(PyClipboardItem::new(0, text, "text".to_string()));
                            }
                        }
                        
                        // 尝试获取文件
                        if item.is_none() {
                            if let Ok(files) = ctx.get_files() {
                                if !files.is_empty() {
                                    let content = serde_json::json!({ "files": files }).to_string();
                                    item = Some(PyClipboardItem::new(0, content, "file".to_string()));
                                }
                            }
                        }
                        
                        // 存储并回调
                        if let Some(mut clipboard_item) = item {
                            let db = self.db.lock();
                            if let Ok(id) = db.insert_item(&clipboard_item) {
                                clipboard_item.id = id;
                                
                                // 调用 Python 回调
                                if let Some(callback) = CALLBACK.lock().as_ref() {
                                    Python::with_gil(|py| {
                                        let _ = callback.call1(py, (clipboard_item.clone(),));
                                    });
                                }
                            }
                        }
                    }
                }
            }
            
            let handler = Handler { db };
            if let Ok(mut watcher) = ClipboardWatcherContext::new() {
                let _ = watcher.add_handler(handler).start_watch();
            }
            IS_RUNNING.store(false, Ordering::SeqCst);
        });
        
        Ok(())
    }
    
    /// 停止剪贴板监听
    fn stop_monitor(&self) -> PyResult<()> {
        IS_RUNNING.store(false, Ordering::SeqCst);
        *CALLBACK.lock() = None;
        Ok(())
    }
    
    /// 检查监听器是否运行中
    /// 
    /// Returns:
    ///     bool: 是否正在监听
    fn is_monitoring(&self) -> bool {
        IS_RUNNING.load(Ordering::Relaxed)
    }
    
    /// 查询剪贴板历史
    /// 
    /// Args:
    ///     offset: 偏移量，默认 0
    ///     limit: 每页数量，默认 50
    ///     search: 搜索关键词
    ///     content_type: 内容类型过滤 ("text", "file", "image", "all")
    /// 
    /// Returns:
    ///     PyPaginatedResult: 分页结果
    #[pyo3(signature = (offset=0, limit=50, search=None, content_type=None))]
    fn get_history(&self, offset: i64, limit: i64, search: Option<String>, content_type: Option<String>) -> PyResult<PyPaginatedResult> {
        let db = self.db.lock();
        db.query_items(offset, limit, search, content_type)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 获取总记录数
    /// 
    /// Returns:
    ///     int: 总记录数
    fn get_count(&self) -> PyResult<i64> {
        let db = self.db.lock();
        db.get_count()
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 根据 ID 获取项
    /// 
    /// Args:
    ///     id: 记录 ID
    /// 
    /// Returns:
    ///     Optional[PyClipboardItem]: 剪贴板项，不存在则返回 None
    fn get_item(&self, id: i64) -> PyResult<Option<PyClipboardItem>> {
        let db = self.db.lock();
        db.get_item_by_id(id)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 删除指定项
    /// 
    /// Args:
    ///     id: 要删除的记录 ID
    fn delete_item(&self, id: i64) -> PyResult<()> {
        let db = self.db.lock();
        db.delete_item(id)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 清空所有历史记录
    fn clear_history(&self) -> PyResult<()> {
        let db = self.db.lock();
        db.clear_all()
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 切换置顶状态
    /// 
    /// Args:
    ///     id: 记录 ID
    /// 
    /// Returns:
    ///     bool: 新的置顶状态
    fn toggle_pin(&self, id: i64) -> PyResult<bool> {
        let db = self.db.lock();
        db.toggle_pin(id)
            .map_err(|e| PyRuntimeError::new_err(e))
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
        let result = self.get_history(0, limit, Some(keyword), None)?;
        Ok(result.items)
    }
    
    /// 手动添加内容到历史
    /// 
    /// Args:
    ///     content: 内容文本
    ///     content_type: 内容类型，默认 "text"
    /// 
    /// Returns:
    ///     int: 新记录的 ID
    #[pyo3(signature = (content, content_type=None))]
    fn add_item(&self, content: String, content_type: Option<String>) -> PyResult<i64> {
        let item = PyClipboardItem::new(0, content, content_type.unwrap_or_else(|| "text".to_string()));
        let db = self.db.lock();
        db.insert_item(&item)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
}
