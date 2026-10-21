#include "core/input_hub.h"
#include "core/input_hooks.h"
#include "core/logger.h"

namespace jietuba {
namespace {
using input::Button;
using input::Engine;
using input::Event;
using input::Events;
using input::Modifiers;

// Modifier names from the UI; each entry may itself be "+"-joined.
std::vector<std::string> modifier_names(const QStringList &modifiers) {
    std::vector<std::string> names;
    for (const QString &entry : modifiers) {
        for (const QString &part : entry.split('+')) {
            const QString name = part.trimmed();
            if (!name.isEmpty()) names.push_back(name.toStdString());
        }
    }
    return names;
}

QStringList to_qt(const std::vector<std::string> &names) {
    QStringList list;
    for (const auto &name : names) list << QString::fromStdString(name);
    return list;
}

class NativeInputHub : public InputHubNative {
public:
    std::optional<InputHubEvent> next_event() override {
        auto event = hooks_.next_event();
        if (!event) return std::nullopt;
        return to_hub_event(*event);
    }
    void close() override { hooks_.stop(); }
    bool configure_gestures(const std::vector<std::pair<QStringList, QString>> &bindings, bool enabled) override {
        std::vector<std::pair<Modifiers, Button>> parsed;
        bool known = true;
        for (const auto &[mods, button] : bindings) {
            auto modifiers = Modifiers::parse(modifier_names(mods));
            auto parsed_button = input::parse_button(button.toStdString());
            if (!modifiers || !parsed_button) {
                known = false;
                break;
            }
            parsed.emplace_back(*modifiers, *parsed_button);
        }
        bool valid = false;
        hooks_.with_engine([&](Engine &engine, Events &out) {
            // An unknown name invalidates the whole configuration, like a wrong modifier count.
            if (known) valid = engine.configure_gestures(std::move(parsed), enabled, out);
            else engine.configure_gestures({}, false, out);
        });
        return valid;
    }
    void set_blocked(bool blocked) override {
        hooks_.with_engine([&](Engine &engine, Events &out) { engine.set_blocked(blocked, out); });
    }
    void cancel_gesture() override {
        hooks_.with_engine([](Engine &engine, Events &out) { engine.cancel(out); });
    }
    std::optional<QPoint> take_position(quint64 gesture_id) override {
        std::optional<QPoint> position;
        hooks_.with_engine([&](Engine &engine, Events &) {
            if (auto p = engine.take_position(gesture_id)) position = QPoint(p->first, p->second);
        });
        return position;
    }
    bool dragging() override {
        bool dragging = false;
        hooks_.with_engine([&](Engine &engine, Events &) { dragging = engine.dragging(); });
        return dragging;
    }
    bool accepts(quint64 gesture_id) override {
        bool accepts = false;
        hooks_.with_engine([&](Engine &engine, Events &) { accepts = engine.accepts(gesture_id); });
        return accepts;
    }
    std::optional<std::pair<QStringList, QString>> gesture_binding(quint64 gesture_id) override {
        std::optional<std::pair<Modifiers, Button>> binding;
        hooks_.with_engine([&](Engine &engine, Events &) { binding = engine.gesture_binding(gesture_id); });
        if (!binding) return std::nullopt;
        return std::make_pair(to_qt(binding->first.names()), QString(input::button_name(binding->second)));
    }
    void configure_side_buttons(bool enabled, const QStringList &suppressed, bool capture_all) override {
        std::vector<Button> buttons;
        for (const QString &name : suppressed) {
            const auto button = input::parse_button(name.toStdString());
            if (button != Button::X1 && button != Button::X2)
                throw std::invalid_argument(("not a side button: " + suppressed.join(", ")).toStdString());
            buttons.push_back(*button);
        }
        hooks_.with_engine([&](Engine &engine, Events &) { engine.configure_side_buttons(enabled, buttons, capture_all); });
    }
    bool bind_hotkey(const QString &name, const QStringList &modifiers, quint32 vk) override {
        const auto parsed = Modifiers::parse(modifier_names(modifiers));
        if (!parsed) throw std::invalid_argument(("unknown modifier in " + modifiers.join(", ")).toStdString());
        bool bound = false;
        hooks_.with_engine([&](Engine &engine, Events &) { bound = engine.bind_hotkey(name.toStdString(), *parsed, vk); });
        return bound;
    }
    void unbind_hotkey(const QString &name) override {
        hooks_.with_engine([&](Engine &engine, Events &) { engine.unbind_hotkey(name.toStdString()); });
    }
    void configure_key_capture(bool enabled) override {
        hooks_.with_engine([&](Engine &engine, Events &) { engine.configure_key_capture(enabled); });
    }
    void watch_wheel(const QString &watcher, const std::optional<InputHubRect> &rect) override {
        std::optional<input::Rect> area;
        if (rect) area = input::Rect{rect->left, rect->top, rect->right, rect->bottom};
        hooks_.with_engine([&](Engine &engine, Events &) { engine.watch_wheel(watcher.toStdString(), area); });
    }
    void unwatch_wheel(const QString &watcher) override {
        hooks_.with_engine([&](Engine &engine, Events &) { engine.unwatch_wheel(watcher.toStdString()); });
    }
    void watch_keys(const QString &watcher, const std::vector<quint32> &keys) override {
        std::vector<uint32_t> watched(keys.begin(), keys.end());
        hooks_.with_engine([&](Engine &engine, Events &) { engine.watch_keys(watcher.toStdString(), std::move(watched)); });
    }
    void unwatch_keys(const QString &watcher) override {
        hooks_.with_engine([&](Engine &engine, Events &) { engine.unwatch_keys(watcher.toStdString()); });
    }
    void watch_mouse(const QString &watcher) override {
        hooks_.with_engine([&](Engine &engine, Events &) { engine.watch_mouse(watcher.toStdString()); });
    }
    void unwatch_mouse(const QString &watcher) override {
        hooks_.with_engine([&](Engine &engine, Events &) { engine.unwatch_mouse(watcher.toStdString()); });
    }
    std::optional<QPoint> take_pointer(const QString &watcher) override {
        std::optional<QPoint> position;
        hooks_.with_engine([&](Engine &engine, Events &) {
            if (auto p = engine.take_pointer(watcher.toStdString())) position = QPoint(p->first, p->second);
        });
        return position;
    }
    void watch_foreground(const QString &watcher) override {
        hooks_.with_engine([&](Engine &engine, Events &) { engine.watch_foreground(watcher.toStdString()); });
    }
    void unwatch_foreground(const QString &watcher) override {
        hooks_.with_engine([&](Engine &engine, Events &) { engine.unwatch_foreground(watcher.toStdString()); });
    }
    bool hooks_installed() override { return hooks_.hooks_installed(); }
    void set_test_marker(quintptr marker) override { hooks_.set_test_marker(marker); }
private:
    input::InputHooks hooks_;
};

InputHub *hub_instance = nullptr;
}

InputHubEvent to_hub_event(const input::Event &event) {
    InputHubEvent out;
    switch (event.kind) {
    case Event::Kind::GestureStart:
    case Event::Kind::GestureFinish:
    case Event::Kind::GestureCancel:
        out.kind = InputHubEvent::Gesture;
        out.text = event.kind == Event::Kind::GestureStart ? "start" : event.kind == Event::Kind::GestureFinish ? "finish" : "cancel";
        out.id = event.id;
        out.x = event.x;
        out.y = event.y;
        break;
    case Event::Kind::GestureMoved:
        out.kind = InputHubEvent::Moved;
        out.id = event.id;
        break;
    case Event::Kind::SideButton:
        out.kind = InputHubEvent::Side;
        out.text = input::button_name(event.button);
        break;
    case Event::Kind::Hotkey:
        out.kind = InputHubEvent::Hotkey;
        out.text = QString::fromStdString(event.text);
        break;
    case Event::Kind::Captured:
        out.kind = InputHubEvent::Captured;
        out.text = to_qt(event.modifiers.names()).join('+');
        out.value = static_cast<int>(event.vk);
        break;
    case Event::Kind::Wheel:
        out.kind = InputHubEvent::Wheel;
        out.text = QString::fromStdString(event.text);
        out.x = event.x;
        out.y = event.y;
        out.value = event.delta;
        out.flag = event.horizontal;
        break;
    case Event::Kind::Key:
        out.kind = InputHubEvent::Key;
        out.text = QString::fromStdString(event.text);
        out.value = static_cast<int>(event.vk);
        out.flag = event.pressed;
        break;
    case Event::Kind::MouseButton:
        out.kind = InputHubEvent::MouseButton;
        out.text = QString::fromStdString(event.text);
        out.value = static_cast<int>(event.button);
        out.flag = event.pressed;
        out.x = event.x;
        out.y = event.y;
        break;
    case Event::Kind::PointerMoved:
        out.kind = InputHubEvent::Pointer;
        out.text = QString::fromStdString(event.text);
        break;
    case Event::Kind::Foreground:
        out.kind = InputHubEvent::Foreground;
        out.hwnd = static_cast<quintptr>(event.hwnd);
        break;
    case Event::Kind::Failure:
        out.kind = InputHubEvent::Failure;
        out.text = QString::fromStdString(event.text);
        break;
    }
    return out;
}

InputHub::InputHub(std::unique_ptr<InputHubNative> native, QObject *parent) : QObject(parent), native(std::move(native)) {}

InputHub::~InputHub() { close(); }

void InputHub::start() { thread_ = std::thread([this] { run(); }); }

void InputHub::run() {
    // After close, next_event drains the queued events before returning nullopt.
    while (auto event = native->next_event()) {
        try {
            dispatch(*event);
        } catch (const std::exception &e) {
            log_exception(e, LogMsg("InputHub"));
        }
    }
}

void InputHub::dispatch(const InputHubEvent &event) {
    switch (event.kind) {
    case InputHubEvent::Gesture: emit gesture(event.text, static_cast<int>(event.id), event.x, event.y); break;
    case InputHubEvent::Moved: emit moved(static_cast<int>(event.id)); break;
    case InputHubEvent::Side: emit side_button(event.text); break;
    case InputHubEvent::Hotkey: emit hotkey(event.text); break;
    case InputHubEvent::Wheel: emit wheel(event.text, event.x, event.y, event.value, event.flag); break;
    case InputHubEvent::Key: emit key(event.text, event.value, event.flag); break;
    case InputHubEvent::Foreground: emit foreground(event.hwnd); break;
    case InputHubEvent::MouseButton:
        emit mouse_button(event.text, QString(input::button_name(static_cast<input::Button>(event.value))), event.flag, event.x, event.y);
        break;
    case InputHubEvent::Pointer: emit pointer_moved(event.text); break;
    case InputHubEvent::Failure: emit failure(event.text); break;
    case InputHubEvent::Captured: emit captured(event.text.split('+', Qt::SkipEmptyParts), event.value); break;
    }
}

void InputHub::close() {
    if (closed_) return;
    closed_ = true;
    native->close();
    if (thread_.joinable()) thread_.join();
}

InputHub *input_hub() {
    if (!hub_instance) {
        hub_instance = new InputHub(std::make_unique<NativeInputHub>());
        hub_instance->start();
    }
    return hub_instance;
}

InputHub *existing_input_hub() { return hub_instance; }

void close_input_hub() {
    if (hub_instance) {
        hub_instance->close();
        delete hub_instance;
        hub_instance = nullptr;
    }
}
}
