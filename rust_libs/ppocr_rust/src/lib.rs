//! ppocr_rust - PP-OCR (PaddleOCR) ONNX 推理
//!
//! 用 Rust + ort(ONNX Runtime)在原生线程跑 det+rec，不依赖 opencv。

pub mod engine;
pub mod geometry;
pub mod models;
