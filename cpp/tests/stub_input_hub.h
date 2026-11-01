#pragma once
#include "core/input_hub.h"
#include <QPoint>
#include <QStringList>
#include <memory>
#include <optional>
#include <utility>

// A hub whose native side does nothing but remember what was watched, so the tests feed the events.
class StubHubNative : public jietuba::InputHubNative {
public:
    QStringList mouse_watchers;
    std::optional<QPoint> pointer;
    std::optional<jietuba::InputHubEvent> next_event() override { return std::nullopt; }
    void close() override {}
    bool configure_gestures(const std::vector<std::pair<QStringList, QString>> &, bool) override { return true; }
    void set_blocked(bool) override {}
    void cancel_gesture() override {}
    std::optional<QPoint> take_position(quint64) override { return std::nullopt; }
    bool dragging() override { return false; }
    bool accepts(quint64) override { return false; }
    std::optional<std::pair<QStringList, QString>> gesture_binding(quint64) override { return std::nullopt; }
    void configure_side_buttons(bool, const QStringList &, bool) override {}
    bool bind_hotkey(const QString &, const QStringList &, quint32) override { return true; }
    void unbind_hotkey(const QString &) override {}
    void configure_key_capture(bool) override {}
    void watch_wheel(const QString &, const std::optional<jietuba::InputHubRect> &) override {}
    void unwatch_wheel(const QString &) override {}
    void watch_keys(const QString &, const std::vector<quint32> &) override {}
    void unwatch_keys(const QString &) override {}
    void watch_mouse(const QString &watcher) override {
        if (!mouse_watchers.contains(watcher)) mouse_watchers << watcher;
    }
    void unwatch_mouse(const QString &watcher) override { mouse_watchers.removeAll(watcher); }
    std::optional<QPoint> take_pointer(const QString &) override { return std::exchange(pointer, std::nullopt); }
    void watch_foreground(const QString &) override {}
    void unwatch_foreground(const QString &) override {}
    bool hooks_installed() override { return false; }
    void set_test_marker(quintptr) override {}
};

// What the hook thread would send to `watcher` unless another watcher is named.
struct StubHub {
    StubHubNative *native = nullptr;
    std::unique_ptr<jietuba::InputHub> hub;
    QString watcher;
    explicit StubHub(QString watcher) : watcher(std::move(watcher)) {
        auto stub = std::make_unique<StubHubNative>();
        native = stub.get();
        hub = std::make_unique<jietuba::InputHub>(std::move(stub));
    }
    // The pointer moved to at.
    void move_to(const QPoint &at) {
        native->pointer = at;
        jietuba::InputHubEvent event;
        event.kind = jietuba::InputHubEvent::Pointer;
        event.text = watcher;
        hub->dispatch(event);
    }
    // A button pressed or released; button is an input::Button: 0 left, 1 middle, 2 right.
    void button(int button, bool pressed, const QPoint &at, const QString &to = {}) {
        jietuba::InputHubEvent event;
        event.kind = jietuba::InputHubEvent::MouseButton;
        event.text = to.isEmpty() ? watcher : to;
        event.value = button;
        event.flag = pressed;
        event.x = at.x();
        event.y = at.y();
        hub->dispatch(event);
    }
};
