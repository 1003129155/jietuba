//! gifrecorder for C. Mirrors the reference Python binding: FrameStore (JPEG frames),
//! FrameDecoder (background decoding for playback) and RecordSession (capture thread).

use std::ffi::c_char;
use std::sync::Arc;

use gifrecorder::decoder::FrameDecoder;
use gifrecorder::frame_store::{FrameStore, RecordConfig, RecordState};
use gifrecorder::gif_export::{self, CursorInfo, CursorSprites, GifExportOptions, Sprite};
use gifrecorder::recorder::RecordSession;

use crate::panic::{guard, guard_error};
use crate::text::{str_arg, write_text};

pub struct JbFrameStore {
    inner: Arc<FrameStore>,
}

pub struct JbFrameDecoder {
    inner: Option<FrameDecoder>,
}

pub struct JbRecordSession {
    inner: Option<RecordSession>,
    /// The capture path noted before stop() takes the session.
    stopped_backend: Option<&'static str>,
}

/// One RGBA32 sprite of w x h.
#[repr(C)]
pub struct JbSprite {
    pub data: *const u8,
    pub len: usize,
    pub w: u32,
    pub h: u32,
}

#[repr(C)]
pub struct JbCursorSprites {
    pub cursor: JbSprite,
    pub burst_left: [JbSprite; 3],
    pub burst_right: [JbSprite; 3],
    pub scroll_up: JbSprite,
    pub scroll_down: JbSprite,
}

/// A frame's cursor in GIF coordinates; present false means no cursor on that frame.
#[repr(C)]
pub struct JbCursorInfo {
    pub present: bool,
    pub x: i32,
    pub y: i32,
    pub press: u8,
    pub scroll: i8,
    pub burst_frame: u8,
    pub burst_side: u8,
}

pub type JbProgressCallback = Option<unsafe extern "C" fn(current: usize, total: usize, user: *mut std::ffi::c_void) -> bool>;

struct ProgressTarget {
    callback: unsafe extern "C" fn(usize, usize, *mut std::ffi::c_void) -> bool,
    user: *mut std::ffi::c_void,
}

// SAFETY: the caller keeps `user` valid and callable from the export's worker threads
// until jb_frame_store_export_gif returns.
unsafe impl Send for ProgressTarget {}

unsafe fn hand_over_bytes(bytes: Vec<u8>, out: *mut *mut u8, out_len: *mut usize) {
    let mut boxed = bytes.into_boxed_slice();
    if let Some(len) = out_len.as_mut() {
        *len = boxed.len();
    }
    if let Some(ptr) = out.as_mut() {
        *ptr = boxed.as_mut_ptr();
        std::mem::forget(boxed);
    }
}

/// Frees bytes returned by this module.
#[no_mangle]
pub unsafe extern "C" fn jb_gif_bytes_free(ptr: *mut u8, len: usize) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)));
        }
    })
}

// ── FrameStore ──

#[no_mangle]
pub extern "C" fn jb_frame_store_new(width: u32, height: u32, fps: u32, jpeg_quality: i32, max_frames: usize, max_memory_bytes: usize) -> *mut JbFrameStore {
    guard(std::ptr::null_mut(), || {
        let config = RecordConfig { jpeg_quality, max_frames, max_memory_bytes };
        Box::into_raw(Box::new(JbFrameStore { inner: Arc::new(FrameStore::new(width, height, fps, config)) }))
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_free(ptr: *mut JbFrameStore) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_width(ptr: *const JbFrameStore) -> u32 {
    guard(0, || {
        ptr.as_ref().map_or(0, |s| s.inner.width())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_height(ptr: *const JbFrameStore) -> u32 {
    guard(0, || {
        ptr.as_ref().map_or(0, |s| s.inner.height())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_fps(ptr: *const JbFrameStore) -> u32 {
    guard(0, || {
        ptr.as_ref().map_or(0, |s| s.inner.fps())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_frame_count(ptr: *const JbFrameStore) -> usize {
    guard(0, || {
        ptr.as_ref().map_or(0, |s| s.inner.frame_count())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_memory_usage_bytes(ptr: *const JbFrameStore) -> u64 {
    guard(0, || {
        ptr.as_ref().map_or(0, |s| s.inner.memory_usage_bytes())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_dropped_frames(ptr: *const JbFrameStore) -> u64 {
    guard(0, || {
        ptr.as_ref().map_or(0, |s| s.inner.dropped_frames())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_total_duration_ms(ptr: *const JbFrameStore) -> u32 {
    guard(0, || {
        ptr.as_ref().map_or(0, |s| s.inner.total_duration_ms())
    })
}

/// Writes up to `capacity` timestamps (ms); returns how many frames there are.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_frame_timestamps(ptr: *const JbFrameStore, out: *mut u32, capacity: usize) -> usize {
    guard(0, || {
        let Some(store) = ptr.as_ref() else { return 0 };
        let timestamps = store.inner.frame_timestamps();
        if !out.is_null() {
            let count = capacity.min(timestamps.len());
            std::ptr::copy_nonoverlapping(timestamps.as_ptr(), out, count);
        }
        timestamps.len()
    })
}

/// False when the index is out of range.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_get_elapsed_ms(ptr: *const JbFrameStore, index: usize, out: *mut u32) -> bool {
    guard(false, || {
        match ptr.as_ref().and_then(|s| s.inner.get_elapsed_ms(index)) {
            Some(value) => {
                if let Some(out) = out.as_mut() {
                    *out = value;
                }
                true
            }
            None => false,
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_is_recording(ptr: *const JbFrameStore) -> bool {
    guard(false, || {
        ptr.as_ref().is_some_and(|s| s.inner.is_recording())
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_is_paused(ptr: *const JbFrameStore) -> bool {
    guard(false, || {
        ptr.as_ref().is_some_and(|s| s.inner.is_paused())
    })
}

/// 0 idle, 1 recording, 2 paused, 3 stopped; false for other values.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_set_state(ptr: *const JbFrameStore, state: u8) -> bool {
    guard(false, || {
        let Some(store) = ptr.as_ref() else { return false };
        let state = match state {
            0 => RecordState::Idle,
            1 => RecordState::Recording,
            2 => RecordState::Paused,
            3 => RecordState::Stopped,
            _ => return false,
        };
        store.inner.set_state(state);
        true
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_state(ptr: *const JbFrameStore) -> u8 {
    guard(0, || {
        match ptr.as_ref().map(|s| s.inner.state()) {
            Some(RecordState::Recording) => 1,
            Some(RecordState::Paused) => 2,
            Some(RecordState::Stopped) => 3,
            _ => 0,
        }
    })
}

/// 1 stored, 0 stored with an older frame dropped, -1 failure with the reason in `error`.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_push_bgra(ptr: *const JbFrameStore, bgra: *const u8, len: usize, elapsed_ms: u32, error: *mut c_char, error_len: usize) -> i32 {
    guard_error(-1, error, error_len, || {
        let Some(store) = ptr.as_ref() else { return -1 };
        let data: &[u8] = if bgra.is_null() { &[] } else { std::slice::from_raw_parts(bgra, len) };
        match store.inner.push_bgra(data, elapsed_ms) {
            Ok(kept) => i32::from(kept),
            Err(e) => {
                write_text(&e, error, error_len);
                -1
            }
        }
    })
}

/// As jb_frame_store_push_bgra, for RGB24 frames.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_push_rgb(ptr: *const JbFrameStore, rgb: *const u8, len: usize, elapsed_ms: u32, error: *mut c_char, error_len: usize) -> i32 {
    guard_error(-1, error, error_len, || {
        let Some(store) = ptr.as_ref() else { return -1 };
        let data: &[u8] = if rgb.is_null() { &[] } else { std::slice::from_raw_parts(rgb, len) };
        match store.inner.push_rgb(data, elapsed_ms) {
            Ok(kept) => i32::from(kept),
            Err(e) => {
                write_text(&e, error, error_len);
                -1
            }
        }
    })
}

/// Decodes one frame to RGB24 (0 for the original size); free the bytes with jb_gif_bytes_free.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_frame_store_get_frame_rgb(
    ptr: *const JbFrameStore,
    index: usize,
    display_w: u32,
    display_h: u32,
    out: *mut *mut u8,
    out_len: *mut usize,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let Some(store) = ptr.as_ref() else { return false };
        match store.inner.get_frame_rgb(index, display_w, display_h) {
            Ok(rgb) => {
                hand_over_bytes(rgb, out, out_len);
                true
            }
            Err(e) => {
                write_text(&e, error, error_len);
                false
            }
        }
    })
}

/// Starts a background decoder for streamed playback.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_start_decoder(ptr: *const JbFrameStore, display_w: u32, display_h: u32, prefetch: usize, start_frame: usize) -> *mut JbFrameDecoder {
    guard(std::ptr::null_mut(), || {
        let Some(store) = ptr.as_ref() else { return std::ptr::null_mut() };
        let decoder = FrameDecoder::start(store.inner.clone(), display_w, display_h, prefetch, start_frame);
        Box::into_raw(Box::new(JbFrameDecoder { inner: Some(decoder) }))
    })
}

unsafe fn sprite(source: &JbSprite) -> Sprite {
    let data = if source.data.is_null() { Vec::new() } else { std::slice::from_raw_parts(source.data, source.len).to_vec() };
    Sprite { data, w: source.w, h: source.h }
}

/// Exports a GIF; decoding and scaling run in parallel, encoding in order. progress
/// returning false cancels. sprites and infos (count entries) may be null.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_frame_store_export_gif(
    ptr: *const JbFrameStore,
    path: *const c_char,
    width: u32,
    height: u32,
    repeat: u16,
    frame_start: usize,
    frame_end: usize,
    progress: JbProgressCallback,
    progress_user: *mut std::ffi::c_void,
    sprites: *const JbCursorSprites,
    infos: *const JbCursorInfo,
    info_count: usize,
    speed: f32,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let (Some(store), Some(path)) = (ptr.as_ref(), str_arg(path)) else {
            write_text("invalid arguments", error, error_len);
            return false;
        };
        let cursor_sprites = sprites.as_ref().map(|s| CursorSprites {
            cursor: sprite(&s.cursor),
            burst_left: [sprite(&s.burst_left[0]), sprite(&s.burst_left[1]), sprite(&s.burst_left[2])],
            burst_right: [sprite(&s.burst_right[0]), sprite(&s.burst_right[1]), sprite(&s.burst_right[2])],
            scroll_up: sprite(&s.scroll_up),
            scroll_down: sprite(&s.scroll_down),
        });
        let cursor_infos = if infos.is_null() {
            None
        } else {
            Some(
                std::slice::from_raw_parts(infos, info_count)
                    .iter()
                    .map(|info| {
                        info.present.then_some(CursorInfo {
                            x: info.x,
                            y: info.y,
                            press: info.press,
                            scroll: info.scroll,
                            burst_frame: info.burst_frame,
                            burst_side: info.burst_side,
                        })
                    })
                    .collect(),
            )
        };
        let opts = GifExportOptions {
            path: path.to_owned(),
            width,
            height,
            repeat,
            frame_start,
            frame_end,
            cursor_sprites,
            cursor_infos,
            speed_multiplier: if speed > 0.0 { speed } else { 1.0 },
        };
        let progress: Option<gif_export::ProgressCallback> = progress.map(|callback| {
            let target = ProgressTarget { callback, user: progress_user };
            Box::new(move |current: usize, total: usize| {
                let target = &target;
                (target.callback)(current, total, target.user)
            }) as gif_export::ProgressCallback
        });
        match gif_export::export_gif(&store.inner, &opts, progress) {
            Ok(()) => true,
            Err(e) => {
                write_text(&e, error, error_len);
                false
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_cancel_export(ptr: *const JbFrameStore) {
    guard((), || {
        if let Some(store) = ptr.as_ref() {
            store.inner.set_cancel(true);
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_store_clear(ptr: *const JbFrameStore) {
    guard((), || {
        if let Some(store) = ptr.as_ref() {
            store.inner.clear();
        }
    })
}

// ── FrameDecoder ──

#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_free(ptr: *mut JbFrameDecoder) {
    guard((), || {
        if !ptr.is_null() {
            let mut decoder = Box::from_raw(ptr);
            if let Some(mut inner) = decoder.inner.take() {
                inner.stop();
            }
        }
    })
}

unsafe fn deliver_frame(frame: Option<gifrecorder::decoder::DecodedFrame>, out: *mut *mut u8, out_len: *mut usize, elapsed_ms: *mut u32) -> i32 {
    match frame {
        Some(frame) => {
            if let Some(elapsed) = elapsed_ms.as_mut() {
                *elapsed = frame.elapsed_ms;
            }
            hand_over_bytes(frame.rgb, out, out_len);
            1
        }
        None => 0,
    }
}

/// Blocks for the next RGB24 frame: 1 with the frame, 0 when all frames are taken,
/// -1 once stopped.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_next_frame(ptr: *mut JbFrameDecoder, out: *mut *mut u8, out_len: *mut usize, elapsed_ms: *mut u32) -> i32 {
    guard(-1, || {
        match ptr.as_mut().and_then(|d| d.inner.as_mut()) {
            Some(decoder) => deliver_frame(decoder.next_frame(), out, out_len, elapsed_ms),
            None => -1,
        }
    })
}

/// As jb_frame_decoder_next_frame without waiting: 0 when no frame is ready.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_try_next_frame(ptr: *mut JbFrameDecoder, out: *mut *mut u8, out_len: *mut usize, elapsed_ms: *mut u32) -> i32 {
    guard(-1, || {
        match ptr.as_mut().and_then(|d| d.inner.as_mut()) {
            Some(decoder) => deliver_frame(decoder.try_next_frame(), out, out_len, elapsed_ms),
            None => -1,
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_buffered_count(ptr: *const JbFrameDecoder) -> usize {
    guard(0, || {
        ptr.as_ref().and_then(|d| d.inner.as_ref()).map_or(0, FrameDecoder::buffered_count)
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_is_finished(ptr: *const JbFrameDecoder) -> bool {
    guard(true, || {
        ptr.as_ref().and_then(|d| d.inner.as_ref()).map_or(true, FrameDecoder::is_finished)
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_total_frames(ptr: *const JbFrameDecoder) -> usize {
    guard(0, || {
        ptr.as_ref().and_then(|d| d.inner.as_ref()).map_or(0, FrameDecoder::total_frames)
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_fetched_count(ptr: *const JbFrameDecoder) -> usize {
    guard(0, || {
        ptr.as_ref().and_then(|d| d.inner.as_ref()).map_or(0, FrameDecoder::fetched_count)
    })
}

/// Skips up to n frames without handing them out; returns how many were skipped.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_skip(ptr: *mut JbFrameDecoder, n: usize) -> usize {
    guard(0, || {
        let Some(decoder) = ptr.as_mut().and_then(|d| d.inner.as_mut()) else { return 0 };
        let mut count = 0usize;
        for _ in 0..n {
            if decoder.next_frame().is_none() {
                break;
            }
            count += 1;
        }
        count
    })
}

/// Stops the background decoding thread.
#[no_mangle]
pub unsafe extern "C" fn jb_frame_decoder_stop(ptr: *mut JbFrameDecoder) {
    guard((), || {
        if let Some(decoder) = ptr.as_mut() {
            if let Some(mut inner) = decoder.inner.take() {
                inner.stop();
            }
        }
    })
}

// ── RecordSession ──

/// Creates and starts a capture session; prefer_dxgi uses DXGI (correct colours on HDR
/// screens) with GDI as fallback. Null with the reason on failure.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_record_session_new(
    store: *const JbFrameStore,
    left: i32,
    top: i32,
    width: i32,
    height: i32,
    fps: u32,
    prefer_dxgi: bool,
    error: *mut c_char,
    error_len: usize,
) -> *mut JbRecordSession {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        let Some(store) = store.as_ref() else {
            write_text("invalid store", error, error_len);
            return std::ptr::null_mut();
        };
        match RecordSession::start(store.inner.clone(), left, top, width, height, fps, prefer_dxgi) {
            Ok(session) => Box::into_raw(Box::new(JbRecordSession { inner: Some(session), stopped_backend: None })),
            Err(e) => {
                write_text(&e, error, error_len);
                std::ptr::null_mut()
            }
        }
    })
}

/// Prepares a session: the DXGI session is built on the capture thread now; begin() starts capturing.
#[no_mangle]
pub extern "C" fn jb_record_session_prepare(prefer_dxgi: bool) -> *mut JbRecordSession {
    guard(std::ptr::null_mut(), || {
        Box::into_raw(Box::new(JbRecordSession { inner: Some(RecordSession::prepare(prefer_dxgi)), stopped_backend: None }))
    })
}

/// Starts capturing; only once.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_record_session_begin(
    ptr: *mut JbRecordSession,
    store: *const JbFrameStore,
    left: i32,
    top: i32,
    width: i32,
    height: i32,
    fps: u32,
    error: *mut c_char,
    error_len: usize,
) -> bool {
    guard_error(false, error, error_len, || {
        let (Some(session), Some(store)) = (ptr.as_mut().and_then(|s| s.inner.as_mut()), store.as_ref()) else {
            write_text("session already stopped", error, error_len);
            return false;
        };
        match session.begin(store.inner.clone(), left, top, width, height, fps) {
            Ok(()) => true,
            Err(e) => {
                write_text(&e, error, error_len);
                false
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_record_session_pause(ptr: *const JbRecordSession) -> bool {
    guard(false, || {
        match ptr.as_ref().and_then(|s| s.inner.as_ref()) {
            Some(session) => {
                session.pause();
                true
            }
            None => false,
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_record_session_resume(ptr: *const JbRecordSession) -> bool {
    guard(false, || {
        match ptr.as_ref().and_then(|s| s.inner.as_ref()) {
            Some(session) => {
                session.resume();
                true
            }
            None => false,
        }
    })
}

/// Stops and waits for the capture thread to exit.
#[no_mangle]
pub unsafe extern "C" fn jb_record_session_stop(ptr: *mut JbRecordSession) {
    guard((), || {
        if let Some(wrapper) = ptr.as_mut() {
            if let Some(mut session) = wrapper.inner.take() {
                session.stop();
                wrapper.stopped_backend = session.backend();
            }
        }
    })
}

/// The latest frame's capture path: 1 dxgi, 2 gdi, 0 before any frame. Kept after stopping.
#[no_mangle]
pub unsafe extern "C" fn jb_record_session_backend(ptr: *const JbRecordSession) -> i32 {
    guard(0, || {
        let Some(wrapper) = ptr.as_ref() else { return 0 };
        match wrapper.inner.as_ref().map_or(wrapper.stopped_backend, RecordSession::backend) {
            Some("dxgi") => 1,
            Some(_) => 2,
            None => 0,
        }
    })
}

/// 0 idle, 1 recording, 2 paused, 3 stopped.
#[no_mangle]
pub unsafe extern "C" fn jb_record_session_state(ptr: *const JbRecordSession) -> u8 {
    guard(3, || {
        ptr.as_ref().and_then(|s| s.inner.as_ref()).map_or(3, RecordSession::state)
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_record_session_is_stopped(ptr: *const JbRecordSession) -> bool {
    guard(true, || {
        ptr.as_ref().and_then(|s| s.inner.as_ref()).map_or(true, RecordSession::is_stopped)
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_record_session_free(ptr: *mut JbRecordSession) {
    guard((), || {
        if !ptr.is_null() {
            let mut wrapper = Box::from_raw(ptr);
            if let Some(mut session) = wrapper.inner.take() {
                session.stop();
            }
        }
    })
}
