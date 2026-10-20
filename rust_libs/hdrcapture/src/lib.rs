//! HDR 正确的 Windows 桌面截图；gifrecorder 与 video_recorder 的录制线程也直接用它。
//!
//! d3d11、dxgi_duplication_api、monitor 三个模块取自 NiiightmareXD/windows-capture（MIT），
//! 只保留 DXGI Desktop Duplication 相关部分；出处与改动见 UPSTREAM.md。

pub mod d3d11;
pub mod dxgi_duplication_api;
pub mod hdr_capture;
pub mod monitor;
