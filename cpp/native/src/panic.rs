//! Panics stop at the C boundary: every export runs its body through one of the guards
//! and returns its failure value instead, as the Python bindings raise an exception
//! instead of aborting. The report goes to the handler C++ registers.

use std::ffi::{c_char, CString};
use std::panic::{self, AssertUnwindSafe};
use std::sync::{Mutex, Once};

use crate::text::write_text;

/// Receives "thread '<name>' panicked at <file>:<line>:\n<message>".
pub type JbPanicHandler = Option<unsafe extern "C" fn(report: *const c_char)>;

static HANDLER: Mutex<JbPanicHandler> = Mutex::new(None);
static HOOK: Once = Once::new();

/// Routes panic reports to `handler`; null restores the default report on stderr.
#[no_mangle]
pub extern "C" fn jb_set_panic_handler(handler: JbPanicHandler) {
    guard((), || {
        if let Ok(mut current) = HANDLER.lock() {
            *current = handler;
        }
        HOOK.call_once(|| {
            let default_hook = panic::take_hook();
            panic::set_hook(Box::new(move |info| {
                // The hook must not panic itself: that aborts the process.
                let handler = HANDLER.lock().map(|current| *current).unwrap_or(None);
                let Some(handler) = handler else {
                    default_hook(info);
                    return;
                };
                let thread = std::thread::current();
                let location = info.location().map(|l| format!("{}:{}:{}", l.file(), l.line(), l.column())).unwrap_or_default();
                let report = format!(
                    "thread '{}' panicked at {location}:\n{}",
                    thread.name().unwrap_or("<unnamed>"),
                    payload_text(info.payload())
                );
                let report = CString::new(report.replace('\0', " ")).unwrap_or_default();
                unsafe { handler(report.as_ptr()) };
            }));
        });
    })
}

fn payload_text(payload: &(dyn std::any::Any + Send)) -> &str {
    payload
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| payload.downcast_ref::<String>().map(String::as_str))
        .unwrap_or("Box<dyn Any>")
}

/// Runs an export's body; a panic returns `failure`.
pub fn guard<R>(failure: R, body: impl FnOnce() -> R) -> R {
    guard_with(|_| failure, body)
}

/// Like `guard`, also writing the panic message into the export's error buffer.
pub unsafe fn guard_error<R>(failure: R, error: *mut c_char, error_len: usize, body: impl FnOnce() -> R) -> R {
    guard_with(
        |message| {
            write_text(message, error, error_len);
            failure
        },
        body,
    )
}

/// Runs an export's body; a panic's message goes to `on_panic`, which gives the result.
pub fn guard_with<R>(on_panic: impl FnOnce(&str) -> R, body: impl FnOnce() -> R) -> R {
    match panic::catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(payload) => on_panic(payload_text(&*payload)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_panic_returns_the_failure_value() {
        assert_eq!(guard(-1, || -> i32 { panic!("boom") }), -1);
        assert_eq!(guard(-1, || 7), 7);
    }

    #[test]
    fn the_message_reaches_the_error_buffer() {
        let mut buffer = [0 as c_char; 32];
        let result = unsafe { guard_error(false, buffer.as_mut_ptr(), buffer.len(), || -> bool { panic!("bad {}", 42) }) };
        assert!(!result);
        let text = unsafe { std::ffi::CStr::from_ptr(buffer.as_ptr()) };
        assert_eq!(text.to_str().unwrap(), "bad 42");
    }

    #[test]
    fn the_handler_receives_the_report() {
        // Other tests panic in parallel while the handler is set.
        static REPORTS: Mutex<Vec<String>> = Mutex::new(Vec::new());
        unsafe extern "C" fn record(report: *const c_char) {
            REPORTS.lock().unwrap().push(std::ffi::CStr::from_ptr(report).to_string_lossy().into_owned());
        }
        jb_set_panic_handler(Some(record));
        std::thread::Builder::new()
            .name("probe".into())
            .spawn(|| guard((), || panic!("handled")))
            .unwrap()
            .join()
            .unwrap();
        jb_set_panic_handler(None);
        let reports = REPORTS.lock().unwrap().clone();
        assert!(
            reports.iter().any(|r| r.starts_with("thread 'probe' panicked at ") && r.ends_with(":\nhandled")),
            "{reports:?}"
        );
    }

    /// An export without a guard would let a panic abort the whole process.
    #[test]
    fn every_export_runs_through_a_guard() {
        let src = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
        let mut checked = 0;
        for entry in std::fs::read_dir(src).unwrap() {
            let path = entry.unwrap().path();
            let text = std::fs::read_to_string(&path).unwrap();
            for (at, _) in text.match_indices("extern \"C\" fn jb_") {
                let body = &text[at + text[at..].find('{').unwrap() + 1..];
                let first = body.trim_start();
                assert!(
                    ["guard(", "guard_error(", "guard_with("].iter().any(|g| first.starts_with(g)),
                    "{} has an export without a guard: {}",
                    path.display(),
                    &text[at..at + text[at..].find('(').unwrap()]
                );
                checked += 1;
            }
        }
        assert!(checked > 100, "only {checked} exports found");
    }
}
