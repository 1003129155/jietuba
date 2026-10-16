//! ppocr_rust - PP-OCR (PaddleOCR) ONNX 推理的 Python 扩展
//!
//! 用 Rust + ort(ONNX Runtime)在原生线程跑 det+rec，避免 Python 侧
//! opencv/numpy 依赖与 GIL 争用（推理时通过 allow_threads 释放 GIL）。
//! 默认带 pyo3 绑定（python.rs）；关掉默认 feature 即是纯 Rust 库。

pub mod engine;
pub mod geometry;
pub mod models;

#[cfg(feature = "python")]
mod python;
