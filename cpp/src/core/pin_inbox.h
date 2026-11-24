#pragma once
#include "core/pin_arguments.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>

// Handing pin requests from a new process to the running one. The running instance, the primary
// one, holds a named mutex for as long as it lives and owns a message-only window; a process
// started with --pin sends its files to that window as a WM_COPYDATA message. Qt Network is not
// linked, and the window needs neither Qt nor a running event loop.
namespace jietuba {
// The window's name and the mutex's suffix: this executable's file name in lower case, so the
// lightweight and the full build each reach only their own instance.
QString own_instance_name();

// The message window of the primary instance. Messages are delivered where the window was made,
// by whatever message loop that thread runs; Qt's event loop does.
class PinInbox {
public:
    explicit PinInbox(const QString &instance_name = own_instance_name());
    ~PinInbox();
    PinInbox(const PinInbox &) = delete;
    PinInbox &operator=(const PinInbox &) = delete;
    bool valid() const { return window_ != nullptr; }
    // Requests received before this was called are delivered now, in order.
    void set_handler(std::function<void(const QStringList &)> handler);
    // The message's payload; false when it is not a request. A request whose id was seen before
    // (the sender timed out and sent it again) is accepted and ignored.
    bool receive(const QByteArray &payload);
private:
    void *window_ = nullptr;
    std::function<void(const QStringList &)> handler_;
    QList<QStringList> pending_;
    RecentRequestIds seen_;
};

enum class SendResult {
    // No instance has the window.
    NoWindow,
    // The window did not take the message within the time, or refused it.
    Failed,
    Delivered,
};
// One message to the window of instance_name; waits at most timeout_ms for the answer.
SendResult send_pin_request(const QString &instance_name, const PinRequest &request, unsigned timeout_ms);

// Which process is the primary instance. The holder keeps the mutex until it exits.
class PrimaryInstance {
public:
    explicit PrimaryInstance(const QString &instance_name = own_instance_name());
    ~PrimaryInstance();
    PrimaryInstance(const PrimaryInstance &) = delete;
    PrimaryInstance &operator=(const PrimaryInstance &) = delete;
    // Takes the mutex, waiting up to wait_ms for the holder to let go or end; true once held.
    // A mutex another instance made out of reach (an elevated one) counts as held by someone else.
    bool try_claim(unsigned wait_ms);
    // Only on the thread that claimed.
    void release();
    bool holds() const { return held_; }
private:
    void *mutex_ = nullptr;
    QString name_;
    bool held_ = false;
};

struct PinDelivery {
    unsigned deadline_ms = 10000;
    unsigned send_timeout_ms = 3000;
    unsigned poll_ms = 50;
    // With admin mode on: starts the elevated instance in place of this process; true when it
    // was started and its window is to be waited for.
    std::function<bool()> start_elevated;
};
enum class PinRoute {
    // The primary instance has the files; this process ends.
    Delivered,
    // Nobody was running: this process holds the mutex and is the primary instance.
    Primary,
    // The instance running did not answer within the deadline (it is stuck, or it is starting
    // and never came up).
    TimedOut,
};
// Tries until the deadline: the window of the running instance gets the request; with no window,
// a free mutex makes this process the primary instance, and a held one means that instance is
// still starting.
PinRoute route_pin_request(PrimaryInstance &primary, const QStringList &files, const PinDelivery &delivery = {}, const QString &instance_name = own_instance_name());
}
