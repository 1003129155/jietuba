#pragma once
#include "gif/video_settings.h"
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QRect>
#include <QStringList>
#include <QTimer>
#include <optional>
#include <utility>

// Controller of the separate MP4 recording process: it only passes commands and
// states, never video frames.
namespace jietuba {
constexpr int VIDEO_PROTOCOL = 1;
constexpr int VIDEO_MAX_MESSAGE = 16 * 1024;
constexpr int VIDEO_MAX_STDERR = 16 * 1024;

// Advice for the user; the raw error still reaches the log through failed.
QString failure_message(const QString &code, const QString &detail);

// The worker command line: this executable with --video-recorder-worker.
QStringList worker_command();

class VideoRecorder : public QObject {
    Q_OBJECT
public:
    static constexpr int START_TIMEOUT = 30000;
    static constexpr int STOP_TIMEOUT = 30000;
    static constexpr int COMMAND_TIMEOUT = 5000;
    // worker_command is an injection point for automated tests; the UI never replaces the component.
    explicit VideoRecorder(QObject *parent = nullptr, QStringList worker_command = {});
    bool active() const { return state_ != "idle"; }
    QString state() const { return state_; }
    QString stderr_tail() const { return QString::fromUtf8(stderr_); }
    bool start(const QRect &rect, const QString &output, const RecordingOptions &options);
    bool pause(bool paused);
    void stop();
    // On exit, waits a bounded time for muxing so no helper outlives the application.
    void shutdown();
signals:
    void ready();
    void progress(double seconds);
    void paused_changed(bool paused);
    void completed(const QString &path);
    void failed(const QString &code, const QString &message);
    void finished();
private:
    bool send(const QString &command);
    void read_stdout();
    void read_stderr();
    // Throws std::invalid_argument for a message that breaks the protocol.
    void handle_message(const QJsonObject &message);
    void fail(const QString &code, const QString &message);
    void on_process_error(QProcess::ProcessError error);
    void on_timeout();
    void kill_unresponsive();
    void on_finished(int exit_code, QProcess::ExitStatus exit_status);
    void finish(bool successful_exit);

    QStringList worker_command_;
    QProcess *process_ = nullptr;
    QString state_ = "idle";
    QByteArray stdout_;
    QByteArray stderr_;
    QString output_;
    std::optional<QString> complete_;
    std::optional<std::pair<QString, QString>> failure_;
    QTimer timer_;
    QTimer kill_timer_;
};
}
