#include "core/pin_request_queue.h"
#include "core/pin_arguments.h"

namespace jietuba {
PinRequestQueue::PinRequestQueue(Sink sink, int quiet_ms, QObject *parent) : QObject(parent), sink_(std::move(sink)) {
    timer_.setSingleShot(true);
    timer_.setInterval(quiet_ms);
    connect(&timer_, &QTimer::timeout, this, &PinRequestQueue::flush);
}

void PinRequestQueue::add(const QStringList &files) {
    pending_.append(files);
    timer_.start();
}

void PinRequestQueue::flush() {
    const QStringList files = unique_pin_paths(std::exchange(pending_, {}));
    if (!files.isEmpty()) sink_(files);
}
}
