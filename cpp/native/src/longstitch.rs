//! longstitch's StitchSession for C. Mirrors the reference Python binding: frames go
//! in as BGRA, each push reports the stitch parameters, the long image stays inside.

use std::ffi::c_char;

use longstitch::error::StitchError;
use longstitch::session::{Heading, StitchSession};

use crate::panic::{guard, guard_error};
use crate::text::write_text;

/// A push's outcome; see the reference binding's PushResult.
#[repr(C)]
pub struct JbPushResult {
    pub head_cut: usize,
    pub head_rows: usize,
    pub keep: usize,
    pub skip: usize,
    /// Total rows after the push.
    pub height: usize,
    /// The row of the frame's first row in the result after the push.
    pub top: usize,
}

fn heading_from(value: i32) -> Option<Heading> {
    match value {
        0 => Some(Heading::Down),
        1 => Some(Heading::Up),
        _ => None,
    }
}

/// Null with the reason in `error` when the size is unusable.
#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_new(
    width: u32,
    height: u32,
    ignore_right_pixels: u32,
    ignore_top_pixels: u32,
    min_overlap_ratio: f32,
    error: *mut c_char,
    error_len: usize,
) -> *mut StitchSession {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        match StitchSession::new(width, height, ignore_right_pixels, ignore_top_pixels, min_overlap_ratio) {
            Ok(session) => Box::into_raw(Box::new(session)),
            Err(e) => {
                write_text(&e.to_string(), error, error_len);
                std::ptr::null_mut()
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_free(ptr: *mut StitchSession) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_width(ptr: *const StitchSession) -> u32 {
    guard(0, || {
        ptr.as_ref().map_or(0, StitchSession::width)
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_height(ptr: *const StitchSession) -> usize {
    guard(0, || {
        ptr.as_ref().map_or(0, StitchSession::height)
    })
}

/// 0 down, 1 up, -1 when there has been no movement yet.
#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_last_move(ptr: *const StitchSession) -> i32 {
    guard(-1, || {
        match ptr.as_ref().and_then(StitchSession::last_move) {
            Some(Heading::Down) => 0,
            Some(Heading::Up) => 1,
            None => -1,
        }
    })
}

/// Pushes one BGRA frame. heading: 0 down, 1 up, anything else unknown.
/// Returns 1 with `*result` filled, 0 when the frame does not overlap (the session is
/// unchanged), -1 with the reason in `error` on failure.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_stitch_session_push(
    ptr: *mut StitchSession,
    bgra: *const u8,
    len: usize,
    heading: i32,
    ignore_img1_top_ratio: f32,
    ignore_img1_bottom_ratio: f32,
    debug: bool,
    result: *mut JbPushResult,
    error: *mut c_char,
    error_len: usize,
) -> i32 {
    guard_error(-1, error, error_len, || {
        let Some(session) = ptr.as_mut() else {
            write_text("invalid session", error, error_len);
            return -1;
        };
        let frame: &[u8] = if bgra.is_null() { &[] } else { std::slice::from_raw_parts(bgra, len) };
        match session.push_bgra(frame, heading_from(heading), ignore_img1_top_ratio, ignore_img1_bottom_ratio, debug) {
            Ok(outcome) => {
                if let Some(out) = result.as_mut() {
                    *out = JbPushResult {
                        head_cut: outcome.head_cut,
                        head_rows: outcome.head_rows,
                        keep: outcome.keep,
                        skip: outcome.skip,
                        height: session.height(),
                        top: outcome.top,
                    };
                }
                1
            }
            Err(StitchError::NoOverlap) => 0,
            Err(e) => {
                write_text(&e.to_string(), error, error_len);
                -1
            }
        }
    })
}

/// Drops everything above the newest frame's top edge; returns the rows removed.
#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_crop_top(ptr: *mut StitchSession) -> usize {
    guard(0, || {
        ptr.as_mut().map_or(0, StitchSession::crop_top)
    })
}

/// Drops everything below the newest frame's bottom edge; returns the rows removed.
#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_crop_bottom(ptr: *mut StitchSession) -> usize {
    guard(0, || {
        ptr.as_mut().map_or(0, StitchSession::crop_bottom)
    })
}

/// The export size in bytes: width * height * 4.
#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_byte_len(ptr: *const StitchSession) -> usize {
    guard(0, || {
        ptr.as_ref().map_or(0, StitchSession::byte_len)
    })
}

/// Writes the current result as BGRA into `out`, which must hold byte_len bytes.
#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_export(ptr: *const StitchSession, out: *mut u8, len: usize) -> bool {
    guard(false, || {
        let Some(session) = ptr.as_ref() else { return false };
        if out.is_null() || len != session.byte_len() {
            return false;
        }
        session.export_into(std::slice::from_raw_parts_mut(out, len));
        true
    })
}

/// Releases the stitched image's memory now.
#[no_mangle]
pub unsafe extern "C" fn jb_stitch_session_close(ptr: *mut StitchSession) {
    guard((), || {
        if let Some(session) = ptr.as_mut() {
            session.clear();
        }
    })
}
