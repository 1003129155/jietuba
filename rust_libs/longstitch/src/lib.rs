//! 长截图拼接。默认带 pyo3 绑定（python.rs）；关掉默认 feature 即是纯 Rust 库。

pub mod error;
pub mod hash;
pub mod lcs;
pub mod session;
pub mod stitch;

#[cfg(feature = "python")]
mod python;
