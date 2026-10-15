//! video_recorder for C. Mirrors the reference Python binding: one Recorder per handle,
//! run once on the calling thread; control calls only set atomic state and may come
//! from any thread. Events reach the caller as a name and a JSON object.

use std::ffi::{c_char, c_void, CString};
use std::path::PathBuf;
use std::sync::Mutex;

use video_recorder::error::Failure;
use video_recorder::options::Options;
use video_recorder::recording;
use video_recorder::session::{ControlState, Event, Statistics};

use crate::panic::{guard, guard_with};
use crate::text::{str_arg, write_text};

pub struct JbVideoRecorder {
    // run takes the options once; recording holds no lock, controls only write atomics.
    options: Mutex<Option<Options>>,
    controls: ControlState,
}

/// Receives (event name, JSON data); returning false fails the recording as a callback error.
pub type JbVideoEventCallback = Option<unsafe extern "C" fn(name: *const c_char, json: *const c_char, user: *mut c_void) -> bool>;

#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_video_recorder_new(
    output: *const c_char,
    left: i32,
    top: i32,
    width: u32,
    height: u32,
    fps: u32,
    bitrate: u32,
    system_audio: bool,
    hardware: bool,
    cursor: bool,
    prefer_dxgi: bool,
) -> *mut JbVideoRecorder {
    guard(std::ptr::null_mut(), || {
        let output = PathBuf::from(str_arg(output).unwrap_or_default());
        let options = Options {
            output,
            x: left,
            y: top,
            width,
            height,
            fps,
            bitrate,
            audio: system_audio,
            hardware,
            cursor,
            prefer_dxgi,
            ..Options::default()
        };
        Box::into_raw(Box::new(JbVideoRecorder { options: Mutex::new(Some(options)), controls: ControlState::default() }))
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_video_recorder_free(ptr: *mut JbVideoRecorder) {
    guard((), || {
        if !ptr.is_null() {
            drop(Box::from_raw(ptr));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_video_recorder_pause(ptr: *const JbVideoRecorder) {
    guard((), || {
        if let Some(recorder) = ptr.as_ref() {
            recorder.controls.pause();
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_video_recorder_resume(ptr: *const JbVideoRecorder) {
    guard((), || {
        if let Some(recorder) = ptr.as_ref() {
            recorder.controls.resume();
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_video_recorder_stop(ptr: *const JbVideoRecorder) {
    guard((), || {
        if let Some(recorder) = ptr.as_ref() {
            recorder.controls.stop();
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_video_recorder_cancel(ptr: *const JbVideoRecorder) {
    guard((), || {
        if let Some(recorder) = ptr.as_ref() {
            recorder.controls.cancel();
        }
    })
}

/// Records until stopped or cancelled, calling `callback` for each event on this thread.
/// False with a stable error code and the raw message on failure; a callback that
/// returned false yields the code "callback", a panic "backend" with its message.
#[no_mangle]
pub unsafe extern "C" fn jb_video_recorder_run(
    ptr: *const JbVideoRecorder,
    callback: JbVideoEventCallback,
    user: *mut c_void,
    code: *mut c_char,
    code_len: usize,
    message: *mut c_char,
    message_len: usize,
) -> bool {
    guard_with(
        |text| {
            write_text("backend", code, code_len);
            write_text(text, message, message_len);
            false
        },
        || {
            let fail = |failure: Failure| {
                write_text(failure.code, code, code_len);
                write_text(&failure.message, message, message_len);
                false
            };
            let Some(recorder) = ptr.as_ref() else {
                return fail(Failure::new("state", "Recorder state is unavailable"));
            };
            let Some(callback) = callback else {
                return fail(Failure::new("arguments", "callback must be callable"));
            };
            let options = match recorder.options.lock() {
                Ok(mut pending) => match pending.take() {
                    Some(options) => options,
                    None => return fail(Failure::new("state", "Recorder.run can only be called once")),
                },
                Err(_) => return fail(Failure::new("state", "Recorder state is unavailable")),
            };
            let result = recording::record(&options, &recorder.controls, &mut |event| {
                let (name, json) = event_json(event);
                let name = CString::new(name).unwrap_or_default();
                let json = CString::new(json).unwrap_or_default();
                if callback(name.as_ptr(), json.as_ptr(), user) {
                    Ok(())
                } else {
                    Err(Failure::new("callback", "Recording callback failed"))
                }
            });
            match result {
                Ok(()) => true,
                Err(failure) => fail(failure),
            }
        },
    )
}

fn json_string(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn stats_fields(stats: Statistics) -> String {
    format!("\"frames\":{},\"elapsed_ms\":{},\"dropped\":{}", stats.frames, stats.elapsed_ms, stats.dropped)
}

/// The event as the reference binding's dict, serialized in the same key order.
fn event_json(event: Event) -> (&'static str, String) {
    match event {
        Event::Ready { encoder, hardware, encoder_identified, width, height, fps, bitrate, audio } => (
            "ready",
            format!(
                "{{\"encoder\":{},\"hardware\":{},\"encoder_identified\":{},\"width\":{},\"height\":{},\"fps\":{},\"bitrate\":{},\"audio\":{}}}",
                json_string(&encoder),
                hardware,
                encoder_identified,
                width,
                height,
                fps,
                bitrate,
                json_string(if audio { "system" } else { "none" })
            ),
        ),
        Event::Progress { statistics, queued_bytes, max_queued_bytes } => (
            "progress",
            format!("{{{},\"queued_bytes\":{},\"max_queued_bytes\":{}}}", stats_fields(statistics), queued_bytes, max_queued_bytes),
        ),
        Event::Paused(stats) => ("paused", format!("{{{}}}", stats_fields(stats))),
        Event::Resumed(stats) => ("resumed", format!("{{{}}}", stats_fields(stats))),
        Event::Cancelled(stats) => ("cancelled", format!("{{{}}}", stats_fields(stats))),
        Event::Complete { statistics, output, encoder, hardware, max_queued_bytes } => {
            let path = json_string(&output.to_string_lossy());
            (
                "complete",
                format!(
                    "{{{},\"output\":{},\"path\":{},\"encoder\":{},\"hardware\":{},\"max_queued_bytes\":{},\"finalized\":true}}",
                    stats_fields(statistics),
                    path,
                    path,
                    json_string(&encoder),
                    hardware,
                    max_queued_bytes
                ),
            )
        }
    }
}
