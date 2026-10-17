#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace jietuba {
// A fresh copy of the bundled updater helper in the cache; the copy, not the exe resource, runs,
// and the worker keeps using it after this process exits. Throws std::runtime_error.
QString helper_path();
// Whether this build bundles the helper (JIETUBA_UPDATES); without it one-click updates are off.
bool updater_bundled();
// Tests copy a fake helper instead of the bundled one.
void set_helper_source_for_test(const QString &path);
// Release package that updates fetch: always "lite". PP-OCR is a pack beside the exe, which
// updates leave in place; the reference's "full" package is its Python build.
QString app_variant();

// One updater helper process speaking the JSON-lines protocol (one transaction per run).
class UpdaterProcess : public QObject {
    Q_OBJECT
public:
    explicit UpdaterProcess(QObject *parent = nullptr);
    // Virtual for the controller tests, which replace the helper process.
    virtual bool busy() const { return busy_; }
    virtual bool start(const QString &command, const QStringList &arguments, int timeout_ms = 900000);
    virtual bool send(const QString &command);
    virtual void cancel();
    // The helper reports a stable code; its own message is Chinese and only goes to the log.
    virtual QString error_text(const QJsonObject &payload) const;
    QString transaction;
    QProcess *process() { return &process_; }
signals:
    void event_received(const QString &event, const QString &transaction, const QJsonObject &data);
    void failed(const QString &reason);
    void finished();
private:
    void fail(const QString &reason);
    void read();
    void read_stderr();
    void on_process_error(QProcess::ProcessError error);
    void on_timeout();
    void on_finished(int code, QProcess::ExitStatus status);
    void cleanup();
    QProcess process_;
    QTimer timer_;
    QByteArray buffer_;
    QByteArray stderr_;
    bool terminal_ = false;
    bool error_ = false;
    bool busy_ = false;
    bool handed_off_ = false;
    bool downloaded_ = false;
    QString helper_;
    QString command_;
};
}
