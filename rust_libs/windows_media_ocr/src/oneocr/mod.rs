//! oneocr.dll 高精度 OCR 引擎封装
//!
//! 通过 FFI 调用 Windows ScreenSketch 自带的 oneocr.dll，
//! 提供比 Windows.Media.Ocr 更高精度的中日韩英混合识别。
//!
//! 架构：
//! - `ffi`: DLL C 函数签名和结构体定义
//! - `dll_loader`: DLL 文件发现、复制和加载
//! - `engine`: 全局单例引擎 + Mutex 并发安全

pub mod dll_loader;
pub mod engine;
pub mod ffi;
