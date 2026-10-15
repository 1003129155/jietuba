#include "gif/video_recorder.h"
#include "capture/capture_service.h"
#include "core/i18n.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <cmath>
#include <set>
#include <stdexcept>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "VideoRecorder"); }

// A JSON number that is not a boolean, finite and not negative.
bool valid_elapsed(const QJsonValue &value) { return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0; }

// str() of a JSON value: strings as they are, other values as their JSON text.
QString value_text(const QJsonValue &value) {
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? "True" : "False";
    if (value.isNull()) return "None";
    if (value.isDouble()) {
        const double number = value.toDouble();
        return number == std::floor(number) && std::abs(number) < 1e15 ? QString::number(static_cast<qint64>(number)) : QString::number(number);
    }
    return QString::fromUtf8(value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact) : QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
}

QString absolute_path(const QString &path) { return QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()); }
}

QString failure_message(const QString &code, const QString &detail) {
    static const std::pair<const char *, const char *> messages[] = {
        {"helper_missing", "录制组件不存在，请重新安装或从当前源码构建。"},
        {"output_exists", "目标文件已存在，请选择新的文件名。"},
        {"protocol", "录制进程返回了无效的状态消息。"},
        {"audio_device", "找不到可用的播放设备，请连接音箱或耳机，或在视频设置中关闭系统声音后重试。"},
        {"audio_format", "当前播放设备的音频格式不受支持，请在 Windows 声音设置中选择立体声格式，或关闭系统声音后重试。"},
        {"audio", "系统声音录制失败，请检查播放设备，或在视频设置中关闭系统声音后重试。"},
        {"audio_timestamp", "播放设备的声音时间戳异常，录制已停止。请重新连接设备，或关闭系统声音后重试。"},
        {"encoding", "视频编码失败，请尝试降低分辨率或帧率，或在视频设置中关闭硬件编码后重试。"},
        {"capture", "无法采集录制区域，请检查屏幕连接和选区后重试。"},
        {"output", "无法创建视频文件，请检查保存位置的写入权限和剩余空间。"},
        {"disk_full", "保存位置空间不足，请释放空间或选择其他磁盘后重试。"},
        {"arguments", "录制参数或区域无效，请重新调整选区和视频设置。"},
    };
    for (const auto &[key, message] : messages)
        if (code == key) return tr_text(message);
    static const std::set<QString> passthrough{"invalid_region", "process_io", "cancelled", "start_failed", "process_crashed", "start_timeout",
                                               "finalize_timeout", "command_timeout", "incomplete", "release_failed", "release_timeout"};
    if (passthrough.count(code)) return detail;
    return tr_text("视频录制失败，请重试。若问题持续，请查看日志中的详细错误。");
}

QStringList worker_command() { return {QCoreApplication::applicationFilePath(), "--video-recorder-worker"}; }

VideoRecorder::VideoRecorder(QObject *parent, QStringList worker_command) : QObject(parent), worker_command_(std::move(worker_command)), timer_(this), kill_timer_(this) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, &VideoRecorder::on_timeout);
    kill_timer_.setSingleShot(true);
    connect(&kill_timer_, &QTimer::timeout, this, &VideoRecorder::kill_unresponsive);
}

bool VideoRecorder::start(const QRect &rect, const QString &output, const RecordingOptions &options) {
    if (active()) return false;
    const QStringList command = worker_command_.isEmpty() ? worker_command() : worker_command_;
    const QString target = absolute_path(output);
    if (command.isEmpty() || !QFileInfo(command.first()).isFile()) {
        emit failed("helper_missing", tr_text("录制组件不存在，请重新安装或从当前源码构建。"));
        return false;
    }
    if (QFileInfo::exists(target)) {
        emit failed("output_exists", tr_text("目标文件已存在，请选择新的文件名。"));
        return false;
    }
    // Inset by the border as GIF does, trimming only a trailing odd pixel, so the blue / red frame stays out.
    const int width = (rect.width() - 2) & ~1;
    const int height = (rect.height() - 2) & ~1;
    if (width < 2 || height < 2) {
        emit failed("invalid_region", tr_text("录制区域太小，请重新调整。"));
        return false;
    }
    stdout_.clear();
    stderr_.clear();
    complete_.reset();
    failure_.reset();
    output_ = target;
    state_ = "starting";
    auto *process = new QProcess(this);
    process_ = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process->setWorkingDirectory(QFileInfo(target).absolutePath());
    connect(process, &QProcess::readyReadStandardOutput, this, &VideoRecorder::read_stdout);
    connect(process, &QProcess::readyReadStandardError, this, &VideoRecorder::read_stderr);
    connect(process, &QProcess::errorOccurred, this, &VideoRecorder::on_process_error);
    connect(process, &QProcess::finished, this, &VideoRecorder::on_finished);
    QStringList args = command.mid(1);
    args << "--output" << target << "--left" << QString::number(rect.x() + 1) << "--top" << QString::number(rect.y() + 1) << "--width"
         << QString::number(width) << "--height" << QString::number(height) << "--fps" << QString::number(options.fps) << "--bitrate"
         << QString::number(options.bitrate) << "--audio" << (options.system_audio ? "system" : "none") << "--encoder"
         << (options.hardware ? "auto" : "software") << "--cursor" << (options.cursor ? "on" : "off")
         // The same engine setting as screenshots and GIF; decided here, the worker does not query HDR state.
         << "--capture" << (uses_hdr_engine() ? "dxgi" : "gdi");
    timer_.start(START_TIMEOUT);
    process->start(command.first(), args);
    return true;
}

bool VideoRecorder::send(const QString &command) {
    if (!process_) return false;
    const QByteArray line = QString(R"({"protocol":%1,"command":"%2"})").arg(VIDEO_PROTOCOL).arg(command).toUtf8() + "\n";
    return process_->write(line) >= 0;
}

bool VideoRecorder::pause(bool paused) {
    if ((paused && state_ != "recording") || (!paused && state_ != "paused")) return false;
    state_ = paused ? "pausing" : "resuming";
    timer_.start(COMMAND_TIMEOUT);
    if (!send(paused ? "pause" : "resume")) fail("process_io", tr_text("无法发送录制命令。"));
    return true;
}

void VideoRecorder::stop() {
    if (!active() || state_ == "stopping") return;
    state_ = "stopping";
    timer_.start(STOP_TIMEOUT);
    if (!send("stop")) {
        fail("process_io", tr_text("无法发送停止命令。"));
    } else {
        // No commands follow a stop; Qt drains written data before closing the pipe, so the helper reads EOF.
        process_->closeWriteChannel();
    }
}

void VideoRecorder::read_stdout() {
    if (!process_) return;
    stdout_.append(process_->readAllStandardOutput());
    while (stdout_.contains('\n')) {
        const int end = static_cast<int>(stdout_.indexOf('\n'));
        if (end > VIDEO_MAX_MESSAGE) {
            stdout_.clear();
            fail("protocol", tr_text("录制进程返回了过大的状态消息。"));
            return;
        }
        QByteArray line = stdout_.left(end);
        while (line.endsWith('\r')) line.chop(1);
        stdout_.remove(0, end + 1);
        try {
            QJsonParseError error{};
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError) throw std::invalid_argument("json");
            if (!document.isObject()) throw std::invalid_argument("protocol");
            handle_message(document.object());
        } catch (const std::invalid_argument &) {
            fail("protocol", tr_text("录制进程返回了无效的状态消息。"));
            return;
        }
    }
    if (stdout_.size() > VIDEO_MAX_MESSAGE) {
        stdout_.clear();
        fail("protocol", tr_text("录制进程返回了过大的状态消息。"));
    }
}

void VideoRecorder::read_stderr() {
    if (!process_) return;
    stderr_.append(process_->readAllStandardError());
    if (stderr_.size() > VIDEO_MAX_STDERR) stderr_ = stderr_.right(VIDEO_MAX_STDERR);
}

void VideoRecorder::handle_message(const QJsonObject &message) {
    const auto protocol = message.value("protocol");
    if (!protocol.isDouble() || protocol.toDouble() != VIDEO_PROTOCOL) throw std::invalid_argument("protocol");
    const auto data_value = message.contains("data") ? message.value("data") : QJsonValue(QJsonObject());
    if (!data_value.isObject()) throw std::invalid_argument("data");
    const auto data = data_value.toObject();
    if (!message.contains("event")) throw std::invalid_argument("event");
    const auto event_value = message.value("event");
    if (failure_) return;
    const QString event = event_value.isString() ? event_value.toString() : QString();
    if (event == "ready") {
        if (state_ == "starting") {
            timer_.stop();
            state_ = "recording";
            emit ready();
        } else if (state_ != "stopping") {
            throw std::invalid_argument("unexpected ready");
        }
    } else if (event == "progress") {
        if (!data.contains("elapsed_ms")) throw std::invalid_argument("elapsed");
        const auto elapsed = data.value("elapsed_ms");
        if (!valid_elapsed(elapsed)) throw std::invalid_argument("elapsed");
        if (state_ == "recording" || state_ == "paused" || state_ == "pausing" || state_ == "resuming") emit progress(elapsed.toDouble() / 1000);
    } else if (event == "paused" || event == "resumed") {
        const bool paused = event == "paused";
        const QString expected = paused ? "pausing" : "resuming";
        if (state_ == expected) {
            const auto elapsed = data.value("elapsed_ms");
            if (!valid_elapsed(elapsed)) throw std::invalid_argument("elapsed");
            timer_.stop();
            state_ = paused ? "paused" : "recording";
            emit paused_changed(paused);
            emit progress(elapsed.toDouble() / 1000);
        } else if (state_ != "stopping") {
            throw std::invalid_argument("unexpected ack");
        }
    } else if (event == "complete") {
        if (!data.value("output").isString()) throw std::invalid_argument("output");
        const QString result_path = absolute_path(data.value("output").toString());
        if (data.value("finalized") != QJsonValue(true) || result_path.compare(output_, Qt::CaseInsensitive) != 0)
            throw std::invalid_argument("incomplete output");
        complete_ = result_path;
        state_ = "stopping";
        timer_.start(STOP_TIMEOUT);
    } else if (event == "error") {
        fail(data.contains("code") ? value_text(data.value("code")) : QString("backend"),
             data.contains("message") ? value_text(data.value("message")) : tr_text("录制失败。"));
    } else if (event == "cancelled") {
        fail("cancelled", tr_text("录制已取消，未保存视频。"));
    } else {
        throw std::invalid_argument("unknown event");
    }
}

void VideoRecorder::fail(const QString &code, const QString &message) {
    if (!failure_) failure_ = std::make_pair(code, message);
    timer_.stop();
    state_ = "stopping";
    if (process_) {
        send("cancel");
        process_->closeWriteChannel();
        kill_timer_.start(2000);
    }
}

void VideoRecorder::on_process_error(QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
        failure_ = std::make_pair(QString("start_failed"), tr_text("无法启动录制进程。"));
        finish(false);
    } else if (error == QProcess::Crashed) {
        fail("process_crashed", tr_text("录制进程意外退出，视频可能未完成。"));
    }
}

void VideoRecorder::on_timeout() {
    if (state_ == "pausing" || state_ == "resuming") {
        fail("command_timeout", tr_text("暂停或恢复录制超时，录制已停止，视频未确认保存成功。"));
    } else {
        const QString code = state_ == "starting" ? "start_timeout" : "finalize_timeout";
        fail(code, tr_text("录制进程响应超时，视频未确认保存成功。"));
    }
}

void VideoRecorder::kill_unresponsive() {
    // Only the helper this controller started is killed; a failed video is never reported as saved.
    if (process_) process_->kill();
}

void VideoRecorder::on_finished(int exit_code, QProcess::ExitStatus exit_status) {
    read_stdout();
    read_stderr();
    finish(exit_code == 0 && exit_status == QProcess::NormalExit);
}

void VideoRecorder::finish(bool successful_exit) {
    if (!process_) return;
    timer_.stop();
    kill_timer_.stop();
    bool success = successful_exit && complete_ && !complete_->isEmpty() && !failure_;
    if (success) {
        const QFileInfo info(output_);
        success = info.isFile() && info.size() > 0;
    }
    const auto failure = failure_.value_or(std::make_pair(QString("incomplete"), tr_text("录制未正常完成，未确认视频保存成功。")));
    auto *process = process_;
    process_ = nullptr;
    process->deleteLater();
    state_ = "idle";
    if (success) emit completed(*complete_);
    else emit failed(failure.first, failure.second);
    emit finished();
}

void VideoRecorder::shutdown() {
    if (!active() || !process_) return;
    stop();
    auto *process = process_;
    if (!process) return;
    if (!process->waitForFinished(STOP_TIMEOUT)) {
        failure_ = std::make_pair(QString("finalize_timeout"), tr_text("退出时录制未能完成。"));
        process->kill();
        process->waitForFinished(2000);
    }
}
}
