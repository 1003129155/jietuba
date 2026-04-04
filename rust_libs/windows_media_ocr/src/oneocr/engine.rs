//! oneocr 引擎核心 — 全局单例 + Mutex 并发安全
//!
//! - 首次调用时自动初始化（DLL 加载 + Pipeline 创建）
//! - Mutex 保证同一时刻只有一个线程调用 DLL
//! - Drop 自动释放所有 DLL 资源（RAII）

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::Path;
use std::sync::{Mutex, OnceLock};

use super::dll_loader;
use super::ffi::{FfiBoundingBox, ImageStructure, OcrDllFunctions};

/// 模型密钥（与 Python 版本一致）
const MODEL_KEY: &[u8] = b"kj)TGtrK>f]b[Piow.gU+nC@s\"\"\"\"\"\"4";
const MODEL_NAME: &str = "oneocr.onemodel";
const DLL_NAME: &str = "oneocr.dll";

// ============================================================
// 结果类型（Rust 侧的结构化输出）
// ============================================================

/// 单个词的识别结果
#[derive(Debug, Clone)]
pub struct OneOcrWord {
    pub text: String,
    pub bounding_rect: Option<BoundingRect>,
    pub confidence: Option<f32>,
}

/// 单行的识别结果
#[derive(Debug, Clone)]
pub struct OneOcrLine {
    pub text: String,
    pub bounding_rect: Option<BoundingRect>,
    pub words: Vec<OneOcrWord>,
}

/// 边界框（4 角坐标）
#[derive(Debug, Clone, Copy)]
pub struct BoundingRect {
    pub x1: f32,
    pub y1: f32,
    pub x2: f32,
    pub y2: f32,
    pub x3: f32,
    pub y3: f32,
    pub x4: f32,
    pub y4: f32,
}

/// 完整的 OCR 识别结果
#[derive(Debug, Clone)]
pub struct OneOcrResult {
    pub text: String,
    pub text_angle: Option<f32>,
    pub lines: Vec<OneOcrLine>,
}

// ============================================================
// 引擎内部状态
// ============================================================

/// 引擎内部持有 DLL 函数表和 pipeline 句柄
struct EngineInner {
    dll: OcrDllFunctions,
    init_options: i64,
    pipeline: i64,
    process_options: i64,
}

// Safety: EngineInner 通过 Mutex 保证同一时刻只有一个线程访问
unsafe impl Send for EngineInner {}

impl EngineInner {
    /// 创建引擎：加载 DLL → 创建 InitOptions → 创建 Pipeline → 创建 ProcessOptions
    fn new() -> Result<Self, String> {
        let dll_dir = dll_loader::find_and_prepare_dll_dir()?;
        let dll_path = dll_dir.join(DLL_NAME);
        let model_path = dll_dir.join(MODEL_NAME);

        if !dll_path.exists() {
            return Err(format!("DLL 文件不存在: {}", dll_path.display()));
        }
        if !model_path.exists() {
            return Err(format!("模型文件不存在: {}", model_path.display()));
        }

        // 设置 DLL 搜索目录（让 oneocr.dll 能找到 onnxruntime.dll）
        Self::set_dll_directory(&dll_dir)?;

        // 加载 DLL
        let lib = unsafe {
            libloading::Library::new(&dll_path)
                .map_err(|e| format!("加载 oneocr.dll 失败: {}", e))?
        };
        let dll = unsafe { OcrDllFunctions::bind(lib)? };

        // 创建 InitOptions
        let mut init_options: i64 = 0;
        let ret = unsafe { (dll.create_init_options)(&mut init_options) };
        if ret != 0 {
            return Err(format!("CreateOcrInitOptions 失败 (code: {})", ret));
        }

        // 设置不延迟加载模型
        let ret = unsafe { (dll.set_use_model_delay_load)(init_options, 0) };
        if ret != 0 {
            unsafe { (dll.release_init_options)(init_options) };
            return Err(format!(
                "OcrInitOptionsSetUseModelDelayLoad 失败 (code: {})",
                ret
            ));
        }

        // 创建 Pipeline
        let model_cstr = CString::new(model_path.to_string_lossy().as_bytes())
            .map_err(|e| format!("模型路径转换失败: {}", e))?;
        let key_cstr =
            CString::new(MODEL_KEY).map_err(|e| format!("密钥转换失败: {}", e))?;

        let mut pipeline: i64 = 0;
        let ret = unsafe {
            (dll.create_pipeline)(
                model_cstr.as_ptr(),
                key_cstr.as_ptr(),
                init_options,
                &mut pipeline,
            )
        };
        if ret != 0 {
            unsafe { (dll.release_init_options)(init_options) };
            return Err(format!("CreateOcrPipeline 失败 (code: {})", ret));
        }

        // 创建 ProcessOptions
        let mut process_options: i64 = 0;
        let ret = unsafe { (dll.create_process_options)(&mut process_options) };
        if ret != 0 {
            unsafe {
                (dll.release_pipeline)(pipeline);
                (dll.release_init_options)(init_options);
            }
            return Err(format!("CreateOcrProcessOptions 失败 (code: {})", ret));
        }

        // 解除默认 100 行的识别上限（设为 0 = 无限制）
        // 部分版本的 DLL 可能没有这个函数，安全跳过
        if let Some(set_max) = dll.set_max_recognition_line_count {
            unsafe { set_max(process_options, 0) };
        }

        Ok(EngineInner {
            dll,
            init_options,
            pipeline,
            process_options,
        })
    }

    #[cfg(windows)]
    fn set_dll_directory(dir: &Path) -> Result<(), String> {
        use std::os::windows::ffi::OsStrExt;
        let wide: Vec<u16> = dir.as_os_str().encode_wide().chain(Some(0)).collect();
        let ret = unsafe {
            windows::Win32::System::LibraryLoader::SetDllDirectoryW(
                windows::core::PCWSTR(wide.as_ptr()),
            )
        };
        match ret {
            Ok(_) => Ok(()),
            Err(e) => Err(format!("SetDllDirectoryW 失败: {} ({})", dir.display(), e)),
        }
    }

    #[cfg(not(windows))]
    fn set_dll_directory(_dir: &Path) -> Result<(), String> {
        Err("oneocr 仅支持 Windows".to_string())
    }

    /// 执行 OCR 识别（已在 Mutex 内，无需额外同步）
    fn recognize_bgra(
        &self,
        width: i32,
        height: i32,
        bgra_data: &[u8],
    ) -> Result<OneOcrResult, String> {
        // 尺寸检查
        if width < 20 || height < 20 || width > 10000 || height > 10000 {
            return Err(format!(
                "图像尺寸不支持: {}x{} (需要 20~10000)",
                width, height
            ));
        }

        let step = width as i64 * 4;
        let img = ImageStructure {
            image_type: 3, // BGRA
            width,
            height,
            _reserved: 0,
            step_size: step,
            data_ptr: bgra_data.as_ptr(),
        };

        // 调用 DLL
        let mut ocr_result: i64 = 0;
        let ret = unsafe {
            (self.dll.run_pipeline)(
                self.pipeline,
                &img as *const ImageStructure,
                self.process_options,
                &mut ocr_result,
            )
        };
        if ret != 0 {
            return Ok(OneOcrResult {
                text: String::new(),
                text_angle: None,
                lines: Vec::new(),
            });
        }

        // 解析结果
        let result = self.parse_result(ocr_result);

        // 释放 DLL 的结果对象
        unsafe { (self.dll.release_result)(ocr_result) };

        result
    }

    /// 从裸指针执行 OCR 识别（零拷贝路径，已在 Mutex 内）
    ///
    /// # Safety
    /// 调用方必须保证 ptr 指向有效的 BGRA 像素数据，
    /// 且在函数返回前不会被释放。参数校验由 pub fn recognize_raw 完成。
    fn recognize_bgra_raw(
        &self,
        ptr: usize,
        width: i32,
        height: i32,
        stride: i32,
    ) -> Result<OneOcrResult, String> {
        let img = ImageStructure {
            image_type: 3, // BGRA
            width,
            height,
            _reserved: 0,
            step_size: stride as i64,
            data_ptr: ptr as *const u8,
        };

        // 调用 DLL
        let mut ocr_result: i64 = 0;
        let ret = unsafe {
            (self.dll.run_pipeline)(
                self.pipeline,
                &img as *const ImageStructure,
                self.process_options,
                &mut ocr_result,
            )
        };
        if ret != 0 {
            return Ok(OneOcrResult {
                text: String::new(),
                text_angle: None,
                lines: Vec::new(),
            });
        }

        // 解析结果
        let result = self.parse_result(ocr_result);

        // 释放 DLL 的结果对象
        unsafe { (self.dll.release_result)(ocr_result) };

        result
    }

    /// 解析 DLL 返回的 OCR 结果
    fn parse_result(&self, ocr_result: i64) -> Result<OneOcrResult, String> {
        // 文本角度
        let text_angle = {
            let mut angle: f32 = 0.0;
            if unsafe { (self.dll.get_image_angle)(ocr_result, &mut angle) } == 0 {
                Some(angle)
            } else {
                None
            }
        };

        // 行数
        let mut line_count: i64 = 0;
        if unsafe { (self.dll.get_line_count)(ocr_result, &mut line_count) } != 0 {
            return Ok(OneOcrResult {
                text: String::new(),
                text_angle,
                lines: Vec::new(),
            });
        }

        let mut lines = Vec::with_capacity(line_count as usize);
        let mut full_text = String::new();

        for i in 0..line_count {
            let line = self.parse_line(ocr_result, i)?;
            if !full_text.is_empty() {
                full_text.push('\n');
            }
            full_text.push_str(&line.text);
            lines.push(line);
        }

        Ok(OneOcrResult {
            text: full_text,
            text_angle,
            lines,
        })
    }

    fn parse_line(&self, ocr_result: i64, line_idx: i64) -> Result<OneOcrLine, String> {
        let mut line_handle: i64 = 0;
        if unsafe { (self.dll.get_line)(ocr_result, line_idx, &mut line_handle) } != 0 {
            return Ok(OneOcrLine {
                text: String::new(),
                bounding_rect: None,
                words: Vec::new(),
            });
        }

        // 行文本
        let text = self.get_text(line_handle, &self.dll.get_line_content);

        // 行边界框
        let bounding_rect = self.get_bbox(line_handle, &self.dll.get_line_bounding_box);

        // 词
        let mut word_count: i64 = 0;
        let words =
            if unsafe { (self.dll.get_line_word_count)(line_handle, &mut word_count) } == 0 {
                (0..word_count)
                    .map(|j| self.parse_word(line_handle, j))
                    .collect()
            } else {
                Vec::new()
            };

        Ok(OneOcrLine {
            text,
            bounding_rect,
            words,
        })
    }

    fn parse_word(&self, line_handle: i64, word_idx: i64) -> OneOcrWord {
        let mut word_handle: i64 = 0;
        if unsafe { (self.dll.get_word)(line_handle, word_idx, &mut word_handle) } != 0 {
            return OneOcrWord {
                text: String::new(),
                bounding_rect: None,
                confidence: None,
            };
        }

        let text = self.get_text(word_handle, &self.dll.get_word_content);
        let bounding_rect = self.get_bbox(word_handle, &self.dll.get_word_bounding_box);

        let confidence = {
            let mut conf: f32 = 0.0;
            if unsafe { (self.dll.get_word_confidence)(word_handle, &mut conf) } == 0 {
                Some(conf)
            } else {
                None
            }
        };

        OneOcrWord {
            text,
            bounding_rect,
            confidence,
        }
    }

    /// 从句柄读取文本（通用辅助）
    fn get_text(
        &self,
        handle: i64,
        func: &unsafe extern "C" fn(i64, *mut *const c_char) -> i64,
    ) -> String {
        let mut ptr: *const c_char = std::ptr::null();
        if unsafe { func(handle, &mut ptr) } == 0 && !ptr.is_null() {
            unsafe { CStr::from_ptr(ptr) }
                .to_string_lossy()
                .to_string()
        } else {
            String::new()
        }
    }

    /// 从句柄读取边界框（通用辅助）
    fn get_bbox(
        &self,
        handle: i64,
        func: &unsafe extern "C" fn(i64, *mut *const FfiBoundingBox) -> i64,
    ) -> Option<BoundingRect> {
        let mut ptr: *const FfiBoundingBox = std::ptr::null();
        if unsafe { func(handle, &mut ptr) } == 0 && !ptr.is_null() {
            let bbox = unsafe { &*ptr };
            Some(BoundingRect {
                x1: bbox.x1,
                y1: bbox.y1,
                x2: bbox.x2,
                y2: bbox.y2,
                x3: bbox.x3,
                y3: bbox.y3,
                x4: bbox.x4,
                y4: bbox.y4,
            })
        } else {
            None
        }
    }
}

impl Drop for EngineInner {
    fn drop(&mut self) {
        // RAII: 自动释放所有 DLL 资源，不可能泄漏
        unsafe {
            (self.dll.release_process_options)(self.process_options);
            (self.dll.release_pipeline)(self.pipeline);
            (self.dll.release_init_options)(self.init_options);
        }
    }
}

// ============================================================
// 全局单例
// ============================================================

/// 全局引擎：OnceLock 保证只初始化一次，Mutex 保证并发安全
static ENGINE: OnceLock<Mutex<EngineInner>> = OnceLock::new();

/// 初始化引擎失败时的错误信息缓存
static INIT_ERROR: OnceLock<String> = OnceLock::new();

/// 确保引擎已初始化
fn ensure_engine() -> Result<&'static Mutex<EngineInner>, String> {
    // 如果之前初始化失败过，直接返回错误
    if let Some(err) = INIT_ERROR.get() {
        return Err(err.clone());
    }

    // 尝试获取已初始化的引擎
    if let Some(engine) = ENGINE.get() {
        return Ok(engine);
    }

    // 首次初始化
    match EngineInner::new() {
        Ok(inner) => {
            let _ = ENGINE.set(Mutex::new(inner));
            ENGINE
                .get()
                .ok_or_else(|| "引擎初始化后获取失败".to_string())
        }
        Err(e) => {
            let _ = INIT_ERROR.set(e.clone());
            Err(e)
        }
    }
}

// ============================================================
// 公开 API
// ============================================================

/// 检查 oneocr 引擎是否可用（尝试初始化）
pub fn is_available() -> bool {
    ensure_engine().is_ok()
}

/// 初始化引擎（可选，首次 recognize 时会自动调用）
pub fn initialize() -> Result<(), String> {
    ensure_engine().map(|_| ())
}

/// 从图像字节数据识别文字
///
/// 接受 PNG/JPG/BMP 格式的 bytes，内部解码为 BGRA 像素后调用 DLL。
/// Mutex 保证多线程安全：多个调用会排队等待，不会并发访问 DLL。
pub fn recognize(image_data: &[u8]) -> Result<OneOcrResult, String> {
    let engine_mutex = ensure_engine()?;

    // 在获取锁之前先解码图像（不需要锁，可以并行）
    let bgra = decode_to_bgra(image_data)?;

    // 获取 Mutex 锁（同一时刻只有一个线程调用 DLL）
    let engine = engine_mutex
        .lock()
        .map_err(|e| format!("引擎锁获取失败(poisoned): {}", e))?;

    engine.recognize_bgra(bgra.width as i32, bgra.height as i32, &bgra.data)
}

/// 从 BGRA 原始像素数据直接识别文字（零拷贝路径）
///
/// # Safety
/// 调用方必须保证：
/// - `ptr` 指向有效的 BGRA 像素数据，在函数返回前不会被释放
/// - `stride * height` ≤ 实际 buffer 大小
/// - `width > 0 && height > 0`
///
/// 设计用于 Qt QImage(Format_ARGB32) 的 bits() 指针直传，
/// 因为 ARGB32 在 little-endian 上的内存布局恰好是 BGRA。
pub fn recognize_raw(
    ptr: usize,
    width: i32,
    height: i32,
    stride: i32,
) -> Result<OneOcrResult, String> {
    // 参数校验
    if width < 20 || height < 20 || width > 10000 || height > 10000 {
        return Err(format!(
            "图像尺寸不支持: {}x{} (需要 20~10000)",
            width, height
        ));
    }
    if stride < width * 4 {
        return Err(format!(
            "stride ({}) 不能小于 width*4 ({})",
            stride,
            width * 4
        ));
    }
    if ptr == 0 {
        return Err("像素数据指针为空".to_string());
    }

    let engine_mutex = ensure_engine()?;

    // 获取 Mutex 锁
    let engine = engine_mutex
        .lock()
        .map_err(|e| format!("引擎锁获取失败(poisoned): {}", e))?;

    engine.recognize_bgra_raw(ptr, width, height, stride)
}

/// 释放引擎资源
///
/// 注意：OnceLock 全局单例的生命周期与进程相同，不支持运行时释放。
/// 引擎资源会在进程退出时通过 Drop trait 自动释放。
/// 此函数保留为 API 兼容性，实际为空操作。
pub fn release() {
    // OnceLock 不提供 take()，引擎会在进程退出时通过 Drop 自动释放。
}

// ============================================================
// 图像解码辅助
// ============================================================

/// BGRA 像素数据
struct BgraImage {
    width: u32,
    height: u32,
    data: Vec<u8>,
}

/// 将 PNG/JPG/BMP 字节解码为 BGRA 像素数据
fn decode_to_bgra(image_data: &[u8]) -> Result<BgraImage, String> {
    let img =
        image::load_from_memory(image_data).map_err(|e| format!("图像解码失败: {}", e))?;

    let rgba = img.to_rgba8();
    let width = rgba.width();
    let height = rgba.height();

    // RGBA → BGRA（oneocr.dll 要求 BGRA 格式）
    let mut bgra_data = rgba.into_raw();
    for chunk in bgra_data.chunks_exact_mut(4) {
        chunk.swap(0, 2); // R ↔ B
    }

    Ok(BgraImage {
        width,
        height,
        data: bgra_data,
    })
}
