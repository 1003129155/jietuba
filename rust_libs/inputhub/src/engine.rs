//! 输入状态机：钩子线程和测试共用，自身不调任何系统 API。
//!
//! 三层按固定顺序处理同一个事件：全局手势 → 侧键热键或键盘热键 → 旁听订阅。前一层吞掉的事件
//! 不再交给后面的热键层，所以修饰键完全匹配的手势优先于侧键热键。

use std::sync::Arc;

pub const VK_ESCAPE: u32 = 0x1B;

const VK_LWIN: u32 = 0x5B;
const VK_RWIN: u32 = 0x5C;
const VK_LMENU: u32 = 0xA4;
const VK_RMENU: u32 = 0xA5;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash, PartialOrd, Ord)]
pub enum Button {
    Left,
    Middle,
    Right,
    X1,
    X2,
}

impl Button {
    pub const ALL: [Button; 5] = [
        Button::Left,
        Button::Middle,
        Button::Right,
        Button::X1,
        Button::X2,
    ];

    fn bit(self) -> u8 {
        1 << (self as u8)
    }

    pub fn name(self) -> &'static str {
        match self {
            Button::Left => "left",
            Button::Middle => "middle",
            Button::Right => "right",
            Button::X1 => "x1",
            Button::X2 => "x2",
        }
    }

    pub fn parse(name: &str) -> Option<Button> {
        Button::ALL.into_iter().find(|b| b.name() == name)
    }
}

/// 一组修饰键。左右两侧不区分，任一侧按着即算按着。
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default, Hash, PartialOrd, Ord)]
pub struct Modifiers(u8);

impl Modifiers {
    pub const CTRL: Modifiers = Modifiers(1);
    pub const SHIFT: Modifiers = Modifiers(2);
    pub const ALT: Modifiers = Modifiers(4);
    pub const WIN: Modifiers = Modifiers(8);
    const NAMED: [(Modifiers, &'static str, [u32; 2]); 4] = [
        (Modifiers::CTRL, "ctrl", [0xA2, 0xA3]),
        (Modifiers::SHIFT, "shift", [0xA0, 0xA1]),
        (Modifiers::ALT, "alt", [VK_LMENU, VK_RMENU]),
        (Modifiers::WIN, "win", [VK_LWIN, VK_RWIN]),
    ];

    pub fn contains(self, other: Modifiers) -> bool {
        other.0 != 0 && self.0 & other.0 == other.0
    }

    pub fn count(self) -> u32 {
        self.0.count_ones()
    }

    /// 名字不认识时返回 None。
    pub fn parse<'a>(names: impl IntoIterator<Item = &'a str>) -> Option<Modifiers> {
        let mut bits = 0;
        for name in names {
            bits |= Modifiers::NAMED.iter().find(|(_, n, _)| *n == name)?.0 .0;
        }
        Some(Modifiers(bits))
    }

    pub fn names(self) -> Vec<&'static str> {
        Modifiers::NAMED
            .iter()
            .filter(|(m, _, _)| self.contains(*m))
            .map(|(_, n, _)| *n)
            .collect()
    }

    /// 某个虚拟键属于哪个修饰键；通用码 VK_SHIFT/VK_CONTROL/VK_MENU 也算。
    fn of_key(vk: u32) -> Option<Modifiers> {
        match vk {
            0x10 => Some(Modifiers::SHIFT),
            0x11 => Some(Modifiers::CTRL),
            0x12 => Some(Modifiers::ALT),
            _ => Modifiers::NAMED
                .iter()
                .find(|(_, _, keys)| keys.contains(&vk))
                .map(|(m, _, _)| *m),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Rect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

impl Rect {
    fn contains(&self, x: i32, y: i32) -> bool {
        self.left <= x && x < self.right && self.top <= y && y < self.bottom
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum MouseAction {
    Move,
    Down(Button),
    Up(Button),
    /// delta 为 WHEEL_DELTA(120) 的倍数；竖向正值向上，横向正值向右
    Wheel {
        delta: i32,
        horizontal: bool,
    },
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct MouseInput {
    pub action: MouseAction,
    pub x: i32,
    pub y: i32,
    /// 模拟输入。测试标记匹配的模拟输入由调用方当成真实输入传进来
    pub injected: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct KeyInput {
    pub vk: u32,
    pub pressed: bool,
    pub injected: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Event {
    GestureStart {
        id: u64,
        x: i32,
        y: i32,
    },
    GestureFinish {
        id: u64,
        x: i32,
        y: i32,
    },
    GestureCancel {
        id: u64,
        x: i32,
        y: i32,
    },
    /// 拖动中位置变了；在 take_position 取走之前只发一次
    GestureMoved {
        id: u64,
    },
    SideButton {
        button: Button,
    },
    /// 键盘热键按下；长按的自动重复不再发
    Hotkey {
        name: Arc<str>,
    },
    Wheel {
        watcher: Arc<str>,
        x: i32,
        y: i32,
        delta: i32,
        horizontal: bool,
    },
    Key {
        watcher: Arc<str>,
        vk: u32,
        pressed: bool,
    },
    /// 前台窗口变了；有订阅时每次切换都发，不做筛选
    Foreground {
        hwnd: isize,
    },
    Failure {
        message: String,
    },
}

/// 状态机需要的两样系统能力；正式运行由 Win32 实现，测试用假实现。
pub trait Platform {
    /// 这个虚拟键此刻是否按着（GetAsyncKeyState 语义）
    fn key_held(&self, vk: u32) -> bool;
    /// 发一对无意义按键，让随后松开的 Win/Alt 不再弹出开始菜单或菜单栏
    fn mask_menu(&mut self) -> Result<(), String>;
}

fn held_modifiers(platform: &dyn Platform, exclude_vk: Option<u32>) -> Modifiers {
    let mut bits = 0;
    for (modifier, _, keys) in Modifiers::NAMED {
        if keys
            .iter()
            .any(|&vk| Some(vk) != exclude_vk && platform.key_held(vk))
        {
            bits |= modifier.0;
        }
    }
    Modifiers(bits)
}

#[derive(Default)]
struct KeySet(Vec<u32>);

impl KeySet {
    fn insert(&mut self, vk: u32) {
        if !self.0.contains(&vk) {
            self.0.push(vk);
        }
    }

    fn remove(&mut self, vk: u32) -> bool {
        let before = self.0.len();
        self.0.retain(|&k| k != vk);
        self.0.len() != before
    }
}

#[derive(Default)]
pub struct Engine {
    // 全局手势
    bindings: Vec<(Modifiers, Button)>,
    enabled: bool,
    blocked: bool,
    closed: bool,
    gesture_id: u64,
    invalid_through: u64,
    dragging: bool,
    gesture_modifiers: Modifiers,
    gesture_button: Option<Button>,
    x: i32,
    y: i32,
    pending_move: Option<u64>,
    /// 按下被吞掉的键，抬起也必须吞掉
    claimed: u8,
    claimed_escape: bool,
    /// 手势或热键期间按着的 Win/Alt，松开时要先屏蔽菜单
    pending_menu_keys: KeySet,
    // 侧键热键
    side_enabled: bool,
    side_suppressed: u8,
    side_capture_all: bool,
    side_claimed: u8,
    // 键盘热键
    hotkeys: Vec<(Arc<str>, Modifiers, u32)>,
    /// 按下被热键吞掉的键，自动重复和抬起也必须吞掉
    hotkey_claimed: KeySet,
    // 旁听订阅
    wheel_watchers: Vec<(Arc<str>, Option<Rect>)>,
    key_watchers: Vec<(Arc<str>, Vec<u32>)>,
    foreground_watchers: Vec<Arc<str>>,
    sync_requested: bool,
}

impl Engine {
    pub fn new() -> Self {
        Self::default()
    }

    // ------------------------------------------------------------------ 输入

    /// 返回是否吞掉这个事件。
    pub fn on_mouse(
        &mut self,
        input: MouseInput,
        platform: &mut dyn Platform,
        emit: &mut dyn FnMut(Event),
    ) -> bool {
        if self.closed {
            return false;
        }
        if !input.injected && self.gesture_mouse(input, platform, emit) {
            return true;
        }
        if self.side_mouse(input, emit) {
            return true;
        }
        self.observe_mouse(input, emit);
        false
    }

    pub fn on_key(
        &mut self,
        input: KeyInput,
        platform: &mut dyn Platform,
        emit: &mut dyn FnMut(Event),
    ) -> bool {
        if self.closed {
            return false;
        }
        let suppress = (!input.injected && self.gesture_key(input, platform, emit))
            || self.hotkey_key(input, platform, emit);
        for (watcher, keys) in &self.key_watchers {
            if keys.contains(&input.vk) {
                emit(Event::Key {
                    watcher: watcher.clone(),
                    vk: input.vk,
                    pressed: input.pressed,
                });
            }
        }
        suppress
    }

    fn gesture_mouse(
        &mut self,
        input: MouseInput,
        platform: &mut dyn Platform,
        emit: &mut dyn FnMut(Event),
    ) -> bool {
        self.x = input.x;
        self.y = input.y;
        match input.action {
            MouseAction::Down(button) => self.button_down(button, platform, emit),
            MouseAction::Up(button) => self.button_up(button, emit),
            MouseAction::Move => {
                if self.dragging && self.pending_move.is_none() {
                    // 高频移动只替换最新位置，界面取走之前不再重复通知
                    self.pending_move = Some(self.gesture_id);
                    emit(Event::GestureMoved {
                        id: self.gesture_id,
                    });
                }
                // 光标照常移动；成对的按键已被吞掉，下面的程序开始不了拖动
                false
            }
            // 框选期间桌面照常刷新，滚动会挪走正在框的内容
            MouseAction::Wheel { .. } => self.dragging,
        }
    }

    fn button_down(
        &mut self,
        button: Button,
        platform: &mut dyn Platform,
        emit: &mut dyn FnMut(Event),
    ) -> bool {
        if self.claimed & button.bit() != 0 {
            return true;
        }
        if self.dragging {
            // 同滚轮：点击会改变正在框的内容
            self.claimed |= button.bit();
            return true;
        }
        if !self.enabled || self.blocked {
            return false;
        }
        let held = held_modifiers(platform, None);
        if !self.bindings.contains(&(held, button)) {
            return false;
        }
        self.gesture_modifiers = held;
        self.gesture_button = Some(button);
        self.gesture_id += 1;
        self.pending_move = None;
        self.dragging = true;
        self.claimed |= button.bit();
        self.mask_menu_on_release(held, platform);
        emit(Event::GestureStart {
            id: self.gesture_id,
            x: self.x,
            y: self.y,
        });
        true
    }

    fn button_up(&mut self, button: Button, emit: &mut dyn FnMut(Event)) -> bool {
        if self.claimed & button.bit() == 0 {
            return false;
        }
        self.claimed &= !button.bit();
        if self.gesture_button == Some(button) {
            self.pending_move = None;
            if self.dragging {
                self.dragging = false;
                emit(Event::GestureFinish {
                    id: self.gesture_id,
                    x: self.x,
                    y: self.y,
                });
            }
        }
        self.sync_requested = true;
        true
    }

    /// 记下此刻按着的 Win/Alt，松开时先屏蔽菜单。
    /// Win 的自动重复可能在提前屏蔽后重新激活开始菜单，所以一直记到它们松开，
    /// 即使手势或热键已经结束、取消或设置变了。
    fn mask_menu_on_release(&mut self, held: Modifiers, platform: &dyn Platform) {
        for (modifier, keys) in [
            (Modifiers::WIN, [VK_LWIN, VK_RWIN]),
            (Modifiers::ALT, [VK_LMENU, VK_RMENU]),
        ] {
            if held.contains(modifier) {
                for vk in keys {
                    if platform.key_held(vk) {
                        self.pending_menu_keys.insert(vk);
                    }
                }
            }
        }
    }

    fn hotkey_key(
        &mut self,
        input: KeyInput,
        platform: &mut dyn Platform,
        emit: &mut dyn FnMut(Event),
    ) -> bool {
        if input.injected {
            return false;
        }
        if !input.pressed {
            if self.hotkey_claimed.remove(input.vk) {
                self.sync_requested = true;
                return true;
            }
            return false;
        }
        if self.hotkey_claimed.0.contains(&input.vk) {
            return true;
        }
        // 钩子先于这个键的异步状态更新被调用，此刻已按着说明是修饰键之前就按下的键在自动重复；
        // 它的按下已经交给了别的程序，吞掉抬起会让那边一直当它按着
        if platform.key_held(input.vk) {
            return false;
        }
        let held = held_modifiers(platform, None);
        let Some(name) = self
            .hotkeys
            .iter()
            .find(|(_, modifiers, vk)| *vk == input.vk && *modifiers == held)
            .map(|(name, _, _)| name.clone())
        else {
            return false;
        };
        self.hotkey_claimed.insert(input.vk);
        self.mask_menu_on_release(held, platform);
        emit(Event::Hotkey { name });
        true
    }

    fn gesture_key(
        &mut self,
        input: KeyInput,
        platform: &mut dyn Platform,
        emit: &mut dyn FnMut(Event),
    ) -> bool {
        let modifier = Modifiers::of_key(input.vk);
        let menu_modifier = modifier.filter(|m| *m == Modifiers::WIN || *m == Modifiers::ALT);
        if input.pressed && self.dragging {
            if let Some(m) = menu_modifier {
                if self.gesture_modifiers.contains(m) {
                    self.pending_menu_keys.insert(input.vk);
                }
            }
        }
        if !input.pressed && self.pending_menu_keys.remove(input.vk) {
            // 在转发这次抬起之前屏蔽；真实的抬起不能吞，系统要靠它清掉修饰键状态
            if let Err(message) = platform.mask_menu() {
                emit(Event::Failure { message });
            }
            self.sync_requested = true;
        }
        if input.vk == VK_ESCAPE {
            if input.pressed && (self.dragging || self.claimed_escape) {
                self.claimed_escape = true;
                self.cancel(emit);
                return true;
            }
            if !input.pressed && self.claimed_escape {
                self.claimed_escape = false;
                self.sync_requested = true;
                return true;
            }
        }
        if self.dragging && !input.pressed {
            if let Some(m) = modifier {
                // 钩子先于这个键的异步状态更新被调用，所以要排除它自己再看另一侧
                if self.gesture_modifiers.contains(m)
                    && !held_modifiers(platform, Some(input.vk)).contains(m)
                {
                    self.cancel(emit);
                }
            }
        }
        false
    }

    fn side_mouse(&mut self, input: MouseInput, emit: &mut dyn FnMut(Event)) -> bool {
        match input.action {
            MouseAction::Down(button @ (Button::X1 | Button::X2)) => {
                if self.side_claimed & button.bit() != 0 {
                    return true;
                }
                if !self.side_enabled {
                    return false;
                }
                emit(Event::SideButton { button });
                if self.side_capture_all || self.side_suppressed & button.bit() != 0 {
                    // 按下和抬起成对吞掉，别的程序不会收到一个没有按下的抬起
                    self.side_claimed |= button.bit();
                    return true;
                }
                false
            }
            MouseAction::Up(button @ (Button::X1 | Button::X2))
                if self.side_claimed & button.bit() != 0 =>
            {
                self.side_claimed &= !button.bit();
                self.sync_requested = true;
                true
            }
            _ => false,
        }
    }

    fn observe_mouse(&self, input: MouseInput, emit: &mut dyn FnMut(Event)) {
        if let MouseAction::Wheel { delta, horizontal } = input.action {
            for (watcher, rect) in &self.wheel_watchers {
                if rect.is_none_or(|r| r.contains(input.x, input.y)) {
                    emit(Event::Wheel {
                        watcher: watcher.clone(),
                        x: input.x,
                        y: input.y,
                        delta,
                        horizontal,
                    });
                }
            }
        }
    }

    // ------------------------------------------------------------------ 手势配置与查询

    /// bindings 为 (修饰键, 按键)；修饰键须 1–2 个，按住的修饰键要与之完全一致。
    /// 返回配置是否合法；不合法时即使要求启用也不会启用。
    pub fn configure_gestures(
        &mut self,
        bindings: Vec<(Modifiers, Button)>,
        enabled: bool,
        emit: &mut dyn FnMut(Event),
    ) -> bool {
        let mut bindings = bindings;
        bindings.sort();
        bindings.dedup();
        let valid =
            !bindings.is_empty() && bindings.iter().all(|(m, _)| (1..=2).contains(&m.count()));
        let enabled = enabled && valid && !self.closed;
        if bindings != self.bindings || !enabled {
            self.cancel(emit);
        }
        self.bindings = bindings;
        self.enabled = enabled;
        self.sync_requested = true;
        valid
    }

    /// 由界面决定何时不可用；钩子自己不查窗口和模态状态。
    pub fn set_blocked(&mut self, blocked: bool, emit: &mut dyn FnMut(Event)) {
        self.blocked = blocked;
        if blocked {
            self.cancel(emit);
        }
    }

    /// 作废当前及之前的手势，已排队但界面还没处理的开始、结束事件也一并失效。
    pub fn cancel(&mut self, emit: &mut dyn FnMut(Event)) {
        self.invalid_through = self.gesture_id;
        self.pending_move = None;
        if self.dragging {
            self.dragging = false;
            emit(Event::GestureCancel {
                id: self.gesture_id,
                x: self.x,
                y: self.y,
            });
        }
    }

    /// 取走最新位置；较新手势的通知不受影响。
    pub fn take_position(&mut self, gesture_id: u64) -> Option<(i32, i32)> {
        if self.pending_move != Some(gesture_id) {
            return None;
        }
        self.pending_move = None;
        Some((self.x, self.y))
    }

    pub fn dragging(&self) -> bool {
        self.dragging
    }

    /// 排队送达的开始、结束事件是否仍然有效；重新配置和取消会让它们作废。
    pub fn accepts(&self, gesture_id: u64) -> bool {
        !self.closed && self.invalid_through < gesture_id && gesture_id <= self.gesture_id
    }

    /// 仅限当前手势；新手势可能在旧手势的开始事件送达前就开始了。
    pub fn gesture_binding(&self, gesture_id: u64) -> Option<(Modifiers, Button)> {
        if gesture_id != self.gesture_id {
            return None;
        }
        self.gesture_button.map(|b| (self.gesture_modifiers, b))
    }

    // ------------------------------------------------------------------ 侧键与订阅

    /// suppressed 里的侧键按下和抬起都吞掉；capture_all 时吞掉全部侧键（录入快捷键用）。
    pub fn configure_side_buttons(
        &mut self,
        enabled: bool,
        suppressed: &[Button],
        capture_all: bool,
    ) {
        self.side_enabled = enabled;
        self.side_suppressed = suppressed.iter().fold(0, |bits, b| bits | b.bit());
        self.side_capture_all = capture_all;
        self.sync_requested = true;
    }

    /// 按住的修饰键与 modifiers 完全一致时按下 vk，吞掉这个键的按下、自动重复和抬起并发一次事件。
    /// 同名绑定会被替换。modifiers 至少一个、vk 不能是修饰键，否则不绑定并返回 false。
    pub fn bind_hotkey(&mut self, name: &str, modifiers: Modifiers, vk: u32) -> bool {
        self.unbind_hotkey(name);
        if modifiers.count() == 0 || Modifiers::of_key(vk).is_some() || self.closed {
            return false;
        }
        self.hotkeys.push((Arc::from(name), modifiers, vk));
        true
    }

    /// 已被吞掉按下的键，抬起照样吞掉。
    pub fn unbind_hotkey(&mut self, name: &str) {
        self.hotkeys.retain(|(n, _, _)| &**n != name);
        self.sync_requested = true;
    }

    pub fn watch_wheel(&mut self, watcher: &str, rect: Option<Rect>) {
        self.unwatch_wheel(watcher);
        self.wheel_watchers.push((Arc::from(watcher), rect));
        self.sync_requested = true;
    }

    pub fn unwatch_wheel(&mut self, watcher: &str) {
        self.wheel_watchers.retain(|(w, _)| &**w != watcher);
        self.sync_requested = true;
    }

    pub fn watch_keys(&mut self, watcher: &str, keys: Vec<u32>) {
        self.unwatch_keys(watcher);
        self.key_watchers.push((Arc::from(watcher), keys));
        self.sync_requested = true;
    }

    pub fn unwatch_keys(&mut self, watcher: &str) {
        self.key_watchers.retain(|(w, _)| &**w != watcher);
        self.sync_requested = true;
    }

    pub fn watch_foreground(&mut self, watcher: &str) {
        if !self.foreground_watchers.iter().any(|w| &**w == watcher) {
            self.foreground_watchers.push(Arc::from(watcher));
            self.sync_requested = true;
        }
    }

    pub fn unwatch_foreground(&mut self, watcher: &str) {
        self.foreground_watchers.retain(|w| &**w != watcher);
        self.sync_requested = true;
    }

    /// 同一次切换只发一个事件，不论有几个订阅方。
    pub fn on_foreground(&mut self, hwnd: isize, emit: &mut dyn FnMut(Event)) {
        if self.foreground_needed() {
            emit(Event::Foreground { hwnd });
        }
    }

    // ------------------------------------------------------------------ 生命周期

    /// 是否需要挂低层钩子：有功能开着，或还有被吞掉的按下在等配对的抬起。
    pub fn hooks_needed(&self) -> bool {
        !self.closed
            && (self.enabled
                || self.claimed != 0
                || self.claimed_escape
                || !self.pending_menu_keys.0.is_empty()
                || self.side_enabled
                || self.side_claimed != 0
                || !self.hotkeys.is_empty()
                || !self.hotkey_claimed.0.is_empty()
                || !self.wheel_watchers.is_empty()
                || !self.key_watchers.is_empty())
    }

    /// 是否需要前台窗口事件钩子。
    pub fn foreground_needed(&self) -> bool {
        !self.closed && !self.foreground_watchers.is_empty()
    }

    /// 取走「钩子是否还需要」的重新评估请求。
    pub fn take_sync_request(&mut self) -> bool {
        std::mem::take(&mut self.sync_requested)
    }

    /// 钩子装不上或出错：停用全部功能并清掉未配对的认领，不再吞任何事件。
    pub fn fail(&mut self, message: String, emit: &mut dyn FnMut(Event)) {
        self.enabled = false;
        self.side_enabled = false;
        self.hotkeys.clear();
        self.cancel(emit);
        self.clear_claims();
        self.sync_requested = true;
        emit(Event::Failure { message });
    }

    pub fn close(&mut self, emit: &mut dyn FnMut(Event)) {
        self.enabled = false;
        self.cancel(emit);
        self.closed = true;
        self.side_enabled = false;
        self.hotkeys.clear();
        self.clear_claims();
        self.wheel_watchers.clear();
        self.key_watchers.clear();
        self.foreground_watchers.clear();
        self.sync_requested = true;
    }

    fn clear_claims(&mut self) {
        self.claimed = 0;
        self.claimed_escape = false;
        self.pending_menu_keys.0.clear();
        self.side_claimed = 0;
        self.hotkey_claimed.0.clear();
    }
}

#[cfg(test)]
mod tests;
