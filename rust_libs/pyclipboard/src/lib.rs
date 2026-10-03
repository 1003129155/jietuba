use pyo3::prelude::*;
use pyo3::exceptions::PyRuntimeError;
use pyo3::types::PyBytes;

mod database;
mod types;
#[cfg(target_os = "windows")]
mod win_clipboard;

use database::Database;
use types::{PyClipboardItem, PyQueryParams, PyPaginatedResult, PyGroup};

use std::sync::Arc;
use parking_lot::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use once_cell::sync::Lazy;
use std::thread;
use std::path::PathBuf;
use std::time::{Duration, Instant};
use zstd;

// ============== 全局状态 ==============

static IS_RUNNING: AtomicBool = AtomicBool::new(false);
static WATCHER_THREAD_ID: AtomicU32 = AtomicU32::new(0);
static WATCHER_SHUTDOWN: Lazy<Mutex<Option<clipboard_rs::WatcherShutdown>>> = Lazy::new(|| Mutex::new(None));
static WATCHER_THREAD: Lazy<Mutex<Option<thread::JoinHandle<()>>>> = Lazy::new(|| Mutex::new(None));
static CALLBACK: Lazy<Arc<Mutex<Option<PyObject>>>> = Lazy::new(|| Arc::new(Mutex::new(None)));
// paste_item 写完剪贴板后的序号：监听看到的还是这个序号，就是自己写回去的内容，不记录
static OWN_WRITE_SEQ: AtomicU32 = AtomicU32::new(0);
// 监听已处理到的剪贴板序号：同一份内容的多条通知只处理一次
static HANDLED_SEQ: AtomicU32 = AtomicU32::new(0);
static LAST_CALLBACK_EVENT: Lazy<Mutex<Option<(i64, Instant)>>> = Lazy::new(|| Mutex::new(None));
const DUPLICATE_CALLBACK_WINDOW: Duration = Duration::from_millis(350);
/// 剪贴板停止变化这么久才去读
const SETTLE_DELAY: Duration = Duration::from_millis(100);
/// 剪贴板一直在变时最多等这么久
const SETTLE_LIMIT: Duration = Duration::from_secs(2);

/// 等剪贴板静止后返回它的序号；这份内容已经处理过、或等待期间监听被停掉时返回 None。
///
/// 写入方常把一次复制拆成几次打开剪贴板（先清空，再写各个格式）。通知一到就去读，
/// 会挤进这几次之间，对方后面那次打开就可能失败、内容写不进系统剪贴板。
#[cfg(target_os = "windows")]
fn wait_until_clipboard_settles() -> Option<u32> {
    let mut seq = win_clipboard::sequence_number();
    if seq == HANDLED_SEQ.load(Ordering::SeqCst) {
        return None;
    }
    let started = Instant::now();
    loop {
        thread::sleep(SETTLE_DELAY);
        if !IS_RUNNING.load(Ordering::Relaxed) {
            return None;
        }
        let now = win_clipboard::sequence_number();
        let settled = now == seq;
        seq = now;
        if settled || started.elapsed() >= SETTLE_LIMIT {
            break;
        }
    }
    HANDLED_SEQ.store(seq, Ordering::SeqCst);
    Some(seq)
}

/// 写入方要求剪贴板历史不要记录这份内容（密码一类）
///
/// 格式含义见 https://learn.microsoft.com/windows/win32/dataxchg/clipboard-formats
#[cfg(target_os = "windows")]
fn is_marked_private(clipboard: &win_clipboard::OpenClipboardGuard) -> bool {
    let present = |name: &str| {
        let format = win_clipboard::register_format(name);
        format != 0 && clipboard.is_available(format)
    };
    if present("ExcludeClipboardContentFromMonitorProcessing") || present("Clipboard Viewer Ignore") {
        return true;
    }
    // CanIncludeInClipboardHistory 是一个 DWORD，0 表示不要进历史
    let format = win_clipboard::register_format("CanIncludeInClipboardHistory");
    format != 0
        && clipboard.is_available(format)
        && clipboard.read(format)
            .is_some_and(|data| data.len() >= 4 && u32::from_le_bytes([data[0], data[1], data[2], data[3]]) == 0)
}

/// 一次打开剪贴板读下来的内容
#[cfg(target_os = "windows")]
struct CapturedClipboard {
    /// 要存的格式：(编号, 名字, 原始数据)
    formats: Vec<(u32, String, Vec<u8>)>,
    /// 剪贴板上所有格式的 (编号, 名字)
    names: Vec<(u32, String)>,
    /// 内容是表格单元格；这时不读位图
    spreadsheet: bool,
}

#[cfg(target_os = "windows")]
impl CapturedClipboard {
    fn data(&self, format: u32) -> Option<&[u8]> {
        self.formats.iter().find(|(f, _, _)| *f == format).map(|(_, _, data)| data.as_slice())
    }
}

/// 读出要存进历史的格式；剪贴板为空、打不开或标了不让记录时返回 None
#[cfg(target_os = "windows")]
fn capture_clipboard(window: &win_clipboard::ClipboardWindow) -> Option<CapturedClipboard> {
    use win_clipboard::*;

    if format_count() == 0 {
        return None;
    }
    // 监听在后台线程，别的程序占着剪贴板时多等一会儿，等不到才放弃这次记录
    let clipboard = window.open(Duration::from_secs(1))?;
    if is_marked_private(&clipboard) {
        return None;
    }
    let names = clipboard.formats();
    // 表格单元格的位图和 RTF 都要对方现场生成：两万行的区域位图近 200 MB、RTF 要将近
    // 一秒，期间对方界面卡住；粘贴回去有 XML Spreadsheet 和 HTML 就够了
    let spreadsheet = names.iter().any(|(_, name)| name == "XML Spreadsheet" || name.starts_with("Biff"));
    let mut wanted = vec![
        CF_TEXT, CF_UNICODETEXT, CF_HDROP, CF_LOCALE,
        register_format("HTML Format"),
        // 电子表格的公式只在这个格式里，HTML/RTF 里只有计算结果
        register_format("XML Spreadsheet"),
    ];
    if !spreadsheet {
        wanted.extend([CF_DIB, CF_DIBV5, register_format("PNG"), register_format("Rich Text Format")]);
    }
    let formats = wanted.into_iter()
        .filter(|format| *format != 0 && clipboard.is_available(*format))
        .filter_map(|format| clipboard.read(format).map(|data| (format, format_name(format), data)))
        .collect();
    Some(CapturedClipboard { formats, names, spreadsheet })
}

/// 记下自己刚写进剪贴板的内容，监听收到它的通知时不记录
fn mark_own_clipboard_write() {
    #[cfg(target_os = "windows")]
    OWN_WRITE_SEQ.store(win_clipboard::sequence_number(), Ordering::SeqCst);
}

fn should_skip_callback(id: i64) -> bool {
    let now = Instant::now();
    let mut last = LAST_CALLBACK_EVENT.lock();

    if let Some((last_id, last_at)) = last.as_ref() {
        if *last_id == id && now.duration_since(*last_at) < DUPLICATE_CALLBACK_WINDOW {
            return true;
        }
    }

    *last = Some((id, now));
    false
}

// ============== Python 模块 ==============

/// pyclipboard - Python 剪贴板管理库
#[pymodule]
fn pyclipboard(m: &Bound<'_, PyModule>) -> PyResult<()> {
    // 注册类
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;
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


/// CF_HTML 在 Windows 剪贴板里的格式名，与 generate_cf_html 写出的头部对应。
const CF_HTML_FORMAT: &str = "HTML Format";

/// 从 CF_HTML 原始字节里取出 HTML 正文。
///
/// 不走 clipboard-rs 的 `get_html()`：它最后一步是 `data[start..end]`，而 Rust 的
/// &str 切片只要落在非字符边界就 panic。写剪贴板的程序把偏移量算在多字节字符
/// 中间并不罕见，中文内容尤其容易撞上；而本文件里有一个调用点在剪贴板监听线程
/// 内，线程里 panic 比返回错误难查得多。
///
/// 所以这里把头部里的偏移量当作**提示**而非事实：越界就钳制，落在字符中间就
/// 退回按 `<html>` 标签定位。写入侧本来就是本文件自己实现的（generate_cf_html），
/// 读取侧一并拿过来也更对称。
fn extract_html_from_cf_html(raw: &[u8]) -> Option<String> {
    // lossy 而非 from_utf8：个别非法字节不该让整块内容作废
    let data = String::from_utf8_lossy(raw);
    let len = data.len();

    let mut start = 0usize;
    let mut end = len;
    for line in data.lines() {
        // 头部是 "Key:Value" 若干行，遇到标签就说明已经进入正文
        if line.starts_with('<') {
            break;
        }
        let Some((key, value)) = line.split_once(':') else {
            continue;
        };
        let slot = match key.trim() {
            "StartHTML" => &mut start,
            "EndHTML" => &mut end,
            _ => continue,
        };
        // 偏移量按 CF_HTML 惯例补足前导零，全零即 0
        let digits = value.trim().trim_start_matches('0');
        match if digits.is_empty() { Ok(0) } else { digits.parse::<usize>() } {
            Ok(v) => *slot = v,
            // 头部都写坏了就别再信它，交给下面的兜底
            Err(_) => break,
        }
    }

    let start = start.min(len);
    let end = end.min(len);
    if start < end {
        if let Some(html) = data.get(start..end) {
            return Some(html.to_string());
        }
    }

    // 偏移量不可用（落在字符中间、或首尾颠倒），退回按标签定位。
    // 标签全是 ASCII，找到的下标必然是合法字符边界。
    let open = data.find("<html").or_else(|| data.find("<HTML"))?;
    let close = data
        .rfind("</html>")
        .or_else(|| data.rfind("</HTML>"))
        .map(|i| i + "</html>".len())
        .unwrap_or(len)
        .min(len);
    data.get(open..close).map(|s| s.to_string())
}

#[cfg(test)]
mod cf_html_tests {
    use super::extract_html_from_cf_html;

    const BODY: &str = "<html><body>中文内容测试</body></html>";

    /// 按 CF_HTML 规范拼一份缓冲，start_delta 用来把偏移量推进多字节字符内部
    fn build(start_delta: usize) -> Vec<u8> {
        let head_probe = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\n";
        let head_len = head_probe.len();
        let total = head_len + BODY.len();
        format!(
            "Version:0.9\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\n{}",
            head_len + start_delta,
            total,
            BODY
        )
        .into_bytes()
    }

    #[test]
    fn offsets_pointing_at_the_body_are_used_as_is() {
        assert_eq!(extract_html_from_cf_html(&build(0)).as_deref(), Some(BODY));
    }

    #[test]
    fn offset_inside_a_multibyte_char_falls_back_instead_of_panicking() {
        // "<html><body>" 之后第一个字是「中」，+13 正好落在它的字节中间。
        // clipboard-rs 的 get_html() 在这里会 panic。
        assert_eq!(extract_html_from_cf_html(&build(13)).as_deref(), Some(BODY));
    }

    #[test]
    fn out_of_range_offsets_are_clamped() {
        let raw = format!(
            "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000999999\r\n{}",
            BODY
        )
        .into_bytes();
        let got = extract_html_from_cf_html(&raw).expect("应当钳制后返回内容");
        assert!(got.ends_with("</html>"));
    }

    #[test]
    fn reversed_offsets_fall_back_to_tag_search() {
        let raw = format!(
            "Version:0.9\r\nStartHTML:0000000200\r\nEndHTML:0000000010\r\n{}",
            BODY
        )
        .into_bytes();
        assert_eq!(extract_html_from_cf_html(&raw).as_deref(), Some(BODY));
    }

    #[test]
    fn uppercase_tags_are_recognised_by_the_fallback() {
        let body = "<HTML><BODY>中文</BODY></HTML>";
        let raw = format!("Version:0.9\r\nStartHTML:0000000200\r\nEndHTML:0000000010\r\n{}", body)
            .into_bytes();
        assert_eq!(extract_html_from_cf_html(&raw).as_deref(), Some(body));
    }

    #[test]
    fn garbage_without_any_html_yields_none() {
        assert_eq!(extract_html_from_cf_html(b"Version:0.9\r\nStartHTML:0000000200\r\n"), None);
    }
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
    #[cfg(target_os = "windows")]
    {
        let data = win_clipboard::encode_unicode(&text);
        if win_clipboard::write(&[(win_clipboard::CF_UNICODETEXT, data)]) {
            Ok(())
        } else {
            Err(PyRuntimeError::new_err("设置剪贴板失败: 剪贴板被占用"))
        }
    }

    #[cfg(not(target_os = "windows"))]
    {
        use clipboard_rs::{Clipboard, ClipboardContext};

        let ctx = ClipboardContext::new()
            .map_err(|e| PyRuntimeError::new_err(format!("创建剪贴板上下文失败: {}", e)))?;
        ctx.set_text(text)
            .map_err(|e| PyRuntimeError::new_err(format!("设置剪贴板失败: {}", e)))
    }
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
    
    // 自行解析 CF_HTML，不用 ctx.get_html()（原因见 extract_html_from_cf_html）
    Ok(ctx
        .get_buffer(CF_HTML_FORMAT)
        .ok()
        .and_then(|raw| extract_html_from_cf_html(&raw)))
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
///     >>> manager = ClipboardManager()
///     >>> manager.add_item("Hello World")
///     >>> result = manager.get_history()
///     >>> for item in result:
///     ...     print(item.content)
#[pyclass(name = "ClipboardManager")]
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
        if let Some(handle) = WATCHER_THREAD.lock().take() {
            let _ = handle.join();
        }

        if let Some(cb) = callback {
            *CALLBACK.lock() = Some(cb);
        }
        
        let db = self.db.clone();
        
        // 获取图片存储路径
        let images_dir = {
            let db_lock = db.lock();
            db_lock.get_images_dir()
        };
        
        let handle = thread::spawn(move || {
            #[cfg(target_os = "windows")]
            {
                #[link(name = "kernel32")]
                extern "system" {
                    fn GetCurrentThreadId() -> u32;
                }
                WATCHER_THREAD_ID.store(unsafe { GetCurrentThreadId() }, Ordering::SeqCst);
            }

            // ── 初始化 COM STA ─────────────────────────────────────────────
            // 监听线程内部运行 Windows 消息循环（start_watch），必须先建立
            // COM STA 公寓，否则与 OLE 剪贴板交互时会在主线程触发
            // RPC_E_WRONG_THREAD (0x8001010e)。
            #[cfg(target_os = "windows")]
            #[link(name = "ole32")]
            extern "system" {
                fn CoInitializeEx(pv_reserved: *mut std::ffi::c_void, dw_co_init: u32) -> i32;
                fn CoUninitialize();
            }
            #[cfg(target_os = "windows")]
            let _com_init_hr = unsafe {
                // COINIT_APARTMENTTHREADED = 0x2
                CoInitializeEx(std::ptr::null_mut(), 0x2)
            };

            use image::codecs::png::PngEncoder;
            use image::ImageEncoder;
            use sha2::{Sha256, Digest};
            use base64::{Engine as _, engine::general_purpose};
            
            struct Handler {
                db: Arc<Mutex<Database>>,
                images_dir: PathBuf,
                #[cfg(target_os = "windows")]
                window: Option<win_clipboard::ClipboardWindow>,
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
                    #[cfg(target_os = "windows")]
                    self.record_change();
                }
            }

            #[cfg(target_os = "windows")]
            impl Handler {
                fn record_change(&mut self) {
                    use win_clipboard::{CF_DIB, CF_DIBV5, CF_HDROP, CF_UNICODETEXT};

                    if !IS_RUNNING.load(Ordering::Relaxed) {
                        return;
                    }
                    let Some(seq) = wait_until_clipboard_settles() else {
                        return;
                    };
                    // paste_item 自己写回去的内容
                    if seq == OWN_WRITE_SEQ.load(Ordering::SeqCst) {
                        return;
                    }

                    // ── 第一步：一次打开剪贴板，按白名单读出原始数据 ──────────
                    let Some(window) = self.window.as_ref() else {
                        return;
                    };
                    let Some(captured) = capture_clipboard(window) else {
                        return;
                    };

                    // ── 第二步：从原始数据解析主记录（用于 UI 展示）──────────
                    let png_format = win_clipboard::register_format("PNG");
                    let source_app = get_clipboard_owner().ok().flatten();
                    let html_content = captured
                        .data(win_clipboard::register_format(CF_HTML_FORMAT))
                        .and_then(extract_html_from_cf_html);
                    let text_val = captured.data(CF_UNICODETEXT)
                        .map(win_clipboard::decode_unicode)
                        .filter(|t| !t.trim().is_empty());
                    let files_val = captured.data(CF_HDROP)
                        .map(win_clipboard::parse_hdrop)
                        .filter(|f| !f.is_empty());
                    // 有文字时按文字记录，图片用不到，不必解码
                    let image_val = if text_val.is_none() {
                        win_clipboard::decode_image(
                            captured.data(png_format), captured.data(CF_DIBV5), captured.data(CF_DIB))
                    } else {
                        None
                    };
                    let spreadsheet = captured.spreadsheet;
                    let raw_formats = captured.formats;
                    let all_names = captured.names;

                    // 解码都失败时，检查白名单数据或全格式名称列表是否含图片类格式
                    // 场景：Word 复制多张图片时解不出单张图，但 raw_formats 里有 PNG/DIB；
                    // 表格单元格的位图没有读，不按图片记录
                    let raw_image_fallback = if !spreadsheet && text_val.is_none() && files_val.is_none() && image_val.is_none() {
                        let has_image_data = raw_formats.iter().any(|(fid, fname, data)| {
                            !data.is_empty() && (*fid == CF_DIB || *fid == CF_DIBV5 || fname.eq_ignore_ascii_case("PNG"))
                        });
                        // 也检查 all_names，防止白名单中没有 PNG/DIB 但剪贴板里有其他图片格式
                        let has_image_name = all_names.iter().any(|(fid, fname)| {
                            *fid == CF_DIB || *fid == CF_DIBV5 || fname.eq_ignore_ascii_case("PNG")
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

                    // 图片优先于文件路径判断：剪贴板带图片数据时，无论是否同时
                    // 带 CF_HDROP（例如截图工具为兼容只认文件路径的粘贴目标而
                    // 附带的文件引用，或看图软件复制文件时附带的预览位图），
                    // 内容本质上还是图片，应按图片展示缩略图。
                    if let Some(text) = text_val {
                        main_item = PyClipboardItem::new(0, text, "text".to_string());
                        main_item.html_content = html_content;
                        main_item.source_app = source_app;
                    } else if let Some(rgba) = image_val {
                        // 单张图片：落盘 PNG，生成缩略图
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
                    } else if let Some(files) = files_val {
                        let content = serde_json::json!({ "files": files }).to_string();
                        main_item = PyClipboardItem::new(0, content, "file".to_string());
                        main_item.source_app = source_app;
                    } else {
                        // raw_image_fallback：多图/EMF 等解不出单张图的图片内容
                        // content 写入格式列表和总字节数，供前端直接显示
                        // 例：[PNG+CF_DIB 7.9 MB] 或 [PNG 1.2 MB]
                        let img_fmt_names: Vec<&str> = {
                            let mut names = Vec::new();
                            for (fid, fname, data) in &raw_formats {
                                if data.is_empty() { continue; }
                                if *fid == CF_DIBV5 { names.push("CF_DIBV5"); }
                                else if *fid == CF_DIB { names.push("CF_DIB"); }
                                else if fname.eq_ignore_ascii_case("PNG") { names.push("PNG"); }
                            }
                            names.dedup();
                            names
                        };
                        let total_bytes: usize = raw_formats.iter()
                            .filter(|(fid, fname, _)| *fid == CF_DIB || *fid == CF_DIBV5 || fname.eq_ignore_ascii_case("PNG"))
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

                    // ── 第四步：压缩，再写入数据库 ────────────────────────────
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
                    // 压缩结果直接复用（存库时不再重复压缩）；压缩放在拿数据库锁之前
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

                    let id = {
                        let db = self.db.lock();
                        let Ok(id) = db.insert_item(&main_item) else {
                            return;
                        };
                        let _ = db.replace_formats(id, &formats_to_store);
                        let limit = HISTORY_LIMIT.load(Ordering::Relaxed);
                        if limit > 0 {
                            let _ = db.cleanup_old_items(limit);
                        }
                        id
                    };
                    // 回调要拿 GIL，而持有 GIL 的线程可能正等着数据库锁，所以先放锁再回调
                    main_item.id = id;
                    main_item.char_count = Some((raw_total as i64) * 10_000_000 + compressed_total as i64);

                    if should_skip_callback(id) {
                        return;
                    }

                    // 同理不能拿着回调锁等 GIL：stop_monitor 是持 GIL 来改回调的
                    if CALLBACK.lock().is_some() {
                        Python::with_gil(|py| {
                            let callback = CALLBACK.lock().as_ref().map(|cb| cb.clone_ref(py));
                            if let Some(callback) = callback {
                                let _ = callback.call1(py, (main_item.clone(),));
                            }
                        });
                    }
                }
            }

            let handler = Handler {
                db,
                images_dir,
                #[cfg(target_os = "windows")]
                window: win_clipboard::ClipboardWindow::new(),
            };
            if let Ok(mut watcher) = ClipboardWatcherContext::new() {
                watcher.add_handler(handler);
                *WATCHER_SHUTDOWN.lock() = Some(watcher.get_shutdown_channel());
                watcher.start_watch();
            }
            *WATCHER_SHUTDOWN.lock() = None;
            IS_RUNNING.store(false, Ordering::SeqCst);
            WATCHER_THREAD_ID.store(0, Ordering::SeqCst);

            // 释放 COM 公寓（仅当 CoInitializeEx 成功时：S_OK=0 或 S_FALSE=1）
            #[cfg(target_os = "windows")]
            if _com_init_hr == 0 || _com_init_hr == 1 {
                unsafe { CoUninitialize(); }
            }
        });
        *WATCHER_THREAD.lock() = Some(handle);
        
        Ok(())
    }
    
    /// 获取图片数据（通过 image_id）
    #[pyo3(signature = (image_id))]
    fn get_image_data<'py>(
        &self,
        py: Python<'py>,
        image_id: String,
    ) -> PyResult<Option<Bound<'py, PyBytes>>> {
        // 路径取出后就释放数据库锁；后续磁盘 I/O 和 Python 对象分配都不需要
        // 访问 SQLite，避免大图读取期间阻塞剪贴板监听线程。
        let image_path = {
            let db = self.db.lock();
            db.get_images_dir().join(format!("{}.png", image_id))
        };
        
        if image_path.exists() {
            let data = std::fs::read(&image_path)
                .map_err(|e| PyRuntimeError::new_err(format!("读取图片失败: {}", e)))?;
            // Vec<u8> 的默认 Python 转换是逐元素的 list。图片稍大时，仅 list
            // 指针数组就会占用约原始字节数的 8 倍，并把进程工作集推到很高。
            // 直接构造 bytes，既符合 Python 包装层声明，也避免这份临时大列表。
            Ok(Some(PyBytes::new_bound(py, &data)))
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
    fn stop_monitor(&self, py: Python<'_>) -> PyResult<()> {
        IS_RUNNING.store(false, Ordering::SeqCst);
        if let Some(shutdown) = WATCHER_SHUTDOWN.lock().take() {
            shutdown.stop();
        }
        #[cfg(target_os = "windows")]
        {
            #[link(name = "user32")]
            extern "system" {
                fn PostThreadMessageW(id_thread: u32, msg: u32, w_param: usize, l_param: isize) -> i32;
            }
            const WM_QUIT: u32 = 0x0012;
            let thread_id = WATCHER_THREAD_ID.load(Ordering::SeqCst);
            if thread_id != 0 {
                unsafe {
                    let _ = PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
                }
            }
        }
        *CALLBACK.lock() = None;
        *LAST_CALLBACK_EVENT.lock() = None;
        if let Some(handle) = WATCHER_THREAD.lock().take() {
            if handle.thread().id() == thread::current().id() {
                *WATCHER_THREAD.lock() = Some(handle);
            } else {
                py.allow_threads(move || {
                    let _ = handle.join();
                });
            }
        }
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
        let db = self.db.lock();
        db.query_items(offset, limit, search, content_type, start_time, end_time)
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
        let result = self.get_history(0, limit, Some(keyword), None, None, None)?;
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
    ///     group_type: 分组类型 (0=普通, 1=文件分组)。默认 0
    /// 
    /// Returns:
    ///     int: 新分组的 ID
    #[pyo3(signature = (name, color=None, icon=None, group_type=0))]
    fn create_group(&self, name: String, color: Option<String>, icon: Option<String>, group_type: i64) -> PyResult<i64> {
        let db = self.db.lock();
        db.create_group(&name, color.as_deref(), icon.as_deref(), group_type)
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
    ///     group_type: 分组类型 (0=普通, 1=文件分组)。默认 0
    #[pyo3(signature = (id, name, color=None, icon=None, group_type=0))]
    fn update_group(&self, id: i64, name: String, color: Option<String>, icon: Option<String>, group_type: i64) -> PyResult<()> {
        let db = self.db.lock();
        db.update_group(id, &name, color.as_deref(), icon.as_deref(), group_type)
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
        // 读完数据库就放锁，写剪贴板时可能要等别的程序让出剪贴板
        let (item, raw_formats, images_dir) = {
            let db = self.db.lock();
            let Some(item) = db.get_item_by_id(id).map_err(PyRuntimeError::new_err)? else {
                return Ok(false);
            };
            let raw_formats = db.get_formats(id).unwrap_or_default();
            (item, raw_formats, db.get_images_dir())
        };

        #[cfg(not(target_os = "windows"))]
        {
            let _ = (item, raw_formats, images_dir, with_html, move_to_top);
            return Err(PyRuntimeError::new_err("paste_item 只支持 Windows"));
        }

        #[cfg(target_os = "windows")]
        {
            let formats = if raw_formats.is_empty() {
                formats_from_item(&item, with_html, &images_dir)?
            } else {
                formats_to_restore(&item, raw_formats, with_html)
            };
            if formats.is_empty() || !win_clipboard::write(&formats) {
                return Ok(false);
            }
            mark_own_clipboard_write();

            let db = self.db.lock();
            let _ = db.increment_paste_count(id);
            if move_to_top {
                let _ = db.move_item_to_top(id);
            }
            Ok(true)
        }
    }
}

/// 存下的原始格式整理成要写回剪贴板的 (当前格式编号, 数据)
#[cfg(target_os = "windows")]
fn formats_to_restore(
    item: &PyClipboardItem,
    raw_formats: Vec<(u32, String, Vec<u8>)>,
    with_html: bool,
) -> Vec<(u32, Vec<u8>)> {
    use win_clipboard::{CF_DIB, CF_DIBV5, FIRST_REGISTERED_FORMAT};

    // 有 CF_DIBV5 时不写 CF_DIB：CF_DIBV5 带 alpha 通道，同时写入时部分程序会先读
    // 不带 alpha 的 CF_DIB；只写 CF_DIBV5，系统会给只认 CF_DIB 的程序自动转换
    let has_dibv5 = raw_formats.iter().any(|(fid, _, data)| *fid == CF_DIBV5 && !data.is_empty());
    raw_formats.into_iter()
        .filter_map(|(fmt_id, name, data)| {
            // 大小为 0 的是延迟渲染占位，只对当时的来源程序有意义
            if data.is_empty() || (fmt_id == CF_DIB && has_dibv5) {
                return None;
            }
            // 关闭「带格式粘贴」时，文本条目只写纯文本；图片、文件条目照常完整还原
            if !with_html && item.content_type == "text"
                && !matches!(name.as_str(), "CF_TEXT" | "CF_UNICODETEXT" | "CF_LOCALE")
            {
                return None;
            }
            let format = if fmt_id >= FIRST_REGISTERED_FORMAT {
                win_clipboard::register_format(&name)
            } else {
                fmt_id
            };
            (format != 0).then_some((format, data))
        })
        .collect()
}

/// 没有原始格式的条目（手动添加的、旧版本记录的）按解析后的内容生成剪贴板格式
#[cfg(target_os = "windows")]
fn formats_from_item(
    item: &PyClipboardItem,
    with_html: bool,
    images_dir: &std::path::Path,
) -> PyResult<Vec<(u32, Vec<u8>)>> {
    use win_clipboard::{CF_DIBV5, CF_HDROP, CF_UNICODETEXT};

    match item.content_type.as_str() {
        "text" => {
            let mut formats = vec![(CF_UNICODETEXT, win_clipboard::encode_unicode(&item.content))];
            if let Some(html) = item.html_content.as_deref().filter(|html| with_html && !html.is_empty()) {
                let mut cf_html = generate_cf_html(html).into_bytes();
                cf_html.push(0);
                formats.push((win_clipboard::register_format(CF_HTML_FORMAT), cf_html));
            }
            Ok(formats)
        }
        "image" => {
            let Some(image_id) = item.image_id.as_deref() else {
                return Ok(Vec::new());
            };
            let Ok(png) = std::fs::read(images_dir.join(format!("{}.png", image_id))) else {
                return Ok(Vec::new());
            };
            let rgba = image::load_from_memory_with_format(&png, image::ImageFormat::Png)
                .map_err(|e| PyRuntimeError::new_err(format!("解析图片失败: {}", e)))?
                .to_rgba8();
            // PNG 加带 alpha 的 CF_DIBV5：只认位图的程序也能粘贴
            let dibv5 = win_clipboard::rgba_to_dibv5(&rgba);
            Ok(vec![(win_clipboard::register_format("PNG"), png), (CF_DIBV5, dibv5)])
        }
        "file" => {
            let files: Vec<String> = serde_json::from_str::<serde_json::Value>(&item.content)
                .ok()
                .and_then(|json| json.get("files").and_then(|f| f.as_array()).cloned())
                .unwrap_or_default()
                .iter()
                .filter_map(|f| f.as_str().map(str::to_string))
                .collect();
            Ok(if files.is_empty() { Vec::new() } else { vec![(CF_HDROP, win_clipboard::build_hdrop(&files))] })
        }
        _ => Ok(Vec::new()),
    }
}

#[cfg(all(test, target_os = "windows"))]
mod paste_format_tests {
    use super::*;
    use win_clipboard::*;

    fn item(content: &str, content_type: &str) -> PyClipboardItem {
        PyClipboardItem::new(1, content.to_string(), content_type.to_string())
    }

    #[test]
    fn stored_registered_formats_are_written_under_their_current_id() {
        let html = register_format("HTML Format");
        let raw = vec![
            (CF_UNICODETEXT, "CF_UNICODETEXT".to_string(), encode_unicode("表")),
            (html + 7, "HTML Format".to_string(), b"<b>x</b>".to_vec()),
        ];
        let formats = formats_to_restore(&item("表", "text"), raw, true);
        assert_eq!(formats.iter().map(|(f, _)| *f).collect::<Vec<_>>(), vec![CF_UNICODETEXT, html]);
    }

    #[test]
    fn plain_paste_keeps_only_text_formats_and_dibv5_replaces_dib() {
        let raw = vec![
            (CF_UNICODETEXT, "CF_UNICODETEXT".to_string(), encode_unicode("x")),
            (register_format("Rich Text Format"), "Rich Text Format".to_string(), br"{\rtf1}".to_vec()),
        ];
        let formats = formats_to_restore(&item("x", "text"), raw, false);
        assert_eq!(formats.len(), 1);

        let raw = vec![
            (CF_DIB, "CF_DIB".to_string(), vec![1]),
            (CF_DIBV5, "CF_DIBV5".to_string(), vec![2]),
        ];
        let formats = formats_to_restore(&item("[1x1]", "image"), raw, false);
        assert_eq!(formats, vec![(CF_DIBV5, vec![2])]);
    }

    #[test]
    fn items_without_stored_formats_are_rebuilt_from_their_content() {
        let mut text = item("文字", "text");
        text.html_content = Some("<i>文字</i>".to_string());
        let formats = formats_from_item(&text, true, std::path::Path::new(".")).unwrap();
        assert_eq!(decode_unicode(&formats[0].1), "文字");
        assert_eq!(formats[1].0, register_format("HTML Format"));
        assert!(extract_html_from_cf_html(&formats[1].1).unwrap().contains("<i>文字</i>"));
        assert_eq!(formats_from_item(&text, false, std::path::Path::new(".")).unwrap().len(), 1);

        let files = item(&serde_json::json!({ "files": [r"C:\a.txt", r"D:\图.png"] }).to_string(), "file");
        let formats = formats_from_item(&files, true, std::path::Path::new(".")).unwrap();
        assert_eq!(parse_hdrop(&formats[0].1), vec![r"C:\a.txt".to_string(), r"D:\图.png".to_string()]);

        let dir = std::env::temp_dir().join(format!("pyclipboard_paste_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let picture = image::RgbaImage::from_pixel(2, 3, image::Rgba([5, 6, 7, 8]));
        picture.save(dir.join("abc.png")).unwrap();
        let mut image_item = item("[2x3]", "image");
        image_item.image_id = Some("abc".to_string());
        let formats = formats_from_item(&image_item, true, &dir).unwrap();
        assert_eq!(formats[0].0, register_format("PNG"));
        assert_eq!(decode_image(None, Some(&formats[1].1), None), Some(picture));
        image_item.image_id = Some("missing".to_string());
        assert!(formats_from_item(&image_item, true, &dir).unwrap().is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
