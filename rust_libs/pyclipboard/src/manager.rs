//! 剪贴板历史的核心：监听线程、历史查询、把记录写回剪贴板。错误以 `Err(String)` 返回，
//! 回调是 Rust 闭包；pyo3 绑定在 python.rs。

use crate::cf_html;
use crate::database::{compress_format, image_file, Database};
use crate::group_kind::FilterRule;
use crate::multi_paste;
use crate::text_kind;
use crate::types::{PyClipboardItem, PyGroup};
#[cfg(target_os = "windows")]
use crate::win_clipboard;

use std::sync::Arc;
use parking_lot::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use once_cell::sync::Lazy;
use std::thread;
use std::path::PathBuf;
use std::time::{Duration, Instant};

/// 监听线程每记下一条就调用一次
pub type ItemCallback = Arc<dyn Fn(&PyClipboardItem) + Send + Sync>;
/// 监听线程调用回调前先经过它；Python 绑定在这里拿 GIL（见 start_monitor）
pub type CallbackGate = fn(&mut dyn FnMut());

// ============== 全局状态 ==============

static IS_RUNNING: AtomicBool = AtomicBool::new(false);
static WATCHER_THREAD_ID: AtomicU32 = AtomicU32::new(0);
static WATCHER_SHUTDOWN: Lazy<Mutex<Option<clipboard_rs::WatcherShutdown>>> = Lazy::new(|| Mutex::new(None));
static WATCHER_THREAD: Lazy<Mutex<Option<thread::JoinHandle<()>>>> = Lazy::new(|| Mutex::new(None));
static CALLBACK: Lazy<Mutex<Option<ItemCallback>>> = Lazy::new(|| Mutex::new(None));
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
        // 取色标记跟着存下来，粘贴回去仍带着它，重判子类型时也认得出
        register_format(text_kind::COLOR_MARKER_FORMAT),
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

#[derive(Default)]
struct UpdateWorkState { paused: bool, active: usize }
static UPDATE_WORK: Lazy<Mutex<UpdateWorkState>> = Lazy::new(|| Mutex::new(UpdateWorkState::default()));
struct UpdateWork<'a>(&'a Mutex<UpdateWorkState>);
impl<'a> UpdateWork<'a> {
    fn begin(state: &'a Mutex<UpdateWorkState>) -> Option<Self> {
        let mut value = state.lock();
        if value.paused { return None; }
        value.active += 1;
        Some(Self(state))
    }
}
impl Drop for UpdateWork<'_> {
    fn drop(&mut self) { self.0.lock().active -= 1; }
}
fn pause_update_work(state: &Mutex<UpdateWorkState>) -> bool {
    let mut value = state.lock();
    if value.active != 0 { return false; }
    value.paused = true;
    true
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

/// 获取剪贴板文本
pub fn get_clipboard_text() -> Result<Option<String>, String> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
    
    match ctx.get_text() {
        Ok(text) => Ok(Some(text)),
        Err(_) => Ok(None),
    }
}

/// 设置剪贴板文本
pub fn set_clipboard_text(text: &str) -> Result<(), String> {
    #[cfg(target_os = "windows")]
    {
        let data = win_clipboard::encode_unicode(text);
        if win_clipboard::write(&[(win_clipboard::CF_UNICODETEXT, data)]) {
            Ok(())
        } else {
            Err("设置剪贴板失败: 剪贴板被占用".to_string())
        }
    }

    #[cfg(not(target_os = "windows"))]
    {
        use clipboard_rs::{Clipboard, ClipboardContext};

        let ctx = ClipboardContext::new()
            .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
        ctx.set_text(text.to_string())
            .map_err(|e| format!("设置剪贴板失败: {}", e))
    }
}

/// 获取剪贴板图片（返回 PNG 字节）
pub fn get_clipboard_image() -> Result<Option<Vec<u8>>, String> {
    use clipboard_rs::{Clipboard, ClipboardContext, common::RustImage};
    use image::codecs::png::PngEncoder;
    use image::ImageEncoder;
    
    let ctx = ClipboardContext::new()
        .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
    
    match ctx.get_image() {
        Ok(rust_image) => {
            let rgba = rust_image.to_rgba8()
                .map_err(|e| e.to_string())?;
            
            let mut png_data = Vec::new();
            let encoder = PngEncoder::new(&mut png_data);
            encoder.write_image(
                rgba.as_raw(),
                rgba.width(),
                rgba.height(),
                image::ExtendedColorType::Rgba8,
            ).map_err(|e| e.to_string())?;
            
            Ok(Some(png_data))
        }
        Err(_) => Ok(None),
    }
}

/// 设置剪贴板图片（从 PNG 字节）
pub fn set_clipboard_image(image_bytes: &[u8]) -> Result<(), String> {
    use clipboard_rs::{Clipboard, ClipboardContext, common::RustImage};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
    
    // 从 PNG 字节创建 RustImage
    let rust_image = RustImage::from_bytes(image_bytes)
        .map_err(|e| format!("解析图片失败: {}", e))?;
    
    ctx.set_image(rust_image)
        .map_err(|e| format!("设置剪贴板图片失败: {}", e))
}

/// 获取剪贴板 HTML 内容
pub fn get_clipboard_html() -> Result<Option<String>, String> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
    
    // 自行解析：头部偏移量算错的内容不能导致 panic
    Ok(ctx
        .get_buffer(cf_html::FORMAT_NAME)
        .ok()
        .and_then(|raw| cf_html::document(&raw)))
}

/// 获取剪贴板 RTF 富文本内容
pub fn get_clipboard_rtf() -> Result<Option<String>, String> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
    
    match ctx.get_rich_text() {
        Ok(rtf) => Ok(Some(rtf)),
        Err(_) => Ok(None),
    }
}

/// 获取剪贴板文件路径列表
pub fn get_clipboard_files() -> Result<Vec<String>, String> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
    
    match ctx.get_files() {
        Ok(files) => Ok(files),
        Err(_) => Ok(vec![]),
    }
}

/// 设置剪贴板文件
pub fn set_clipboard_files(files: Vec<String>) -> Result<(), String> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
    
    ctx.set_files(files)
        .map_err(|e| format!("设置剪贴板文件失败: {}", e))
}

/// 获取剪贴板可用格式列表
pub fn get_available_formats() -> Result<Vec<String>, String> {
    use clipboard_rs::{Clipboard, ClipboardContext};
    
    let ctx = ClipboardContext::new()
        .map_err(|e| format!("创建剪贴板上下文失败: {}", e))?;
    
    match ctx.available_formats() {
        Ok(formats) => Ok(formats),
        Err(_) => Ok(vec![]),
    }
}

/// 获取剪贴板内容的来源应用（仅 Windows）
pub fn get_clipboard_owner() -> Option<String> {
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
                return None;
            }
            
            let mut process_id: u32 = 0;
            GetWindowThreadProcessId(hwnd, &mut process_id);
            
            if process_id == 0 {
                return None;
            }
            
            let handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, process_id);
            if handle.is_null() {
                return None;
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
                    return Some(name.to_string_lossy().to_string());
                }
                return Some(path_str);
            }
        }
        
        None
    }
    
    #[cfg(not(target_os = "windows"))]
    {
        None
    }
}

// ============== 剪贴板管理器 ==============

/// 剪贴板历史管理器：数据存在 SQLite 里
pub struct ClipboardManager {
    db: Arc<Mutex<Database>>,
    /// 数据库文件路径
    db_path: String,
    /// 历史记录数量限制，0 表示不限制
    history_limit: Arc<std::sync::atomic::AtomicI64>,
}

/// 全局历史限制（供监听线程使用）
static HISTORY_LIMIT: std::sync::atomic::AtomicI64 = std::sync::atomic::AtomicI64::new(0);

impl ClipboardManager {
    pub fn new(db_path: Option<String>) -> Result<Self, String> {
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
                .map_err(|e| format!("创建目录失败: {}", e))?;
        }
        
        let db = Database::new(&path)?;
        
        Ok(Self {
            db: Arc::new(Mutex::new(db)),
            db_path: path,
            history_limit: Arc::new(std::sync::atomic::AtomicI64::new(0)),
        })
    }

    /// 获取数据库文件路径
    pub fn db_path(&self) -> String {
        self.db_path.clone()
    }

    /// 获取图片存储目录路径
    /// 
    /// Returns:
    ///     str: 图片存储目录的完整路径
    pub fn get_images_dir(&self) -> String {
        let db = self.db.lock();
        db.get_images_dir().to_string_lossy().to_string()
    }

    /// 设置历史记录数量限制
    /// 
    /// Args:
    ///     limit: 最大记录数，0 表示不限制
    /// 
    /// 设置后，插入新记录时会自动清理超出限制的旧记录（保留置顶项）
    pub fn set_history_limit(&self, limit: i64) {
        self.history_limit.store(limit, Ordering::Relaxed);
        HISTORY_LIMIT.store(limit, Ordering::Relaxed);
        
        // 立即清理一次
        if limit > 0 {
            let db = self.db.lock();
            let _ = db.cleanup_old_items(limit);
        }
    }

    /// 获取当前历史记录数量限制
    pub fn get_history_limit(&self) -> i64 {
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
    pub fn start_monitor(&self, callback: Option<ItemCallback>, gate: CallbackGate) -> Result<(), String> {
        use clipboard_rs::{ClipboardHandler, ClipboardWatcher, ClipboardWatcherContext};
        
        if IS_RUNNING.compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst).is_err() {
            return Err("监听器已在运行".to_string());
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
                gate: CallbackGate,
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

                    let Some(_work) = UpdateWork::begin(&UPDATE_WORK) else { return; };
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
                    let source_app = get_clipboard_owner();
                    let html_content = captured
                        .data(win_clipboard::register_format(cf_html::FORMAT_NAME))
                        .and_then(cf_html::document);
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
                        let color_marked = all_names.iter().any(|(_, name)| name == text_kind::COLOR_MARKER_FORMAT);
                        main_item = PyClipboardItem::new(0, text, "text".to_string());
                        main_item.sub_kind = text_kind::detect(&main_item.content, color_marked).to_string();
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

                        let image_path = image_file(&self.images_dir, &image_id);
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

                    // 统计字节数并在拿数据库锁之前压缩好，存库时直接用
                    // 格式：(format_id, format_name, data, is_compressed)
                    let mut raw_total: usize = 0;
                    let mut compressed_total: usize = 0;
                    let formats_to_store: Vec<(u32, String, Vec<u8>, bool)> = filtered_formats
                        .into_iter()
                        .map(|(fid, fname, data)| {
                            raw_total += data.len();
                            let (stored, compressed) = compress_format(data);
                            compressed_total += stored.len();
                            (fid, fname, stored, compressed)
                        })
                        .collect();

                    let id = {
                        let db = self.db.lock();
                        let limit = HISTORY_LIMIT.load(Ordering::Relaxed);
                        let Ok(id) = db.store_capture(&main_item, &formats_to_store, limit) else {
                            return;
                        };
                        id
                    };
                    // 回调可能要拿绑定层的锁（Python 的 GIL），而持有它的线程可能正等着数据库锁，所以先放锁再回调
                    main_item.id = id;
                    main_item.char_count = Some((raw_total as i64) * 10_000_000 + compressed_total as i64);

                    if should_skip_callback(id) {
                        return;
                    }

                    // 同理不能拿着回调锁等 GIL：stop_monitor 是持 GIL 来改回调的，进了 gate 再取一次
                    if CALLBACK.lock().is_some() {
                        (self.gate)(&mut || {
                            let callback = CALLBACK.lock().clone();
                            if let Some(callback) = callback {
                                callback(&main_item);
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
                gate,
            };
            if let Ok(mut watcher) = ClipboardWatcherContext::new() {
                watcher.add_handler(handler);
                // stop_monitor 先清 IS_RUNNING 再取关闭通道：通道登记前到来的停止取不到通道，
                // 这里不能再进入监听循环，否则 stop_monitor 的 join 永远等不到本线程结束
                let listening = {
                    let mut shutdown = WATCHER_SHUTDOWN.lock();
                    let running = IS_RUNNING.load(Ordering::SeqCst);
                    if running {
                        *shutdown = Some(watcher.get_shutdown_channel());
                    }
                    running
                };
                if listening {
                    watcher.start_watch();
                }
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
    pub fn get_image_data(&self, image_id: &str) -> Result<Option<Vec<u8>>, String> {
        // 路径取出后就释放数据库锁；后续磁盘 I/O 不需要访问 SQLite，
        // 避免大图读取期间阻塞剪贴板监听线程。
        let image_path = {
            let db = self.db.lock();
            image_file(&db.get_images_dir(), image_id)
        };
        
        if image_path.exists() {
            let data = std::fs::read(&image_path)
                .map_err(|e| format!("读取图片失败: {}", e))?;
            Ok(Some(data))
        } else {
            Ok(self.image_from_formats(image_id))
        }
    }

    /// 图片文件不在时（同步盘还没传到、被删了），用库里存的原始图片数据现场生成 PNG。
    /// 不写回图片文件夹：同步盘上会和稍后同步来的原文件同名冲突
    #[cfg(target_os = "windows")]
    fn image_from_formats(&self, image_id: &str) -> Option<Vec<u8>> {
        use image::codecs::png::PngEncoder;
        use image::ImageEncoder;

        let formats = {
            let db = self.db.lock();
            let id = db.image_item_id(image_id).ok()??;
            db.get_formats(id).ok()?
        };
        if let Some(png) = stored_format(&formats, "PNG") {
            return Some(png.to_vec());
        }
        let rgba = win_clipboard::decode_image(None, stored_format(&formats, "CF_DIBV5"), stored_format(&formats, "CF_DIB"))?;
        let mut png = Vec::new();
        PngEncoder::new(&mut png)
            .write_image(rgba.as_raw(), rgba.width(), rgba.height(), image::ExtendedColorType::Rgba8)
            .ok()?;
        Some(png)
    }

    #[cfg(not(target_os = "windows"))]
    fn image_from_formats(&self, _image_id: &str) -> Option<Vec<u8>> {
        None
    }

    /// 获取某条记录保存的所有原始剪贴板格式（Ditto 风格）
    /// 
    /// Returns:
    ///     List[Tuple[int, str, bytes]]: [(format_id, format_name, raw_data), ...]
    pub fn get_raw_formats(&self, id: i64) -> Result<Vec<(u32, String, Vec<u8>)>, String> {
        let db = self.db.lock();
        db.get_formats(id)
    }

    /// 手动保存一批原始剪贴板格式数据（主要用于测试或外部调用）
    ///
    /// Args:
    ///     event_id: 关联的 clipboard.id
    ///     formats: List[Tuple[int, str, bytes]]，每项为 (format_id, format_name, raw_data)
    pub fn insert_formats(&self, event_id: i64, formats: Vec<(u32, String, Vec<u8>)>) -> Result<(), String> {
        let db = self.db.lock();
        db.insert_formats(event_id, &formats)
    }

    /// 停止剪贴板监听
    ///
    /// `join` 负责等监听线程结束：Python 绑定只在这一步放开 GIL，其余步骤持着它，
    /// 这样被清掉的回调不会再被调用。
    pub fn stop_monitor(&self, join: impl FnOnce(&mut (dyn FnMut() + Send))) {
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
                let mut handle = Some(handle);
                join(&mut || {
                    if let Some(handle) = handle.take() {
                        let _ = handle.join();
                    }
                });
            }
        }
    }

    /// 检查监听器是否运行中
    /// 
    /// Returns:
    ///     bool: 是否正在监听
    pub fn is_monitoring(&self) -> bool {
        IS_RUNNING.load(Ordering::Relaxed)
    }

    /// Return without waiting; the GUI keeps the download for a later retry when busy.
    pub fn pause_for_update(&self) -> bool { pause_update_work(&UPDATE_WORK) }

    pub fn resume_after_update(&self) { UPDATE_WORK.lock().paused = false; }

    pub fn pending_writes(&self) -> usize { UPDATE_WORK.lock().active }

    /// 查询剪贴板历史
    /// 
    /// Args:
    ///     offset: 偏移量，默认 0
    ///     limit: 每页数量，
    ///     search: 搜索关键词
    ///     filter: 内容类型和来源应用，空规则表示全部
    /// 
    /// Returns:
    ///     这一页的记录
    pub fn get_history(
        &self,
        offset: i64,
        limit: i64,
        search: Option<String>,
        filter: &FilterRule,
        start_time: Option<i64>,
        end_time: Option<i64>,
    ) -> Result<Vec<PyClipboardItem>, String> {
        let db = self.db.lock();
        db.query_items(offset, limit, search, filter, start_time, end_time)
    }

    /// 出现过的来源应用及条数，按条数从多到少；本程序的几种 exe 合并成 group_kind::OWN_APP
    pub fn source_apps(&self) -> Result<Vec<(String, i64)>, String> {
        let db = self.db.lock();
        db.source_apps()
    }

    /// 获取总记录数
    /// 
    /// Returns:
    ///     int: 总记录数
    pub fn get_count(&self) -> Result<i64, String> {
        let db = self.db.lock();
        db.get_count()
    }

    /// 根据 ID 获取项
    /// 
    /// Args:
    ///     id: 记录 ID
    /// 
    /// Returns:
    ///     Optional[PyClipboardItem]: 剪贴板项，不存在则返回 None
    pub fn get_item(&self, id: i64) -> Result<Option<PyClipboardItem>, String> {
        let db = self.db.lock();
        db.get_item_by_id(id)
    }

    /// 删除指定项
    /// 
    /// Args:
    ///     id: 要删除的记录 ID
    pub fn delete_item(&self, id: i64) -> Result<(), String> {
        let db = self.db.lock();
        db.delete_item(id)
    }

    /// 清空历史记录
    ///
    /// Args:
    ///     keep_grouped: True = 保留已加入分组的条目，只删历史区；False = 删除全部（默认）
    pub fn clear_history(&self, keep_grouped: bool) -> Result<(), String> {
        let db = self.db.lock();
        db.clear_all(keep_grouped)
    }

    /// 清理没有置顶、也没有加入分组的条目；置顶和分组里的内容都保留
    ///
    /// Args:
    ///     vacuum: True = 删完整理数据库文件、立即回收磁盘空间；
    ///             False = 只删行，空间留在库里复用，适合只想让条目不再显示的场合
    pub fn clear_unpinned_ungrouped(&self, vacuum: bool) -> Result<(), String> {
        let db = self.db.lock();
        db.clear_unpinned_ungrouped(vacuum)
    }

    /// 库是否只用一个文件，见 Database::single_file
    pub fn single_file(&self) -> bool {
        self.db.lock().single_file()
    }

    pub fn set_single_file(&self, on: bool) -> Result<(), String> {
        self.db.lock().set_single_file(on)
    }

    /// 切换置顶状态
    /// 
    /// Args:
    ///     id: 记录 ID
    /// 
    /// Returns:
    ///     bool: 新的置顶状态
    pub fn toggle_pin(&self, id: i64) -> Result<bool, String> {
        let db = self.db.lock();
        db.toggle_pin(id)
    }

    /// 搜索内容
    /// 
    /// Args:
    ///     keyword: 搜索关键词
    ///     limit: 返回数量限制，默认 50
    /// 
    /// Returns:
    ///     List[PyClipboardItem]: 匹配的记录列表
    pub fn search(&self, keyword: String, limit: i64) -> Result<Vec<PyClipboardItem>, String> {
        self.get_history(0, limit, Some(keyword), &FilterRule::default(), None, None)
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
    pub fn add_item(&self, content: String, content_type: Option<String>, title: Option<String>) -> Result<i64, String> {
        let mut item = PyClipboardItem::new(0, content, content_type.unwrap_or_else(|| "text".to_string()));
        item.title = title;
        let db = self.db.lock();
        db.insert_item(&item)
    }

    /// 更新内容项
    /// 
    /// Args:
    ///     id: 内容 ID
    ///     title: 标题（可选）
    ///     content: 内容文本
    pub fn update_item(&self, id: i64, content: String, title: Option<String>) -> Result<(), String> {
        let db = self.db.lock();
        db.update_item(id, title.as_deref(), &content)
    }

    /// 移动剪贴板内容到指定位置（拖拽排序）
    /// 
    /// Args:
    ///     id: 要移动的项 ID
    ///     before_id: 它前面的项 ID（None = 移到最前）
    ///     after_id: 它后面的项 ID（None = 移到最后）
    /// 
    /// 例如把 3 移到 1 和 2 之间传 (3, Some(1), Some(2))，移到最前传 (3, None, Some(1))。
    pub fn move_item_between(&self, id: i64, before_id: Option<i64>, after_id: Option<i64>) -> Result<(), String> {
        let db = self.db.lock();
        db.move_item_between(id, before_id, after_id)
    }

    /// 创建分组
    /// 
    /// Args:
    ///     name: 分组名称
    ///     color: 分组颜色（可选，如 "#FF0000"）
    ///     icon: 分组图标（可选）
    ///     group_type: 分组类型 (0=普通, 1=快速启动, 2=隐藏, 3=筛选)
    ///     filter_rule: 筛选分组的条件，其他分组传 None
    /// 
    /// Returns:
    ///     int: 新分组的 ID
    pub fn create_group(
        &self,
        name: String,
        color: Option<String>,
        icon: Option<String>,
        group_type: i64,
        filter_rule: Option<FilterRule>,
    ) -> Result<i64, String> {
        let db = self.db.lock();
        db.create_group(&name, color.as_deref(), icon.as_deref(), group_type, filter_rule.as_ref())
    }

    /// 获取所有分组
    /// 
    /// Returns:
    ///     List[PyGroup]: 分组列表
    pub fn get_groups(&self) -> Result<Vec<PyGroup>, String> {
        let db = self.db.lock();
        db.get_groups()
    }

    /// 删除分组
    /// 
    /// Args:
    ///     id: 分组 ID
    pub fn delete_group(&self, id: i64) -> Result<(), String> {
        let db = self.db.lock();
        db.delete_group(id)
    }

    /// 重命名分组
    /// 
    /// Args:
    ///     id: 分组 ID
    ///     name: 新名称
    pub fn rename_group(&self, id: i64, name: String) -> Result<(), String> {
        let db = self.db.lock();
        db.rename_group(id, &name)
    }

    /// 更新分组
    /// 
    /// Args:
    ///     id: 分组 ID
    ///     name: 名称
    ///     color: 颜色（可选）
    ///     icon: 图标（可选）
    ///     group_type: 分组类型 (0=普通, 1=快速启动, 2=隐藏, 3=筛选)
    ///     filter_rule: 筛选分组的条件，其他分组传 None
    pub fn update_group(
        &self,
        id: i64,
        name: String,
        color: Option<String>,
        icon: Option<String>,
        group_type: i64,
        filter_rule: Option<FilterRule>,
    ) -> Result<(), String> {
        let db = self.db.lock();
        db.update_group(id, &name, color.as_deref(), icon.as_deref(), group_type, filter_rule.as_ref())
    }

    /// 将项目移动到分组
    /// 
    /// Args:
    ///     item_id: 剪贴板项 ID
    ///     group_id: 目标分组 ID（None 表示移出分组）
    pub fn move_to_group(&self, item_id: i64, group_id: Option<i64>) -> Result<(), String> {
        let db = self.db.lock();
        db.move_to_group(item_id, group_id)
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
    pub fn move_group_between(&self, id: i64, before_id: Option<i64>, after_id: Option<i64>) -> Result<(), String> {
        let db = self.db.lock();
        db.move_group_between(id, before_id, after_id)
    }

    /// 按分组查询
    /// 
    /// Args:
    ///     group_id: 分组 ID（None 表示查询未分组的项目）
    ///     offset: 偏移量，默认 0
    ///     limit: 每页数量，默认 50
    /// 
    /// Returns:
    ///     这一页的记录
    pub fn get_by_group(&self, group_id: Option<i64>, offset: i64, limit: i64) -> Result<Vec<PyClipboardItem>, String> {
        let db = self.db.lock();
        db.query_by_group(group_id, offset, limit)
    }

    /// 增加粘贴次数（当用户粘贴某项时调用）
    /// 
    /// Args:
    ///     id: 剪贴板项 ID
    /// 
    /// Returns:
    ///     int: 新的粘贴次数
    pub fn increment_paste_count(&self, id: i64) -> Result<i64, String> {
        let db = self.db.lock();
        db.increment_paste_count(id)
    }

    /// 将项目内容设置到剪贴板（用于粘贴）
    ///
    /// Args:
    ///     id: 剪贴板项 ID
    ///     with_html: 是否包含 HTML 格式（默认 true）
    ///
    /// Returns:
    ///     bool: 是否成功
    pub fn paste_item(&self, id: i64, with_html: bool, move_to_top: bool) -> Result<bool, String> {
        // 读完数据库就放锁，写剪贴板时可能要等别的程序让出剪贴板
        let (item, raw_formats, images_dir) = {
            let db = self.db.lock();
            let Some(item) = db.get_item_by_id(id)? else {
                return Ok(false);
            };
            let raw_formats = db.get_formats(id).unwrap_or_default();
            (item, raw_formats, db.get_images_dir())
        };

        #[cfg(not(target_os = "windows"))]
        {
            let _ = (item, raw_formats, images_dir, with_html, move_to_top);
            return Err("paste_item 只支持 Windows".to_string());
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

    /// 把路径文本里还在的文件当作文件写进剪贴板（CF_HDROP），粘到资源管理器、聊天窗口里就是文件本身。
    /// 不是路径文本或路径都已不存在时返回 false
    pub fn paste_as_files(&self, id: i64) -> Result<bool, String> {
        let item = {
            let db = self.db.lock();
            db.get_item_by_id(id)?
        };
        let Some(item) = item.filter(|item| item.content_type == "text" && item.sub_kind == text_kind::PATH) else {
            return Ok(false);
        };
        let files: Vec<String> = text_kind::paths(&item.content)
            .into_iter()
            .filter(|path| std::path::Path::new(path).exists())
            .collect();
        if files.is_empty() {
            return Ok(false);
        }

        #[cfg(not(target_os = "windows"))]
        {
            let _ = files;
            return Err("paste_as_files 只支持 Windows".to_string());
        }

        #[cfg(target_os = "windows")]
        {
            if !win_clipboard::write(&[(win_clipboard::CF_HDROP, win_clipboard::build_hdrop(&files))]) {
                return Ok(false);
            }
            mark_own_clipboard_write();
            let _ = self.db.lock().increment_paste_count(id);
            Ok(true)
        }
    }

    /// 把几条记录合成一份内容写进剪贴板（多选粘贴）
    ///
    /// 文本按分隔符拼接，HTML、RTF 各合成一份，文件取并集；选中的全是图片时拼成
    /// 一张，否则图片不参与。
    ///
    /// Returns:
    ///     list[int]: 实际用上的记录 ID；什么都没写进剪贴板时为空
    pub fn paste_items(&self, ids: &[i64], options: &MultiPasteOptions) -> Result<Vec<i64>, String> {
        #[cfg(not(target_os = "windows"))]
        {
            let _ = (ids, options);
            Err("paste_items 只支持 Windows".to_string())
        }

        #[cfg(target_os = "windows")]
        {
            paste_items_impl(&self.db, ids, options)
        }
    }
}

/// 多选粘贴的选项
pub struct MultiPasteOptions {
    with_html: bool,
    move_to_top: bool,
    separator: String,
    plain_text: bool,
    vertical: bool,
    keep_in_history: bool,
}

impl MultiPasteOptions {
    /// separator 默认换行；layout 是全为图片时的拼接方向，"vertical"（默认）或 "horizontal"，
    /// 其他值返回 Err
    pub fn new(
        with_html: bool,
        move_to_top: bool,
        separator: Option<String>,
        plain_text: bool,
        layout: Option<&str>,
        keep_in_history: bool,
    ) -> Result<Self, String> {
        let vertical = match layout {
            None | Some("vertical") => true,
            Some("horizontal") => false,
            Some(other) => return Err(format!("未知的拼接方向: {other}")),
        };
        Ok(Self {
            with_html,
            move_to_top,
            separator: separator.unwrap_or_else(|| "\n".to_string()),
            plain_text,
            vertical,
            keep_in_history,
        })
    }
}

#[cfg(target_os = "windows")]
fn paste_items_impl(db: &Mutex<Database>, ids: &[i64], options: &MultiPasteOptions) -> Result<Vec<i64>, String> {
    let (items, images_dir) = {
        let db = db.lock();
        let items: Vec<PyClipboardItem> = ids.iter()
            .filter_map(|id| db.get_item_by_id(*id).ok().flatten())
            .collect();
        (items, db.get_images_dir())
    };
    let all_images = !options.plain_text
        && !items.is_empty()
        && items.iter().all(|item| item.content_type == "image");
    let (formats, used) = if all_images {
        stitched_image_formats(db, &items, &images_dir, options.vertical)?
    } else {
        merged_formats(db, &items, options)
    };
    if formats.is_empty() || !win_clipboard::write(&formats) {
        return Ok(Vec::new());
    }
    if !options.keep_in_history {
        mark_own_clipboard_write();
    }

    let db = db.lock();
    for id in &used {
        let _ = db.increment_paste_count(*id);
    }
    if options.move_to_top {
        let _ = db.move_items_to_top(&used);
    }
    Ok(used)
}

/// 按名字找一个存下的格式；注册格式的编号开机后会变，名字不会
#[cfg(target_os = "windows")]
fn stored_format<'a>(formats: &'a [(u32, String, Vec<u8>)], name: &str) -> Option<&'a [u8]> {
    formats.iter()
        .find(|(_, format_name, data)| format_name == name && !data.is_empty())
        .map(|(_, _, data)| data.as_slice())
}

/// 文本、文件记录合并成的格式，以及用上的记录 ID
#[cfg(target_os = "windows")]
fn merged_formats(
    db: &Mutex<Database>,
    items: &[PyClipboardItem],
    options: &MultiPasteOptions,
) -> (Vec<(u32, Vec<u8>)>, Vec<i64>) {
    use multi_paste::{HtmlPart, RtfSource};
    use win_clipboard::{CF_HDROP, CF_UNICODETEXT};

    struct TextPiece {
        text: String,
        html: Option<HtmlPart>,
        rtf: Option<Vec<u8>>,
    }

    let rich = options.with_html && !options.plain_text;
    let mut texts = Vec::new();
    let mut file_lists = Vec::new();
    let mut used = Vec::new();
    for item in items {
        if !matches!(item.content_type.as_str(), "text" | "file") {
            continue;
        }
        let formats = db.lock().get_formats(item.id).unwrap_or_default();
        if item.content_type == "text" {
            let text = stored_format(&formats, "CF_UNICODETEXT")
                .map(win_clipboard::decode_unicode)
                .filter(|text| !text.is_empty())
                .unwrap_or_else(|| item.content.clone());
            let html = rich.then(|| {
                stored_format(&formats, cf_html::FORMAT_NAME)
                    .and_then(HtmlPart::from_cf_html)
                    .or_else(|| item.html_content.as_deref()
                        .filter(|html| !html.is_empty())
                        .map(|html| HtmlPart::from_document(html, None)))
            }).flatten();
            let rtf = rich.then(|| stored_format(&formats, "Rich Text Format").map(<[u8]>::to_vec)).flatten();
            texts.push(TextPiece { text, html, rtf });
        } else {
            let files = stored_format(&formats, "CF_HDROP")
                .map(win_clipboard::parse_hdrop)
                .filter(|files| !files.is_empty())
                .unwrap_or_else(|| item_files(item));
            if files.is_empty() {
                continue;
            }
            if options.plain_text {
                texts.push(TextPiece { text: files.join("\r\n"), html: None, rtf: None });
            } else {
                file_lists.push(files);
            }
        }
        used.push(item.id);
    }

    let mut formats = Vec::new();
    if !texts.is_empty() {
        let plain: Vec<String> = texts.iter().map(|piece| piece.text.clone()).collect();
        let text = multi_paste::join_text(&plain, &options.separator);
        formats.push((CF_UNICODETEXT, win_clipboard::encode_unicode(&text)));
        // 有一条带富文本就生成整份，其余条目按纯文本转进去，免得贴进 Word 时只剩带格式的那几条
        if texts.iter().any(|piece| piece.html.is_some()) {
            let parts: Vec<HtmlPart> = texts.iter()
                .map(|piece| piece.html.clone().unwrap_or_else(|| HtmlPart::from_text(&piece.text)))
                .collect();
            let html = cf_html::encode(&multi_paste::merge_html(&parts, &options.separator));
            formats.push((win_clipboard::register_format(cf_html::FORMAT_NAME), html));
        }
        if texts.iter().any(|piece| piece.rtf.is_some()) {
            let sources: Vec<RtfSource> = texts.iter()
                .map(|piece| match &piece.rtf {
                    Some(rtf) => RtfSource::Rtf(rtf, &piece.text),
                    None => RtfSource::Text(&piece.text),
                })
                .collect();
            if let Some(rtf) = multi_paste::merge_rtf(&sources, &options.separator) {
                formats.push((win_clipboard::register_format("Rich Text Format"), rtf));
            }
        }
    }
    if !file_lists.is_empty() {
        formats.push((CF_HDROP, win_clipboard::build_hdrop(&multi_paste::union_files(&file_lists))));
    }
    (formats, used)
}

/// 全是图片时拼成一张，写 PNG 和 CF_DIBV5
#[cfg(target_os = "windows")]
fn stitched_image_formats(
    db: &Mutex<Database>,
    items: &[PyClipboardItem],
    images_dir: &std::path::Path,
    vertical: bool,
) -> Result<(Vec<(u32, Vec<u8>)>, Vec<i64>), String> {
    use multi_paste::ImageSource;

    // 先只读头部取尺寸，画布一次分配好，再逐张解码贴上，不必同时留着所有解码后的图
    let mut sources = Vec::new();
    for item in items {
        let formats = db.lock().get_formats(item.id).unwrap_or_default();
        let source = stored_format(&formats, "PNG")
            .map(|png| ImageSource::Png(png.to_vec()))
            .or_else(|| stored_format(&formats, "CF_DIBV5")
                .or_else(|| stored_format(&formats, "CF_DIB"))
                .map(|dib| ImageSource::Dib(dib.to_vec())))
            .or_else(|| item.image_id.as_deref()
                .and_then(|id| std::fs::read(image_file(images_dir, id)).ok())
                .map(ImageSource::Png));
        if let Some((source, size)) = source.and_then(|source| source.dimensions().map(|size| (source, size))) {
            sources.push((item.id, source, size));
        }
    }
    if sources.is_empty() {
        return Ok((Vec::new(), Vec::new()));
    }
    let sizes: Vec<(u32, u32)> = sources.iter().map(|(_, _, size)| *size).collect();
    let (width, height) = multi_paste::stitched_size(&sizes, vertical).ok_or("拼接后的图片太大")?;

    let mut canvas = image::RgbaImage::from_pixel(width, height, multi_paste::STITCH_BACKGROUND);
    let mut offset = 0;
    let mut used = Vec::new();
    for (id, source, (source_width, source_height)) in sources {
        let decoded = match &source {
            ImageSource::Png(png) => win_clipboard::decode_image(Some(png), None, None),
            ImageSource::Dib(dib) => win_clipboard::decode_image(None, Some(dib), None),
        };
        if let Some(image) = decoded {
            let (x, y) = if vertical { (0, offset) } else { (offset, 0) };
            multi_paste::paste_image(&mut canvas, &image, x, y);
            used.push(id);
        }
        offset += if vertical { source_height } else { source_width };
    }

    // 快速压缩：图可能很大，用户在等这一步
    let mut png = Vec::new();
    {
        use image::codecs::png::{CompressionType, FilterType, PngEncoder};
        use image::ImageEncoder;
        PngEncoder::new_with_quality(&mut png, CompressionType::Fast, FilterType::Adaptive)
            .write_image(canvas.as_raw(), width, height, image::ExtendedColorType::Rgba8)
            .map_err(|e| format!("编码拼接图片失败: {e}"))?;
    }
    let dibv5 = win_clipboard::rgba_to_dibv5(&canvas);
    drop(canvas);
    Ok((vec![(win_clipboard::register_format("PNG"), png), (win_clipboard::CF_DIBV5, dibv5)], used))
}

/// 文件记录里的路径列表
fn item_files(item: &PyClipboardItem) -> Vec<String> {
    serde_json::from_str::<serde_json::Value>(&item.content)
        .ok()
        .and_then(|json| json.get("files").and_then(|files| files.as_array()).cloned())
        .unwrap_or_default()
        .iter()
        .filter_map(|file| file.as_str().map(str::to_string))
        .collect()
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
) -> Result<Vec<(u32, Vec<u8>)>, String> {
    use win_clipboard::{CF_DIBV5, CF_HDROP, CF_UNICODETEXT};

    match item.content_type.as_str() {
        "text" => {
            let mut formats = vec![(CF_UNICODETEXT, win_clipboard::encode_unicode(&item.content))];
            if let Some(html) = item.html_content.as_deref().filter(|html| with_html && !html.is_empty()) {
                formats.push((win_clipboard::register_format(cf_html::FORMAT_NAME), cf_html::encode(html)));
            }
            Ok(formats)
        }
        "image" => {
            let Some(image_id) = item.image_id.as_deref() else {
                return Ok(Vec::new());
            };
            let Ok(png) = std::fs::read(image_file(images_dir, image_id)) else {
                return Ok(Vec::new());
            };
            let rgba = image::load_from_memory_with_format(&png, image::ImageFormat::Png)
                .map_err(|e| format!("解析图片失败: {}", e))?
                .to_rgba8();
            // PNG 加带 alpha 的 CF_DIBV5：只认位图的程序也能粘贴
            let dibv5 = win_clipboard::rgba_to_dibv5(&rgba);
            Ok(vec![(win_clipboard::register_format("PNG"), png), (CF_DIBV5, dibv5)])
        }
        "file" => {
            let files = item_files(item);
            Ok(if files.is_empty() { Vec::new() } else { vec![(CF_HDROP, win_clipboard::build_hdrop(&files))] })
        }
        _ => Ok(Vec::new()),
    }
}

#[cfg(all(test, target_os = "windows"))]
mod paste_format_tests {
    use super::*;
    use win_clipboard::*;

    #[test]
    fn a_missing_image_file_is_rebuilt_from_the_stored_formats() {
        let dir = std::env::temp_dir().join(format!("pyclipboard_image_fallback_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        {
            let manager = ClipboardManager::new(Some(dir.join("clipboard.db").to_string_lossy().to_string())).unwrap();
            let picture = image::RgbaImage::from_fn(5, 3, |x, y| image::Rgba([x as u8 * 40, y as u8 * 80, 7, 200]));
            let store = |image_id: &str, formats: Vec<(u32, String, Vec<u8>, bool)>| {
                let mut item = PyClipboardItem::new(0, "[5x3]".to_string(), "image".to_string());
                item.image_id = Some(image_id.to_string());
                manager.db.lock().store_capture(&item, &formats, 0).unwrap();
            };
            store("from-dib", vec![(CF_DIBV5, "CF_DIBV5".to_string(), rgba_to_dibv5(&picture), false)]);
            store("from-png", vec![(register_format("PNG"), "PNG".to_string(), b"original png".to_vec(), false)]);
            store("no-data", vec![]);

            let rebuilt = manager.get_image_data("from-dib").unwrap().unwrap();
            assert_eq!(image::load_from_memory_with_format(&rebuilt, image::ImageFormat::Png).unwrap().to_rgba8(), picture);
            assert_eq!(manager.get_image_data("from-png").unwrap().unwrap(), b"original png".to_vec());
            assert_eq!(manager.get_image_data("no-data").unwrap(), None);
            assert_eq!(manager.get_image_data("unknown").unwrap(), None);
            // 不写回图片文件夹
            assert!(!image_file(std::path::Path::new(&manager.get_images_dir()), "from-dib").exists());

            let file = image_file(std::path::Path::new(&manager.get_images_dir()), "from-dib");
            std::fs::write(&file, b"file wins").unwrap();
            assert_eq!(manager.get_image_data("from-dib").unwrap().unwrap(), b"file wins".to_vec());
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

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
        assert!(cf_html::document(&formats[1].1).unwrap().contains("<i>文字</i>"));
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
