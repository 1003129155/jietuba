//! ppocr_rust's Engine for C. Mirrors the reference Python binding: one engine per
//! handle holding det + rec models; close() drops the models, the handle stays valid.

use std::ffi::{c_char, CString};
use std::sync::Mutex;

use ppocr_rust::engine;
use crate::panic::{guard, guard_error};
use crate::text::{str_arg, write_text};

pub struct JbPpocr {
    // None after close: ONNX models are tens of MB and must not wait for the handle.
    inner: Mutex<Option<engine::Engine>>,
}

/// One recognized line: four corners (x, y) from top-left clockwise in source
/// coordinates, the text (owned by the line array), and a 0..1 confidence.
#[repr(C)]
pub struct JbOcrLine {
    pub points: [f64; 8],
    pub text: *mut c_char,
    pub score: f64,
}

/// Loads ONNX Runtime from an absolute path; required before jb_ppocr_open. Once loaded,
/// later calls succeed without loading again; a failed load can be retried.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_init_runtime(dll_path: *const c_char, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let Some(path) = str_arg(dll_path) else {
            write_text("invalid runtime path", error, error_len);
            return false;
        };
        match engine::init_runtime(std::path::Path::new(path)) {
            Ok(()) => true,
            Err(message) => {
                write_text(&message, error, error_len);
                false
            }
        }
    })
}

/// Loads both models; the dictionary comes from the rec model's metadata. Null on failure.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_open(det_path: *const c_char, rec_path: *const c_char, error: *mut c_char, error_len: usize) -> *mut JbPpocr {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        let (Some(det), Some(rec)) = (str_arg(det_path), str_arg(rec_path)) else {
            write_text("invalid model path", error, error_len);
            return std::ptr::null_mut();
        };
        match engine::Engine::open(det, rec) {
            Ok(inner) => Box::into_raw(Box::new(JbPpocr { inner: Mutex::new(Some(inner)) })),
            Err(message) => {
                write_text(&message, error, error_len);
                std::ptr::null_mut()
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_free(ptr: *mut JbPpocr) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    })
}

/// Releases the models; later recognition fails. Repeated calls do nothing.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_close(ptr: *const JbPpocr) {
    guard((), || {
        if let Some(ocr) = ptr.as_ref() {
            if let Ok(mut guard) = ocr.inner.lock() {
                *guard = None;
            }
        }
    })
}

/// Recognizes a whole RGB image (rows of `stride` bytes). On success `*lines` holds
/// `*count` lines to free with jb_ppocr_lines_free.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_recognize(
    ptr: *const JbPpocr,
    data: *const u8,
    len: usize,
    w: usize,
    h: usize,
    stride: usize,
    lines: *mut *mut JbOcrLine,
    count: *mut usize,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let (Some(ocr), false, false, false) = (ptr.as_ref(), data.is_null(), lines.is_null(), count.is_null()) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        let pixels = std::slice::from_raw_parts(data, len);
        let result = (|| {
            let mut guard = ocr.inner.lock().map_err(|e| e.to_string())?;
            let engine = guard.as_mut().ok_or("引擎已关闭")?;
            engine.recognize(pixels, w, h, stride)
        })();
        match result {
            Ok(found) => {
                let mut out: Vec<JbOcrLine> = found
                    .into_iter()
                    .map(|line| {
                        let mut points = [0.0; 8];
                        for (index, point) in line.box_pts.iter().take(4).enumerate() {
                            points[index * 2] = point[0] as f64;
                            points[index * 2 + 1] = point[1] as f64;
                        }
                        let text = CString::new(line.text.replace('\0', "")).unwrap_or_default();
                        JbOcrLine { points, text: text.into_raw(), score: line.score as f64 }
                    })
                    .collect();
                out.shrink_to_fit();
                *count = out.len();
                *lines = Box::into_raw(out.into_boxed_slice()) as *mut JbOcrLine;
                true
            }
            Err(message) => {
                write_text(&message, error, error_len);
                false
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_lines_free(lines: *mut JbOcrLine, count: usize) {
    guard((), || {
        if lines.is_null() {
            return;
        }
        let slice = Box::from_raw(std::ptr::slice_from_raw_parts_mut(lines, count));
        for line in slice.iter() {
            if !line.text.is_null() {
                drop(CString::from_raw(line.text));
            }
        }
    })
}
