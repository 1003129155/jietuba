#include "capture/display_watcher.h"
#include <QCoreApplication>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
class DisplayChangeFilter : public QAbstractNativeEventFilter {
public:
    explicit DisplayChangeFilter(std::function<void()> on_message) : on_message_(std::move(on_message)) {}
    bool nativeEventFilter(const QByteArray &event_type, void *message, qintptr *) override {
        if ((event_type == "windows_generic_MSG" || event_type == "windows_dispatcher_MSG") && static_cast<MSG *>(message)->message == WM_DISPLAYCHANGE)
            on_message_();
        return false;
    }
private:
    std::function<void()> on_message_;
};

DisplayChangeWatcher::DisplayChangeWatcher(std::function<void()> on_change, QObject *parent, int settle_ms) : QObject(parent), timer_(this) {
    timer_.setSingleShot(true);
    timer_.setInterval(settle_ms);
    connect(&timer_, &QTimer::timeout, this, [on_change = std::move(on_change)] { on_change(); });
    window_ = std::make_unique<QWindow>();
    window_->create();
    filter_ = std::make_unique<DisplayChangeFilter>([this] { timer_.start(); });
    QCoreApplication::instance()->installNativeEventFilter(filter_.get());
}

DisplayChangeWatcher::~DisplayChangeWatcher() { close(); }

void DisplayChangeWatcher::close() {
    if (filter_ && QCoreApplication::instance()) QCoreApplication::instance()->removeNativeEventFilter(filter_.get());
    timer_.stop();
    if (window_) window_->destroy();
}
}
