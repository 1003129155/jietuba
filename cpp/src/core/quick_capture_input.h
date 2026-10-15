#pragma once
#include <QObject>
#include <QPoint>
#include <QStringList>
#include <optional>
#include <utility>
#include <vector>

// Input for the global mouse shortcuts: dragging with modifiers held. The hooks and the
// gesture state machine run natively in the shared input hub; this only connects them to
// the capture controller. Receivers connect the public signals queued and collect
// positions for moved through take_position.
namespace jietuba {
class InputHub;

class QuickCaptureInput : public QObject {
    Q_OBJECT
public:
    explicit QuickCaptureInput(QObject *parent = nullptr, InputHub *hub = nullptr);
    // Takes the latest position; notifications of newer gestures are unaffected.
    std::optional<QPoint> take_position(int gesture_id);
    bool dragging() const;
    // Whether a queued start/finish is still valid; cancel is always accepted.
    // Reconfiguring, cancelling and closing void gestures already finished whose finish
    // has not reached the UI yet.
    bool accepts(int gesture_id) const;
    // (modifier set, button) of the current gesture only; a new gesture may start before
    // an old one's start is delivered.
    std::optional<std::pair<QStringList, QString>> gesture_binding(int gesture_id) const;
    // Each binding is (modifier set, button); the held modifiers must match exactly.
    void configure(const std::vector<std::pair<QStringList, QString>> &bindings, bool enabled);
    void cancel();
    // The UI decides when input is unavailable; the hook checks no windows or modality.
    void set_blocked(bool blocked);
    // Turns gestures off. A press already swallowed still waits for its release before
    // the hook lets go.
    void close();
signals:
    // kind, gesture_id, physical pixel x/y
    void event(const QString &kind, int gesture_id, int x, int y);
    // gesture_id; at most one uncollected notification per gesture
    void moved(int gesture_id);
    void failure(const QString &message);
private:
    InputHub *hub_;
    bool closed_ = false;
    std::vector<QMetaObject::Connection> forwards_;
};
}
