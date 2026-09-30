//! Python 绑定。Hub 是正式入口，进程内只有一个；Engine 只驱动状态机、不装钩子，供测试逐条验证行为。
//!
//! 事件以元组交给 Python：
//!   ("gesture", "start" | "finish" | "cancel", id, x, y)
//!   ("moved", id)
//!   ("side", "x1" | "x2")
//!   ("hotkey", name)
//!   ("wheel", watcher, x, y, delta, horizontal)
//!   ("key", watcher, vk, pressed)
//!   ("foreground", hwnd)
//!   ("failure", message)

use std::collections::HashSet;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Receiver;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use pyo3::exceptions::{PyRuntimeError, PyValueError};
use pyo3::prelude::*;
use pyo3::types::PyDict;

use crate::engine::{
    Button, Engine as StateMachine, Event, KeyInput, Modifiers, MouseAction, MouseInput, Platform,
    Rect,
};
use crate::hooks::{HookThread, Shared};

static HUB_ACTIVE: AtomicBool = AtomicBool::new(false);

fn event_to_py(py: Python<'_>, event: Event) -> PyObject {
    match event {
        Event::GestureStart { id, x, y } => ("gesture", "start", id, x, y).into_py(py),
        Event::GestureFinish { id, x, y } => ("gesture", "finish", id, x, y).into_py(py),
        Event::GestureCancel { id, x, y } => ("gesture", "cancel", id, x, y).into_py(py),
        Event::GestureMoved { id } => ("moved", id).into_py(py),
        Event::SideButton { button } => ("side", button.name()).into_py(py),
        Event::Hotkey { name } => ("hotkey", &*name).into_py(py),
        Event::Wheel {
            watcher,
            x,
            y,
            delta,
            horizontal,
        } => ("wheel", &*watcher, x, y, delta, horizontal).into_py(py),
        Event::Key {
            watcher,
            vk,
            pressed,
        } => ("key", &*watcher, vk, pressed).into_py(py),
        Event::Foreground { hwnd } => ("foreground", hwnd).into_py(py),
        Event::Failure { message } => ("failure", message).into_py(py),
    }
}

/// 名字不认识的绑定让整份配置无效，和修饰键个数不对一样按不启用处理。
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

fn configure_gestures(
    engine: &mut StateMachine,
    bindings: &[(Vec<String>, String)],
    enabled: bool,
    emit: &mut dyn FnMut(Event),
) -> bool {
    match parse_bindings(bindings) {
        Some(parsed) => engine.configure_gestures(parsed, enabled, emit),
        None => {
            engine.configure_gestures(Vec::new(), false, emit);
            false
        }
    }
}

fn parse_side_buttons(names: &[String]) -> PyResult<Vec<Button>> {
    names
        .iter()
        .map(|name| match Button::parse(name) {
            Some(button @ (Button::X1 | Button::X2)) => Ok(button),
            _ => Err(PyValueError::new_err(format!("not a side button: {name}"))),
        })
        .collect()
}

fn parse_modifiers(names: &[String]) -> PyResult<Modifiers> {
    Modifiers::parse(names.iter().map(String::as_str))
        .ok_or_else(|| PyValueError::new_err(format!("unknown modifier in {names:?}")))
}

fn binding_to_py(
    binding: Option<(Modifiers, Button)>,
) -> Option<(Vec<&'static str>, &'static str)> {
    binding.map(|(modifiers, button)| (modifiers.names(), button.name()))
}

fn to_rect(rect: Option<(i32, i32, i32, i32)>) -> Option<Rect> {
    rect.map(|(left, top, right, bottom)| Rect {
        left,
        top,
        right,
        bottom,
    })
}

// ---------------------------------------------------------------------- Hub

/// 全局输入的唯一入口。
#[pyclass(module = "inputhub")]
pub struct Hub {
    shared: Arc<Shared>,
    thread: Mutex<Option<HookThread>>,
    events: Mutex<Receiver<Event>>,
}

impl Hub {
    fn with_engine<R>(&self, f: impl FnOnce(&mut StateMachine, &mut dyn FnMut(Event)) -> R) -> R {
        self.shared.with_engine(f)
    }
}

#[pymethods]
impl Hub {
    #[new]
    fn new() -> PyResult<Self> {
        if HUB_ACTIVE.swap(true, Ordering::AcqRel) {
            return Err(PyRuntimeError::new_err(
                "only one inputhub.Hub may exist per process",
            ));
        }
        match HookThread::start() {
            Ok((thread, events)) => Ok(Hub {
                shared: thread.shared().clone(),
                thread: Mutex::new(Some(thread)),
                events: Mutex::new(events),
            }),
            Err(message) => {
                HUB_ACTIVE.store(false, Ordering::Release);
                Err(PyRuntimeError::new_err(message))
            }
        }
    }

    /// bindings: [(修饰键名列表, 按键名)]；返回配置是否合法。
    fn configure_gestures(&self, bindings: Vec<(Vec<String>, String)>, enabled: bool) -> bool {
        self.with_engine(|engine, emit| configure_gestures(engine, &bindings, enabled, emit))
    }

    fn set_blocked(&self, blocked: bool) {
        self.with_engine(|engine, emit| engine.set_blocked(blocked, emit));
    }

    fn cancel_gesture(&self) {
        self.with_engine(|engine, emit| engine.cancel(emit));
    }

    fn take_position(&self, gesture_id: u64) -> Option<(i32, i32)> {
        self.with_engine(|engine, _| engine.take_position(gesture_id))
    }

    #[getter]
    fn dragging(&self) -> bool {
        self.with_engine(|engine, _| engine.dragging())
    }

    fn accepts(&self, gesture_id: u64) -> bool {
        self.with_engine(|engine, _| engine.accepts(gesture_id))
    }

    fn gesture_binding(&self, gesture_id: u64) -> Option<(Vec<&'static str>, &'static str)> {
        binding_to_py(self.with_engine(|engine, _| engine.gesture_binding(gesture_id)))
    }

    #[pyo3(signature = (enabled, suppressed, capture_all=false))]
    fn configure_side_buttons(
        &self,
        enabled: bool,
        suppressed: Vec<String>,
        capture_all: bool,
    ) -> PyResult<()> {
        let suppressed = parse_side_buttons(&suppressed)?;
        self.with_engine(|engine, _| {
            engine.configure_side_buttons(enabled, &suppressed, capture_all)
        });
        Ok(())
    }

    /// 按住的修饰键与 modifiers 完全一致时按下 vk：吞掉这个键的按下、自动重复和抬起，报告一次
    /// ("hotkey", name)。同名绑定会被替换；返回是否绑定成功（至少一个修饰键、vk 不是修饰键）。
    fn bind_hotkey(&self, name: &str, modifiers: Vec<String>, vk: u32) -> PyResult<bool> {
        let modifiers = parse_modifiers(&modifiers)?;
        Ok(self.with_engine(|engine, _| engine.bind_hotkey(name, modifiers, vk)))
    }

    fn unbind_hotkey(&self, name: &str) {
        self.with_engine(|engine, _| engine.unbind_hotkey(name));
    }

    /// rect 为 (left, top, right, bottom) 物理像素；None 表示整个桌面。
    #[pyo3(signature = (watcher, rect=None))]
    fn watch_wheel(&self, watcher: &str, rect: Option<(i32, i32, i32, i32)>) {
        self.with_engine(|engine, _| engine.watch_wheel(watcher, to_rect(rect)));
    }

    fn unwatch_wheel(&self, watcher: &str) {
        self.with_engine(|engine, _| engine.unwatch_wheel(watcher));
    }

    fn watch_keys(&self, watcher: &str, keys: Vec<u32>) {
        self.with_engine(|engine, _| engine.watch_keys(watcher, keys));
    }

    fn unwatch_keys(&self, watcher: &str) {
        self.with_engine(|engine, _| engine.unwatch_keys(watcher));
    }

    /// 有订阅时报告每一次前台窗口切换（含本进程的窗口），不做筛选。
    fn watch_foreground(&self, watcher: &str) {
        self.with_engine(|engine, _| engine.watch_foreground(watcher));
    }

    fn unwatch_foreground(&self, watcher: &str) {
        self.with_engine(|engine, _| engine.unwatch_foreground(watcher));
    }

    /// 等下一个事件，等待期间释放 GIL。timeout_ms 为 None 时一直等到有事件或 Hub 关闭。
    /// 超时或已关闭返回 None；关闭前排队的事件仍会先取完。
    #[pyo3(signature = (timeout_ms=None))]
    fn next_event(&self, py: Python<'_>, timeout_ms: Option<u64>) -> Option<PyObject> {
        let received = py.allow_threads(|| {
            let events = self
                .events
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            match timeout_ms {
                Some(ms) => events.recv_timeout(Duration::from_millis(ms)).ok(),
                None => events.recv().ok(),
            }
        });
        received.map(|event| event_to_py(py, event))
    }

    /// 仅供测试：附加信息等于 marker 的模拟输入按真实输入处理；0 关闭。
    fn set_test_marker(&self, marker: usize) {
        self.shared.set_test_marker(marker);
    }

    fn stats<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyDict>> {
        let stats = &self.shared.stats;
        let dict = PyDict::new_bound(py);
        dict.set_item("mouse_events", stats.mouse_events.load(Ordering::Relaxed))?;
        dict.set_item("key_events", stats.key_events.load(Ordering::Relaxed))?;
        dict.set_item(
            "max_handle_us",
            stats.max_handle_ns.load(Ordering::Relaxed) as f64 / 1000.0,
        )?;
        dict.set_item(
            "hooks_installed",
            stats.hooks_installed.load(Ordering::Acquire),
        )?;
        dict.set_item(
            "foreground_hook_installed",
            stats.foreground_hook_installed.load(Ordering::Acquire),
        )?;
        Ok(dict)
    }

    /// 取消进行中的手势、卸掉钩子并结束原生线程；之后不再产生事件，正在等事件的调用随即返回。
    fn close(&self, py: Python<'_>) {
        self.shared.close();
        let thread = self
            .thread
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take();
        if let Some(mut thread) = thread {
            py.allow_threads(|| thread.stop());
            HUB_ACTIVE.store(false, Ordering::Release);
        }
    }
}

impl Drop for Hub {
    fn drop(&mut self) {
        let thread = self
            .thread
            .get_mut()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .take();
        if let Some(mut thread) = thread {
            self.shared.close();
            thread.stop();
            HUB_ACTIVE.store(false, Ordering::Release);
        }
    }
}

// ---------------------------------------------------------------------- Engine（测试用）

#[derive(Default)]
struct FakePlatform {
    held: HashSet<u32>,
    masks: u32,
    mask_error: Option<String>,
}

impl Platform for FakePlatform {
    fn key_held(&self, vk: u32) -> bool {
        self.held.contains(&vk)
    }

    fn mask_menu(&mut self) -> Result<(), String> {
        self.masks += 1;
        self.mask_error.clone().map_or(Ok(()), Err)
    }
}

/// 不装钩子、直接驱动状态机；按住的键由测试指定。每个方法返回这一步产生的事件。
/// 可以从别的线程调用，测试借此模拟钩子线程上的输入。
#[pyclass(module = "inputhub")]
pub struct Engine {
    engine: StateMachine,
    platform: FakePlatform,
}

impl Engine {
    fn run<R>(
        &mut self,
        py: Python<'_>,
        f: impl FnOnce(&mut StateMachine, &mut FakePlatform, &mut dyn FnMut(Event)) -> R,
    ) -> (R, Vec<PyObject>) {
        let mut events = Vec::new();
        let result = f(&mut self.engine, &mut self.platform, &mut |event| {
            events.push(event)
        });
        (
            result,
            events
                .into_iter()
                .map(|event| event_to_py(py, event))
                .collect(),
        )
    }
}

#[pymethods]
impl Engine {
    #[new]
    fn new() -> Self {
        Engine {
            engine: StateMachine::new(),
            platform: FakePlatform::default(),
        }
    }

    fn set_held(&mut self, keys: Vec<u32>) {
        self.platform.held = keys.into_iter().collect();
    }

    #[getter]
    fn mask_calls(&self) -> u32 {
        self.platform.masks
    }

    #[pyo3(signature = (message=None))]
    fn set_mask_error(&mut self, message: Option<String>) {
        self.platform.mask_error = message;
    }

    /// action: move / down / up / wheel / hwheel；down、up 需给 button。
    #[allow(clippy::too_many_arguments)]
    #[pyo3(signature = (action, x=0, y=0, button=None, delta=0, injected=false))]
    fn mouse(
        &mut self,
        py: Python<'_>,
        action: &str,
        x: i32,
        y: i32,
        button: Option<&str>,
        delta: i32,
        injected: bool,
    ) -> PyResult<(bool, Vec<PyObject>)> {
        let button = || {
            button
                .and_then(Button::parse)
                .ok_or_else(|| PyValueError::new_err("down/up need a valid button"))
        };
        let action = match action {
            "move" => MouseAction::Move,
            "down" => MouseAction::Down(button()?),
            "up" => MouseAction::Up(button()?),
            "wheel" => MouseAction::Wheel {
                delta,
                horizontal: false,
            },
            "hwheel" => MouseAction::Wheel {
                delta,
                horizontal: true,
            },
            other => {
                return Err(PyValueError::new_err(format!(
                    "unknown mouse action: {other}"
                )))
            }
        };
        let input = MouseInput {
            action,
            x,
            y,
            injected,
        };
        Ok(self.run(py, |engine, platform, emit| {
            engine.on_mouse(input, platform, emit)
        }))
    }

    /// 和真实钩子一样，先交给状态机，再更新按住的键。
    #[pyo3(signature = (vk, pressed, injected=false))]
    fn key(
        &mut self,
        py: Python<'_>,
        vk: u32,
        pressed: bool,
        injected: bool,
    ) -> (bool, Vec<PyObject>) {
        let input = KeyInput {
            vk,
            pressed,
            injected,
        };
        let result = self.run(py, |engine, platform, emit| {
            engine.on_key(input, platform, emit)
        });
        if pressed {
            self.platform.held.insert(vk);
        } else {
            self.platform.held.remove(&vk);
        }
        result
    }

    fn configure_gestures(
        &mut self,
        py: Python<'_>,
        bindings: Vec<(Vec<String>, String)>,
        enabled: bool,
    ) -> (bool, Vec<PyObject>) {
        self.run(py, |engine, _, emit| {
            configure_gestures(engine, &bindings, enabled, emit)
        })
    }

    fn set_blocked(&mut self, py: Python<'_>, blocked: bool) -> Vec<PyObject> {
        self.run(py, |engine, _, emit| engine.set_blocked(blocked, emit))
            .1
    }

    fn cancel(&mut self, py: Python<'_>) -> Vec<PyObject> {
        self.run(py, |engine, _, emit| engine.cancel(emit)).1
    }

    fn take_position(&mut self, gesture_id: u64) -> Option<(i32, i32)> {
        self.engine.take_position(gesture_id)
    }

    #[getter]
    fn dragging(&self) -> bool {
        self.engine.dragging()
    }

    fn accepts(&self, gesture_id: u64) -> bool {
        self.engine.accepts(gesture_id)
    }

    fn gesture_binding(&self, gesture_id: u64) -> Option<(Vec<&'static str>, &'static str)> {
        binding_to_py(self.engine.gesture_binding(gesture_id))
    }

    #[pyo3(signature = (enabled, suppressed, capture_all=false))]
    fn configure_side_buttons(
        &mut self,
        enabled: bool,
        suppressed: Vec<String>,
        capture_all: bool,
    ) -> PyResult<()> {
        let suppressed = parse_side_buttons(&suppressed)?;
        self.engine
            .configure_side_buttons(enabled, &suppressed, capture_all);
        Ok(())
    }

    fn bind_hotkey(&mut self, name: &str, modifiers: Vec<String>, vk: u32) -> PyResult<bool> {
        let modifiers = parse_modifiers(&modifiers)?;
        Ok(self.engine.bind_hotkey(name, modifiers, vk))
    }

    fn unbind_hotkey(&mut self, name: &str) {
        self.engine.unbind_hotkey(name);
    }

    #[pyo3(signature = (watcher, rect=None))]
    fn watch_wheel(&mut self, watcher: &str, rect: Option<(i32, i32, i32, i32)>) {
        self.engine.watch_wheel(watcher, to_rect(rect));
    }

    fn unwatch_wheel(&mut self, watcher: &str) {
        self.engine.unwatch_wheel(watcher);
    }

    fn watch_keys(&mut self, watcher: &str, keys: Vec<u32>) {
        self.engine.watch_keys(watcher, keys);
    }

    fn unwatch_keys(&mut self, watcher: &str) {
        self.engine.unwatch_keys(watcher);
    }

    fn watch_foreground(&mut self, watcher: &str) {
        self.engine.watch_foreground(watcher);
    }

    fn unwatch_foreground(&mut self, watcher: &str) {
        self.engine.unwatch_foreground(watcher);
    }

    /// 模拟一次前台窗口切换。
    fn foreground(&mut self, py: Python<'_>, hwnd: isize) -> Vec<PyObject> {
        self.run(py, |engine, _, emit| engine.on_foreground(hwnd, emit))
            .1
    }

    #[getter]
    fn foreground_needed(&self) -> bool {
        self.engine.foreground_needed()
    }

    #[getter]
    fn hooks_needed(&self) -> bool {
        self.engine.hooks_needed()
    }

    fn fail(&mut self, py: Python<'_>, message: String) -> Vec<PyObject> {
        self.run(py, |engine, _, emit| engine.fail(message, emit)).1
    }

    fn close(&mut self, py: Python<'_>) -> Vec<PyObject> {
        self.run(py, |engine, _, emit| engine.close(emit)).1
    }
}

#[pymodule]
fn inputhub(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<Hub>()?;
    m.add_class::<Engine>()?;
    Ok(())
}
