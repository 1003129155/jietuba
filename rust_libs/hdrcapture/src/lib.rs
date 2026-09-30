//! HDR 正确的 Windows 桌面截图。默认带 pyo3 绑定供 Python 调用；关掉默认 feature 即是纯 Rust 库，
//! gifrecorder 的录制线程就这样依赖它。
//!
//! d3d11、dxgi_duplication_api、monitor 三个模块取自 NiiightmareXD/windows-capture（MIT），
//! 只保留 DXGI Desktop Duplication 相关部分；出处与改动见 UPSTREAM.md。

pub mod d3d11;
pub mod dxgi_duplication_api;
pub mod hdr_capture;
pub mod monitor;

#[cfg(feature = "python")]
mod python;
