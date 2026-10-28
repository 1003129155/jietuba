#include "core/game_mode_monitor.h"
#include "core/i18n.h"
#include "core/input_hub.h"
#include "core/shortcut_manager.h"
#include "settings/tool_settings.h"
#include "ui/hotkey_edit.h"
#include <QApplication>
#include <QComboBox>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace jietuba;

namespace {
// Injected input carrying this value counts as real input for the hooks.
constexpr ULONG_PTR TEST_MARKER = 0x4A425432;

void send_key(WORD vk, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = TEST_MARKER;
    SendInput(1, &input, sizeof input);
}

void press_combo(std::initializer_list<WORD> keys) {
    for (auto key : keys) send_key(key, true);
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) send_key(*it, false);
}

// Records what reaches it and consumes what it is told to; owns one window.
class RecordingHandler : public ShortcutHandler {
public:
    RecordingHandler(QString name, const QWidget *window) : name_(std::move(name)), window_(window) {}
    bool owns_window(const QWidget *window) const override { return window == window_; }
    bool is_active() override { return active; }
    bool handle_key(QInputEvent *event) override {
        keys << event_key(event);
        if (throw_on_key) throw std::runtime_error("handler failed");
        return consume_keys;
    }
    bool handle_mouse(QMouseEvent *event) override {
        mouse_buttons << event->button();
        return consume_mouse;
    }
    bool handle_hotkey(int hotkey_id, const std::function<void()> &) override {
        hotkey_ids << hotkey_id;
        return consume_hotkeys;
    }
    bool handle_mouse_hotkey(const QString &token, const std::function<void()> &callback) override {
        mouse_tokens << token;
        mouse_callbacks_bound << bool(callback);
        return consume_hotkeys;
    }
    bool handle_captured_key(const QString &text) override {
        captured << text;
        return consume_hotkeys;
    }
    QString handler_name() const override { return name_; }
    bool active = true, consume_keys = false, consume_mouse = false, consume_hotkeys = false, throw_on_key = false;
    QList<int> keys, hotkey_ids;
    QList<Qt::MouseButton> mouse_buttons;
    QStringList mouse_tokens, captured;
    QList<bool> mouse_callbacks_bound;
private:
    QString name_;
    const QWidget *window_;
};

// Global hotkeys and side buttons are offered to the owner of the foreground window.
bool show_in_front(QWidget &window) {
    window.resize(120, 80);
    window.show();
    window.activateWindow();
    return QTest::qWaitForWindowActive(&window) && GetForegroundWindow() == reinterpret_cast<HWND>(window.winId());
}

int hotkey_id_of(const QString &hotkey) {
    const auto parsed = ShortcutManager::parse_hotkey(hotkey);
    for (int id = 1; id < 5000; ++id)
        if (ShortcutManager::instance()->hotkey_metadata(id) == std::optional<std::pair<int, int>>(parsed)) return id;
    return 0;
}

void key_press(QWidget *target, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(target, &event);
}

// Ctrl+Alt+Shift with keys no keyboard has, so neither a running copy of the app nor the person at
// the computer collides with them; each test uses its own.
constexpr UINT HYPER = MOD_CONTROL | MOD_ALT | MOD_SHIFT;

// Whether another program could register the combination now.
bool system_can_register(UINT mods, UINT vk) {
    constexpr int PROBE_ID = 0x6F10;
    if (!RegisterHotKey(nullptr, PROBE_ID, mods, vk)) return false;
    UnregisterHotKey(nullptr, PROBE_ID);
    return true;
}

// Another program holding a system hotkey: its own thread and message loop, counting WM_HOTKEY.
class OtherProgram {
public:
    OtherProgram(UINT mods, UINT vk) {
        thread_ = std::thread([this, mods, vk] {
            MSG msg;
            PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
            registered_ = RegisterHotKey(nullptr, 1, mods, vk);
            thread_id_ = GetCurrentThreadId();
            while (GetMessageW(&msg, nullptr, 0, 0) > 0)
                if (msg.message == WM_HOTKEY) ++hits;
            if (registered_) UnregisterHotKey(nullptr, 1);
        });
        while (!thread_id_) std::this_thread::yield();
    }
    ~OtherProgram() {
        PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        thread_.join();
    }
    bool registered() const { return registered_; }
    std::atomic<int> hits{0};
private:
    std::atomic<bool> registered_{false};
    std::atomic<DWORD> thread_id_{0};
    std::thread thread_;
};
}

// The shortcut manager on a real desktop: system hotkeys, side buttons, hook combinations and
// the in-app handler chain.
class ShortcutManagerTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    ShortcutManager *manager_ = nullptr;
private slots:
    void initTestCase() {
        if (QGuiApplication::platformName() != "windows") QSKIP("needs -platform windows: system hotkeys arrive as thread messages");
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        get_tool_settings_manager(settings_);
        manager_ = ShortcutManager::instance();
        input_hub()->native->set_test_marker(TEST_MARKER);
    }

    void cleanup() {
        manager_->unregister_all_hotkeys();
        manager_->set_global_hotkeys_suppressed(false);
        manager_->set_game_mode_suppressed(false);
        // Nothing may stay held after an injected combination.
        for (WORD key : {VK_CONTROL, VK_MENU, VK_SHIFT}) send_key(key, false);
    }

    void cleanupTestCase() {
        if (manager_) input_hub()->native->set_test_marker(0);
    }

    // A registered system hotkey fires from the keyboard; handlers and suppression can stop it.
    void aSystemHotkeyRunsItsCallback() {
        int fired = 0;
        QVERIFY(manager_->register_hotkey("ctrl+alt+shift+f9", [&fired] { ++fired; }));
        QVERIFY(manager_->has_registered_hotkeys());
        // Registered here already: still reported available, and not twice.
        QVERIFY(manager_->check_hotkey_availability("ctrl+alt+shift+f9"));
        QVERIFY(manager_->check_hotkey_availability("ctrl+alt+shift+f8"));
        QVERIFY(manager_->check_hotkey_availability(MOUSE_BUTTON_BACK));
        QVERIFY(!manager_->check_hotkey_availability("ctrl+nonsense"));
        QVERIFY(!manager_->register_hotkey("ctrl+nonsense", [] {}));
        press_combo({VK_CONTROL, VK_MENU, VK_SHIFT, VK_F9});
        QTRY_COMPARE(fired, 1);

        const int id = hotkey_id_of("ctrl+alt+shift+f9");
        QVERIFY(id > 0);
        QVERIFY(!manager_->on_wm_hotkey(id + 1000));
        QWidget front;
        QVERIFY(show_in_front(front));
        RecordingHandler handler("recording", &front);
        manager_->register_handler(&handler);
        manager_->register_handler(&handler);
        handler.consume_hotkeys = true;
        QVERIFY(manager_->on_wm_hotkey(id));
        QCOMPARE(fired, 1);
        QCOMPARE(handler.hotkey_ids, QList<int>{id});
        // An inactive handler is skipped, and so is one owning no window in front or under the pointer.
        handler.active = false;
        QVERIFY(manager_->on_wm_hotkey(id));
        QCOMPARE(fired, 2);
        handler.active = true;
        QWidget hidden;
        RecordingHandler elsewhere("elsewhere", &hidden);
        elsewhere.consume_hotkeys = true;
        manager_->register_handler(&elsewhere);
        handler.consume_hotkeys = false;
        QVERIFY(manager_->on_wm_hotkey(id));
        QCOMPARE(fired, 3);
        QVERIFY(elsewhere.hotkey_ids.isEmpty());
        manager_->unregister_handler(&elsewhere);
        handler.consume_hotkeys = true;
        QVERIFY(manager_->on_wm_hotkey(id));
        QCOMPARE(fired, 3);
        handler.consume_hotkeys = false;
        // Suppressed: swallowed before any handler sees it.
        manager_->set_global_hotkeys_suppressed(true);
        QVERIFY(manager_->global_hotkeys_suppressed());
        QVERIFY(manager_->on_wm_hotkey(id));
        QCOMPARE(fired, 3);
        QCOMPARE(handler.hotkey_ids.size(), 3);
        manager_->set_global_hotkeys_suppressed(false);
        QVERIFY(manager_->on_wm_hotkey(id));
        QCOMPARE(fired, 4);
        manager_->unregister_handler(&handler);
        manager_->unregister_handler(&handler);
        manager_->unregister_all_hotkeys();
        QVERIFY(!manager_->has_registered_hotkeys());
        QVERIFY(!manager_->on_wm_hotkey(id));
    }

    // Side buttons arrive from the input hook as names; handlers may record them first.
    void sideButtonsRunTheirCallbacks() {
        int back = 0;
        QVERIFY(manager_->register_hotkey(MOUSE_BUTTON_BACK, [&back] { ++back; }));
        QVERIFY(!manager_->register_hotkey(MOUSE_BUTTON_BACK, [] {}));
        emit input_hub()->side_button("x1");
        QTRY_COMPARE(back, 1);
        QWidget front;
        QVERIFY(show_in_front(front));
        RecordingHandler handler("recording", &front);
        manager_->register_handler(&handler);
        // An unbound button still reaches handlers, without a callback.
        emit input_hub()->side_button("x2");
        QTRY_COMPARE(handler.mouse_tokens.size(), 1);
        QCOMPARE(handler.mouse_tokens.first(), MOUSE_BUTTON_FORWARD);
        QCOMPARE(handler.mouse_callbacks_bound.first(), false);
        handler.consume_hotkeys = true;
        emit input_hub()->side_button("x1");
        QTRY_COMPARE(handler.mouse_tokens.size(), 2);
        QCOMPARE(back, 1);
        handler.consume_hotkeys = false;
        manager_->set_global_hotkeys_suppressed(true);
        emit input_hub()->side_button("x1");
        QTest::qWait(50);
        QCOMPARE(back, 1);
        manager_->set_global_hotkeys_suppressed(false);
        emit input_hub()->side_button("unknown");
        QTest::qWait(50);
        QCOMPARE(handler.mouse_tokens.size(), 2);
        manager_->begin_mouse_capture();
        manager_->end_mouse_capture();
        manager_->end_mouse_capture();
        manager_->unregister_handler(&handler);
    }

    // Win combinations go through the keyboard hook with the system registration as a backstop:
    // fired once, through the handler chain, released on suppression.
    void hookHotkeysFireUntilSuppressed() {
        int fired = 0;
        QVERIFY(manager_->register_hotkey("win+f10", [&fired] { ++fired; }));
        QVERIFY(manager_->has_registered_hotkeys());
        const int hid = hotkey_id_of("win+f10");
        QVERIFY(hid);
        const QString name = QString("hotkey-%1").arg(hid);
        // Pressed before the hook is in, the key would go to the backstop registration instead.
        QTRY_VERIFY(input_hub()->native->hooks_installed());
        press_combo({VK_LWIN, VK_F10});
        QTRY_COMPARE(fired, 1);
        // The hook swallowed it, so the backstop registration did not fire as well.
        QTest::qWait(100);
        QCOMPARE(fired, 1);
        emit input_hub()->hotkey(name);
        QTRY_COMPARE(fired, 2);
        QWidget front;
        QVERIFY(show_in_front(front));
        RecordingHandler interceptor("interceptor", &front);
        interceptor.consume_hotkeys = true;
        manager_->register_handler(&interceptor);
        emit input_hub()->hotkey(name);
        QTRY_COMPARE(interceptor.hotkey_ids, QList<int>{hid});
        QCOMPARE(fired, 2);
        manager_->unregister_handler(&interceptor);
        emit input_hub()->hotkey("someone-else");
        manager_->set_global_hotkeys_suppressed(true);
        emit input_hub()->hotkey(name);
        QTest::qWait(50);
        QCOMPARE(fired, 2);
        manager_->set_global_hotkeys_suppressed(false);
        emit input_hub()->hotkey(name);
        QTRY_COMPARE(fired, 3);
    }

    // Values no one can bind register nothing.
    void reservedAndInvalidValuesAreRefused() {
        for (const char *hotkey : {"win+l", "ctrl+alt+delete", "a", "ctrl+!", "ctrl+"})
            QVERIFY2(!manager_->register_hotkey(hotkey, [] {}), hotkey);
        QVERIFY(!manager_->has_registered_hotkeys());
    }

    // While an editor records, the keys the hook took reach the handlers in the recorder's spelling.
    void capturedKeysReachTheRecorder() {
        RecordingHandler recorder("recorder", nullptr);
        recorder.consume_hotkeys = true;
        manager_->register_handler(&recorder);
        manager_->begin_key_capture();
        emit input_hub()->captured({"shift", "win"}, 'S');
        emit input_hub()->captured({}, VK_SNAPSHOT);
        emit input_hub()->captured({"win"}, 0xFF);
        QTRY_COMPARE(recorder.captured, QStringList({"shift+win+s", "print"}));
        manager_->end_key_capture();
        manager_->end_key_capture();
        manager_->unregister_handler(&recorder);
    }

    // In-app keys go to the handlers owning the window they are sent to, in registration order,
    // except into focused text fields.
    void inAppKeysGoToTheWindowOwner() {
        QWidget window, other;
        RecordingHandler high("high", &window), low("low", &window), elsewhere("elsewhere", &other);
        manager_->register_handler(&high);
        manager_->register_handler(&elsewhere);
        manager_->register_handler(&low);
        auto *line = new QLineEdit(&window);
        auto *spin = new QSpinBox(&window);
        auto *combo = new QComboBox(&window);
        combo->setEditable(true);
        auto *plain = new QWidget(&window);
        plain->setFocusPolicy(Qt::StrongFocus);
        auto *view = new QGraphicsView(&window);
        auto *scene = new QGraphicsScene(view);
        view->setScene(scene);
        auto *text = scene->addText("edit me");
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));

        plain->setFocus();
        high.consume_keys = true;
        key_press(plain, Qt::Key_A);
        QCOMPARE(high.keys, QList<int>{Qt::Key_A});
        QVERIFY(low.keys.isEmpty());
        QVERIFY(elsewhere.keys.isEmpty());
        // An inactive owner is skipped.
        high.active = false;
        key_press(plain, Qt::Key_G);
        QVERIFY(low.keys.contains(Qt::Key_G));
        high.active = true;
        high.consume_keys = false;
        // Unconsumed keys propagate to the parent and pass the filter again there.
        key_press(plain, Qt::Key_B);
        QVERIFY(low.keys.contains(Qt::Key_B));
        // A failing handler does not stop the next one.
        high.throw_on_key = true;
        key_press(plain, Qt::Key_C);
        QCOMPARE(low.keys.last(), int(Qt::Key_C));
        high.throw_on_key = false;
        high.consume_keys = true;

        for (QWidget *field : {static_cast<QWidget *>(line), static_cast<QWidget *>(spin), static_cast<QWidget *>(combo)}) {
            field->setFocus();
            const int before = high.keys.size();
            key_press(field, Qt::Key_D);
            QCOMPARE(high.keys.size(), before);
            // Escape and function keys still go to shortcuts.
            key_press(field, Qt::Key_Escape);
            key_press(field, Qt::Key_F5);
            QCOMPARE(high.keys.size(), before + 2);
        }
        // A text item being edited in a scene counts as a text field too.
        view->setFocus();
        text->setTextInteractionFlags(Qt::TextEditorInteraction);
        text->setFocus();
        int before = high.keys.size();
        key_press(view, Qt::Key_E);
        QCOMPARE(high.keys.size(), before);
        text->clearFocus();
        key_press(view, Qt::Key_E);
        QCOMPARE(high.keys.size(), before + 1);
        // Keys sent to another window go to its owner alone.
        before = high.keys.size();
        key_press(&other, Qt::Key_H);
        QCOMPARE(elsewhere.keys, QList<int>{Qt::Key_H});
        QCOMPARE(high.keys.size(), before);
        manager_->unregister_handler(&low);
        manager_->unregister_handler(&high);
        manager_->unregister_handler(&elsewhere);
    }

    // The middle button runs in-app shortcuts; a press claimed there also claims its release.
    void theMiddleButtonGoesToHandlers() {
        QWidget target;
        RecordingHandler handler("mouse", &target);
        manager_->register_handler(&handler);
        target.resize(100, 100);
        target.show();
        const auto send = [&target](QEvent::Type type, Qt::MouseButton button) {
            QMouseEvent event(type, QPointF(10, 10), QPointF(target.mapToGlobal(QPoint(10, 10))), button,
                              type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button), Qt::NoModifier);
            return QApplication::sendEvent(&target, &event) && event.isAccepted();
        };
        send(QEvent::MouseButtonPress, Qt::LeftButton);
        QVERIFY(handler.mouse_buttons.isEmpty());
        handler.consume_mouse = true;
        send(QEvent::MouseButtonPress, Qt::MiddleButton);
        QCOMPARE(handler.mouse_buttons, QList<Qt::MouseButton>{Qt::MiddleButton});
        send(QEvent::MouseButtonRelease, Qt::MiddleButton);
        QCOMPARE(handler.mouse_buttons.size(), 1);
        // Hotkey editors record the button themselves.
        target.setProperty("_captures_inapp_mouse_shortcut", true);
        send(QEvent::MouseButtonPress, Qt::MiddleButton);
        QCOMPARE(handler.mouse_buttons.size(), 1);
        manager_->unregister_handler(&handler);
    }


    // A focused hotkey editor takes the system hotkeys and side buttons it would trigger and records them.
    void aFocusedHotkeyEditorRecordsWhatWouldFire() {
        int fired = 0;
        QVERIFY(manager_->register_hotkey("ctrl+alt+shift+f9", [&fired] { ++fired; }));
        QWidget window;
        auto *editor = new HotkeyEdit(&window);
        window.show();
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        editor->edit->setFocus();
        QTRY_VERIFY(editor->edit->hasFocus());
        QVERIFY(manager_->on_wm_hotkey(hotkey_id_of("ctrl+alt+shift+f9")));
        QCOMPARE(fired, 0);
        QCOMPARE(editor->text(), QString("ctrl+shift+alt+f9"));
        emit input_hub()->side_button("x2");
        QTRY_COMPARE(editor->text(), MOUSE_BUTTON_FORWARD);
        // Win combinations and PrtSc come from the hook, which takes them while the editor has focus.
        // Pressed before the hook is in, Win+F10 would reach the system and the Win release open Start.
        QTRY_VERIFY(input_hub()->native->hooks_installed());
        // Paced like a person: the editor sees Win before the hook reports F10.
        send_key(VK_LWIN, true);
        QTRY_COMPARE(editor->text(), QString("win+"));
        send_key(VK_F10, true);
        send_key(VK_F10, false);
        QTRY_COMPARE(editor->text(), QString("win+f10"));
        send_key(VK_LWIN, false);
        // The key injected to keep Start closed reaches the editor too and must change nothing.
        QTest::qWait(300);
        QCOMPARE(editor->text(), QString("win+f10"));
        QVERIFY(window.isActiveWindow());
        emit input_hub()->captured({"win"}, 'V');
        QTRY_COMPARE(editor->text(), QString("win+v"));
        emit input_hub()->captured({}, VK_SNAPSHOT);
        QTRY_COMPARE(editor->text(), QString("print"));
        // Typed combinations: a held modifier shows the prefix, releasing it puts back the value from before it.
        QKeyEvent ctrl(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
        QApplication::sendEvent(editor->edit, &ctrl);
        QCOMPARE(editor->text(), QString("ctrl+"));
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::ControlModifier);
        QApplication::sendEvent(editor->edit, &release);
        QCOMPARE(editor->text(), QString("print"));
        QKeyEvent f13(QEvent::KeyPress, Qt::Key_F13, Qt::ControlModifier);
        QApplication::sendEvent(editor->edit, &f13);
        QCOMPARE(editor->text(), QString("ctrl+f13"));
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(editor->edit, &escape);
        editor->edit->clearFocus();
        QVERIFY(manager_->on_wm_hotkey(hotkey_id_of("ctrl+alt+shift+f9")));
        QCOMPARE(fired, 1);
    }
    void bindingsMatchKeysAndTheMiddleButton() {
        QHash<QString, KeyBinding> keys{{"undo", KeyBinding{Qt::Key_Z, Qt::ControlModifier}}};
        QHash<QString, MouseBinding> mouse{{"pin", MouseBinding{Qt::MiddleButton, Qt::ShiftModifier}}};
        QKeyEvent undo(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(match_inapp_binding(&undo, "undo", keys, mouse));
        QVERIFY(!match_inapp_binding(&undo, "redo", keys, mouse));
        QMouseEvent middle(QEvent::MouseButtonPress, QPointF(), QPointF(), Qt::MiddleButton, Qt::MiddleButton, Qt::ShiftModifier);
        QVERIFY(match_inapp_binding(&middle, "pin", keys, mouse));
        QVERIFY(!match_inapp_binding(&middle, "undo", keys, mouse));
        QVERIFY(is_mouse_shortcut_event(&middle));
        QVERIFY(!is_mouse_shortcut_event(&undo));
        QCOMPARE(event_key(&middle), int(Qt::Key_unknown));
        QVERIFY(!event_is_auto_repeat(&middle));
        QCOMPARE(inapp_shortcut_display_text("shift+mousemiddle"), QString("SHIFT+") + QCoreApplication::translate("InAppShortcut", "Middle"));
        QCOMPARE(inapp_shortcut_display_text(MOUSE_BUTTON_MIDDLE), QCoreApplication::translate("InAppShortcut", "Middle"));
        QCOMPARE(inapp_shortcut_display_text("ctrl+z"), QString("CTRL+Z"));
        QCOMPARE(inapp_shortcut_display_text(""), QString());
        QCOMPARE(hotkey_identity("ctrl+bogus"), std::optional<QString>("invalid:ctrl+bogus"));
        QCOMPARE(hotkey_identity("MouseForward"), std::optional<QString>("mouse:mouseforward"));
        QCOMPARE(hotkey_identity("ctrl+"), std::nullopt);
        QCOMPARE(ShortcutManager::parse_hotkey("ctrl+comma").second, VK_OEM_COMMA);
        QVERIFY(mouse_gesture_binding_matches("ctrl+dragleft", &undo, "dragleft"));
        QVERIFY(!mouse_gesture_binding_matches("", &undo, "dragleft"));
    }

    // Paused by the user or a game, every registration and hook binding is released, so the keys
    // reach other programs and no low-level hook stays in the input path; resuming restores them.
    void pausingReleasesRegistrationsAndHooks() {
        int fired = 0;
        QVERIFY(manager_->register_hotkey("ctrl+alt+shift+f21", [&fired] { ++fired; }));
        QVERIFY(manager_->register_hotkey("win+f13", [] {}));
        QVERIFY(manager_->register_hotkey(MOUSE_BUTTON_BACK, [] {}));
        QTRY_VERIFY(input_hub()->native->hooks_installed());
        QVERIFY(!system_can_register(HYPER, VK_F21));
        QVERIFY(!system_can_register(MOD_WIN, VK_F13));
        QSignalSpy paused(manager_, &ShortcutManager::global_hotkeys_paused_changed);
        manager_->set_game_mode_suppressed(true);
        QCOMPARE(paused.size(), 1);
        QCOMPARE(paused.last().first().toBool(), true);
        QVERIFY(manager_->global_hotkeys_suppressed());
        // The backstop registration of the hook-route key goes too.
        QVERIFY(system_can_register(HYPER, VK_F21));
        QVERIFY(system_can_register(MOD_WIN, VK_F13));
        QTRY_VERIFY(!input_hub()->native->hooks_installed());
        QVERIFY(manager_->has_registered_hotkeys());
        press_combo({VK_CONTROL, VK_MENU, VK_SHIFT, VK_F21});
        QTest::qWait(100);
        QCOMPARE(fired, 0);
        // Either reason keeps it paused.
        manager_->set_global_hotkeys_suppressed(true);
        manager_->set_game_mode_suppressed(false);
        QCOMPARE(paused.size(), 1);
        QVERIFY(system_can_register(HYPER, VK_F21));
        // Registered while paused: only recorded.
        QVERIFY(manager_->register_hotkey("ctrl+alt+shift+f20", [] {}));
        QVERIFY(system_can_register(HYPER, VK_F20));
        manager_->set_global_hotkeys_suppressed(false);
        QCOMPARE(paused.size(), 2);
        QCOMPARE(paused.last().first().toBool(), false);
        QVERIFY(!system_can_register(HYPER, VK_F21));
        QVERIFY(!system_can_register(HYPER, VK_F20));
        QVERIFY(!system_can_register(MOD_WIN, VK_F13));
        QTRY_VERIFY(input_hub()->native->hooks_installed());
        press_combo({VK_CONTROL, VK_MENU, VK_SHIFT, VK_F21});
        QTRY_COMPARE(fired, 1);
    }

    // Paused, a recording hotkey editor still gets side buttons, without their callbacks.
    void aRecordingEditorGetsSideButtonsWhilePaused() {
        int back = 0;
        QVERIFY(manager_->register_hotkey(MOUSE_BUTTON_BACK, [&back] { ++back; }));
        QWidget front;
        QVERIFY(show_in_front(front));
        RecordingHandler recorder("recorder", &front);
        recorder.consume_hotkeys = true;
        manager_->register_handler(&recorder);
        manager_->set_global_hotkeys_suppressed(true);
        emit input_hub()->side_button("x1");
        QTest::qWait(50);
        QVERIFY(recorder.mouse_tokens.isEmpty());
        manager_->begin_mouse_capture();
        QTRY_VERIFY(input_hub()->native->hooks_installed());
        emit input_hub()->side_button("x1");
        QTRY_COMPARE(recorder.mouse_tokens, QStringList{MOUSE_BUTTON_BACK});
        QCOMPARE(recorder.mouse_callbacks_bound, QList<bool>{false});
        manager_->end_mouse_capture();
        QTRY_VERIFY(!input_hub()->native->hooks_installed());
        QCOMPARE(back, 0);
        manager_->unregister_handler(&recorder);
    }

    // A key another program holds goes to the hook, which sees it first: this program fires and the
    // other does not. Once the key is free, the next resume moves it back to the system.
    void anOccupiedHotkeyFallsBackToTheHook() {
        int fired = 0;
        {
            OtherProgram other(HYPER, VK_F19);
            QVERIFY(other.registered());
            // The other program does receive the key while nothing takes it first.
            press_combo({VK_CONTROL, VK_MENU, VK_SHIFT, VK_F19});
            QTRY_COMPARE(other.hits.load(), 1);
            QVERIFY(manager_->register_hotkey("ctrl+alt+shift+f19", [&fired] { ++fired; }));
            QTRY_VERIFY(input_hub()->native->hooks_installed());
            press_combo({VK_CONTROL, VK_MENU, VK_SHIFT, VK_F19});
            QTRY_COMPARE(fired, 1);
            QTest::qWait(100);
            QCOMPARE(other.hits.load(), 1);
            // Repeats are refused here, since the system no longer refuses them.
            QVERIFY(!manager_->register_hotkey("ctrl+alt+shift+f19", [] {}));
        }
        QVERIFY(system_can_register(HYPER, VK_F19));
        manager_->set_game_mode_suppressed(true);
        manager_->set_game_mode_suppressed(false);
        QVERIFY(!system_can_register(HYPER, VK_F19));
        QTRY_VERIFY(!input_hub()->native->hooks_installed());
        press_combo({VK_CONTROL, VK_MENU, VK_SHIFT, VK_F19});
        QTRY_COMPARE(fired, 2);
        // The hook binds only combinations with a modifier, so a held key bound alone is refused.
        OtherProgram bare(0, VK_F18);
        QVERIFY(bare.registered());
        QVERIFY(!manager_->register_hotkey("f18", [] {}));
    }

    // The game monitor checks at once on start, reports only changes, and on stop reports a game it saw as gone.
    void theGameMonitorReportsOnlyChanges() {
        GameModeMonitor monitor;
        bool game = true;
        monitor.set_probe([&game] { return game; });
        QSignalSpy changed(&monitor, &GameModeMonitor::changed);
        monitor.start();
        QVERIFY(monitor.running());
        QCOMPARE(changed.size(), 1);
        QCOMPARE(changed.last().first().toBool(), true);
        monitor.check();
        QCOMPARE(changed.size(), 1);
        game = false;
        monitor.check();
        QCOMPARE(changed.size(), 2);
        QCOMPARE(changed.last().first().toBool(), false);
        monitor.stop();
        QVERIFY(!monitor.running());
        QCOMPARE(changed.size(), 2);
        game = true;
        monitor.start();
        monitor.stop();
        QCOMPARE(changed.size(), 4);
        QCOMPARE(changed.last().first().toBool(), false);
        QVERIFY(!monitor.active());
    }

    // A foreground switch is checked at once and again a little later, since a game goes full screen
    // after its window appears; after stop, switches are ignored.
    void theGameMonitorFollowsForegroundSwitches() {
        GameModeMonitor monitor;
        bool game = false;
        int checks = 0;
        monitor.set_probe([&] {
            ++checks;
            return game;
        });
        monitor.start();
        QCOMPARE(checks, 1);
        game = true;
        emit input_hub()->foreground(0);
        QTRY_VERIFY(monitor.active());
        // Full screen only after the switch: the follow-up sees it.
        game = false;
        emit input_hub()->foreground(0);
        QTRY_VERIFY(!monitor.active());
        game = true;
        QTRY_VERIFY_WITH_TIMEOUT(monitor.active(), GameModeMonitor::FOLLOW_UP_MS * 2);
        monitor.stop();
        QVERIFY(!monitor.active());
        const int stopped_at = checks;
        emit input_hub()->foreground(0);
        QTest::qWait(GameModeMonitor::FOLLOW_UP_MS + 500);
        QCOMPARE(checks, stopped_at);
    }

    // A key held by another app shows as taken over when the hook can bind it, and as in use when it is bound alone.
    void theEditorTellsWhichHeldKeysAreTakenOver() {
        const auto tip = [](const char *text) { return i18n_tr(text, "HotkeyEdit"); };
        OtherProgram combination(HYPER, VK_F17), alone(0, VK_F16);
        QVERIFY(combination.registered() && alone.registered());
        HotkeyEdit edit;
        edit.setText("ctrl+alt+shift+f17");
        QCOMPARE(edit.status_state(), QString("ok"));
        QVERIFY(edit.taken_over());
        QCOMPARE(edit.edit->toolTip(), tip("Another app uses this shortcut too. This app takes it over while running; clear it to give it back."));
        QVERIFY(edit.validate_now());
        edit.setText("f16");
        QCOMPARE(edit.status_state(), QString("error"));
        QVERIFY(!edit.taken_over());
        QCOMPARE(edit.status_icon->toolTip(), tip("Already in use by another app."));
        QVERIFY(!edit.validate_now());
    }

    // A side button has nothing for the system to hold: it is available at once and again after the
    // 200 ms check that follows typing, which must not try to read it as a keyboard key.
    void aSideButtonHotkeyIsAvailableAndTheDelayedCheckIsQuiet() {
        for (const QString &token : {MOUSE_BUTTON_FORWARD, MOUSE_BUTTON_BACK}) {
            HotkeyEdit edit;
            edit.setText(token);
            QCOMPARE(edit.status_state(), QString("ok"));
            QVERIFY(!edit.taken_over());
            QVERIFY(edit.validate_now());
            // As the recording editor sets it: through the line edit, so the delayed check runs.
            edit.edit->setText("");
            edit.edit->setText(token);
            QTest::qWait(450);
            QCOMPARE(edit.status_state(), QString("ok"));
            QVERIFY(!edit.taken_over());
        }
    }
};

QTEST_MAIN(ShortcutManagerTests)
#include "test_shortcut_manager.moc"
