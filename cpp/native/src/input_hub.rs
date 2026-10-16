//! inputhub's Hub for C. Mirrors `inputhub::python::Hub`: one per process, hooks on
//! a native thread, events pulled through `jb_input_hub_next_event`.

use std::ffi::c_char;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Receiver;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use inputhub::engine::{Button, Engine as StateMachine, Event, Modifiers, Rect};
use inputhub::hooks::{HookThread, Shared};

use crate::panic::{guard, guard_error};
use crate::text::{plus_list, str_arg, str_array, write_text};

static HUB_ACTIVE: AtomicBool = AtomicBool::new(false);

pub const EVENT_GESTURE: i32 = 0;
pub const EVENT_MOVED: i32 = 1;
pub const EVENT_SIDE: i32 = 2;
pub const EVENT_HOTKEY: i32 = 3;
pub const EVENT_WHEEL: i32 = 4;
pub const EVENT_KEY: i32 = 5;
pub const EVENT_FOREGROUND: i32 = 6;
pub const EVENT_FAILURE: i32 = 7;
pub const EVENT_CAPTURED: i32 = 8;

pub const TEXT_LEN: usize = 512;

/// One hub event. `text` holds the gesture phase ("start"/"finish"/"cancel"), the
/// side button ("x1"/"x2"), the hotkey or watcher name, the "+"-joined modifiers of a
/// captured key, or the failure message. `value` is the wheel delta or the virtual key;
/// `flag` is horizontal or pressed.
#[repr(C)]
pub struct JbInputEvent {
    pub kind: i32,
    pub id: u64,
    pub x: i32,
    pub y: i32,
    pub value: i32,
    pub flag: bool,
    pub hwnd: isize,
    pub text: [c_char; TEXT_LEN],
}

pub struct JbInputHub {
    shared: Arc<Shared>,
    thread: Mutex<Option<HookThread>>,
    events: Mutex<Receiver<Event>>,
}

impl JbInputHub {
    fn with_engine<R>(&self, f: impl FnOnce(&mut StateMachine, &mut dyn FnMut(Event)) -> R) -> R {
        self.shared.with_engine(f)
    }

    fn stop(&self) {
        self.shared.close();
        let thread = self
            .thread
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take();
        if let Some(mut thread) = thread {
            thread.stop();
            HUB_ACTIVE.store(false, Ordering::Release);
        }
    }
}

unsafe fn hub<'a>(ptr: *const JbInputHub) -> Option<&'a JbInputHub> {
    ptr.as_ref()
}

fn fill_event(event: Event, out: &mut JbInputEvent) {
    out.id = 0;
    out.x = 0;
    out.y = 0;
    out.value = 0;
    out.flag = false;
    out.hwnd = 0;
    let text: String;
    match event {
        Event::GestureStart { id, x, y } => {
            (out.kind, out.id, out.x, out.y) = (EVENT_GESTURE, id, x, y);
            text = "start".into();
        }
        Event::GestureFinish { id, x, y } => {
            (out.kind, out.id, out.x, out.y) = (EVENT_GESTURE, id, x, y);
            text = "finish".into();
        }
        Event::GestureCancel { id, x, y } => {
            (out.kind, out.id, out.x, out.y) = (EVENT_GESTURE, id, x, y);
            text = "cancel".into();
        }
        Event::GestureMoved { id } => {
            (out.kind, out.id) = (EVENT_MOVED, id);
            text = String::new();
        }
        Event::SideButton { button } => {
            out.kind = EVENT_SIDE;
            text = button.name().into();
        }
        Event::Hotkey { name } => {
            out.kind = EVENT_HOTKEY;
            text = name.to_string();
        }
        Event::Captured { modifiers, vk } => {
            (out.kind, out.value) = (EVENT_CAPTURED, vk as i32);
            text = modifiers.names().join("+");
        }
        Event::Wheel { watcher, x, y, delta, horizontal } => {
            (out.kind, out.x, out.y, out.value, out.flag) = (EVENT_WHEEL, x, y, delta, horizontal);
            text = watcher.to_string();
        }
        Event::Key { watcher, vk, pressed } => {
            (out.kind, out.value, out.flag) = (EVENT_KEY, vk as i32, pressed);
            text = watcher.to_string();
        }
        Event::Foreground { hwnd } => {
            (out.kind, out.hwnd) = (EVENT_FOREGROUND, hwnd);
            text = String::new();
        }
        Event::Failure { message } => {
            out.kind = EVENT_FAILURE;
            text = message;
        }
    }
    unsafe { write_text(&text, out.text.as_mut_ptr(), TEXT_LEN) };
}

/// Unknown binding names invalidate the whole configuration, like a wrong modifier count.
fn parse_bindings(bindings: &[(Vec<String>, String)]) -> Option<Vec<(Modifiers, Button)>> {
    bindings
        .iter()
        .map(|(modifiers, button)| {
            Some((
                Modifiers::parse(modifiers.iter().map(String::as_str))?,
                Button::parse(button)?,
            ))
        })
        .collect()
}

/// Null on failure, with the reason written to `error`.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_new(error: *mut c_char, error_len: usize) -> *mut JbInputHub {
    guard_error(std::ptr::null_mut(), error, error_len, || {
        if HUB_ACTIVE.swap(true, Ordering::AcqRel) {
            write_text("only one inputhub.Hub may exist per process", error, error_len);
            return std::ptr::null_mut();
        }
        match HookThread::start() {
            Ok((thread, events)) => Box::into_raw(Box::new(JbInputHub {
                shared: thread.shared().clone(),
                thread: Mutex::new(Some(thread)),
                events: Mutex::new(events),
            })),
            Err(message) => {
                HUB_ACTIVE.store(false, Ordering::Release);
                write_text(&message, error, error_len);
                std::ptr::null_mut()
            }
        }
    })
}

/// Stops the hook thread if still running, then frees the hub.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_free(ptr: *mut JbInputHub) {
    guard((), || {
        if !ptr.is_null() {
            let hub = Box::from_raw(ptr);
            hub.stop();
        }
    })
}

/// Cancels any gesture, removes the hooks and ends the native thread; a pending
/// `next_event` returns once queued events are drained.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_close(ptr: *const JbInputHub) {
    guard((), || {
        if let Some(hub) = hub(ptr) {
            hub.stop();
        }
    })
}

/// Waits for the next event; `timeout_ms` < 0 waits until one arrives or the hub
/// closes. Returns false on timeout or once closed and drained.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_next_event(ptr: *const JbInputHub, timeout_ms: i64, out: *mut JbInputEvent) -> bool {
    guard(false, || {
        let (Some(hub), Some(out)) = (hub(ptr), out.as_mut()) else {
            return false;
        };
        let events = hub.events.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let received = if timeout_ms < 0 {
            events.recv().ok()
        } else {
            events.recv_timeout(Duration::from_millis(timeout_ms as u64)).ok()
        };
        match received {
            Some(event) => {
                fill_event(event, out);
                true
            }
            None => false,
        }
    })
}

/// Each binding: "+"-joined modifier names and a button name. Returns whether the configuration is valid.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_configure_gestures(
    ptr: *const JbInputHub,
    modifiers: *const *const c_char,
    buttons: *const *const c_char,
    count: usize,
    enabled: bool,
) -> bool {
    guard(false, || {
        let Some(hub) = hub(ptr) else { return false };
        let bindings = match (str_array(modifiers, count), str_array(buttons, count)) {
            (Some(modifiers), Some(buttons)) => modifiers
                .iter()
                .zip(buttons)
                .map(|(mods, button)| {
                    let mods = mods.split('+').map(str::trim).filter(|part| !part.is_empty()).map(str::to_owned).collect();
                    (mods, button)
                })
                .collect::<Vec<_>>(),
            _ => return false,
        };
        hub.with_engine(|engine, emit| match parse_bindings(&bindings) {
            Some(parsed) => engine.configure_gestures(parsed, enabled, emit),
            None => {
                engine.configure_gestures(Vec::new(), false, emit);
                false
            }
        })
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_set_blocked(ptr: *const JbInputHub, blocked: bool) {
    guard((), || {
        if let Some(hub) = hub(ptr) {
            hub.with_engine(|engine, emit| engine.set_blocked(blocked, emit));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_cancel_gesture(ptr: *const JbInputHub) {
    guard((), || {
        if let Some(hub) = hub(ptr) {
            hub.with_engine(|engine, emit| engine.cancel(emit));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_take_position(ptr: *const JbInputHub, gesture_id: u64, x: *mut i32, y: *mut i32) -> bool {
    guard(false, || {
        let Some(hub) = hub(ptr) else { return false };
        match hub.with_engine(|engine, _| engine.take_position(gesture_id)) {
            Some((px, py)) => {
                if let Some(x) = x.as_mut() {
                    *x = px;
                }
                if let Some(y) = y.as_mut() {
                    *y = py;
                }
                true
            }
            None => false,
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_dragging(ptr: *const JbInputHub) -> bool {
    guard(false, || {
        hub(ptr).is_some_and(|hub| hub.with_engine(|engine, _| engine.dragging()))
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_accepts(ptr: *const JbInputHub, gesture_id: u64) -> bool {
    guard(false, || {
        hub(ptr).is_some_and(|hub| hub.with_engine(|engine, _| engine.accepts(gesture_id)))
    })
}

/// Writes "+"-joined modifiers and the button name; false when the gesture has no binding.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_gesture_binding(
    ptr: *const JbInputHub,
    gesture_id: u64,
    modifiers: *mut c_char,
    modifiers_len: usize,
    button: *mut c_char,
    button_len: usize,
) -> bool {
    guard(false, || {
        let Some(hub) = hub(ptr) else { return false };
        match hub.with_engine(|engine, _| engine.gesture_binding(gesture_id)) {
            Some((mods, btn)) => {
                write_text(&mods.names().join("+"), modifiers, modifiers_len);
                write_text(btn.name(), button, button_len);
                true
            }
            None => false,
        }
    })
}

/// `suppressed` names side buttons ("x1"/"x2"); false when a name is not a side button.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_configure_side_buttons(
    ptr: *const JbInputHub,
    enabled: bool,
    suppressed: *const *const c_char,
    count: usize,
    capture_all: bool,
) -> bool {
    guard(false, || {
        let Some(hub) = hub(ptr) else { return false };
        let Some(names) = str_array(suppressed, count) else { return false };
        let mut buttons = Vec::with_capacity(names.len());
        for name in &names {
            match Button::parse(name) {
                Some(button @ (Button::X1 | Button::X2)) => buttons.push(button),
                _ => return false,
            }
        }
        hub.with_engine(|engine, _| engine.configure_side_buttons(enabled, &buttons, capture_all));
        true
    })
}

/// Swallows vk pressed with exactly these modifiers and reports ("hotkey", name) once.
/// Returns 1 when bound, 0 when refused (no modifier except for PrtSc, or vk is a modifier),
/// -1 for an unknown modifier.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_bind_hotkey(ptr: *const JbInputHub, name: *const c_char, modifiers: *const c_char, vk: u32) -> i32 {
    guard(-1, || {
        let (Some(hub), Some(name), Some(modifiers)) = (hub(ptr), str_arg(name), plus_list(modifiers)) else {
            return -1;
        };
        let Some(modifiers) = Modifiers::parse(modifiers.iter().map(String::as_str)) else {
            return -1;
        };
        i32::from(hub.with_engine(|engine, _| engine.bind_hotkey(name, modifiers, vk)))
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_unbind_hotkey(ptr: *const JbInputHub, name: *const c_char) {
    guard((), || {
        if let (Some(hub), Some(name)) = (hub(ptr), str_arg(name)) {
            hub.with_engine(|engine, _| engine.unbind_hotkey(name));
        }
    })
}

/// While recording a hotkey: keys pressed with Win held, and PrtSc, are swallowed and
/// reported as captured events.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_configure_key_capture(ptr: *const JbInputHub, enabled: bool) {
    guard((), || {
        if let Some(hub) = hub(ptr) {
            hub.with_engine(|engine, _| engine.configure_key_capture(enabled));
        }
    })
}

/// rect is (left, top, right, bottom) in physical pixels; has_rect false means the whole desktop.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_watch_wheel(
    ptr: *const JbInputHub,
    watcher: *const c_char,
    has_rect: bool,
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
) {
    guard((), || {
        if let (Some(hub), Some(watcher)) = (hub(ptr), str_arg(watcher)) {
            let rect = has_rect.then_some(Rect { left, top, right, bottom });
            hub.with_engine(|engine, _| engine.watch_wheel(watcher, rect));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_unwatch_wheel(ptr: *const JbInputHub, watcher: *const c_char) {
    guard((), || {
        if let (Some(hub), Some(watcher)) = (hub(ptr), str_arg(watcher)) {
            hub.with_engine(|engine, _| engine.unwatch_wheel(watcher));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_watch_keys(ptr: *const JbInputHub, watcher: *const c_char, keys: *const u32, count: usize) {
    guard((), || {
        if let (Some(hub), Some(watcher)) = (hub(ptr), str_arg(watcher)) {
            let keys = if count == 0 || keys.is_null() { Vec::new() } else { std::slice::from_raw_parts(keys, count).to_vec() };
            hub.with_engine(|engine, _| engine.watch_keys(watcher, keys));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_unwatch_keys(ptr: *const JbInputHub, watcher: *const c_char) {
    guard((), || {
        if let (Some(hub), Some(watcher)) = (hub(ptr), str_arg(watcher)) {
            hub.with_engine(|engine, _| engine.unwatch_keys(watcher));
        }
    })
}

/// Reports every foreground window change (including this process's windows) while watched.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_watch_foreground(ptr: *const JbInputHub, watcher: *const c_char) {
    guard((), || {
        if let (Some(hub), Some(watcher)) = (hub(ptr), str_arg(watcher)) {
            hub.with_engine(|engine, _| engine.watch_foreground(watcher));
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_unwatch_foreground(ptr: *const JbInputHub, watcher: *const c_char) {
    guard((), || {
        if let (Some(hub), Some(watcher)) = (hub(ptr), str_arg(watcher)) {
            hub.with_engine(|engine, _| engine.unwatch_foreground(watcher));
        }
    })
}

/// Whether the low-level hooks are installed; they install on the hook thread after a feature asks for them.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_hooks_installed(ptr: *const JbInputHub) -> bool {
    guard(false, || {
        hub(ptr).is_some_and(|hub| hub.shared.stats.hooks_installed.load(Ordering::Acquire))
    })
}

/// Tests only: simulated input carrying this extra info counts as real input; 0 turns it off.
#[no_mangle]
pub unsafe extern "C" fn jb_input_hub_set_test_marker(ptr: *const JbInputHub, marker: usize) {
    guard((), || {
        if let Some(hub) = hub(ptr) {
            hub.shared.set_test_marker(marker);
        }
    })
}
