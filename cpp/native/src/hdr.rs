//! hdrcapture for C. Mirrors the reference Python binding: a resident Capture per
//! handle, used only on the thread that created it (D3D11 contexts are not thread safe).

use std::ffi::c_char;

use hdrcapture::hdr_capture::display::{self, Rect};
use hdrcapture::hdr_capture::{Capture, Error, Frame, ToneMapping};

use crate::panic::{guard, guard_error};
use crate::text::write_text;

/// A monitor: index 0 is the whole virtual desktop, positive ones are physical monitors.
#[repr(C)]
pub struct JbHdrMonitor {
    pub index: usize,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub is_virtual_desktop: bool,
    pub hdr_enabled: bool,
    pub hdr_supported: bool,
}

/// A contributing monitor's tone mapping; has_peak is false when mapped statically.
#[repr(C)]
pub struct JbHdrFrameInfo {
    pub index: usize,
    pub has_peak: bool,
    pub tone_map_peak: f32,
}

/// A tightly packed sRGB BGRA8 frame; free with jb_hdr_frame_free.
#[repr(C)]
pub struct JbHdrFrame {
    pub bgra: *mut u8,
    pub len: usize,
    pub width: u32,
    pub height: u32,
    pub infos: *mut JbHdrFrameInfo,
    pub info_count: usize,
}

/// Error classes, as the binding's exception types.
const OK: i32 = 0;
const CAPTURE_ERROR: i32 = 1;
const INITIAL_FRAME_TIMEOUT: i32 = 2;
const ACCESS_LOST: i32 = 3;
const DIMENSIONS_CHANGED: i32 = 4;
const INVALID_MONITOR_INDEX: i32 = 5;

fn error_code(error: &Error) -> i32 {
    match error {
        Error::InitialFrameTimeout { .. } => INITIAL_FRAME_TIMEOUT,
        Error::AccessLost => ACCESS_LOST,
        Error::DimensionsChanged { .. } => DIMENSIONS_CHANGED,
        Error::InvalidMonitorIndex { .. } => INVALID_MONITOR_INDEX,
        _ => CAPTURE_ERROR,
    }
}

unsafe fn report(error: &Error, message: *mut c_char, message_len: usize) -> i32 {
    write_text(&error.to_string(), message, message_len);
    error_code(error)
}

unsafe fn hand_over(frame: Frame, out: *mut JbHdrFrame) {
    let Some(out) = out.as_mut() else { return };
    let infos: Vec<JbHdrFrameInfo> = frame
        .monitor_info
        .iter()
        .map(|info| JbHdrFrameInfo { index: info.index, has_peak: info.tone_map_peak.is_some(), tone_map_peak: info.tone_map_peak.unwrap_or(0.0) })
        .collect();
    let mut bgra = frame.bgra.into_boxed_slice();
    let mut infos = infos.into_boxed_slice();
    *out = JbHdrFrame {
        bgra: bgra.as_mut_ptr(),
        len: bgra.len(),
        width: frame.width,
        height: frame.height,
        infos: infos.as_mut_ptr(),
        info_count: infos.len(),
    };
    std::mem::forget(bgra);
    std::mem::forget(infos);
}

fn tone_mapping(adaptive: bool) -> ToneMapping {
    if adaptive {
        ToneMapping::Adaptive
    } else {
        ToneMapping::Static
    }
}

/// Builds the D3D device, duplications and shaders (about 90 ms). Null with the reason on failure.
#[no_mangle]
pub unsafe extern "C" fn jb_hdr_capture_new(timeout_ms: u32, error: *mut c_char, error_len: usize, error_kind: *mut i32) -> *mut Capture {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        match Capture::with_timeout(timeout_ms) {
            Ok(capture) => Box::into_raw(Box::new(capture)),
            Err(e) => {
                let code = report(&e, error, error_len);
                if let Some(kind) = error_kind.as_mut() {
                    *kind = code;
                }
                std::ptr::null_mut()
            }
        }
    })
}

/// Releases DXGI and D3D11 resources; on the creating thread.
#[no_mangle]
pub unsafe extern "C" fn jb_hdr_capture_free(ptr: *mut Capture) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_hdr_capture_timeout_ms(ptr: *const Capture) -> u32 {
    guard(0, || {
        ptr.as_ref().map_or(0, Capture::timeout_ms)
    })
}

/// Writes up to `capacity` monitors; returns how many exist. Re-read after hot plugging.
#[no_mangle]
pub unsafe extern "C" fn jb_hdr_capture_monitors(ptr: *const Capture, out: *mut JbHdrMonitor, capacity: usize) -> usize {
    guard(0, || {
        let Some(capture) = ptr.as_ref() else { return 0 };
        let monitors = capture.monitors();
        if !out.is_null() {
            for (slot, monitor) in std::slice::from_raw_parts_mut(out, capacity.min(monitors.len())).iter_mut().zip(monitors) {
                *slot = JbHdrMonitor {
                    index: monitor.index,
                    x: monitor.rect.x,
                    y: monitor.rect.y,
                    width: monitor.rect.width,
                    height: monitor.rect.height,
                    is_virtual_desktop: monitor.is_virtual_desktop,
                    hdr_enabled: monitor.hdr_enabled,
                    hdr_supported: monitor.hdr_supported,
                };
            }
        }
        monitors.len()
    })
}

/// Grabs the virtual desktop (0) or one monitor. timeout_ms below zero uses the session's
/// default; it is the budget per physical output. Returns 0, or an error class.
#[no_mangle]
pub unsafe extern "C" fn jb_hdr_capture_grab(
    ptr: *mut Capture,
    monitor_index: usize,
    timeout_ms: i64,
    adaptive: bool,
    out: *mut JbHdrFrame,
    error: *mut c_char,
    error_len: usize,
) -> i32 {
    guard_error(CAPTURE_ERROR, error, error_len, || {
        let Some(capture) = ptr.as_mut() else {
            write_text("capture is closed", error, error_len);
            return CAPTURE_ERROR;
        };
        let timeout = if timeout_ms < 0 { capture.timeout_ms() } else { timeout_ms as u32 };
        match capture.grab_with_tone_mapping(monitor_index, timeout, tone_mapping(adaptive)) {
            Ok(frame) => {
                hand_over(frame, out);
                OK
            }
            Err(e) => report(&e, error, error_len),
        }
    })
}

/// Grabs a region of the virtual desktop (physical pixels); arguments as jb_hdr_capture_grab.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_hdr_capture_grab_region(
    ptr: *mut Capture,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    timeout_ms: i64,
    adaptive: bool,
    out: *mut JbHdrFrame,
    error: *mut c_char,
    error_len: usize,
) -> i32 {
    guard_error(CAPTURE_ERROR, error, error_len, || {
        let Some(capture) = ptr.as_mut() else {
            write_text("capture is closed", error, error_len);
            return CAPTURE_ERROR;
        };
        let timeout = if timeout_ms < 0 { capture.timeout_ms() } else { timeout_ms as u32 };
        match capture.grab_region(Rect::new(x, y, width, height), timeout, tone_mapping(adaptive)) {
            Ok(frame) => {
                hand_over(frame, out);
                OK
            }
            Err(e) => report(&e, error, error_len),
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_hdr_frame_free(frame: *mut JbHdrFrame) {
    guard((), || {
        let Some(frame) = frame.as_mut() else { return };
        if !frame.bgra.is_null() {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(frame.bgra, frame.len)));
        }
        if !frame.infos.is_null() {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(frame.infos, frame.info_count)));
        }
        frame.bgra = std::ptr::null_mut();
        frame.infos = std::ptr::null_mut();
        frame.len = 0;
        frame.info_count = 0;
    })
}

/// The physical displays and their HDR state, without a capture session or D3D device.
/// Writes up to `capacity`, stores the total in `*count`; false with the reason on failure.
#[no_mangle]
pub unsafe extern "C" fn jb_hdr_displays(out: *mut JbHdrMonitor, capacity: usize, count: *mut usize, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        match display::enumerate_displays() {
            Ok(displays) => {
                if !out.is_null() {
                    for (slot, display) in std::slice::from_raw_parts_mut(out, capacity.min(displays.len())).iter_mut().zip(&displays) {
                        *slot = JbHdrMonitor {
                            index: display.index,
                            x: display.rect.x,
                            y: display.rect.y,
                            width: display.rect.width,
                            height: display.rect.height,
                            is_virtual_desktop: false,
                            hdr_enabled: display.hdr_enabled,
                            hdr_supported: display.hdr_supported,
                        };
                    }
                }
                if let Some(count) = count.as_mut() {
                    *count = displays.len();
                }
                true
            }
            Err(e) => {
                write_text(&Error::from(e).to_string(), error, error_len);
                false
            }
        }
    })
}
