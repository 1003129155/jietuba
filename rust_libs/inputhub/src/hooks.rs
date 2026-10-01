//! Win32 胶水：原生线程、低层鼠标键盘钩子、前台窗口事件。
//!
//! 钩子回调只把系统结构转成状态机输入，当场返回吞或不吞；要交给界面的事件放进通道。
//! 钩子都按需安装：状态机不需要时整个卸掉，不给系统里每个输入事件多绕一趟。

use std::cell::{Cell, RefCell};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr::null_mut;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::Instant;

use windows_sys::Win32::Foundation::{GetLastError, HWND, LPARAM, LRESULT, WPARAM};
use windows_sys::Win32::System::Threading::GetCurrentThreadId;
use windows_sys::Win32::UI::Accessibility::{SetWinEventHook, UnhookWinEvent, HWINEVENTHOOK};
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    GetAsyncKeyState, SendInput, INPUT, INPUT_0, INPUT_KEYBOARD, KEYBDINPUT, KEYEVENTF_KEYUP,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    CallNextHookEx, GetMessageW, PeekMessageW, PostThreadMessageW, SetWindowsHookExW,
    UnhookWindowsHookEx, EVENT_SYSTEM_FOREGROUND, HHOOK, KBDLLHOOKSTRUCT, LLKHF_INJECTED,
    LLKHF_LOWER_IL_INJECTED, LLMHF_INJECTED, LLMHF_LOWER_IL_INJECTED, MSG, MSLLHOOKSTRUCT,
    PM_NOREMOVE, WH_KEYBOARD_LL, WH_MOUSE_LL, WINEVENT_OUTOFCONTEXT, WM_APP, WM_KEYDOWN, WM_KEYUP,
    WM_LBUTTONDOWN, WM_LBUTTONUP, WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MOUSEHWHEEL, WM_MOUSEMOVE,
    WM_MOUSEWHEEL, WM_QUIT, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_SYSKEYDOWN, WM_SYSKEYUP,
    WM_XBUTTONDOWN, WM_XBUTTONUP,
};

use crate::engine::{Button, Engine, Event, KeyInput, MouseAction, MouseInput, Platform};

const WM_APP_SYNC: u32 = WM_APP + 1;

#[derive(Default)]
pub struct Stats {
    pub mouse_events: AtomicU64,
    pub key_events: AtomicU64,
    /// 单次钩子处理的最长耗时
    pub max_handle_ns: AtomicU64,
    pub hooks_installed: AtomicBool,
    pub foreground_hook_installed: AtomicBool,
}

struct Guarded {
    engine: Engine,
    /// 关闭时取走，等事件的一方随即返回
    tx: Option<Sender<Event>>,
}

pub struct Shared {
    state: Mutex<Guarded>,
    test_marker: AtomicUsize,
    thread_id: AtomicU32,
    pub stats: Stats,
}

impl Shared {
    /// 锁住状态机执行 f，事件送进通道；状态机要求重新评估钩子时通知钩子线程。
    pub fn with_engine<R>(&self, f: impl FnOnce(&mut Engine, &mut dyn FnMut(Event)) -> R) -> R {
        let mut guard = self.lock();
        let Guarded { engine, tx } = &mut *guard;
        let result = f(engine, &mut |event| {
            if let Some(tx) = tx {
                let _ = tx.send(event);
            }
        });
        let sync = engine.take_sync_request();
        drop(guard);
        if sync {
            self.request_sync();
        }
        result
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, Guarded> {
        self.state
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    /// 关闭状态机并断开事件通道：已排队的事件照常取完，之后等事件的调用立即返回。
    pub fn close(&self) {
        self.with_engine(|engine, emit| engine.close(emit));
        self.lock().tx = None;
    }

    fn request_sync(&self) {
        let thread_id = self.thread_id.load(Ordering::Acquire);
        if thread_id != 0 {
            unsafe { PostThreadMessageW(thread_id, WM_APP_SYNC, 0, 0) };
        }
    }

    /// 0 表示关闭。非 0 时，附加信息等于它的模拟输入按真实输入处理，供自动化测试驱动钩子。
    pub fn set_test_marker(&self, marker: usize) {
        self.test_marker.store(marker, Ordering::Release);
    }

    fn counts_as_real(&self, injected: bool, extra_info: usize) -> bool {
        let marker = self.test_marker.load(Ordering::Acquire);
        !injected || (marker != 0 && extra_info == marker)
    }

    fn handle(
        &self,
        run: impl FnOnce(&mut Engine, &mut dyn Platform, &mut dyn FnMut(Event)) -> bool,
    ) -> bool {
        // 系统可能在本线程的系统调用里重入钩子回调；外层还拿着状态机的锁，重入时直接放行，
        // 不能等锁，否则整个系统的输入都会卡住
        if IN_ENGINE.get() {
            return false;
        }
        let start = Instant::now();
        let mut platform = Win32Platform::default();
        IN_ENGINE.set(true);
        let suppress = self.with_engine(|engine, emit| {
            match catch_unwind(AssertUnwindSafe(|| run(engine, &mut platform, emit))) {
                Ok(suppress) => suppress,
                Err(_) => {
                    engine.fail("input hook panicked".into(), emit);
                    false
                }
            }
        });
        IN_ENGINE.set(false);
        if platform.mask_requested {
            // 解锁后、放行这次抬起之前发送：SendInput 会重入钩子回调
            if let Err(message) = send_menu_mask() {
                self.with_engine(|_, emit| emit(Event::Failure { message }));
            }
        }
        let elapsed = start.elapsed().as_nanos() as u64;
        self.stats
            .max_handle_ns
            .fetch_max(elapsed, Ordering::Relaxed);
        suppress
    }

    fn handle_mouse(&self, message: u32, data: &MSLLHOOKSTRUCT) -> bool {
        let high_word = (data.mouseData >> 16) as u16;
        let action = match message {
            WM_MOUSEMOVE => MouseAction::Move,
            WM_LBUTTONDOWN => MouseAction::Down(Button::Left),
            WM_LBUTTONUP => MouseAction::Up(Button::Left),
            WM_RBUTTONDOWN => MouseAction::Down(Button::Right),
            WM_RBUTTONUP => MouseAction::Up(Button::Right),
            WM_MBUTTONDOWN => MouseAction::Down(Button::Middle),
            WM_MBUTTONUP => MouseAction::Up(Button::Middle),
            WM_XBUTTONDOWN | WM_XBUTTONUP => {
                // 高位字区分两个侧键：XBUTTON1 = 1，XBUTTON2 = 2
                let button = match high_word {
                    1 => Button::X1,
                    2 => Button::X2,
                    _ => return false,
                };
                if message == WM_XBUTTONDOWN {
                    MouseAction::Down(button)
                } else {
                    MouseAction::Up(button)
                }
            }
            WM_MOUSEWHEEL => MouseAction::Wheel {
                delta: high_word as i16 as i32,
                horizontal: false,
            },
            WM_MOUSEHWHEEL => MouseAction::Wheel {
                delta: high_word as i16 as i32,
                horizontal: true,
            },
            _ => return false,
        };
        self.stats.mouse_events.fetch_add(1, Ordering::Relaxed);
        let injected = data.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED) != 0;
        let input = MouseInput {
            action,
            x: data.pt.x,
            y: data.pt.y,
            injected: !self.counts_as_real(injected, data.dwExtraInfo),
        };
        self.handle(|engine, platform, emit| engine.on_mouse(input, platform, emit))
    }

    fn handle_key(&self, message: u32, data: &KBDLLHOOKSTRUCT) -> bool {
        let pressed = match message {
            WM_KEYDOWN | WM_SYSKEYDOWN => true,
            WM_KEYUP | WM_SYSKEYUP => false,
            _ => return false,
        };
        self.stats.key_events.fetch_add(1, Ordering::Relaxed);
        let injected = data.flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED) != 0;
        let input = KeyInput {
            vk: data.vkCode,
            pressed,
            injected: !self.counts_as_real(injected, data.dwExtraInfo),
        };
        self.handle(|engine, platform, emit| engine.on_key(input, platform, emit))
    }
}

#[derive(Default)]
struct Win32Platform {
    mask_requested: bool,
}

impl Platform for Win32Platform {
    fn key_held(&self, vk: u32) -> bool {
        unsafe { GetAsyncKeyState(vk as i32) as u16 & 0x8000 != 0 }
    }

    /// 只记下请求，由 handle 在释放状态机的锁之后发送。
    fn mask_menu(&mut self) -> Result<(), String> {
        self.mask_requested = true;
        Ok(())
    }
}

fn send_menu_mask() -> Result<(), String> {
    // 0xE8 未分配：成对的按下、抬起让随后松开的 Win/Alt 不弹出菜单，又不吞掉真实的松开
    let key = |flags| INPUT {
        r#type: INPUT_KEYBOARD,
        Anonymous: INPUT_0 {
            ki: KEYBDINPUT {
                wVk: 0xE8,
                wScan: 0,
                dwFlags: flags,
                time: 0,
                dwExtraInfo: 0,
            },
        },
    };
    let inputs = [key(0), key(KEYEVENTF_KEYUP)];
    let sent = unsafe { SendInput(2, inputs.as_ptr(), std::mem::size_of::<INPUT>() as i32) };
    if sent == 2 {
        Ok(())
    } else {
        Err(format!(
            "Could not mask the modifier menu (error {})",
            unsafe { GetLastError() }
        ))
    }
}

thread_local! {
    /// 钩子回调都在钩子线程上运行，从这里取共享状态，不经过全局锁
    static CURRENT: RefCell<Option<Arc<Shared>>> = const { RefCell::new(None) };
    /// 本线程正拿着状态机的锁处理一个钩子事件
    static IN_ENGINE: Cell<bool> = const { Cell::new(false) };
}

fn with_current<R>(f: impl FnOnce(&Shared) -> R) -> Option<R> {
    CURRENT.with(|current| current.borrow().as_deref().map(f))
}

unsafe extern "system" fn mouse_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    if code >= 0 {
        let data = &*(lparam as *const MSLLHOOKSTRUCT);
        if with_current(|shared| shared.handle_mouse(wparam as u32, data)).unwrap_or(false) {
            return 1;
        }
    }
    CallNextHookEx(null_mut(), code, wparam, lparam)
}

unsafe extern "system" fn keyboard_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    if code >= 0 {
        let data = &*(lparam as *const KBDLLHOOKSTRUCT);
        if with_current(|shared| shared.handle_key(wparam as u32, data)).unwrap_or(false) {
            return 1;
        }
    }
    CallNextHookEx(null_mut(), code, wparam, lparam)
}

unsafe extern "system" fn foreground_proc(
    _hook: HWINEVENTHOOK,
    event: u32,
    hwnd: HWND,
    _object: i32,
    _child: i32,
    _thread: u32,
    _time: u32,
) {
    // 拿着状态机的锁时不会分发消息，这里只是防止将来改动引入重入
    if event == EVENT_SYSTEM_FOREGROUND && !IN_ENGINE.get() {
        with_current(|shared| {
            shared.with_engine(|engine, emit| engine.on_foreground(hwnd as isize, emit))
        });
    }
}

/// 原生输入线程的句柄；drop 时退出消息循环、卸掉全部钩子。
pub struct HookThread {
    shared: Arc<Shared>,
    thread: Option<JoinHandle<()>>,
}

impl HookThread {
    pub fn start() -> Result<(HookThread, Receiver<Event>), String> {
        let (tx, rx) = channel();
        let shared = Arc::new(Shared {
            state: Mutex::new(Guarded {
                engine: Engine::new(),
                tx: Some(tx),
            }),
            test_marker: AtomicUsize::new(0),
            thread_id: AtomicU32::new(0),
            stats: Stats::default(),
        });
        let (ready_tx, ready_rx) = channel();
        let thread_shared = shared.clone();
        let thread = std::thread::Builder::new()
            .name("inputhub".into())
            .spawn(move || run(thread_shared, ready_tx))
            .map_err(|e| e.to_string())?;
        ready_rx.recv().map_err(|e| e.to_string())?;
        Ok((
            HookThread {
                shared,
                thread: Some(thread),
            },
            rx,
        ))
    }

    pub fn shared(&self) -> &Arc<Shared> {
        &self.shared
    }

    pub fn stop(&mut self) {
        if let Some(thread) = self.thread.take() {
            let thread_id = self.shared.thread_id.load(Ordering::Acquire);
            if thread_id != 0 {
                unsafe { PostThreadMessageW(thread_id, WM_QUIT, 0, 0) };
            }
            let _ = thread.join();
        }
    }
}

impl Drop for HookThread {
    fn drop(&mut self) {
        self.stop();
    }
}

fn run(shared: Arc<Shared>, ready: Sender<()>) {
    unsafe {
        let mut msg: MSG = std::mem::zeroed();
        // 先建好消息队列，之后别的线程投递的同步请求才不会丢
        PeekMessageW(&mut msg, null_mut(), 0, 0, PM_NOREMOVE);
        CURRENT.with(|current| *current.borrow_mut() = Some(shared.clone()));
        shared
            .thread_id
            .store(GetCurrentThreadId(), Ordering::Release);
        let _ = ready.send(());

        let mut hooks = Installed::default();
        sync_hooks(&shared, &mut hooks);
        while GetMessageW(&mut msg, null_mut(), 0, 0) > 0 {
            if msg.message == WM_APP_SYNC {
                sync_hooks(&shared, &mut hooks);
            }
        }
        remove_low_level(&shared, &mut hooks);
        remove_foreground(&shared, &mut hooks);
        shared.thread_id.store(0, Ordering::Release);
        CURRENT.with(|current| *current.borrow_mut() = None);
    }
}

#[derive(Default)]
struct Installed {
    low_level: Option<(HHOOK, HHOOK)>,
    foreground: Option<HWINEVENTHOOK>,
}

fn sync_hooks(shared: &Shared, hooks: &mut Installed) {
    let (low_level, foreground) =
        shared.with_engine(|engine, _| (engine.hooks_needed(), engine.foreground_needed()));
    if low_level && hooks.low_level.is_none() {
        unsafe {
            let mouse = SetWindowsHookExW(WH_MOUSE_LL, Some(mouse_proc), null_mut(), 0);
            let keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, Some(keyboard_proc), null_mut(), 0);
            if mouse.is_null() || keyboard.is_null() {
                let error = GetLastError();
                for hook in [mouse, keyboard] {
                    if !hook.is_null() {
                        UnhookWindowsHookEx(hook);
                    }
                }
                shared.with_engine(|engine, emit| {
                    engine.fail(format!("SetWindowsHookExW failed (error {error})"), emit)
                });
                return;
            }
            hooks.low_level = Some((mouse, keyboard));
        }
        shared.stats.hooks_installed.store(true, Ordering::Release);
    } else if !low_level {
        remove_low_level(shared, hooks);
    }
    if foreground && hooks.foreground.is_none() {
        // 本进程的窗口也要：程序自己的其他窗口同样可能是粘贴之类的目标
        let hook = unsafe {
            SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND,
                EVENT_SYSTEM_FOREGROUND,
                null_mut(),
                Some(foreground_proc),
                0,
                0,
                WINEVENT_OUTOFCONTEXT,
            )
        };
        if hook.is_null() {
            let message = format!("SetWinEventHook failed (error {})", unsafe {
                GetLastError()
            });
            shared.with_engine(|_, emit| emit(Event::Failure { message }));
            return;
        }
        hooks.foreground = Some(hook);
        shared
            .stats
            .foreground_hook_installed
            .store(true, Ordering::Release);
    } else if !foreground {
        remove_foreground(shared, hooks);
    }
}

fn remove_low_level(shared: &Shared, hooks: &mut Installed) {
    if let Some((mouse, keyboard)) = hooks.low_level.take() {
        unsafe {
            UnhookWindowsHookEx(mouse);
            UnhookWindowsHookEx(keyboard);
        }
    }
    shared.stats.hooks_installed.store(false, Ordering::Release);
}

fn remove_foreground(shared: &Shared, hooks: &mut Installed) {
    if let Some(hook) = hooks.foreground.take() {
        unsafe { UnhookWinEvent(hook) };
    }
    shared
        .stats
        .foreground_hook_installed
        .store(false, Ordering::Release);
}
