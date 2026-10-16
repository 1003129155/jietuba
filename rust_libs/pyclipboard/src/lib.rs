//! 剪贴板历史：监听、存储、写回剪贴板。默认带 pyo3 绑定（python.rs）；关掉默认 feature 即是纯 Rust 库。

pub mod database;
pub mod manager;
pub mod multi_paste;
pub mod types;
#[cfg(target_os = "windows")]
pub mod win_clipboard;

#[cfg(feature = "python")]
mod python;
