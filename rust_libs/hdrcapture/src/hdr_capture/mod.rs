//! HDR-aware synchronous desktop capture.
//!
//! [`Capture`] exposes an `mss`-style monitor list and `grab` method while retaining one DXGI
//! Desktop Duplication session per physical output. HDR FP16 frames are tone-mapped into sRGB
//! BGRA8 on the GPU; same-adapter outputs are composed into the virtual desktop on the GPU before
//! one CPU readback.

/// Portable HDR color conversion helpers and the tested CPU fallback implementation.
pub mod color;
/// Windows display topology, physical-pixel geometry, and Advanced Color diagnostics.
pub mod display;

mod capture;
mod gpu;

pub use capture::{Capture, CaptureStats, Error, Frame, FrameMonitorInfo, Monitor};
