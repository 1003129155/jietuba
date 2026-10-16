#include "core/quick_capture_input.h"
#include "core/input_hub.h"
#include <algorithm>

namespace jietuba {
QuickCaptureInput::QuickCaptureInput(QObject *parent, InputHub *hub) : QObject(parent), hub_(hub ? hub : input_hub()) {
    // Forwarded directly on the event thread; receivers queue only once.
    forwards_.push_back(connect(hub_, &InputHub::gesture, this, &QuickCaptureInput::event, Qt::DirectConnection));
    forwards_.push_back(connect(hub_, &InputHub::moved, this, &QuickCaptureInput::moved, Qt::DirectConnection));
    forwards_.push_back(connect(hub_, &InputHub::failure, this, &QuickCaptureInput::failure, Qt::DirectConnection));
}

std::optional<QPoint> QuickCaptureInput::take_position(int gesture_id) { return hub_->native->take_position(static_cast<quint64>(gesture_id)); }

bool QuickCaptureInput::dragging() const { return hub_->native->dragging(); }

bool QuickCaptureInput::accepts(int gesture_id) const { return hub_->native->accepts(static_cast<quint64>(gesture_id)); }

std::optional<std::pair<QStringList, QString>> QuickCaptureInput::gesture_binding(int gesture_id) const {
    auto binding = hub_->native->gesture_binding(static_cast<quint64>(gesture_id));
    if (!binding) return std::nullopt;
    // A frozenset in the Python version: compared without regard to order.
    std::sort(binding->first.begin(), binding->first.end());
    return binding;
}

void QuickCaptureInput::configure(const std::vector<std::pair<QStringList, QString>> &bindings, bool enabled) {
    if (closed_) return;
    std::vector<std::pair<QStringList, QString>> requested;
    for (auto [modifiers, button] : bindings) {
        std::sort(modifiers.begin(), modifiers.end());
        requested.emplace_back(modifiers, button);
    }
    if (!hub_->native->configure_gestures(requested, enabled) && enabled) emit failure("Quick capture requires one or two modifiers");
}

void QuickCaptureInput::cancel() { hub_->native->cancel_gesture(); }

void QuickCaptureInput::set_blocked(bool blocked) { hub_->native->set_blocked(blocked); }

void QuickCaptureInput::close() {
    if (closed_) return;
    closed_ = true;
    hub_->native->configure_gestures({}, false);
    for (const auto &connection : forwards_) disconnect(connection);
    forwards_.clear();
}
}
