use pyo3::prelude::*;
use pyo3::exceptions::PyRuntimeError;

mod database;
mod types;

use database::Database;
use types::{PyClipboardItem, PyQueryParams, PyPaginatedResult, PyGroup};

use std::sync::Arc;
use parking_lot::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use once_cell::sync::Lazy;
use std::thread;
use std::path::PathBuf;

// ============== 全局状态 ==============

static IS_RUNNING: AtomicBool = AtomicBool::new(false);
static CALLBACK: Lazy<Arc<Mutex<Option<PyObject>>>> = Lazy::new(|| Arc::new(Mutex::new(None)));
// 跳过下一次剪贴板变化（用于防止 paste_item 自己触发监听）
static SKIP_NEXT_CHANGE: AtomicBool = AtomicBool::new(false);

// ============== Python 模块 ==============

/// pyclipboard - Python 剪贴板管理库
#[pymodule]
fn pyclipboard(m: &Bound<'_, PyModule>) -> PyResult<()> {
    // 注册类
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
    
    Ok(())
}

// ============== 简单函数 ==============

/// 生成 CF_HTML 格式（Windows 剪贴板 HTML 格式）
fn generate_cf_html(html: &str) -> String {
    // 如果 HTML 不完整，包装成完整的 HTML 文档
    let html_content = if !html.contains("<html") {
        format!(
            "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n</head>\n<body>\n<!--StartFragment-->{}<!--EndFragment-->\n</body>\n</html>",
            html
        )
    } else if !html.contains("<!--StartFragment-->") {
        html.replace("<body>", "<body>\n<!--StartFragment-->")
            .replace("</body>", "<!--EndFragment-->\n</body>")
    } else {
        html.to_string()
    };

    // CF_HTML 需要特定的头部格式
    let header = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    let start_html = header.len();
    let end_html = start_html + html_content.len();
    
    let start_fragment = start_html + html_content.find("<!--StartFragment-->").unwrap_or(0) + "<!--StartFragment-->".len();
    let end_fragment = start_html + html_content.find("<!--EndFragment-->").unwrap_or(html_content.len());

    format!(
        "Version:0.9\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\nStartFragment:{:010}\r\nEndFragment:{:010}\r\n{}",
        start_html,
        end_html,
        start_fragment,
        end_fragment,
        html_content
    )
}

/// 获取剪贴板文本
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
#[pyfunction]
fn set_clipboard_text(text: String) -> PyResult<()> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    ctx.set_text(text)
        .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板失败: {}", e)))
}

/// 获取剪贴板图片（返回 PNG 字节）
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

/// 设置剪贴板图片（从 PNG 字节）
#[pyfunction]
fn set_clipboard_image(image_bytes: Vec<u8>) -> PyResult<()> {
    use clipboard_rs::{Clipboard, ClipboardContext, common::RustImage};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    // 从 PNG 字节创建 RustImage
    let rust_image = RustImage::from_bytes(&image_bytes)
        .map_err(|e| PyRuntimeError::new_err(format!("解析图片失败: {}", e)))?;
    
    ctx.set_image(rust_image)
        .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板图片失败: {}", e)))
}

/// 获取剪贴板 HTML 内容
#[pyfunction]
fn get_clipboard_html() -> PyResult<Option<String>> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    match ctx.get_html() {
        Ok(html) => Ok(Some(html)),
        Err(_) => Ok(None),
    }
}

/// 获取剪贴板 RTF 富文本内容
#[pyfunction]
fn get_clipboard_rtf() -> PyResult<Option<String>> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    match ctx.get_rich_text() {
        Ok(rtf) => Ok(Some(rtf)),
        Err(_) => Ok(None),
    }
}

/// 获取剪贴板文件路径列表
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
#[pyfunction]
fn set_clipboard_files(files: Vec<String>) -> PyResult<()> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    ctx.set_files(files)
        .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板文件失败: {}", e)))
}

/// 获取剪贴板可用格式列表
#[pyfunction]
fn get_available_formats() -> PyResult<Vec<String>> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
    
    match ctx.available_formats() {
        Ok(formats) => Ok(formats),
        Err(_) => Ok(vec![]),
    }
}

/// 获取剪贴板内容的来源应用（仅 Windows）
#[pyfunction]
fn get_clipboard_owner() -> PyResult<Option<String>> {
    #[cfg(target_os = "windows")]
    {
        use std::ffi::OsString;
        use std::os::windows::ffi::OsStringExt;
        
        // Windows API 调用
        #[link(name = "user32")]
        extern "system" {
            fn GetClipboardOwner() -> *mut std::ffi::c_void;
            fn GetWindowThreadProcessId(hwnd: *mut std::ffi::c_void, lpdwProcessId: *mut u32) -> u32;
        }
        
        #[link(name = "kernel32")]
        extern "system" {
            fn OpenProcess(dwDesiredAccess: u32, bInheritHandle: i32, dwProcessId: u32) -> *mut std::ffi::c_void;
            fn CloseHandle(hObject: *mut std::ffi::c_void) -> i32;
            fn QueryFullProcessImageNameW(hProcess: *mut std::ffi::c_void, dwFlags: u32, lpExeName: *mut u16, lpdwSize: *mut u32) -> i32;
        }
        
        const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
        
        unsafe {
            let hwnd = GetClipboardOwner();
            if hwnd.is_null() {
                return Ok(None);
            }
            
            let mut process_id: u32 = 0;
            GetWindowThreadProcessId(hwnd, &mut process_id);
            
            if process_id == 0 {
                return Ok(None);
            }
            
            let handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, process_id);
            if handle.is_null() {
                return Ok(None);
            }
            
            let mut buffer = [0u16; 260];
            let mut size: u32 = 260;
            
            let result = QueryFullProcessImageNameW(handle, 0, buffer.as_mut_ptr(), &mut size);
            CloseHandle(handle);
            
            if result != 0 && size > 0 {
                let path = OsString::from_wide(&buffer[..size as usize]);
                let path_str = path.to_string_lossy().to_string();
                // 提取文件名
                if let Some(name) = std::path::Path::new(&path_str).file_name() {
                    return Ok(Some(name.to_string_lossy().to_string()));
                }
                return Ok(Some(path_str));
            }
        }
        
        Ok(None)
    }
    
    #[cfg(not(target_os = "windows"))]
    {
        Ok(None)
    }
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
    /// 数据库文件路径
    db_path: String,
    /// 历史记录数量限制，0 表示不限制
    history_limit: Arc<std::sync::atomic::AtomicI64>,
}

/// 全局历史限制（供监听线程使用）
static HISTORY_LIMIT: std::sync::atomic::AtomicI64 = std::sync::atomic::AtomicI64::new(0);

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
            db_path: path,
            history_limit: Arc::new(std::sync::atomic::AtomicI64::new(0)),
        })
    }
    
    /// 获取数据库文件路径
    /// 
    /// Returns:
    ///     str: 数据库文件的完整路径
    #[pyo3(name = "get_db_path")]
    fn get_db_path(&self) -> String {
        self.db_path.clone()
    }
    
    /// 获取图片存储目录路径
    /// 
    /// Returns:
    ///     str: 图片存储目录的完整路径
    #[pyo3(name = "get_images_dir")]
    fn get_images_dir_path(&self) -> String {
        let db = self.db.lock();
        db.get_images_dir().to_string_lossy().to_string()
    }
    
    /// 设置历史记录数量限制
    /// 
    /// Args:
    ///     limit: 最大记录数，0 表示不限制
    /// 
    /// 设置后，插入新记录时会自动清理超出限制的旧记录（保留置顶项）
    #[pyo3(name = "set_history_limit")]
    fn set_history_limit(&self, limit: i64) {
        self.history_limit.store(limit, Ordering::Relaxed);
        HISTORY_LIMIT.store(limit, Ordering::Relaxed);
        
        // 立即清理一次
        if limit > 0 {
            let db = self.db.lock();
            let _ = db.cleanup_old_items(limit);
        }
    }
    
    /// 获取当前历史记录数量限制
    #[pyo3(name = "get_history_limit")]
    fn get_history_limit(&self) -> i64 {
        self.history_limit.load(Ordering::Relaxed)
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
        
        // 获取图片存储路径
        let images_dir = {
            let db_lock = db.lock();
            db_lock.get_images_dir()
        };
        
        thread::spawn(move || {
            use clipboard_rs::common::RustImage;
            use image::codecs::png::PngEncoder;
            use image::ImageEncoder;
            use sha2::{Sha256, Digest};
            use base64::{Engine as _, engine::general_purpose};
            
            struct Handler {
                db: Arc<Mutex<Database>>,
                images_dir: PathBuf,
            }
            
            // 生成缩略图 Base64
            fn generate_thumbnail(rgba: &image::RgbaImage, max_size: u32) -> Option<String> {
                use image::imageops::FilterType;
                
                let (w, h) = (rgba.width(), rgba.height());
                let (new_w, new_h) = if w > h {
                    (max_size, (max_size as f32 * h as f32 / w as f32) as u32)
                } else {
                    ((max_size as f32 * w as f32 / h as f32) as u32, max_size)
                };
                
                let thumbnail = image::imageops::resize(rgba, new_w.max(1), new_h.max(1), FilterType::Triangle);
                
                let mut png_data = Vec::new();
                let encoder = PngEncoder::new(&mut png_data);
                if encoder.write_image(
                    thumbnail.as_raw(),
                    thumbnail.width(),
                    thumbnail.height(),
                    image::ExtendedColorType::Rgba8,
                ).is_ok() {
                    let base64_str = general_purpose::STANDARD.encode(&png_data);
                    Some(format!("data:image/png;base64,{}", base64_str))
                } else {
                    None
                }
            }
            
            impl ClipboardHandler for Handler {
                fn on_clipboard_change(&mut self) {
                    if !IS_RUNNING.load(Ordering::Relaxed) {
                        return;
                    }
                    
                    // 检查是否需要跳过（paste_item 触发的变化）
                    if SKIP_NEXT_CHANGE.compare_exchange(true, false, Ordering::SeqCst, Ordering::SeqCst).is_ok() {
                        return;
                    }
                    
                    if let Ok(ctx) = ClipboardContext::new() {
                        let mut item: Option<PyClipboardItem> = None;
                        
                        // 获取来源应用
                        let source_app = get_clipboard_owner().ok().flatten();
                        
                        // 获取 HTML 内容（如果有）
                        let html_content = ctx.get_html().ok();
                        
                        // 1. 优先尝试获取文本（避免 Excel 等应用的内容被误识别为图片）
                        if let Ok(text) = ctx.get_text() {
                            if !text.trim().is_empty() {
                                let mut text_item = PyClipboardItem::new(0, text, "text".to_string());
                                text_item.html_content = html_content.clone();
                                text_item.source_app = source_app.clone();
                                item = Some(text_item);
                            }
                        }
                        
                        // 2. 尝试获取文件
                        if item.is_none() {
                            if let Ok(files) = ctx.get_files() {
                                if !files.is_empty() {
                                    let content = serde_json::json!({ "files": files }).to_string();
                                    let mut file_item = PyClipboardItem::new(0, content, "file".to_string());
                                    file_item.source_app = source_app.clone();
                                    item = Some(file_item);
                                }
                            }
                        }
                        
                        // 3. 最后尝试获取图片（纯图片复制，如截图、图片编辑器等）
                        if item.is_none() {
                            if let Ok(rust_image) = ctx.get_image() {
                                if let Ok(rgba) = rust_image.to_rgba8() {
                                    let mut png_data = Vec::new();
                                    let encoder = PngEncoder::new(&mut png_data);
                                    if encoder.write_image(
                                        rgba.as_raw(),
                                        rgba.width(),
                                        rgba.height(),
                                        image::ExtendedColorType::Rgba8,
                                    ).is_ok() {
                                        // 计算图片哈希作为 ID
                                        let mut hasher = Sha256::new();
                                        hasher.update(&png_data);
                                        let hash = format!("{:x}", hasher.finalize());
                                        let image_id = hash[..16].to_string();
                                        
                                        // 保存图片到文件
                                        let image_path = self.images_dir.join(format!("{}.png", &image_id));
                                        if !image_path.exists() {
                                            let _ = std::fs::write(&image_path, &png_data);
                                        }
                                        
                                        // 生成缩略图 Base64 (64x64)
                                        let thumbnail = generate_thumbnail(&rgba, 64);
                                        
                                        // 创建图片类型的 item
                                        let mut img_item = PyClipboardItem::new(
                                            0,
                                            format!("[图片 {}x{}]", rgba.width(), rgba.height()),
                                            "image".to_string()
                                        );
                                        img_item.image_id = Some(image_id);
                                        img_item.thumbnail = thumbnail;
                                        img_item.source_app = source_app.clone();
                                        item = Some(img_item);
                                    }
                                }
                            }
                        }
                        
                        // 存储并回调
                        if let Some(mut clipboard_item) = item {
                            let db = self.db.lock();
                            if let Ok(id) = db.insert_item(&clipboard_item) {
                                clipboard_item.id = id;
                                
                                // 自动清理超出限制的旧记录
                                let limit = HISTORY_LIMIT.load(Ordering::Relaxed);
                                if limit > 0 {
                                    let _ = db.cleanup_old_items(limit);
                                }
                                
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
            
            let handler = Handler { db, images_dir };
            if let Ok(mut watcher) = ClipboardWatcherContext::new() {
                let _ = watcher.add_handler(handler).start_watch();
            }
            IS_RUNNING.store(false, Ordering::SeqCst);
        });
        
        Ok(())
    }
    
    /// 获取图片数据（通过 image_id）
    #[pyo3(signature = (image_id))]
    fn get_image_data(&self, image_id: String) -> PyResult<Option<Vec<u8>>> {
        let db = self.db.lock();
        let image_path = db.get_images_dir().join(format!("{}.png", image_id));
        
        if image_path.exists() {
            std::fs::read(&image_path)
                .map(Some)
                .map_err(|e| PyRuntimeError::new_err(format!("读取图片失败: {}", e)))
        } else {
            Ok(None)
        }
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
    ///     title: 标题（可选，用于收藏内容）
    /// 
    /// Returns:
    ///     int: 新记录的 ID
    #[pyo3(signature = (content, content_type=None, title=None))]
    fn add_item(&self, content: String, content_type: Option<String>, title: Option<String>) -> PyResult<i64> {
        let mut item = PyClipboardItem::new(0, content, content_type.unwrap_or_else(|| "text".to_string()));
        item.title = title;
        let db = self.db.lock();
        db.insert_item(&item)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 更新内容项
    /// 
    /// Args:
    ///     id: 内容 ID
    ///     title: 标题（可选）
    ///     content: 内容文本
    #[pyo3(signature = (id, content, title=None))]
    fn update_item(&self, id: i64, content: String, title: Option<String>) -> PyResult<()> {
        let db = self.db.lock();
        db.update_item(id, title.as_deref(), &content)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    // ==================== 分组功能 ====================
    
    /// 创建分组
    /// 
    /// Args:
    ///     name: 分组名称
    ///     color: 分组颜色（可选，如 "#FF0000"）
    ///     icon: 分组图标（可选）
    /// 
    /// Returns:
    ///     int: 新分组的 ID
    #[pyo3(signature = (name, color=None, icon=None))]
    fn create_group(&self, name: String, color: Option<String>, icon: Option<String>) -> PyResult<i64> {
        let db = self.db.lock();
        db.create_group(&name, color.as_deref(), icon.as_deref())
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 获取所有分组
    /// 
    /// Returns:
    ///     List[PyGroup]: 分组列表
    fn get_groups(&self) -> PyResult<Vec<PyGroup>> {
        let db = self.db.lock();
        db.get_groups()
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 删除分组
    /// 
    /// Args:
    ///     id: 分组 ID
    fn delete_group(&self, id: i64) -> PyResult<()> {
        let db = self.db.lock();
        db.delete_group(id)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 重命名分组
    /// 
    /// Args:
    ///     id: 分组 ID
    ///     name: 新名称
    fn rename_group(&self, id: i64, name: String) -> PyResult<()> {
        let db = self.db.lock();
        db.rename_group(id, &name)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 更新分组
    /// 
    /// Args:
    ///     id: 分组 ID
    ///     name: 名称
    ///     color: 颜色（可选）
    ///     icon: 图标（可选）
    #[pyo3(signature = (id, name, color=None, icon=None))]
    fn update_group(&self, id: i64, name: String, color: Option<String>, icon: Option<String>) -> PyResult<()> {
        let db = self.db.lock();
        db.update_group(id, &name, color.as_deref(), icon.as_deref())
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 将项目移动到分组
    /// 
    /// Args:
    ///     item_id: 剪贴板项 ID
    ///     group_id: 目标分组 ID（None 表示移出分组）
    #[pyo3(signature = (item_id, group_id=None))]
    fn move_to_group(&self, item_id: i64, group_id: Option<i64>) -> PyResult<()> {
        let db = self.db.lock();
        db.move_to_group(item_id, group_id)
            .map_err(|e| PyRuntimeError::new_err(e))
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
        let db = self.db.lock();
        db.query_by_group(group_id, offset, limit)
            .map_err(|e| PyRuntimeError::new_err(e))
    }
    
    /// 增加粘贴次数（当用户粘贴某项时调用）
    /// 
    /// Args:
    ///     id: 剪贴板项 ID
    /// 
    /// Returns:
    ///     int: 新的粘贴次数
    fn increment_paste_count(&self, id: i64) -> PyResult<i64> {
        let db = self.db.lock();
        db.increment_paste_count(id)
            .map_err(|e| PyRuntimeError::new_err(e))
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
        use clipboard_rs::{Clipboard, ClipboardContext, ClipboardContent, common::RustImage};
        
        // 设置跳过标志，防止自己触发监听
        SKIP_NEXT_CHANGE.store(true, Ordering::SeqCst);
        
        let db = self.db.lock();
        let item = db.get_item_by_id(id)
            .map_err(|e| PyRuntimeError::new_err(e))?;
        
        if let Some(item) = item {
            let ctx = ClipboardContext::new()
                .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
            
            match item.content_type.as_str() {
                "text" => {
                    // 如果有 HTML 内容且启用了带格式粘贴，同时设置文本和 HTML
                    if with_html {
                        if let Some(ref html) = item.html_content {
                            if !html.is_empty() {
                                // 生成 CF_HTML 格式
                                let cf_html = generate_cf_html(html);
                                ctx.set(vec![
                                    ClipboardContent::Text(item.content),
                                    ClipboardContent::Html(cf_html),
                                ])
                                .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板失败: {}", e)))?;
                            } else {
                                ctx.set_text(item.content)
                                    .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板失败: {}", e)))?;
                            }
                        } else {
                            ctx.set_text(item.content)
                                .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板失败: {}", e)))?;
                        }
                    } else {
                        // 不带格式，只粘贴纯文本
                        ctx.set_text(item.content)
                            .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板失败: {}", e)))?;
                    }
                }
                "image" => {
                    if let Some(image_id) = item.image_id {
                        let image_path = db.get_images_dir().join(format!("{}.png", image_id));
                        if image_path.exists() {
                            let image_bytes = std::fs::read(&image_path)
                                .map_err(|e| PyRuntimeError::new_err(format!("读取图片失败: {}", e)))?;
                            let rust_image = RustImage::from_bytes(&image_bytes)
                                .map_err(|e| PyRuntimeError::new_err(format!("解析图片失败: {}", e)))?;
                            ctx.set_image(rust_image)
                                .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板图片失败: {}", e)))?;
                        }
                    }
                }
                "file" => {
                    // 解析文件列表
                    if let Ok(json) = serde_json::from_str::<serde_json::Value>(&item.content) {
                        if let Some(files) = json.get("files").and_then(|f| f.as_array()) {
                            let file_paths: Vec<String> = files.iter()
                                .filter_map(|f| f.as_str().map(|s| s.to_string()))
                                .collect();
                            ctx.set_files(file_paths)
                                .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板文件失败: {}", e)))?;
                        }
                    }
                }
                _ => {}
            }
            
            // 增加粘贴次数
            drop(db);
            let db = self.db.lock();
            let _ = db.increment_paste_count(id);
            
            // 如果开启了"粘贴后移到最前"，更新 item_order
            if move_to_top {
                let _ = db.move_item_to_top(id);
            }
            
            Ok(true)
        } else {
            Ok(false)
        }
    }
}
