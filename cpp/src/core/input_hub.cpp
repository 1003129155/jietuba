#include "core/input_hub.h"
#include "core/logger.h"
#include "core/native.h"

namespace jietuba {
namespace {
// The Rust hub from native/.
class RustInputHub : public InputHubNative {
public:
    RustInputHub() {
        char error[512] = {};
        hub_ = jb_input_hub_new(error, sizeof error);
        if (!hub_) throw std::runtime_error(error);
    }
    ~RustInputHub() override { jb_input_hub_free(hub_); }
    std::optional<InputHubEvent> next_event() override {
        JbInputEvent raw{};
        if (!jb_input_hub_next_event(hub_, -1, &raw)) return std::nullopt;
        InputHubEvent event;
        event.kind = static_cast<InputHubEvent::Kind>(raw.kind);
        event.text = QString::fromUtf8(raw.text);
        event.id = raw.id;
        event.x = raw.x;
        event.y = raw.y;
        event.value = raw.value;
        event.flag = raw.flag;
        event.hwnd = static_cast<quintptr>(raw.hwnd);
        return event;
    }
    void close() override { jb_input_hub_close(hub_); }
    bool configure_gestures(const std::vector<std::pair<QStringList, QString>> &bindings, bool enabled) override {
        std::vector<QByteArray> storage;
        std::vector<const char *> modifiers, buttons;
        storage.reserve(bindings.size() * 2);
        for (const auto &[mods, button] : bindings) {
            storage.push_back(mods.join('+').toUtf8());
            modifiers.push_back(storage.back().constData());
            storage.push_back(button.toUtf8());
            buttons.push_back(storage.back().constData());
        }
        return jb_input_hub_configure_gestures(hub_, modifiers.data(), buttons.data(), bindings.size(), enabled);
    }
    void set_blocked(bool blocked) override { jb_input_hub_set_blocked(hub_, blocked); }
    void cancel_gesture() override { jb_input_hub_cancel_gesture(hub_); }
    std::optional<QPoint> take_position(quint64 gesture_id) override {
        int32_t x = 0, y = 0;
        if (!jb_input_hub_take_position(hub_, gesture_id, &x, &y)) return std::nullopt;
        return QPoint(x, y);
    }
    bool dragging() override { return jb_input_hub_dragging(hub_); }
    bool accepts(quint64 gesture_id) override { return jb_input_hub_accepts(hub_, gesture_id); }
    std::optional<std::pair<QStringList, QString>> gesture_binding(quint64 gesture_id) override {
        char modifiers[128] = {}, button[64] = {};
        if (!jb_input_hub_gesture_binding(hub_, gesture_id, modifiers, sizeof modifiers, button, sizeof button)) return std::nullopt;
        return std::make_pair(QString::fromUtf8(modifiers).split('+', Qt::SkipEmptyParts), QString::fromUtf8(button));
    }
    void configure_side_buttons(bool enabled, const QStringList &suppressed, bool capture_all) override {
        std::vector<QByteArray> storage;
        std::vector<const char *> names;
        storage.reserve(suppressed.size());
        for (const auto &name : suppressed) {
            storage.push_back(name.toUtf8());
            names.push_back(storage.back().constData());
        }
        if (!jb_input_hub_configure_side_buttons(hub_, enabled, names.data(), names.size(), capture_all))
            throw std::invalid_argument(("not a side button: " + suppressed.join(", ")).toStdString());
    }
    bool bind_hotkey(const QString &name, const QStringList &modifiers, quint32 vk) override {
        const int result = jb_input_hub_bind_hotkey(hub_, name.toUtf8().constData(), modifiers.join('+').toUtf8().constData(), vk);
        if (result < 0) throw std::invalid_argument(("unknown modifier in " + modifiers.join(", ")).toStdString());
        return result == 1;
    }
    void unbind_hotkey(const QString &name) override { jb_input_hub_unbind_hotkey(hub_, name.toUtf8().constData()); }
    void configure_key_capture(bool enabled) override { jb_input_hub_configure_key_capture(hub_, enabled); }
    void watch_wheel(const QString &watcher, const std::optional<InputHubRect> &rect) override {
        if (rect) jb_input_hub_watch_wheel(hub_, watcher.toUtf8().constData(), true, rect->left, rect->top, rect->right, rect->bottom);
        else jb_input_hub_watch_wheel(hub_, watcher.toUtf8().constData(), false, 0, 0, 0, 0);
    }
    void unwatch_wheel(const QString &watcher) override { jb_input_hub_unwatch_wheel(hub_, watcher.toUtf8().constData()); }
    void watch_keys(const QString &watcher, const std::vector<quint32> &keys) override {
        jb_input_hub_watch_keys(hub_, watcher.toUtf8().constData(), keys.data(), keys.size());
    }
    void unwatch_keys(const QString &watcher) override { jb_input_hub_unwatch_keys(hub_, watcher.toUtf8().constData()); }
    void watch_foreground(const QString &watcher) override { jb_input_hub_watch_foreground(hub_, watcher.toUtf8().constData()); }
    void unwatch_foreground(const QString &watcher) override { jb_input_hub_unwatch_foreground(hub_, watcher.toUtf8().constData()); }
    bool hooks_installed() override { return jb_input_hub_hooks_installed(hub_); }
    void set_test_marker(quintptr marker) override { jb_input_hub_set_test_marker(hub_, marker); }
private:
    JbInputHub *hub_ = nullptr;
};

InputHub *hub_instance = nullptr;
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
        hub_instance = new InputHub(std::make_unique<RustInputHub>());
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
