#include "video/worker.h"
#include "core/native.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba::video {
namespace {
constexpr int PROTOCOL = 1;
constexpr size_t MAX_COMMAND = 2048;
constexpr size_t MAX_MESSAGE = 16 * 1024;

// A failure carrying the stable error code reported to the parent.
struct WorkerError : std::runtime_error {
    WorkerError(std::string code, const std::string &message) : std::runtime_error(message), code(std::move(code)) {}
    std::string code;
};

// Media Foundation is delay loaded so the app itself starts without the Media Feature Pack;
// a missing DLL is reported here, as the original reports a failed `import video_recorder`.
void require_media_foundation() {
    for (const wchar_t *dll : {L"mfplat.dll", L"mfreadwrite.dll"}) {
        if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            throw WorkerError("helper_missing", "DLL load failed while importing video_recorder: " +
                                                    std::system_category().message(static_cast<int>(GetLastError())));
    }
}

// Writes one event line; the pipe may take it in pieces.
void emit_line(HANDLE output, const QByteArray &message) {
    if (static_cast<size_t>(message.size()) > MAX_MESSAGE) throw std::invalid_argument("Recording event exceeds 16384 bytes");
    const char *data = message.constData();
    qsizetype left = message.size();
    while (left > 0) {
        DWORD written = 0;
        if (!WriteFile(output, data, static_cast<DWORD>(left), &written, nullptr) || written == 0)
            throw std::runtime_error("Recording status pipe is closed");
        data += written;
        left -= written;
    }
}

void emit_event(HANDLE output, const char *event, const QByteArray &data_json) {
    emit_line(output, QByteArray(R"({"protocol":)") + QByteArray::number(PROTOCOL) + R"(,"event":")" + event + R"(","data":)" + data_json + "}\n");
}

void emit_error(HANDLE output, const QString &code, const QString &message) {
    const QJsonObject data{{"code", code}, {"message", message.left(1024)}};
    emit_event(output, "error", QJsonDocument(data).toJson(QJsonDocument::Compact));
}

// The control pipe is read by available bytes, so stopping does not wait for the parent to close stdin.
// nullopt: nothing yet; empty: end of input.
std::optional<std::string> read_available(HANDLE input) {
    DWORD available = 0;
    if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED) return std::string();
        throw std::runtime_error("PeekNamedPipe failed: " + std::to_string(error));
    }
    if (!available) return std::nullopt;
    char buffer[256];
    DWORD read = 0;
    if (!ReadFile(input, buffer, std::min<DWORD>(available, sizeof buffer), &read, nullptr)) throw std::runtime_error("ReadFile failed: " + std::to_string(GetLastError()));
    return std::string(buffer, read);
}

// Top-level keys and their raw value text, so duplicate keys and non-integer protocols are caught.
std::vector<std::pair<QString, QByteArray>> top_level_fields(const QByteArray &line) {
    std::vector<std::pair<QString, QByteArray>> fields;
    int depth = 0;
    bool in_string = false, escaped = false;
    qsizetype key_start = -1, value_start = -1;
    QString key;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') {
                in_string = false;
                if (depth == 1 && value_start < 0 && key_start >= 0) {
                    key = QJsonDocument::fromJson("[" + line.mid(key_start, i - key_start + 1) + "]").array().at(0).toString();
                    key_start = -1;
                }
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            if (depth == 1 && value_start < 0) key_start = i;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (depth == 1 && value_start >= 0) fields.emplace_back(key, line.mid(value_start, i - value_start).trimmed());
            --depth;
        } else if (depth == 1 && c == ':') {
            value_start = i + 1;
        } else if (depth == 1 && c == ',') {
            if (value_start >= 0) fields.emplace_back(key, line.mid(value_start, i - value_start).trimmed());
            value_start = -1;
        }
    }
    return fields;
}

// Reads control commands on a separate thread while the recorder runs.
class Controls {
public:
    Controls(HANDLE input, const JbVideoRecorder *recorder) : input_(input), recorder_(recorder) {}
    ~Controls() { close(); }
    void start() { thread_ = std::thread([this] { read(); }); }
    void close() {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    std::optional<std::string> error() const {
        std::lock_guard lock(mutex_);
        return error_;
    }
private:
    void invalid(const std::string &message) {
        {
            std::lock_guard lock(mutex_);
            error_ = message;
        }
        // A protocol failure still finalises the frames written so far, but never reports success.
        jb_video_recorder_stop(recorder_);
    }
    // True when the command ends the control loop.
    bool command(const QByteArray &line) {
        QJsonParseError parse{};
        const auto document = QJsonDocument::fromJson(line, &parse);
        if (parse.error != QJsonParseError::NoError) throw std::invalid_argument(parse.errorString().toStdString());
        const auto fields = top_level_fields(line);
        std::map<QString, QByteArray> values;
        for (const auto &[key, raw] : fields)
            if (!values.emplace(key, raw).second) throw std::invalid_argument("Duplicate command field");
        static const std::set<QString> commands{"pause", "resume", "stop", "cancel"};
        const auto object = document.object();
        if (!document.isObject() || values.size() != 2 || !values.count("protocol") || !values.count("command") || values["protocol"] != QByteArray::number(PROTOCOL)
            || !object.value("command").isString() || !commands.count(object.value("command").toString()))
            throw std::invalid_argument("Invalid recording command");
        const QString name = object.value("command").toString();
        if (name == "pause") jb_video_recorder_pause(recorder_);
        else if (name == "resume") jb_video_recorder_resume(recorder_);
        else if (name == "stop") jb_video_recorder_stop(recorder_);
        else jb_video_recorder_cancel(recorder_);
        return name == "stop" || name == "cancel";
    }
    void read() {
        QByteArray line;
        try {
            while (true) {
                {
                    std::lock_guard lock(mutex_);
                    if (done_) return;
                }
                const auto chunk = read_available(input_);
                if (!chunk) {
                    std::unique_lock lock(mutex_);
                    wake_.wait_for(lock, std::chrono::milliseconds(10), [this] { return done_; });
                    continue;
                }
                if (chunk->empty()) {
                    if (!line.isEmpty()) invalid("Incomplete command at EOF");
                    else jb_video_recorder_stop(recorder_);
                    return;
                }
                for (const char byte : *chunk) {
                    if (byte == '\n') {
                        if (command(line)) return;
                        line.clear();
                    } else {
                        if (static_cast<size_t>(line.size()) == MAX_COMMAND) throw std::invalid_argument("Command exceeds 2048 bytes");
                        line.append(byte);
                    }
                }
            }
        } catch (const std::exception &error) {
            invalid(error.what());
        }
    }
    HANDLE input_;
    const JbVideoRecorder *recorder_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool done_ = false;
    std::optional<std::string> error_;
    std::thread thread_;
};

struct Arguments {
    std::string output;
    qint64 left = 0, top = 0, width = 0, height = 0;
    qint64 fps = 30, bitrate = 4000000;
    std::string audio = "system", encoder = "auto", cursor = "on", capture = "dxgi";
};

qint64 parse_int(const std::string &name, const std::string &text) {
    bool ok = false;
    const qint64 value = QString::fromStdString(text).trimmed().toLongLong(&ok);
    if (!ok) throw std::invalid_argument("argument --" + name + ": invalid int value: '" + text + "'");
    return value;
}

Arguments parse_arguments(const std::vector<std::string> &argv) {
    Arguments args;
    std::set<std::string> seen;
    static const std::map<std::string, std::set<std::string>> choices{
        {"audio", {"system", "none"}}, {"encoder", {"auto", "software"}}, {"cursor", {"on", "off"}}, {"capture", {"dxgi", "gdi"}}};
    static const std::set<std::string> ints{"left", "top", "width", "height", "fps", "bitrate"};
    for (size_t i = 0; i < argv.size(); ++i) {
        std::string token = argv[i], value;
        if (token.rfind("--", 0) != 0) throw std::invalid_argument("unrecognized arguments: " + token);
        const auto equals = token.find('=');
        std::string name = token.substr(2, equals == std::string::npos ? std::string::npos : equals - 2);
        if (name != "output" && !ints.count(name) && !choices.count(name)) throw std::invalid_argument("unrecognized arguments: " + token);
        if (equals != std::string::npos) {
            value = token.substr(equals + 1);
        } else {
            if (i + 1 >= argv.size()) throw std::invalid_argument("argument --" + name + ": expected one argument");
            value = argv[++i];
        }
        seen.insert(name);
        if (name == "output") args.output = value;
        else if (const auto choice = choices.find(name); choice != choices.end()) {
            if (!choice->second.count(value)) throw std::invalid_argument("argument --" + name + ": invalid choice: '" + value + "'");
            if (name == "audio") args.audio = value;
            else if (name == "encoder") args.encoder = value;
            else if (name == "cursor") args.cursor = value;
            else args.capture = value;
        } else {
            const qint64 number = parse_int(name, value);
            if (name == "left") args.left = number;
            else if (name == "top") args.top = number;
            else if (name == "width") args.width = number;
            else if (name == "height") args.height = number;
            else if (name == "fps") args.fps = number;
            else args.bitrate = number;
        }
    }
    for (const char *required : {"output", "left", "top", "width", "height"})
        if (!seen.count(required)) throw std::invalid_argument(std::string("the following arguments are required: --") + required);
    return args;
}

template <class T>
T checked(qint64 value) {
    if (value < static_cast<qint64>(std::numeric_limits<T>::min()) || value > static_cast<qint64>(std::numeric_limits<T>::max()))
        throw std::overflow_error("int too large or negative to convert");
    return static_cast<T>(value);
}

struct EventContext {
    HANDLE output;
    Controls *controls;
};

bool on_event(const char *name, const char *json, void *user) {
    auto *context = static_cast<EventContext *>(user);
    const std::string event = name;
    if (!context->controls->error() || (event != "complete" && event != "cancelled")) {
        try {
            emit_event(context->output, name, QByteArray(json));
        } catch (const std::exception &) {
            return false;
        }
    }
    return true;
}

int run(const std::vector<std::string> &argv, HANDLE input, HANDLE output) {
    std::unique_ptr<Controls> controls;
    JbVideoRecorder *recorder = nullptr;
    auto free_recorder = [&recorder] {
        if (recorder) jb_video_recorder_free(recorder);
        recorder = nullptr;
    };
    try {
        require_media_foundation();
        const auto args = parse_arguments(argv);
        // The same physical pixel coordinates as the main program; this process never loads Qt widgets.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        recorder = jb_video_recorder_new(args.output.c_str(), checked<int32_t>(args.left), checked<int32_t>(args.top), checked<uint32_t>(args.width),
                                         checked<uint32_t>(args.height), checked<uint32_t>(args.fps), checked<uint32_t>(args.bitrate),
                                         args.audio == "system", args.encoder == "auto", args.cursor == "on", args.capture == "dxgi");
        controls = std::make_unique<Controls>(input, recorder);
        EventContext context{output, controls.get()};
        controls->start();
        char code[64] = {};
        char message[1024] = {};
        const bool ok = jb_video_recorder_run(recorder, on_event, &context, code, sizeof code, message, sizeof message);
        controls->close();
        if (!ok) throw WorkerError(code, message);
        if (const auto error = controls->error()) {
            emit_error(output, "protocol", QString::fromStdString(*error));
            free_recorder();
            return 1;
        }
        free_recorder();
        return 0;
    } catch (const std::exception &error) {
        if (controls) controls->close();
        QString code;
        if (controls && controls->error()) code = "protocol";
        else if (const auto *worker = dynamic_cast<const WorkerError *>(&error)) code = QString::fromStdString(worker->code);
        else if (dynamic_cast<const std::invalid_argument *>(&error) || dynamic_cast<const std::overflow_error *>(&error)) code = "arguments";
        else code = "backend";
        try {
            emit_error(output, code, QString::fromUtf8(error.what()));
        } catch (const std::exception &) {
        }
        controls.reset();
        free_recorder();
        return 1;
    }
}

// The windowed executable has no C runtime streams; QProcess still passes the pipes as the standard handles.
HANDLE standard_handle(DWORD which) {
    HANDLE handle = GetStdHandle(which);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) throw std::runtime_error("The standard handle is unavailable: " + std::to_string(GetLastError()));
    return handle;
}
}

int worker_main(const std::vector<std::string> &args) {
    HANDLE output = nullptr;
    try {
        output = standard_handle(STD_OUTPUT_HANDLE);
    } catch (const std::exception &) {
        return 1;
    }
    HANDLE input = nullptr;
    try {
        input = standard_handle(STD_INPUT_HANDLE);
    } catch (const std::exception &error) {
        try {
            emit_error(output, "process_io", QString::fromUtf8(error.what()));
        } catch (const std::exception &) {
        }
        return 1;
    }
    return run(args, input, output);
}
}
