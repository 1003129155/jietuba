#pragma once
#include <QFile>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVariant>
#include <exception>
#include <initializer_list>
#include <mutex>
#include <utility>
#include <vector>

namespace jietuba {
class ToolSettingsManager;

// Log text: a Chinese template plus named arguments, formatted only when the
// line is actually written. Templates built with T() are looked up in the
// zh->en table when the UI language is not Chinese; plain strings are not.
class LogMsg {
public:
    using Args = std::vector<std::pair<QString, QVariant>>;
    LogMsg() = default;
    LogMsg(const QString &text) : template_(text) {}
    LogMsg(const char *text) : template_(QString::fromUtf8(text)) {}
    LogMsg(QString text, Args args, bool translatable) : template_(std::move(text)), args_(std::move(args)), translatable_(translatable) {}
    QString render() const;
    bool isEmpty() const { return template_.isEmpty(); }
    const QString &template_text() const { return template_; }
private:
    QString template_;
    Args args_;
    bool translatable_ = false;
};

inline LogMsg T(const QString &text) { return LogMsg(text, {}, true); }
inline LogMsg T(const QString &text, std::initializer_list<std::pair<QString, QVariant>> args) { return LogMsg(text, LogMsg::Args(args), true); }

// str.format with named fields; supports {name}, {name:.Nf}, {name:d}, {name:X} and {{ }}.
// Returns false (leaving *out untouched) when a field is missing or malformed.
bool python_format(const QString &format, const LogMsg::Args &args, QString *out);
// Python str() of the common argument types.
QString python_str(const QVariant &value);
// Python f"{value:.{precision}f}": rounds the exact binary value, ties to even.
QString python_fixed(double value, int precision);
// Qt's debug text of the value types, as they appear in log arguments: QRectF(868,123 1654x1039).
QString geometry_text(const QPointF &value);
QString geometry_text(const QPoint &value);
QString geometry_text(const QRectF &value);
QString geometry_text(const QRect &value);
QString geometry_text(const QSizeF &value);
QString geometry_text(const QSize &value);

struct LogLevel {
    // CamelCase because windows.h defines ERROR.
    static constexpr int Debug = 10, Info = 20, Warning = 30, Error = 40, Silent = 50;
    static QString name(int level);
};

class Logger {
public:
    static Logger &instance();
    void setup(bool enabled = true, const QString &log_dir = {});
    void debug(const LogMsg &message, const QString &module = {}) { log(LogLevel::Debug, message, module); }
    void info(const LogMsg &message, const QString &module = {}) { log(LogLevel::Info, message, module); }
    void warning(const LogMsg &message, const QString &module = {}) { log(LogLevel::Warning, message, module); }
    void error(const LogMsg &message, const QString &module = {}) { log(LogLevel::Error, message, module); }
    void set_level(int level);
    void set_console_level(int level) { console_min_level = level; }
    void exception(const QString &exc_type, const QString &exc_msg, const LogMsg &context = {}, bool silent = true);
    void exception_with_traceback(const QString &exc_type, const QString &exc_msg, const LogMsg &context = {});
    void set_enabled(bool enabled);
    void set_log_dir(const QString &log_dir);
    void close();
    // Equivalent of print() after setup: stdout, plus the log file once ready.
    void print(const QString &text);
    bool ready() const { return ready_; }
    // The open log file's path, empty when none is open.
    QString log_file_path() const { return log_file_.isOpen() ? log_file_.fileName() : QString(); }

    bool enabled = false;
    QString log_dir;
    int min_level = LogLevel::Warning;
    int console_min_level = LogLevel::Warning;
private:
    Logger() = default;
    void log(int level, const LogMsg &message, const QString &module);
    void write_header();
    void write_file(const QString &text, bool flush_now);
    static void write_console(const QString &text);
    static void message_handler(QtMsgType, const QMessageLogContext &, const QString &);

    QFile log_file_;
    bool ready_ = false;
    std::recursive_mutex write_lock_;
};

Logger &get_logger();
void setup_logger(ToolSettingsManager *config_manager = nullptr);
void cleanup_old_logs(const QString &log_dir, qint64 retention_days);
// e.what() as text: messages built from QString are UTF-8, system and runtime-library ones use the ANSI code page.
QString exception_text(const std::exception &e);
void log_exception(const std::exception &e, const LogMsg &context = {}, bool silent = true);
void log_exception(const QString &exc_type, const QString &exc_msg, const LogMsg &context = {}, bool silent = true);
void log_exception_full(const std::exception &e, const LogMsg &context = {});
void log_debug(const LogMsg &message, const QString &module = {});
void log_info(const LogMsg &message, const QString &module = {});
void log_warning(const LogMsg &message, const QString &module = {});
void log_error(const LogMsg &message, const QString &module = {});
void set_log_level(int level);
void set_console_log_level(int level);
}
