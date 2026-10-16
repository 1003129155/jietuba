#pragma once
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QStringList>
#include <QThread>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// Global mouse shortcuts: drag a selection with modifiers held while the desktop stays
// live; on release the region is captured and the bound action runs.
namespace jietuba {
class MainApp;
class QuickCaptureInput;
class QuickCaptureOverlay;
class SystemCursor;

// The physical-pixel desktop: Qt scaling is off, so capture coordinates are physical.
QRect desktop_bounds();
// The overlay is hidden on the GUI thread; the worker waits one composition so the frame
// is not captured.
void flush_desktop();

class QuickCaptureWorker : public QThread {
    Q_OBJECT
public:
    QuickCaptureWorker(const QRect &region, const QString &action, QObject *parent = nullptr) : QThread(parent), region(region), action(action) {}
    QRect region;
    QString action;
    bool include_cursor = false;
    std::shared_ptr<SystemCursor> cursor;
signals:
    void captured(const QImage &image, const QRectF &bounds);
    void failed(const QString &message);
protected:
    void run() override;
};

// Holds the input hook, one transparent overlay and at most one capture thread.
class QuickCaptureController : public QObject {
    Q_OBJECT
public:
    explicit QuickCaptureController(MainApp *main_app);
    bool busy() const { return active_.has_value() || worker_ != nullptr; }
    // Applies the saved settings, including the tray's "disable global hotkeys".
    void refresh();
    void suspend();
    // From queueing a normal capture until its window exists, mouse shortcuts do nothing.
    void set_capture_pending(bool pending);
    // Called when a capture is queued and when sessions start or end. The only
    // application-modal window is the welcome wizard, during which this is suspended.
    void sync_input_availability();
    void cancel();
    void close();
    MainApp *main_app;
    QuickCaptureInput *input;
    QuickCaptureOverlay *overlay = nullptr;
private:
    bool blocked() const;
    void on_input(const QString &kind, int token, int x, int y);
    void on_moved(int token);
    bool hide_selection();
    // Requested once the capture thread has exited; a new drag or a normal capture
    // postpones it through the shared busy check.
    void schedule_working_set_trim();
    void capture(const QRect &region);
    void deliver(const QImage &image, const QRectF &bounds);
    void open_normal_capture(const QImage &image, const QRectF &bounds, std::shared_ptr<SystemCursor> cursor, const QRect &region);
    void worker_finished(QuickCaptureWorker *worker);
    void on_failure(const QString &message);

    std::optional<int> active_;
    QuickCaptureWorker *worker_ = nullptr;
    bool result_valid_ = false;
    bool closed_ = false;
    bool enabled_ = false;
    std::vector<std::pair<std::pair<QStringList, QString>, QString>> actions_;
    QString active_action_;
    QPoint start_;
    QRect bounds_;
    bool capture_pending_ = false;
};
}
