//! ppocr_rust's Engine for C. Mirrors the reference Python binding: one engine per
//! handle holding det + rec models; close() drops the models, the handle stays valid.

use std::ffi::{c_char, c_void, CString};
use std::sync::Mutex;

use std::path::Path;

use ppocr_rust::{engine, models};
use crate::panic::{guard, guard_error};
use crate::text::{str_arg, write_text};

pub struct JbPpocr {
    // None after close: ONNX models are tens of MB and must not wait for the handle.
    inner: Mutex<Option<engine::Engine>>,
}

/// Where one character sits along the line's reading direction, the line's length being 1.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JbOcrChar {
    pub start: f32,
    pub center: f32,
    pub end: f32,
}

/// One recognized line: four corners (x, y) from top-left clockwise in source
/// coordinates, the text and characters (owned by the line array), and a 0..1 confidence.
/// Reading runs from the top-right corner down when `vertical`, else from the top-left
/// corner right. `chars` has one entry per character (Unicode scalar) of `text`, or none.
#[repr(C)]
pub struct JbOcrLine {
    pub points: [f64; 8],
    pub text: *mut c_char,
    pub score: f64,
    pub vertical: bool,
    pub chars: *mut JbOcrChar,
    pub char_count: usize,
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

/// Loads both models; with a null `dict_path` the dictionary comes from the rec model's
/// metadata. Null on failure.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_open(det_path: *const c_char, rec_path: *const c_char, dict_path: *const c_char, error: *mut c_char, error_len: usize) -> *mut JbPpocr {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        let (Some(det), Some(rec)) = (str_arg(det_path), str_arg(rec_path)) else {
            write_text("invalid model path", error, error_len);
            return std::ptr::null_mut();
        };
        let dict = str_arg(dict_path);
        if !dict_path.is_null() && dict.is_none() {
            write_text("invalid dictionary path", error, error_len);
            return std::ptr::null_mut();
        }
        match engine::Engine::open_with_dict(det, rec, dict) {
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

/// Asked with the caller's context during recognition; true stops it.
pub type JbCancelled = Option<unsafe extern "C" fn(context: *const c_void) -> bool>;

/// Recognizes a whole RGB image (rows of `stride` bytes). On success `*lines` holds
/// `*count` lines to free with jb_ppocr_lines_free. `cancelled` (may be null) is asked
/// before each detection tile and each line; once it returns true recognition stops and
/// succeeds with no lines.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_recognize(
    ptr: *const JbPpocr,
    data: *const u8,
    len: usize,
    w: usize,
    h: usize,
    stride: usize,
    cancelled: JbCancelled,
    context: *const c_void,
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
        let stop = || cancelled.is_some_and(|asked| asked(context));
        let result = (|| {
            let mut guard = ocr.inner.lock().map_err(|e| e.to_string())?;
            let engine = guard.as_mut().ok_or("引擎已关闭")?;
            engine.recognize(pixels, w, h, stride, &stop).map(Option::unwrap_or_default)
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
                        // Removing NULs would shift the characters, so such a line keeps none.
                        let chars: Vec<JbOcrChar> = if line.text.contains('\0') {
                            Vec::new()
                        } else {
                            line.chars.iter().map(|c| JbOcrChar { start: c.start, center: c.center, end: c.end }).collect()
                        };
                        let text = CString::new(line.text.replace('\0', "")).unwrap_or_default();
                        let char_count = chars.len();
                        let chars = if chars.is_empty() { std::ptr::null_mut() } else { Box::into_raw(chars.into_boxed_slice()) as *mut JbOcrChar };
                        JbOcrLine { points, text: text.into_raw(), score: line.score as f64, vertical: line.vertical, chars, char_count }
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
            if !line.chars.is_null() {
                drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(line.chars, line.char_count)));
            }
        }
    })
}

fn owned_text(text: &str) -> *mut c_char {
    CString::new(text.replace('\0', "")).map(CString::into_raw).unwrap_or(std::ptr::null_mut())
}

fn owned_json(value: &impl serde::Serialize) -> *mut c_char {
    serde_json::to_string(value).map(|text| owned_text(&text)).unwrap_or(std::ptr::null_mut())
}

/// Frees a string returned by the jb_ppocr_models_* functions.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_text_free(text: *mut c_char) {
    guard((), || {
        if !text.is_null() {
            drop(CString::from_raw(text));
        }
    })
}

/// The built-in download catalog as JSON.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_catalog() -> *mut c_char {
    guard(std::ptr::null_mut(), || owned_json(models::catalog()))
}

/// Installed models under `root` as a JSON array; the default model has an empty id.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_discover(root: *const c_char) -> *mut c_char {
    guard(std::ptr::null_mut(), || match str_arg(root) {
        Some(root) => owned_json(&models::discover(Path::new(root))),
        None => std::ptr::null_mut(),
    })
}

/// One installed model as JSON, or null when it is not installed.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_resolve(root: *const c_char, id: *const c_char) -> *mut c_char {
    guard(std::ptr::null_mut(), || match (str_arg(root), str_arg(id)) {
        (Some(root), Some(id)) => models::resolve(Path::new(root), id).map_or(std::ptr::null_mut(), |m| owned_json(&m)),
        _ => std::ptr::null_mut(),
    })
}

/// The installed model the automatic choice uses, as JSON; null when none is installed.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_auto_choice(root: *const c_char) -> *mut c_char {
    guard(std::ptr::null_mut(), || match str_arg(root) {
        Some(root) => models::auto_choice(Path::new(root)).map_or(std::ptr::null_mut(), |m| owned_json(&m)),
        None => std::ptr::null_mut(),
    })
}

/// Copies and validates user-chosen model files (`dict_path` may be null); the new model as
/// JSON, or null with `error` set.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_import(
    root: *const c_char,
    name: *const c_char,
    det_path: *const c_char,
    rec_path: *const c_char,
    dict_path: *const c_char,
    error: *mut c_char,
    error_len: usize,
) -> *mut c_char {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        let (Some(root), Some(name), Some(det), Some(rec)) = (str_arg(root), str_arg(name), str_arg(det_path), str_arg(rec_path)) else {
            write_text("invalid argument", error, error_len);
            return std::ptr::null_mut();
        };
        let dict = str_arg(dict_path);
        if !dict_path.is_null() && dict.is_none() {
            write_text("invalid dictionary path", error, error_len);
            return std::ptr::null_mut();
        }
        match models::import(Path::new(root), name, Path::new(det), Path::new(rec), dict.map(Path::new)) {
            Ok(model) => owned_json(&model),
            Err(message) => {
                write_text(&message, error, error_len);
                std::ptr::null_mut()
            }
        }
    })
}

/// Places a catalog model the updater downloaded; the model as JSON, or null with `error` set.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_finish_download(root: *const c_char, catalog_id: *const c_char, error: *mut c_char, error_len: usize) -> *mut c_char {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        let (Some(root), Some(id)) = (str_arg(root), str_arg(catalog_id)) else {
            write_text("invalid argument", error, error_len);
            return std::ptr::null_mut();
        };
        match models::finish_download(Path::new(root), id) {
            Ok(model) => owned_json(&model),
            Err(message) => {
                write_text(&message, error, error_len);
                std::ptr::null_mut()
            }
        }
    })
}

/// Moves the downloaded ONNX Runtime into `root`; call only before the runtime is loaded.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_finish_runtime_download(root: *const c_char, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let Some(root) = str_arg(root) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        match models::finish_runtime_download(Path::new(root)) {
            Ok(_) => true,
            Err(message) => {
                write_text(&message, error, error_len);
                false
            }
        }
    })
}

/// Deletes a model subdirectory; the default model cannot be removed.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_remove(root: *const c_char, id: *const c_char, error: *mut c_char, error_len: usize) -> bool {
    guard_error(false, error, error_len, || {
        let (Some(root), Some(id)) = (str_arg(root), str_arg(id)) else {
            write_text("invalid argument", error, error_len);
            return false;
        };
        match models::remove(Path::new(root), id) {
            Ok(()) => true,
            Err(message) => {
                write_text(&message, error, error_len);
                false
            }
        }
    })
}

/// Removes staging directories left by unfinished downloads or imports.
#[no_mangle]
pub unsafe extern "C" fn jb_ppocr_models_clean_staging(root: *const c_char) {
    guard((), || {
        if let Some(root) = str_arg(root) {
            models::clean_staging(Path::new(root));
        }
    })
}
