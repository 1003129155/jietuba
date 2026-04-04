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
use zstd;

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
    #[getter]
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
        use clipboard_rs::{ClipboardHandler, ClipboardWatcher, ClipboardWatcherContext};
        
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

            // ── Ditto 风格：按白名单逐个取，不枚举全部格式 ─────────────────
            // 策略：先用 IsClipboardFormatAvailable 轻量探测（不分配内存），
            //       命中后才调用 GetClipboardData + GlobalLock 真正读取。
            // 优势：Word/WPS 等程序会往剪贴板塞几十种私有格式（总计可达数十 MB），
            //       先全枚举再筛选会把这些全读进内存再丢掉；按白名单取则完全跳过它们。
            //
            // 同时做一次轻量的"全格式探测"（只拿名称+ID，不读数据），
            // 用于兜底判断剪贴板是否含有图片类数据（raw_image_fallback 逻辑）。
            #[cfg(target_os = "windows")]
            fn read_whitelisted_formats() -> (Vec<(u32, String, Vec<u8>)>, Vec<(u32, String)>) {
                // 返回值：
                //   .0  whitelisted_data  — 白名单格式的完整数据（存入 DB）
                //   .1  all_format_names  — 剪贴板上所有格式的 (id, name)（仅用于兜底探测）
                use std::ffi::OsString;
                use std::os::windows::ffi::OsStringExt;

                #[link(name = "user32")]
                extern "system" {
                    fn OpenClipboard(hwnd: *mut std::ffi::c_void) -> i32;
                    fn CloseClipboard() -> i32;
                    fn IsClipboardFormatAvailable(format: u32) -> i32;
                    fn EnumClipboardFormats(format: u32) -> u32;
                    fn GetClipboardData(format: u32) -> *mut std::ffi::c_void;
                    fn GetClipboardFormatNameW(fmt: u32, buf: *mut u16, max: i32) -> i32;
                    fn RegisterClipboardFormatW(lpszFormat: *const u16) -> u32;
                }
                #[link(name = "kernel32")]
                extern "system" {
                    fn GlobalLock(hmem: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
                    fn GlobalUnlock(hmem: *mut std::ffi::c_void) -> i32;
                    fn GlobalSize(hmem: *mut std::ffi::c_void) -> usize;
                }

                // 把格式名称字符串转为 wide 用于 RegisterClipboardFormatW
                fn to_wide(s: &str) -> Vec<u16> {
                    s.encode_utf16().chain(std::iter::once(0)).collect()
                }

                // 标准格式名称
                fn standard_name(id: u32) -> Option<&'static str> {
                    match id {
                        1  => Some("CF_TEXT"),
                        7  => Some("CF_OEMTEXT"),
                        8  => Some("CF_DIB"),
                        13 => Some("CF_UNICODETEXT"),
                        15 => Some("CF_HDROP"),
                        16 => Some("CF_LOCALE"),
                        17 => Some("CF_DIBV5"),
                        _  => None,
                    }
                }

                // 白名单定义：(format_id_or_0, name)
                // format_id=0 表示需要用 RegisterClipboardFormatW 动态查询 ID
                // format_id 已知的标准格式直接填写
                struct WlEntry { id: u32, name: &'static str }
                let whitelist: &[WlEntry] = &[
                    WlEntry { id: 1,  name: "CF_TEXT" },
                    WlEntry { id: 8,  name: "CF_DIB" },
                    WlEntry { id: 13, name: "CF_UNICODETEXT" },
                    WlEntry { id: 15, name: "CF_HDROP" },
                    WlEntry { id: 16, name: "CF_LOCALE" },
                    WlEntry { id: 17, name: "CF_DIBV5" },
                    WlEntry { id: 0,  name: "PNG" },
                    WlEntry { id: 0,  name: "HTML Format" },
                    WlEntry { id: 0,  name: "Rich Text Format" },
                ];

                let mut data_result: Vec<(u32, String, Vec<u8>)> = Vec::new();
                let mut all_names: Vec<(u32, String)> = Vec::new();

                unsafe {
                    // ── 阶段1：轻量探测 + 全格式枚举（仅取名称，不读数据）────
                    // 目的：为 raw_image_fallback 收集全部格式名称列表
                    if OpenClipboard(std::ptr::null_mut()) == 0 {
                        return (data_result, all_names);
                    }
                    let mut fmt: u32 = 0;
                    loop {
                        fmt = EnumClipboardFormats(fmt);
                        if fmt == 0 { break; }
                        let name = if let Some(s) = standard_name(fmt) {
                            s.to_string()
                        } else {
                            let mut buf = [0u16; 256];
                            let len = GetClipboardFormatNameW(fmt, buf.as_mut_ptr(), 256);
                            if len > 0 {
                                OsString::from_wide(&buf[..len as usize]).to_string_lossy().into_owned()
                            } else {
                                format!("UNKNOWN_{}", fmt)
                            }
                        };
                        all_names.push((fmt, name));
                    }
                    CloseClipboard();

                    // ── 阶段2：按白名单逐个取数据（Ditto 风格）──────────────
                    // IsClipboardFormatAvailable 不需要打开剪贴板，直接探测
                    // 先收集命中的 (id, name) 列表，再一次性打开剪贴板读取
                    let mut to_read: Vec<(u32, &'static str)> = Vec::new();
                    for entry in whitelist {
                        let fmt_id = if entry.id != 0 {
                            entry.id
                        } else {
                            // 动态格式：用 RegisterClipboardFormatW 获取 ID（若未注册则返回 0）
                            let wide = to_wide(entry.name);
                            RegisterClipboardFormatW(wide.as_ptr())
                        };
                        if fmt_id == 0 { continue; }
                        if IsClipboardFormatAvailable(fmt_id) != 0 {
                            to_read.push((fmt_id, entry.name));
                        }
                    }

                    if to_read.is_empty() {
                        return (data_result, all_names);
                    }

                    // 一次打开剪贴板，读取所有命中的白名单格式
                    if OpenClipboard(std::ptr::null_mut()) == 0 {
                        return (data_result, all_names);
                    }
                    for (fmt_id, name) in &to_read {
                        let hmem = GetClipboardData(*fmt_id);
                        if hmem.is_null() { continue; }
                        let ptr = GlobalLock(hmem);
                        if ptr.is_null() { continue; }
                        let size = GlobalSize(hmem);
                        let data = if size > 0 && size <= 64 * 1024 * 1024 {
                            std::slice::from_raw_parts(ptr as *const u8, size).to_vec()
                        } else {
                            GlobalUnlock(hmem);
                            continue;
                        };
                        GlobalUnlock(hmem);
                        data_result.push((*fmt_id, name.to_string(), data));
                    }
                    CloseClipboard();
                }

                (data_result, all_names)
            }

            #[cfg(not(target_os = "windows"))]
            fn read_whitelisted_formats() -> (Vec<(u32, String, Vec<u8>)>, Vec<(u32, String)>) {
                (Vec::new(), Vec::new())
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

                    // ── 第一步：Ditto 风格按白名单读取格式数据 ────────────────
                    // raw_formats  = 白名单格式的完整数据（直接存 DB，已经过滤好）
                    // all_names    = 剪贴板上所有格式的 (id, name)（仅用于 fallback 探测）
                    let (raw_formats, all_names) = read_whitelisted_formats();

                    // ── 第二步：高层 API 解析主记录（用于 UI 展示）────────────
                    use clipboard_rs::{Clipboard, ClipboardContext};
                    let ctx = match ClipboardContext::new() {
                        Ok(c) => c,
                        Err(_) => return,
                    };

                    let source_app = get_clipboard_owner().ok().flatten();
                    let html_content = ctx.get_html().ok();

                    let text_val  = ctx.get_text().ok().filter(|t| !t.trim().is_empty());
                    let files_val = ctx.get_files().ok().filter(|f| !f.is_empty());
                    let image_val = ctx.get_image().ok();

                    // 高层 API 全部失败时，检查白名单数据或全格式名称列表是否含图片类格式
                    // 场景：Word 复制多张图片时 get_image() 返回 None，但 raw_formats 里有 PNG/DIB
                    let raw_image_fallback = if text_val.is_none() && files_val.is_none() && image_val.is_none() {
                        let has_image_data = raw_formats.iter().any(|(fid, fname, data)| {
                            !data.is_empty() && (*fid == 8 || *fid == 17 || fname.eq_ignore_ascii_case("PNG"))
                        });
                        // 也检查 all_names，防止白名单中没有 PNG/DIB 但剪贴板里有其他图片格式
                        let has_image_name = all_names.iter().any(|(fid, fname)| {
                            *fid == 8 || *fid == 17 || fname.eq_ignore_ascii_case("PNG")
                        });
                        has_image_data || has_image_name
                    } else {
                        false
                    };

                    if text_val.is_none() && files_val.is_none() && image_val.is_none() && !raw_image_fallback {
                        return;
                    }

                    // ── 第三步：构造主记录 ────────────────────────────────────
                    let mut main_item: PyClipboardItem;

                    if let Some(text) = text_val {
                        main_item = PyClipboardItem::new(0, text, "text".to_string());
                        main_item.html_content = html_content;
                        main_item.source_app = source_app;
                    } else if let Some(files) = files_val {
                        let content = serde_json::json!({ "files": files }).to_string();
                        main_item = PyClipboardItem::new(0, content, "file".to_string());
                        main_item.source_app = source_app;
                    } else if image_val.is_some() {
                        // 单张图片：落盘 PNG，生成缩略图
                        let rust_image = image_val.unwrap();
                        let rgba = match rust_image.to_rgba8() {
                            Ok(r) => r,
                            Err(_) => return,
                        };
                        let mut png_data = Vec::new();
                        let encoder = PngEncoder::new(&mut png_data);
                        if encoder.write_image(
                            rgba.as_raw(),
                            rgba.width(),
                            rgba.height(),
                            image::ExtendedColorType::Rgba8,
                        ).is_err() {
                            return;
                        }

                        let mut hasher = Sha256::new();
                        hasher.update(&png_data);
                        let hash = format!("{:x}", hasher.finalize());
                        let image_id = hash[..16].to_string();

                        let image_path = self.images_dir.join(format!("{}.png", &image_id));
                        if !image_path.exists() {
                            let _ = std::fs::write(&image_path, &png_data);
                        }

                        let thumbnail = generate_thumbnail(&rgba, 64);

                        main_item = PyClipboardItem::new(
                            0,
                            format!("[{}x{}]", rgba.width(), rgba.height()),
                            "image".to_string(),
                        );
                        main_item.image_id = Some(image_id);
                        main_item.thumbnail = thumbnail;
                        main_item.source_app = source_app;
                    } else {
                        // raw_image_fallback：多图/EMF 等高层 API 无法解析的图片内容
                        // content 写入格式列表和总字节数，供前端直接显示
                        // 例：[PNG+CF_DIB 7.9 MB] 或 [PNG 1.2 MB]
                        let img_fmt_names: Vec<&str> = {
                            let mut names = Vec::new();
                            for (fid, fname, data) in &raw_formats {
                                if data.is_empty() { continue; }
                                if *fid == 17 { names.push("CF_DIBV5"); }
                                else if *fid == 8 { names.push("CF_DIB"); }
                                else if fname.eq_ignore_ascii_case("PNG") { names.push("PNG"); }
                            }
                            names.dedup();
                            names
                        };
                        let total_bytes: usize = raw_formats.iter()
                            .filter(|(fid, fname, _)| *fid == 8 || *fid == 17 || fname.eq_ignore_ascii_case("PNG"))
                            .map(|(_, _, d)| d.len())
                            .sum();
                        let size_str = if total_bytes >= 1024 * 1024 {
                            format!("{:.1} MB", total_bytes as f64 / 1024.0 / 1024.0)
                        } else if total_bytes > 0 {
                            format!("{:.0} KB", total_bytes as f64 / 1024.0)
                        } else {
                            "0 B".to_string()
                        };
                        let fmt_str = if img_fmt_names.is_empty() { "raw".to_string() }
                                      else { img_fmt_names.join("+") };
                        main_item = PyClipboardItem::new(
                            0,
                            format!("[{} {}]", fmt_str, size_str),
                            "image".to_string(),
                        );
                        main_item.source_app = source_app;
                    }

                    // ── 第四步：写入数据库 ────────────────────────────────────
                    let db = self.db.lock();
                    if let Ok(id) = db.insert_item(&main_item) {
                        main_item.id = id;

                        // 图片优化：
                        // CF_DIBV5(17) 是 CF_DIB(8) 的超集（含 alpha 通道），
                        // 有 CF_DIBV5 时跳过 CF_DIB 以避免粘贴时丢失透明通道。
                        let has_dibv5 = raw_formats.iter().any(|(fid, _, data)| {
                            *fid == 17 && !data.is_empty()
                        });
                        let filtered_formats: Vec<(u32, String, Vec<u8>)> = raw_formats
                            .into_iter()
                            .filter(|(fid, _, _)| !(*fid == 8 && has_dibv5))
                            .collect();

                        // 统计字节数，同时对 >100KB 的数据做一次压缩，
                        // 压缩结果直接复用（存库时不再重复压缩）
                        // 格式：(format_id, format_name, data, is_compressed)
                        const THRESHOLD: usize = 100 * 1024;
                        let mut raw_total: usize = 0;
                        let mut compressed_total: usize = 0;
                        let formats_to_store: Vec<(u32, String, Vec<u8>, bool)> = filtered_formats
                            .into_iter()
                            .map(|(fid, fname, data)| {
                                raw_total += data.len();
                                if data.len() > THRESHOLD {
                                    match zstd::encode_all(data.as_slice(), 3) {
                                        Ok(cdata) => {
                                            compressed_total += cdata.len();
                                            (fid, fname, cdata, true)   // 已压缩
                                        }
                                        Err(_) => {
                                            compressed_total += data.len();
                                            (fid, fname, data, false)   // 压缩失败，存原始
                                        }
                                    }
                                } else {
                                    compressed_total += data.len();
                                    (fid, fname, data, false)           // 不需压缩
                                }
                            })
                            .collect();
                        main_item.char_count = Some((raw_total as i64) * 10_000_000 + compressed_total as i64);

                        if !formats_to_store.is_empty() {
                            let _ = db.insert_precompressed_formats(id, &formats_to_store);
                        }

                        let limit = HISTORY_LIMIT.load(Ordering::Relaxed);
                        if limit > 0 {
                            let _ = db.cleanup_old_items(limit);
                        }

                        if let Some(callback) = CALLBACK.lock().as_ref() {
                            Python::with_gil(|py| {
                                let _ = callback.call1(py, (main_item.clone(),));
                            });
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

    /// 获取某条记录保存的所有原始剪贴板格式（Ditto 风格）
    /// 
    /// Returns:
    ///     List[Tuple[int, str, bytes]]: [(format_id, format_name, raw_data), ...]
    fn get_raw_formats(&self, id: i64) -> PyResult<Vec<(u32, String, Vec<u8>)>> {
        let db = self.db.lock();
        db.get_formats(id).map_err(|e| PyRuntimeError::new_err(e))
    }

    /// 手动保存一批原始剪贴板格式数据（主要用于测试或外部调用）
    ///
    /// Args:
    ///     event_id: 关联的 clipboard.id
    ///     formats: List[Tuple[int, str, bytes]]，每项为 (format_id, format_name, raw_data)
    fn insert_formats(&self, event_id: i64, formats: Vec<(u32, String, Vec<u8>)>) -> PyResult<()> {
        let db = self.db.lock();
        db.insert_formats(event_id, &formats).map_err(|e| PyRuntimeError::new_err(e))
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
    ///     limit: 每页数量，
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
    
    /// 清空历史记录
    ///
    /// Args:
    ///     keep_grouped: True = 保留已加入分组的条目，只删历史区；False = 删除全部（默认）
    #[pyo3(signature = (keep_grouped=false))]
    fn clear_history(&self, keep_grouped: bool) -> PyResult<()> {
        let db = self.db.lock();
        db.clear_all(keep_grouped)
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
        let db = self.db.lock();
        db.move_item_between(id, before_id, after_id)
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
        let db = self.db.lock();
        db.move_group_between(id, before_id, after_id)
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

            // ── 优先路径：用原始格式数据完整还原（Ditto 风格）────────────────
            let raw_formats = db.get_formats(id).unwrap_or_default();
            if !raw_formats.is_empty() {
                // write_all_raw_formats 在监听线程里定义为模块级 fn，
                // 这里重新内联一份（paste_item 在主线程/pymethods 里调用）
                #[cfg(target_os = "windows")]
                {
                    #[link(name = "user32")]
                    extern "system" {
                        fn OpenClipboard(hwnd: *mut std::ffi::c_void) -> i32;
                        fn CloseClipboard() -> i32;
                        fn EmptyClipboard() -> i32;
                        fn SetClipboardData(format: u32, hmem: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
                    }
                    #[link(name = "kernel32")]
                    extern "system" {
                        fn GlobalAlloc(uflags: u32, dwbytes: usize) -> *mut std::ffi::c_void;
                        fn GlobalLock(hmem: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
                        fn GlobalUnlock(hmem: *mut std::ffi::c_void) -> i32;
                    }
                    const GMEM_MOVEABLE: u32 = 0x0002;
                    unsafe {
                        if OpenClipboard(std::ptr::null_mut()) != 0 {
                            EmptyClipboard();

                            // 有 CF_DIBV5(17) 时跳过 CF_DIB(8)：
                            // CF_DIBV5 保留 alpha 通道，CF_DIB 不保留。
                            // 若同时写入两者，部分应用会优先读 CF_DIB 导致透明丢失。
                            // 只写 CF_DIBV5，Windows 会自动合成 CF_DIB 供不支持 V5 的应用使用。
                            let has_dibv5 = raw_formats.iter().any(|(fid, _, data)| {
                                *fid == 17 && !data.is_empty()
                            });

                            for (fmt_id, name, data) in &raw_formats {
                                if data.is_empty() {
                                    // size=0 的格式（ObjectLink/Native 等延迟渲染占位符）
                                    // SetClipboardData(fmt, null) 可触发目标程序重新提供数据，
                                    // 但仅当同一进程仍作为剪贴板所有者时才有意义；
                                    // 跨进程/跨会话恢复时直接跳过，避免写入无效句柄。
                                    continue;
                                }
                                // 有 CF_DIBV5 时跳过 CF_DIB：避免目标应用优先读取无 alpha 的 CF_DIB
                                // Windows 会从 CF_DIBV5 自动合成 CF_DIB 供不支持 V5 的应用使用
                                if *fmt_id == 8 && has_dibv5 {
                                    continue;
                                }
                                // 关闭"带格式粘贴"时，仅对文本类型条目过滤掉富文本格式，
                                // 图片/文件类型条目不受影响，完整还原所有格式
                                if !with_html && item.content_type == "text" {
                                    // 纯文本白名单：与监听白名单保持一致
                                    // CF_OEMTEXT(7) 不在监听白名单内，粘贴时也不还原
                                    let is_plain_text = matches!(
                                        name.as_str(),
                                        "CF_TEXT"          // 1  — ANSI 文本
                                        | "CF_UNICODETEXT" // 13 — Unicode 文本
                                        | "CF_LOCALE"      // 16 — 文本语言区域
                                    );
                                    if !is_plain_text {
                                        continue;
                                    }
                                }
                                let hmem = GlobalAlloc(GMEM_MOVEABLE, data.len());
                                if hmem.is_null() { continue; }
                                let ptr = GlobalLock(hmem);
                                if ptr.is_null() { continue; }  // GlobalLock 失败极罕见，跳过即可
                                std::ptr::copy_nonoverlapping(data.as_ptr(), ptr as *mut u8, data.len());
                                GlobalUnlock(hmem);
                                SetClipboardData(*fmt_id, hmem);
                            }
                            CloseClipboard();

                            // 增加粘贴次数 + 可选移到最前
                            drop(db);
                            let db = self.db.lock();
                            let _ = db.increment_paste_count(id);
                            if move_to_top { let _ = db.move_item_to_top(id); }
                            return Ok(true);
                        }
                    }
                }
            }

            // ── 降级路径：原始格式不存在时，用解析后的内容还原（兼容旧数据）──
            let ctx = ClipboardContext::new()
                .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
            
            match item.content_type.as_str() {
                "text" => {
                    if with_html {
                        if let Some(ref html) = item.html_content {
                            if !html.is_empty() {
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
            
            drop(db);
            let db = self.db.lock();
            let _ = db.increment_paste_count(id);
            if move_to_top { let _ = db.move_item_to_top(id); }
            
            Ok(true)
        } else {
            Ok(false)
        }
    }
}
