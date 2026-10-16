// Stand-in for the updater helper in tests; JIETUBA_FAKE_UPDATER names the scenario.
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <string>

namespace {
QString transaction_id;

void write_raw(const QByteArray &bytes) {
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
}

void emit_event(const QString &event, const QJsonObject &data) {
    const QJsonObject message{{"protocol", 1}, {"transaction", transaction_id}, {"event", event}, {"data", data}};
    write_raw(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n");
}

QJsonObject release(const QString &tag) {
    return {{"tag_name", tag},          {"title", tag}, {"notes", "Bug fixes"}, {"url", "https://github.com/1003129155/jietuba/releases/latest"},
            {"asset_name", "package.zip"}, {"arch", "x64"},
            {"artifact", QJsonObject{{"urls", QJsonArray{"https://example.com/package.zip"}}, {"size", 123}}}};
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    _setmode(_fileno(stdout), _O_BINARY);
    const QStringList args = app.arguments();
    const QString scenario = qEnvironmentVariable("JIETUBA_FAKE_UPDATER");
    const qsizetype at = args.indexOf("--transaction");
    transaction_id = at >= 0 && at + 1 < args.size() ? args.at(at + 1) : QString();
    if (const QString record = qEnvironmentVariable("JIETUBA_FAKE_UPDATER_ARGS"); !record.isEmpty()) {
        QFile file(record);
        if (file.open(QIODevice::WriteOnly)) file.write(args.mid(1).join('\n').toUtf8());
    }
    if (scenario == "lock") {
        // A session lock left by a process that died without unlocking.
        QLockFile lease(args.value(1));
        lease.setStaleLockTime(0);
        if (!lease.tryLock(0)) return 2;
        std::_Exit(0);
    }
    if (scenario == "check-available" || scenario == "check-up-to-date") {
        const QString tag = qEnvironmentVariable("JIETUBA_FAKE_TAG", "v99.0.0");
        transaction_id.clear();
        emit_event(scenario == "check-available" ? "available" : "up_to_date", release(tag));
        return 0;
    }
    if (scenario == "check-error") {
        emit_event("error", {{"code", "network"}, {"message", "network unavailable"}});
        return 1;
    }
    if (scenario == "protocol") {
        emit_event("progress", {{"received", 50}, {"total", 100}});
        std::string line;
        if (!std::getline(std::cin, line)) return 4;
        const QJsonObject control = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        if (control.value("transaction").toString() != transaction_id || control.value("protocol").toInt() != 1) return 5;
        if (control.value("command").toString() == "cancel") {
            emit_event("error", {{"code", "cancelled"}, {"message", "下载已取消"}});
            return 1;
        }
        emit_event("downloaded", {});
        return 0;
    }
    if (scenario == "chunked") {
        const QJsonObject message{{"protocol", 1}, {"transaction", transaction_id}, {"event", "progress"}, {"data", QJsonObject{{"received", 1}}}};
        const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n";
        write_raw(line.left(20));
        QThread::msleep(300);
        write_raw(line.mid(20));
        QThread::msleep(300);
        write_raw(R"({"protocol":1,"transaction":"other"})"
                  "\n");
        QThread::msleep(10000);
        return 0;
    }
    if (scenario == "large") {
        write_raw(QByteArray(65537, 'x'));
        QThread::msleep(10000);
        return 0;
    }
    if (scenario == "stderr") {
        std::fputs("diagnostic", stderr);
        return 1;
    }
    if (scenario == "sleep") {
        QThread::msleep(10000);
        return 0;
    }
    if (scenario == "downloaded") {
        emit_event("downloaded", {});
        return 0;
    }
    if (scenario == "fail") {
        emit_event("error", {{"code", "network"}, {"message", "下载失败"}});
        return 1;
    }
    return 3;
}
