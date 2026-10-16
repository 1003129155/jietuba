#pragma once
#include "core/shortcut_manager.h"
#include <QObject>
#include <QPoint>
#include <functional>

namespace jietuba {
class ToolSettingsManager;

// One-hotkey selected-text probe: sends a copy shortcut, then routes the copied text
// to the compact popup's translation mode, or opens its input mode when nothing
// usable arrives.
class SmartTranslationController : public QObject, public SelectionTranslator {
    Q_OBJECT
public:
    static constexpr int COPY_DELAY_MS = 55;
    static constexpr int FALLBACK_READ_DELAY_MS = 120;
    static constexpr int PROBE_TIMEOUT_MS = 500;
    explicit SmartTranslationController(QObject *parent = nullptr);
    bool probe_active() const { return probe_active_; }
    // Begins one non-blocking selection probe.
    void trigger();
    // The hotkey callback, recognizable by handlers that own a selection.
    SelectionTranslationHotkey hotkey() { return SelectionTranslationHotkey{this, [this] { trigger(); }}; }
    // Translates text from a caller owning its selection (self-drawn text layers the
    // clipboard shortcut cannot reach). Cancels any probe in flight.
    void translate_selection(const QString &text) override;
    // Consumes only the clipboard event belonging to an active probe.
    void on_clipboard_item(const QString &content_type, const QString &content);
    // Whether the shared clipboard history event stream is active; set by the
    // application. Unset means no monitoring, so the clipboard is read once instead.
    std::function<bool()> clipboard_monitor_available;

private:
    void dispatch_copy(int token);
    void read_clipboard_fallback(int token);
    // Sends Ctrl+Insert, the alternate copy shortcut, which does not raise SIGINT in consoles.
    static void send_copy_shortcut();
    void on_timeout(int token);
    bool is_current(int token) const { return probe_active_ && token == probe_token_; }
    QVariantMap translation_params() const;
    void open_compact(const QString &text);
    void open_compact_input(int token, const QString &reason);

    ToolSettingsManager *config_;
    bool probe_active_ = false;
    bool copy_dispatched_ = false;
    int probe_token_ = 0;
    QPoint cursor_position_;
};
}
