use super::*;
use std::collections::HashSet;

const VK_LCONTROL: u32 = 0xA2;
const VK_RCONTROL: u32 = 0xA3;
const VK_LSHIFT: u32 = 0xA0;

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
        match &self.mask_error {
            Some(message) => Err(message.clone()),
            None => Ok(()),
        }
    }
}

struct Rig {
    engine: Engine,
    platform: FakePlatform,
}

impl Rig {
    fn new(bindings: &[(Modifiers, Button)]) -> Rig {
        let mut engine = Engine::new();
        assert!(engine.configure_gestures(bindings.to_vec(), true, &mut |_| {}));
        Rig {
            engine,
            platform: FakePlatform::default(),
        }
    }

    fn hold(&mut self, vk: u32) {
        self.platform.held.insert(vk);
    }

    fn release(&mut self, vk: u32) {
        self.platform.held.remove(&vk);
    }

    fn mouse(&mut self, action: MouseAction, x: i32, y: i32) -> (bool, Vec<Event>) {
        self.mouse_input(MouseInput {
            action,
            x,
            y,
            injected: false,
        })
    }

    fn mouse_input(&mut self, input: MouseInput) -> (bool, Vec<Event>) {
        let mut events = Vec::new();
        let suppress = self
            .engine
            .on_mouse(input, &mut self.platform, &mut |e| events.push(e));
        (suppress, events)
    }

    /// 真实按键顺序：按下时钩子先于异步状态被调用，抬起同理
    fn key(&mut self, vk: u32, pressed: bool) -> (bool, Vec<Event>) {
        let mut events = Vec::new();
        let input = KeyInput {
            vk,
            pressed,
            injected: false,
        };
        let suppress = self
            .engine
            .on_key(input, &mut self.platform, &mut |e| events.push(e));
        if pressed {
            self.hold(vk);
        } else {
            self.release(vk);
        }
        (suppress, events)
    }

    fn with(&mut self, f: impl FnOnce(&mut Engine, &mut dyn FnMut(Event))) -> Vec<Event> {
        let mut events = Vec::new();
        f(&mut self.engine, &mut |e| events.push(e));
        events
    }

    fn start_ctrl_left(&mut self) -> u64 {
        self.hold(VK_LCONTROL);
        let (suppress, events) = self.mouse(MouseAction::Down(Button::Left), 10, 20);
        assert!(suppress);
        match events.as_slice() {
            [Event::GestureStart { id, x: 10, y: 20 }] => *id,
            other => panic!("expected a start, got {other:?}"),
        }
    }
}

fn ctrl() -> Modifiers {
    Modifiers::CTRL
}

fn ctrl_alt() -> Modifiers {
    Modifiers::parse(["ctrl", "alt"]).unwrap()
}

// ---------------------------------------------------------------- 手势开始与结束

#[test]
fn a_gesture_starts_only_when_held_modifiers_match_exactly() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    rig.hold(VK_LCONTROL);
    rig.hold(0xA4);
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::Left), 0, 0),
        (false, vec![])
    );
    rig.release(0xA4);
    rig.hold(VK_RCONTROL);
    let (suppress, events) = rig.mouse(MouseAction::Down(Button::Left), 1, 2);
    assert!(suppress);
    assert_eq!(events, vec![Event::GestureStart { id: 1, x: 1, y: 2 }]);
}

#[test]
fn an_unbound_button_or_no_modifier_passes_through() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::Left), 0, 0),
        (false, vec![])
    );
    assert_eq!(
        rig.mouse(MouseAction::Up(Button::Left), 0, 0),
        (false, vec![])
    );
    rig.hold(VK_LCONTROL);
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::Right), 0, 0),
        (false, vec![])
    );
}

#[test]
fn press_and_release_are_swallowed_as_a_pair_and_release_finishes() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let id = rig.start_ctrl_left();
    rig.release(VK_LCONTROL);
    let (suppress, events) = rig.mouse(MouseAction::Up(Button::Left), 30, 40);
    assert!(suppress);
    assert_eq!(events, vec![Event::GestureFinish { id, x: 30, y: 40 }]);
    assert!(!rig.engine.dragging());
    assert_eq!(
        rig.mouse(MouseAction::Up(Button::Left), 0, 0),
        (false, vec![])
    );
}

#[test]
fn every_button_can_start_its_own_binding() {
    for button in Button::ALL {
        let mut rig = Rig::new(&[(ctrl_alt(), button)]);
        rig.hold(VK_LCONTROL);
        rig.hold(0xA5);
        let (suppress, events) = rig.mouse(MouseAction::Down(button), 0, 0);
        assert!(suppress, "{button:?}");
        assert!(
            matches!(events.as_slice(), [Event::GestureStart { .. }]),
            "{button:?}"
        );
    }
}

// ---------------------------------------------------------------- 拖动中

#[test]
fn moves_pass_through_and_notify_once_until_taken() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let id = rig.start_ctrl_left();
    assert_eq!(
        rig.mouse(MouseAction::Move, 11, 21),
        (false, vec![Event::GestureMoved { id }])
    );
    assert_eq!(rig.mouse(MouseAction::Move, 12, 22), (false, vec![]));
    assert_eq!(rig.engine.take_position(id + 1), None);
    assert_eq!(rig.engine.take_position(id), Some((12, 22)));
    assert_eq!(rig.engine.take_position(id), None);
    assert_eq!(
        rig.mouse(MouseAction::Move, 13, 23),
        (false, vec![Event::GestureMoved { id }])
    );
}

#[test]
fn moves_outside_a_gesture_do_not_notify() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    assert_eq!(rig.mouse(MouseAction::Move, 1, 1), (false, vec![]));
}

#[test]
fn the_wheel_is_swallowed_only_while_dragging() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let wheel = MouseAction::Wheel {
        delta: -120,
        horizontal: false,
    };
    assert_eq!(rig.mouse(wheel, 0, 0), (false, vec![]));
    rig.start_ctrl_left();
    assert_eq!(rig.mouse(wheel, 0, 0), (true, vec![]));
}

#[test]
fn other_clicks_during_a_drag_are_swallowed_with_their_release() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let id = rig.start_ctrl_left();
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::Right), 0, 0),
        (true, vec![])
    );
    assert_eq!(
        rig.mouse(MouseAction::Up(Button::Left), 5, 5),
        (true, vec![Event::GestureFinish { id, x: 5, y: 5 }])
    );
    // 右键在手势结束后才松开，仍要吞掉，下面的程序不会收到落单的抬起
    assert_eq!(
        rig.mouse(MouseAction::Up(Button::Right), 0, 0),
        (true, vec![])
    );
    assert_eq!(
        rig.mouse(MouseAction::Up(Button::Right), 0, 0),
        (false, vec![])
    );
}

// ---------------------------------------------------------------- 取消

#[test]
fn escape_cancels_and_both_its_press_and_release_are_swallowed() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let id = rig.start_ctrl_left();
    let (suppress, events) = rig.key(VK_ESCAPE, true);
    assert!(suppress);
    assert_eq!(events, vec![Event::GestureCancel { id, x: 10, y: 20 }]);
    // 自动重复的按下也吞掉
    assert_eq!(rig.key(VK_ESCAPE, true), (true, vec![]));
    assert_eq!(rig.key(VK_ESCAPE, false), (true, vec![]));
    assert_eq!(rig.key(VK_ESCAPE, true), (false, vec![]));
}

#[test]
fn escape_outside_a_gesture_passes_through() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    assert_eq!(rig.key(VK_ESCAPE, true), (false, vec![]));
    assert_eq!(rig.key(VK_ESCAPE, false), (false, vec![]));
}

#[test]
fn releasing_the_gesture_modifier_cancels_unless_the_other_side_is_held() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let id = rig.start_ctrl_left();
    rig.hold(VK_RCONTROL);
    assert_eq!(rig.key(VK_LCONTROL, false), (false, vec![]));
    assert!(rig.engine.dragging());
    let (suppress, events) = rig.key(VK_RCONTROL, false);
    assert!(!suppress, "the real release must reach Windows");
    assert_eq!(events, vec![Event::GestureCancel { id, x: 10, y: 20 }]);
}

#[test]
fn releasing_an_unrelated_modifier_keeps_the_drag() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    rig.start_ctrl_left();
    rig.hold(VK_LSHIFT);
    assert_eq!(rig.key(VK_LSHIFT, false), (false, vec![]));
    assert!(rig.engine.dragging());
}

#[test]
fn cancel_invalidates_queued_events_of_earlier_gestures() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let first = rig.start_ctrl_left();
    rig.mouse(MouseAction::Up(Button::Left), 0, 0);
    assert!(
        rig.engine.accepts(first),
        "a finished gesture stays valid until something invalidates it"
    );
    let events = rig.with(|e, emit| e.cancel(emit));
    assert_eq!(events, vec![], "nothing is dragging, so no cancel event");
    assert!(!rig.engine.accepts(first));
    let second = rig.start_ctrl_left();
    assert!(rig.engine.accepts(second));
    assert!(!rig.engine.accepts(second + 1));
}

// ---------------------------------------------------------------- Win/Alt 防菜单

#[test]
fn releasing_win_after_a_win_gesture_masks_the_start_menu_once() {
    let win = Modifiers::WIN;
    let mut rig = Rig::new(&[(win, Button::Left)]);
    rig.hold(VK_LWIN);
    let (_, events) = rig.mouse(MouseAction::Down(Button::Left), 0, 0);
    let id = match events.as_slice() {
        [Event::GestureStart { id, .. }] => *id,
        other => panic!("{other:?}"),
    };
    rig.mouse(MouseAction::Up(Button::Left), 0, 0);
    // 手势结束后才松开 Win，仍要屏蔽，真实抬起照常转发
    let (suppress, events) = rig.key(VK_LWIN, false);
    assert!(!suppress);
    assert_eq!(events, vec![]);
    assert_eq!(rig.platform.masks, 1);
    assert!(!rig.key(VK_LWIN, false).0);
    assert_eq!(rig.platform.masks, 1);
    assert!(rig.engine.accepts(id));
}

#[test]
fn win_pressed_again_during_the_drag_is_masked_on_release_too() {
    let mut rig = Rig::new(&[(Modifiers::parse(["ctrl", "win"]).unwrap(), Button::Left)]);
    rig.hold(VK_LCONTROL);
    rig.hold(VK_LWIN);
    rig.mouse(MouseAction::Down(Button::Left), 0, 0);
    rig.key(VK_RWIN, true);
    rig.key(VK_RWIN, false);
    assert_eq!(rig.platform.masks, 1);
}

#[test]
fn ctrl_gestures_never_mask_menus() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    rig.start_ctrl_left();
    rig.hold(VK_LWIN);
    rig.key(VK_LWIN, false);
    assert_eq!(rig.platform.masks, 0);
}

#[test]
fn a_failed_mask_is_reported_without_swallowing_the_release() {
    let mut rig = Rig::new(&[(Modifiers::ALT, Button::Left)]);
    rig.platform.mask_error = Some("boom".into());
    rig.hold(0xA4);
    rig.mouse(MouseAction::Down(Button::Left), 0, 0);
    let (suppress, events) = rig.key(0xA4, false);
    assert!(!suppress);
    assert!(events.contains(&Event::Failure {
        message: "boom".into()
    }));
}

// ---------------------------------------------------------------- 配置

#[test]
fn blocking_prevents_new_gestures_and_cancels_the_current_one() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let id = rig.start_ctrl_left();
    let events = rig.with(|e, emit| e.set_blocked(true, emit));
    assert_eq!(events, vec![Event::GestureCancel { id, x: 10, y: 20 }]);
    // 被吞掉的按下仍要配对吞掉抬起
    assert!(rig.mouse(MouseAction::Up(Button::Left), 0, 0).0);
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::Left), 0, 0),
        (false, vec![])
    );
    rig.with(|e, emit| e.set_blocked(false, emit));
    assert!(rig.mouse(MouseAction::Down(Button::Left), 0, 0).0);
}

#[test]
fn reconfiguring_cancels_only_when_bindings_change_or_disable() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    rig.start_ctrl_left();
    let same = rig.with(|e, emit| {
        e.configure_gestures(vec![(ctrl(), Button::Left)], true, emit);
    });
    assert_eq!(same, vec![]);
    assert!(rig.engine.dragging());
    let changed = rig.with(|e, emit| {
        e.configure_gestures(vec![(ctrl(), Button::Right)], true, emit);
    });
    assert!(matches!(changed.as_slice(), [Event::GestureCancel { .. }]));
}

#[test]
fn bindings_need_one_or_two_modifiers() {
    let mut engine = Engine::new();
    let noop = &mut |_| {};
    assert!(!engine.configure_gestures(vec![], true, noop));
    assert!(!engine.configure_gestures(vec![(Modifiers::default(), Button::Left)], true, noop));
    let three = Modifiers::parse(["ctrl", "alt", "shift"]).unwrap();
    assert!(!engine.configure_gestures(vec![(three, Button::Left)], true, noop));
    assert!(
        !engine.hooks_needed(),
        "an invalid configuration never enables the hooks"
    );
    assert!(engine.configure_gestures(vec![(ctrl_alt(), Button::Middle)], true, noop));
    assert!(engine.hooks_needed());
    assert_eq!(Modifiers::parse(["ctrl", "hyper"]), None);
}

#[test]
fn the_binding_of_only_the_current_gesture_is_reported() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let id = rig.start_ctrl_left();
    assert_eq!(rig.engine.gesture_binding(id), Some((ctrl(), Button::Left)));
    assert_eq!(rig.engine.gesture_binding(id - 1), None);
}

// ---------------------------------------------------------------- 模拟输入

#[test]
fn injected_input_never_drives_gestures() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    rig.hold(VK_LCONTROL);
    let press = MouseInput {
        action: MouseAction::Down(Button::Left),
        x: 0,
        y: 0,
        injected: true,
    };
    assert_eq!(rig.mouse_input(press), (false, vec![]));
    let id = rig.start_ctrl_left();
    let esc = KeyInput {
        vk: VK_ESCAPE,
        pressed: true,
        injected: true,
    };
    assert!(!rig.engine.on_key(esc, &mut rig.platform, &mut |_| {}));
    assert!(rig.engine.accepts(id) && rig.engine.dragging());
}

// ---------------------------------------------------------------- 侧键热键

fn side_rig(suppressed: &[Button], capture_all: bool) -> Rig {
    let mut engine = Engine::new();
    engine.configure_side_buttons(true, suppressed, capture_all);
    Rig {
        engine,
        platform: FakePlatform::default(),
    }
}

#[test]
fn side_buttons_are_reported_and_only_bound_ones_are_swallowed() {
    let mut rig = side_rig(&[Button::X1], false);
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::X1), 0, 0),
        (true, vec![Event::SideButton { button: Button::X1 }])
    );
    assert_eq!(rig.mouse(MouseAction::Up(Button::X1), 0, 0), (true, vec![]));
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::X2), 0, 0),
        (false, vec![Event::SideButton { button: Button::X2 }])
    );
    assert_eq!(
        rig.mouse(MouseAction::Up(Button::X2), 0, 0),
        (false, vec![])
    );
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::Middle), 0, 0),
        (false, vec![])
    );
}

#[test]
fn recording_swallows_every_side_button() {
    let mut rig = side_rig(&[], true);
    assert!(rig.mouse(MouseAction::Down(Button::X2), 0, 0).0);
    assert!(rig.mouse(MouseAction::Up(Button::X2), 0, 0).0);
}

#[test]
fn a_swallowed_side_press_keeps_its_release_after_unbinding() {
    let mut rig = side_rig(&[Button::X1], false);
    rig.mouse(MouseAction::Down(Button::X1), 0, 0);
    rig.engine.configure_side_buttons(false, &[], false);
    assert!(
        rig.engine.hooks_needed(),
        "the pending release keeps the hook"
    );
    assert_eq!(rig.mouse(MouseAction::Up(Button::X1), 0, 0), (true, vec![]));
    assert!(!rig.engine.hooks_needed());
}

#[test]
fn disabled_side_buttons_pass_through_silently() {
    let mut rig = side_rig(&[Button::X1], false);
    rig.engine
        .configure_side_buttons(false, &[Button::X1], false);
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::X1), 0, 0),
        (false, vec![])
    );
}

#[test]
fn injected_side_buttons_still_count() {
    // 鼠标驱动软件改键后发出的是模拟输入
    let mut rig = side_rig(&[Button::X1], false);
    let press = MouseInput {
        action: MouseAction::Down(Button::X1),
        x: 0,
        y: 0,
        injected: true,
    };
    assert_eq!(
        rig.mouse_input(press),
        (true, vec![Event::SideButton { button: Button::X1 }])
    );
}

#[test]
fn a_matching_gesture_takes_precedence_over_the_side_button_hotkey() {
    let mut rig = Rig::new(&[(ctrl(), Button::X1)]);
    rig.engine
        .configure_side_buttons(true, &[Button::X1], false);
    rig.hold(VK_LCONTROL);
    let (suppress, events) = rig.mouse(MouseAction::Down(Button::X1), 0, 0);
    assert!(suppress);
    assert!(matches!(events.as_slice(), [Event::GestureStart { .. }]));
    rig.release(VK_LCONTROL);
    rig.mouse(MouseAction::Up(Button::X1), 0, 0);
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::X1), 0, 0),
        (true, vec![Event::SideButton { button: Button::X1 }])
    );
}

// ---------------------------------------------------------------- 键盘热键

const VK_V: u32 = 0x56;

fn hotkey_rig() -> Rig {
    let mut engine = Engine::new();
    assert!(engine.bind_hotkey("clipboard", Modifiers::WIN, VK_V));
    Rig {
        engine,
        platform: FakePlatform::default(),
    }
}

fn clipboard_hotkey() -> Event {
    Event::Hotkey {
        name: Arc::from("clipboard"),
    }
}

#[test]
fn a_hotkey_swallows_press_repeats_and_release_and_reports_once() {
    let mut rig = hotkey_rig();
    rig.key(VK_LWIN, true);
    assert_eq!(rig.key(VK_V, true), (true, vec![clipboard_hotkey()]));
    assert_eq!(rig.key(VK_V, true), (true, vec![]), "auto-repeat");
    assert_eq!(rig.key(VK_V, false), (true, vec![]));
    assert_eq!(rig.key(VK_V, true), (true, vec![clipboard_hotkey()]));
    rig.key(VK_V, false);
}

#[test]
fn releasing_win_after_a_hotkey_masks_the_start_menu_without_swallowing() {
    let mut rig = hotkey_rig();
    rig.key(VK_RWIN, true);
    rig.key(VK_V, true);
    rig.key(VK_V, false);
    assert_eq!(rig.platform.masks, 0);
    assert_eq!(rig.key(VK_RWIN, false), (false, vec![]));
    assert_eq!(rig.platform.masks, 1);
}

#[test]
fn releasing_win_before_the_key_still_masks_and_swallows_the_release() {
    let mut rig = hotkey_rig();
    rig.key(VK_LWIN, true);
    rig.key(VK_V, true);
    assert!(!rig.key(VK_LWIN, false).0);
    assert_eq!(rig.platform.masks, 1);
    assert_eq!(rig.key(VK_V, false), (true, vec![]));
    assert!(!rig.key(VK_V, true).0, "plain V after Win is released");
}

#[test]
fn hotkeys_need_exactly_their_modifiers() {
    let mut rig = hotkey_rig();
    assert_eq!(rig.key(VK_V, true), (false, vec![]));
    rig.key(VK_V, false);
    rig.key(VK_LWIN, true);
    rig.key(VK_LSHIFT, true);
    assert_eq!(rig.key(VK_V, true), (false, vec![]));
    rig.key(VK_V, false);
    rig.key(VK_LWIN, false);
    assert_eq!(rig.platform.masks, 0, "nothing was swallowed");
}

#[test]
fn a_key_held_before_the_modifiers_is_left_alone() {
    let mut rig = hotkey_rig();
    rig.key(VK_V, true);
    rig.key(VK_LWIN, true);
    assert_eq!(rig.key(VK_V, true), (false, vec![]), "auto-repeat");
    assert!(!rig.key(VK_V, false).0);
}

#[test]
fn injected_keys_never_trigger_hotkeys() {
    let mut rig = hotkey_rig();
    rig.key(VK_LWIN, true);
    let mut events = Vec::new();
    let input = KeyInput {
        vk: VK_V,
        pressed: true,
        injected: true,
    };
    assert!(!rig
        .engine
        .on_key(input, &mut rig.platform, &mut |e| events.push(e)));
    assert!(events.is_empty());
}

#[test]
fn unbinding_keeps_swallowing_the_pending_release() {
    let mut rig = hotkey_rig();
    rig.key(VK_LWIN, true);
    rig.key(VK_V, true);
    rig.key(VK_LWIN, false);
    rig.engine.unbind_hotkey("clipboard");
    assert!(rig.engine.hooks_needed());
    assert!(rig.key(VK_V, false).0);
    assert!(!rig.engine.hooks_needed());
    rig.key(VK_LWIN, true);
    assert_eq!(rig.key(VK_V, true), (false, vec![]));
}

#[test]
fn hotkeys_need_a_modifier_and_a_non_modifier_key() {
    let mut engine = Engine::new();
    assert!(!engine.bind_hotkey("bare", Modifiers::default(), VK_V));
    assert!(!engine.bind_hotkey("modifier", Modifiers::WIN, VK_LSHIFT));
    assert!(!engine.bind_hotkey("generic", Modifiers::WIN, 0x10));
    assert!(!engine.hooks_needed());
    assert!(engine.bind_hotkey("clipboard", Modifiers::WIN, VK_V));
    assert!(engine.hooks_needed());
    assert!(!engine.bind_hotkey("clipboard", Modifiers::default(), VK_V));
    assert!(
        !engine.hooks_needed(),
        "an invalid rebind removes the old one"
    );
}

#[test]
fn a_gesture_swallowing_escape_takes_precedence_over_a_hotkey() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    assert!(rig.engine.bind_hotkey("esc", ctrl(), VK_ESCAPE));
    rig.start_ctrl_left();
    let (suppress, events) = rig.key(VK_ESCAPE, true);
    assert!(suppress);
    assert!(matches!(events.as_slice(), [Event::GestureCancel { .. }]));
    rig.key(VK_ESCAPE, false);
    assert_eq!(
        rig.key(VK_ESCAPE, true),
        (
            true,
            vec![Event::Hotkey {
                name: Arc::from("esc")
            }]
        )
    );
}

#[test]
fn a_failure_drops_hotkeys_and_their_claims() {
    let mut rig = hotkey_rig();
    rig.key(VK_LWIN, true);
    rig.key(VK_V, true);
    rig.with(|e, emit| e.fail("hook lost".into(), emit));
    assert!(!rig.engine.hooks_needed());
    assert!(!rig.key(VK_V, false).0);
    assert_eq!(rig.key(VK_V, true), (false, vec![]));
}

// ---------------------------------------------------------------- 旁听订阅

#[test]
fn wheel_watchers_hear_only_their_area_and_never_swallow() {
    let mut engine = Engine::new();
    engine.watch_wheel(
        "stitch",
        Some(Rect {
            left: 0,
            top: 0,
            right: 100,
            bottom: 100,
        }),
    );
    engine.watch_wheel("gif", None);
    let mut rig = Rig {
        engine,
        platform: FakePlatform::default(),
    };
    let wheel = MouseAction::Wheel {
        delta: -240,
        horizontal: false,
    };
    let (suppress, events) = rig.mouse(wheel, 50, 50);
    assert!(!suppress);
    assert_eq!(events.len(), 2);
    let (_, outside) = rig.mouse(wheel, 150, 50);
    assert_eq!(
        outside,
        vec![Event::Wheel {
            watcher: Arc::from("gif"),
            x: 150,
            y: 50,
            delta: -240,
            horizontal: false
        }]
    );
    rig.engine.unwatch_wheel("gif");
    assert_eq!(rig.mouse(wheel, 150, 50), (false, vec![]));
}

#[test]
fn injected_wheel_events_are_heard() {
    let mut engine = Engine::new();
    engine.watch_wheel("stitch", None);
    let mut rig = Rig {
        engine,
        platform: FakePlatform::default(),
    };
    let input = MouseInput {
        action: MouseAction::Wheel {
            delta: 120,
            horizontal: true,
        },
        x: 0,
        y: 0,
        injected: true,
    };
    assert_eq!(rig.mouse_input(input).1.len(), 1);
}

#[test]
fn key_watchers_hear_presses_and_releases_without_swallowing() {
    let mut engine = Engine::new();
    engine.watch_keys("stitch", vec![0x10, VK_LSHIFT]);
    let mut rig = Rig {
        engine,
        platform: FakePlatform::default(),
    };
    assert_eq!(
        rig.key(VK_LSHIFT, true),
        (
            false,
            vec![Event::Key {
                watcher: Arc::from("stitch"),
                vk: VK_LSHIFT,
                pressed: true
            }]
        )
    );
    assert_eq!(rig.key(VK_LSHIFT, false).1.len(), 1);
    assert_eq!(rig.key(0x41, true), (false, vec![]));
}

#[test]
fn foreground_changes_are_reported_once_while_anyone_watches() {
    let mut engine = Engine::new();
    let mut events = Vec::new();
    engine.on_foreground(7, &mut |e| events.push(e));
    assert!(events.is_empty());
    assert!(!engine.foreground_needed());
    engine.watch_foreground("clipboard");
    engine.watch_foreground("clipboard");
    engine.watch_foreground("other");
    assert!(engine.foreground_needed());
    assert!(
        !engine.hooks_needed(),
        "foreground events need no low-level hooks"
    );
    engine.on_foreground(7, &mut |e| events.push(e));
    assert_eq!(events, vec![Event::Foreground { hwnd: 7 }]);
    engine.unwatch_foreground("clipboard");
    assert!(engine.foreground_needed());
    engine.unwatch_foreground("other");
    assert!(!engine.foreground_needed());
    engine.watch_foreground("clipboard");
    engine.close(&mut |_| {});
    assert!(!engine.foreground_needed());
}

// ---------------------------------------------------------------- 生命周期

#[test]
fn hooks_are_needed_only_while_something_listens_or_waits_for_a_release() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    rig.start_ctrl_left();
    rig.with(|e, emit| {
        e.configure_gestures(vec![(ctrl(), Button::Left)], false, emit);
    });
    assert!(
        rig.engine.hooks_needed(),
        "the swallowed press still waits for its release"
    );
    rig.mouse(MouseAction::Up(Button::Left), 0, 0);
    assert!(!rig.engine.hooks_needed());
    assert!(rig.engine.take_sync_request());
    assert!(!rig.engine.take_sync_request());
    rig.engine.watch_keys("stitch", vec![0x10]);
    assert!(rig.engine.hooks_needed());
}

#[test]
fn a_failure_disables_everything_and_releases_claims() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    rig.engine
        .configure_side_buttons(true, &[Button::X2], false);
    let id = rig.start_ctrl_left();
    let events = rig.with(|e, emit| e.fail("hook lost".into(), emit));
    assert_eq!(
        events,
        vec![
            Event::GestureCancel { id, x: 10, y: 20 },
            Event::Failure {
                message: "hook lost".into()
            },
        ]
    );
    assert!(!rig.engine.hooks_needed());
    assert_eq!(
        rig.mouse(MouseAction::Up(Button::Left), 0, 0),
        (false, vec![])
    );
}

#[test]
fn a_closed_engine_ignores_everything() {
    let mut rig = Rig::new(&[(ctrl(), Button::Left)]);
    let id = rig.start_ctrl_left();
    let events = rig.with(|e, emit| e.close(emit));
    assert_eq!(events, vec![Event::GestureCancel { id, x: 10, y: 20 }]);
    assert!(!rig.engine.accepts(id));
    assert!(!rig.engine.hooks_needed());
    assert_eq!(
        rig.mouse(MouseAction::Down(Button::Left), 0, 0),
        (false, vec![])
    );
    rig.engine
        .configure_gestures(vec![(ctrl(), Button::Left)], true, &mut |_| {});
    assert!(
        !rig.engine.hooks_needed(),
        "a closed engine cannot be re-enabled"
    );
}
