#pragma once
#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QTimer>
#include <QWindow>
#include <functional>
#include <memory>

// Display configuration changes: plugging monitors, resolution changes, HDR on/off.
namespace jietuba {
class DisplayChangeFilter;

// Calls on_change on the GUI thread after the display configuration changes.
// WM_DISPLAYCHANGE only goes to top-level windows and a tray program may have no visible
// one, so a hidden native window makes sure it arrives. One change often sends several
// messages; the callback runs once after settle_ms without new ones.
class DisplayChangeWatcher : public QObject {
    Q_OBJECT
public:
    DisplayChangeWatcher(std::function<void()> on_change, QObject *parent = nullptr, int settle_ms = 500);
    ~DisplayChangeWatcher() override;
    void close();
private:
    QTimer timer_;
    std::unique_ptr<QWindow> window_;
    std::unique_ptr<DisplayChangeFilter> filter_;
};
}
