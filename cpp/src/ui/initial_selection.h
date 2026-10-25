#pragma once
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace jietuba {
class CanvasView;
class InitialSelectionGate : public QObject {
    Q_OBJECT
public:
    // Matches the backend's slow-provider threshold: a cold provider still gets the first
    // frame right, while one that hangs cannot keep the capture from showing.
    static constexpr int WAIT_MS = 300;
    explicit InitialSelectionGate(QObject *parent = nullptr);
    void start(CanvasView *view);
    void cancel();
    bool is_waiting() const { return !view_.isNull(); }
signals:
    void ready(CanvasView *view);
private:
    void check();
    void finish();
    QPointer<CanvasView> view_;
    QMetaObject::Connection result_connection_;
    QTimer poll_, deadline_;
    bool checking_ = false;
};
}
