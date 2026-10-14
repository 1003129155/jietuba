//! gifrecorder — Rust 实现的 GIF 录制器
//!
//! 提供帧存储、JPEG 压缩、后台解码、GIF 导出、屏幕截取（DXGI 优先，GDI 兜底）。
//! 默认带 pyo3 绑定（python.rs）；关掉默认 feature 即是纯 Rust 库。

pub mod capture;
pub mod decoder;
pub mod frame_diff;
mod frame_source;
pub mod frame_store;
pub mod gif_export;
pub mod jpeg;
pub mod recorder;
pub mod resize;

#[cfg(feature = "python")]
mod python;
