//! MP4 录制。默认带 pyo3 绑定（bindings.rs）；关掉默认 feature 即是纯 Rust 库。

pub mod audio;
#[cfg(feature = "python")]
mod bindings;
pub mod capture;
mod diagnostic;
pub mod error;
pub mod options;
pub mod recording;
pub mod session;
pub mod writer;
