//! oneocr.dll 的 C FFI 结构体和函数签名定义
//!
//! 对应 Python windos_ocr.py 中的 ctypes 结构体和 DLL_FUNCTIONS 列表。
//! 使用 libloading 动态加载，编译期类型安全。

use std::os::raw::{c_char, c_float};

/// 图像数据结构（传给 DLL 的输入）
#[repr(C)]
pub struct ImageStructure {
    /// 图像类型（3 = BGRA）
    pub image_type: i32,
    /// 图像宽度（像素）
    pub width: i32,
    /// 图像高度（像素）
    pub height: i32,
    /// 保留字段
    pub _reserved: i32,
    /// 每行字节数
    pub step_size: i64,
    /// 像素数据指针
    pub data_ptr: *const u8,
}

/// 文字边界框（DLL 返回的 4 个角坐标）
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct FfiBoundingBox {
    pub x1: c_float,
    pub y1: c_float,
    pub x2: c_float,
    pub y2: c_float,
    pub x3: c_float,
    pub y3: c_float,
    pub x4: c_float,
    pub y4: c_float,
}

/// DLL 函数表
///
/// 通过 libloading 动态绑定 oneocr.dll 的所有导出函数。
/// 每个字段对应一个 DLL 函数指针，类型在编译期检查。
pub struct OcrDllFunctions {
    // 保持 Library 存活，防止 DLL 被卸载
    _lib: libloading::Library,

    // ---- 初始化 ----
    pub create_init_options: unsafe extern "C" fn(*mut i64) -> i64,
    pub set_use_model_delay_load: unsafe extern "C" fn(i64, c_char) -> i64,

    // ---- Pipeline ----
    pub create_pipeline:
        unsafe extern "C" fn(*const c_char, *const c_char, i64, *mut i64) -> i64,
    pub create_process_options: unsafe extern "C" fn(*mut i64) -> i64,
    pub run_pipeline:
        unsafe extern "C" fn(i64, *const ImageStructure, i64, *mut i64) -> i64,

    // ---- 结果读取 ----
    pub get_image_angle: unsafe extern "C" fn(i64, *mut c_float) -> i64,
    pub get_line_count: unsafe extern "C" fn(i64, *mut i64) -> i64,
    pub get_line: unsafe extern "C" fn(i64, i64, *mut i64) -> i64,
    pub get_line_content: unsafe extern "C" fn(i64, *mut *const c_char) -> i64,
    pub get_line_bounding_box:
        unsafe extern "C" fn(i64, *mut *const FfiBoundingBox) -> i64,
    pub get_line_word_count: unsafe extern "C" fn(i64, *mut i64) -> i64,
    pub get_word: unsafe extern "C" fn(i64, i64, *mut i64) -> i64,
    pub get_word_content: unsafe extern "C" fn(i64, *mut *const c_char) -> i64,
    pub get_word_bounding_box:
        unsafe extern "C" fn(i64, *mut *const FfiBoundingBox) -> i64,
    pub get_word_confidence: unsafe extern "C" fn(i64, *mut c_float) -> i64,

    // ---- ProcessOptions 配置（可选，部分版本 DLL 可能不存在） ----
    pub set_max_recognition_line_count:
        Option<unsafe extern "C" fn(i64, i64) -> i64>,

    // ---- 资源释放 ----
    pub release_result: unsafe extern "C" fn(i64),
    pub release_init_options: unsafe extern "C" fn(i64),
    pub release_pipeline: unsafe extern "C" fn(i64),
    pub release_process_options: unsafe extern "C" fn(i64),
}

impl OcrDllFunctions {
    /// 从已加载的 DLL 中绑定所有函数
    ///
    /// # Safety
    /// 调用方必须确保 `lib` 是有效的 oneocr.dll
    pub unsafe fn bind(lib: libloading::Library) -> Result<Self, String> {
        macro_rules! load_fn {
            ($lib:expr, $name:expr) => {
                *$lib
                    .get::<_>(concat!($name, "\0").as_bytes())
                    .map_err(|e| format!("找不到 DLL 函数 {}: {}", $name, e))?
            };
        }

        // 可选函数加载：找不到时返回 None 而不是报错
        macro_rules! try_load_fn {
            ($lib:expr, $name:expr) => {
                $lib.get::<_>(concat!($name, "\0").as_bytes())
                    .ok()
                    .map(|f| *f)
            };
        }

        let fns = OcrDllFunctions {
            create_init_options: load_fn!(lib, "CreateOcrInitOptions"),
            set_use_model_delay_load: load_fn!(lib, "OcrInitOptionsSetUseModelDelayLoad"),
            create_pipeline: load_fn!(lib, "CreateOcrPipeline"),
            create_process_options: load_fn!(lib, "CreateOcrProcessOptions"),
            run_pipeline: load_fn!(lib, "RunOcrPipeline"),
            get_image_angle: load_fn!(lib, "GetImageAngle"),
            get_line_count: load_fn!(lib, "GetOcrLineCount"),
            get_line: load_fn!(lib, "GetOcrLine"),
            get_line_content: load_fn!(lib, "GetOcrLineContent"),
            get_line_bounding_box: load_fn!(lib, "GetOcrLineBoundingBox"),
            get_line_word_count: load_fn!(lib, "GetOcrLineWordCount"),
            get_word: load_fn!(lib, "GetOcrWord"),
            get_word_content: load_fn!(lib, "GetOcrWordContent"),
            get_word_bounding_box: load_fn!(lib, "GetOcrWordBoundingBox"),
            get_word_confidence: load_fn!(lib, "GetOcrWordConfidence"),
            set_max_recognition_line_count: try_load_fn!(lib, "OcrProcessOptionsSetMaxRecognitionLineCount"),
            release_result: load_fn!(lib, "ReleaseOcrResult"),
            release_init_options: load_fn!(lib, "ReleaseOcrInitOptions"),
            release_pipeline: load_fn!(lib, "ReleaseOcrPipeline"),
            release_process_options: load_fn!(lib, "ReleaseOcrProcessOptions"),
            _lib: lib,
        };
        Ok(fns)
    }
}
