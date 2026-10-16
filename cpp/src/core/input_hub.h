#pragma once
#include <QObject>
#include <QRect>
#include <QStringList>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace jietuba {
// One event from the native hub; text, value and flag depend on kind (see core/native.h).
struct InputHubEvent {
    enum Kind { Gesture, Moved, Side, Hotkey, Wheel, Key, Foreground, Failure, Captured } kind = Failure;
    QString text;
    quint64 id = 0;
    int x = 0, y = 0, value = 0;
    bool flag = false;
    quintptr hwnd = 0;
};

// (left, top, right, bottom) in physical pixels, as the native hub takes it.
struct InputHubRect {
    int left, top, right, bottom;
};

// The native hub's operations; the Rust implementation in production, a stand-in in tests.
class InputHubNative {
public:
    virtual ~InputHubNative() = default;
    // Blocks until an event arrives; nullopt once closed and drained.
    virtual std::optional<InputHubEvent> next_event() = 0;
    virtual void close() = 0;
    // bindings: (modifier names, button name); returns whether the configuration is valid.
    virtual bool configure_gestures(const std::vector<std::pair<QStringList, QString>> &bindings, bool enabled) = 0;
    virtual void set_blocked(bool blocked) = 0;
    virtual void cancel_gesture() = 0;
    virtual std::optional<QPoint> take_position(quint64 gesture_id) = 0;
    virtual bool dragging() = 0;
    virtual bool accepts(quint64 gesture_id) = 0;
    virtual std::optional<std::pair<QStringList, QString>> gesture_binding(quint64 gesture_id) = 0;
    // Throws std::invalid_argument for a name that is not a side button.
    virtual void configure_side_buttons(bool enabled, const QStringList &suppressed, bool capture_all = false) = 0;
    // Throws std::invalid_argument for an unknown modifier.
    virtual bool bind_hotkey(const QString &name, const QStringList &modifiers, quint32 vk) = 0;
    virtual void unbind_hotkey(const QString &name) = 0;
    // While recording: keys pressed with Win held, and PrtSc, are swallowed and reported as Captured.
    virtual void configure_key_capture(bool enabled) = 0;
    // nullopt watches the whole desktop.
    virtual void watch_wheel(const QString &watcher, const std::optional<InputHubRect> &rect = std::nullopt) = 0;
    virtual void unwatch_wheel(const QString &watcher) = 0;
    virtual void watch_keys(const QString &watcher, const std::vector<quint32> &keys) = 0;
    virtual void unwatch_keys(const QString &watcher) = 0;
    virtual void watch_foreground(const QString &watcher) = 0;
    virtual void unwatch_foreground(const QString &watcher) = 0;
    // The low-level hooks install on the native thread after a feature asks for them.
    virtual bool hooks_installed() = 0;
    virtual void set_test_marker(quintptr marker) = 0;
};

// The low-level mouse and keyboard hooks and their state machines run on the
// native library's own thread and decide on the spot whether to swallow an event,
// so a busy GUI thread never holds up system input. Events the UI needs are turned
// into signals by an event thread; receivers use queued connections.
class InputHub : public QObject {
    Q_OBJECT
public:
    explicit InputHub(std::unique_ptr<InputHubNative> native, QObject *parent = nullptr);
    ~InputHub() override;
    void start();
    void dispatch(const InputHubEvent &event);
    // Removes the hooks and ends both threads; no signals afterwards.
    void close();
    std::unique_ptr<InputHubNative> native;
signals:
    void gesture(const QString &phase, int gesture_id, int x, int y);  // start / finish / cancel, physical pixels
    void moved(int gesture_id);  // once until take_position collects it
    void side_button(const QString &name);  // x1 / x2
    void hotkey(const QString &name);  // the name given to bind_hotkey
    void captured(const QStringList &modifiers, int vk);  // a key taken while recording
    void wheel(const QString &watcher, int x, int y, int delta, bool horizontal);  // delta in WHEEL_DELTA multiples
    void key(const QString &watcher, int vk, bool pressed);
    void foreground(quintptr hwnd);
    void failure(const QString &message);
private:
    void run();
    std::thread thread_;
    bool closed_ = false;
};

// Created on first call, GUI thread only; hooks install only once a feature uses them.
InputHub *input_hub();
// The hub if already created; never creates it.
InputHub *existing_input_hub();
void close_input_hub();
}
