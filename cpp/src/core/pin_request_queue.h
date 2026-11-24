#pragma once
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <functional>

namespace jietuba {
// Files to pin as they arrive, one request per process an Explorer multi-selection starts. Each
// request restarts a short timer; when it runs out, everything that came in goes to the sink as one
// batch, so the pins open together and cascade instead of each landing on the same spot.
class PinRequestQueue : public QObject {
    Q_OBJECT
public:
    static constexpr int QUIET_MS = 300;
    using Sink = std::function<void(const QStringList &)>;
    explicit PinRequestQueue(Sink sink, int quiet_ms = QUIET_MS, QObject *parent = nullptr);
    void add(const QStringList &files);
private:
    void flush();
    Sink sink_;
    QTimer timer_;
    QStringList pending_;
};
}
