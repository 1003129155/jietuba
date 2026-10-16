#include "core/logger.h"
#include "core/constants.h"
#include "core/i18n.h"
#include "settings/tool_settings.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringDecoder>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <typeinfo>

namespace jietuba {
namespace {
const QHash<QString, QString> &log_translations() {
    // Loaded on first use; Chinese UI never needs the table.
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> result;
        QFile file(":/core/log_translations.json");
        if (file.open(QIODevice::ReadOnly)) {
            const auto object = QJsonDocument::fromJson(file.readAll()).object();
            for (auto it = object.begin(); it != object.end(); ++it) result.insert(it.key(), it.value().toString());
        }
        return result;
    }();
    return table;
}

QString python_float_repr(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
    if (value == 0) return std::signbit(value) ? "-0.0" : "0.0";
    // Shortest round-trip digits, laid out like Python's repr: fixed notation
    // for exponents in [-4, 16), scientific otherwise.
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    QString sci = QString::fromLatin1(buffer, static_cast<int>(result.ptr - buffer));
    const auto e_pos = sci.indexOf('e');
    const int exponent = sci.mid(e_pos + 1).toInt();
    QString mantissa = sci.left(e_pos);
    const bool negative = mantissa.startsWith('-');
    if (negative) mantissa.remove(0, 1);
    QString digits = mantissa; digits.remove('.');
    if (exponent >= -4 && exponent < 16) {
        QString out;
        if (exponent < 0) out = "0." + QString(-exponent - 1, '0') + digits;
        else if (digits.size() > exponent + 1) out = digits.left(exponent + 1) + "." + digits.mid(exponent + 1);
        else out = digits + QString(exponent + 1 - digits.size(), '0') + ".0";
        return negative ? "-" + out : out;
    }
    QString out = digits.left(1);
    if (digits.size() > 1) out += "." + digits.mid(1);
    out += QString("e%1%2").arg(exponent < 0 ? '-' : '+').arg(std::abs(exponent), 2, 10, QChar('0'));
    return negative ? "-" + out : out;
}

bool format_field(const QVariant &value, const QString &spec, QString *out) {
    if (spec.isEmpty()) { *out = python_str(value); return true; }
    static const QRegularExpression fixed(R"(^\.(\d+)f$)");
    if (const auto match = fixed.match(spec); match.hasMatch()) {
        bool ok = false; const double number = value.toDouble(&ok);
        if (!ok) return false;
        *out = python_fixed(number, match.captured(1).toInt());
        return !out->isEmpty();
    }
    if (spec == "d") {
        bool ok = false; const auto number = value.toLongLong(&ok);
        if (!ok || value.metaType().id() == QMetaType::Double) return false;
        *out = QString::number(number);
        return true;
    }
    if (spec == "X" || spec == "x") {
        bool ok = false; const auto number = value.toLongLong(&ok);
        if (!ok || value.metaType().id() == QMetaType::Double) return false;
        const auto digits = QString::number(std::abs(number), 16);
        *out = (number < 0 ? "-" : "") + (spec == "X" ? digits.toUpper() : digits);
        return true;
    }
    return false;
}

void write_stdout(const QString &text) {
    const auto bytes = text.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
    std::fflush(stdout);
}
}

QString python_fixed(double value, int precision) {
    // to_chars rounds the exact binary value (ties to even) like Python; QString::number does not.
    char buffer[400];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, precision);
    if (result.ec != std::errc()) return {};
    return QString::fromLatin1(buffer, static_cast<int>(result.ptr - buffer));
}

QString python_str(const QVariant &value) {
    switch (value.metaType().id()) {
    case QMetaType::UnknownType: return "None";
    case QMetaType::Bool: return value.toBool() ? "True" : "False";
    case QMetaType::Double: case QMetaType::Float: return python_float_repr(value.toDouble());
    default: return value.toString();
    }
}

namespace {
QString fixed6(double value) { return QString::number(value, 'f', 6); }
}
QString py_repr(const QPointF &v) { return QString("PySide6.QtCore.QPointF(%1, %2)").arg(fixed6(v.x()), fixed6(v.y())); }
QString py_repr(const QPoint &v) { return QString("PySide6.QtCore.QPoint(%1, %2)").arg(v.x()).arg(v.y()); }
QString py_repr(const QRectF &v) { return QString("PySide6.QtCore.QRectF(%1, %2, %3, %4)").arg(fixed6(v.x()), fixed6(v.y()), fixed6(v.width()), fixed6(v.height())); }
QString py_repr(const QRect &v) { return QString("PySide6.QtCore.QRect(%1, %2, %3, %4)").arg(v.x()).arg(v.y()).arg(v.width()).arg(v.height()); }
QString py_repr(const QSizeF &v) { return QString("PySide6.QtCore.QSizeF(%1, %2)").arg(fixed6(v.width()), fixed6(v.height())); }
QString py_repr(const QSize &v) { return QString("PySide6.QtCore.QSize(%1, %2)").arg(v.width()).arg(v.height()); }

bool python_format(const QString &format, const LogMsg::Args &args, QString *out) {
    QString result;
    result.reserve(format.size());
    for (qsizetype i = 0; i < format.size(); ++i) {
        const QChar ch = format[i];
        if (ch == '{') {
            if (i + 1 < format.size() && format[i + 1] == '{') { result += '{'; ++i; continue; }
            const auto end = format.indexOf('}', i + 1);
            if (end < 0) return false;
            const QString field = format.mid(i + 1, end - i - 1);
            const auto colon = field.indexOf(':');
            const QString name = colon < 0 ? field : field.left(colon);
            const QString spec = colon < 0 ? QString() : field.mid(colon + 1);
            const auto found = std::find_if(args.begin(), args.end(), [&](const auto &arg) { return arg.first == name; });
            if (found == args.end()) return false;
            QString text;
            if (!format_field(found->second, spec, &text)) return false;
            result += text;
            i = end;
        } else if (ch == '}') {
            if (i + 1 < format.size() && format[i + 1] == '}') { result += '}'; ++i; continue; }
            return false;
        } else {
            result += ch;
        }
    }
    *out = result;
    return true;
}

QString LogMsg::render() const {
    QString text = template_;
    if (translatable_ && I18nManager::get_current_language() != "zh")
        text = log_translations().value(template_, template_);
    if (args_.empty()) return text;
    QString out;
    if (python_format(text, args_, &out)) return out;
    // A translated template whose placeholders differ falls back to the Chinese one.
    if (python_format(template_, args_, &out)) return out;
    return template_;
}

QString LogLevel::name(int level) {
    switch (level) {
    case Debug: return "DEBUG";
    case Info: return "INFO";
    case Warning: return "WARNING";
    case Error: return "ERROR";
    case Silent: return "SILENT";
    default: return "UNKNOWN";
    }
}

Logger &Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::write_console(const QString &text) { write_stdout(text); }

void Logger::write_file(const QString &text, bool flush_now) {
    std::lock_guard lock(write_lock_);
    if (!log_file_.isOpen() || !ready_) return;
    log_file_.write(text.toUtf8());
    if (flush_now) log_file_.flush();
}

void Logger::print(const QString &text) {
    write_console(text + "\n");
    // The original tees stdout into the line-buffered log file.
    write_file(text + "\n", true);
}

void Logger::message_handler(QtMsgType, const QMessageLogContext &, const QString &message) {
    // Qt diagnostics go to stderr, which the original also tees into the log file.
    const auto line = message + "\n";
    const auto bytes = line.toUtf8();
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    instance().write_file(line, true);
}

void Logger::setup(bool enable, const QString &dir) {
    enabled = enable;
    if (!enable) { print(T("[WARN] [Logger] 日志功能已禁用").render()); return; }
    if (ready_) { print(T("[WARN] [Logger] 日志系统已经初始化").render()); return; }
    log_dir = dir.isEmpty() ? get_log_dir() : dir;
    const auto log_path = QDir(log_dir).filePath(RUNTIME_LOG_PREFIX + QDate::currentDate().toString("yyyyMMdd") + ".log");
    if (!QDir().mkpath(log_dir)) {
        print(T("[ERROR] [Logger] 无法创建日志文件: {e}", {{"e", QDir::toNativeSeparators(log_dir)}}).render());
        enabled = false;
        return;
    }
    log_file_.setFileName(log_path);
    if (!log_file_.open(QIODevice::WriteOnly | QIODevice::Append)) {
        print(T("[ERROR] [Logger] 无法创建日志文件: {e}", {{"e", log_file_.errorString()}}).render());
        enabled = false;
        return;
    }
    write_header();
    qInstallMessageHandler(&Logger::message_handler);
    ready_ = true;
    info(T("[OK] [Logger] 日志系统启动成功，日志文件：{log_path}", {{"log_path", QDir::toNativeSeparators(log_path)}}));
}

void Logger::write_header() {
    if (!log_file_.isOpen()) return;
    const auto header = T("\n{sep}\nJietuba 截图工具 - 运行日志\n启动时间: {start_time}\n日志目录: {log_dir}\n"
                          "日志级别: {file_level} (文件) / {console_level} (控制台)\n{sep}\n",
                          {{"sep", QString(80, '=')},
                           {"start_time", QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")},
                           {"log_dir", QDir::toNativeSeparators(log_dir)},
                           {"file_level", LogLevel::name(min_level)},
                           {"console_level", LogLevel::name(console_min_level)}}).render();
    log_file_.write(header.toUtf8());
    log_file_.flush();
}

void Logger::log(int level, const LogMsg &message, const QString &module) {
    if (!enabled || level < min_level) return;
    const auto text = message.render();
    const auto timestamp = QTime::currentTime().toString("HH:mm:ss");
    const auto line = module.isEmpty()
        ? QString("[%1] [%2] %3").arg(timestamp, LogLevel::name(level), text)
        : QString("[%1] [%2] [%3] %4").arg(timestamp, LogLevel::name(level), module, text);
    // Line buffered like the original; the extra flush keeps errors on disk if the process dies.
    write_file(line + "\n", true);
    if (level >= console_min_level) write_console(line + "\n");
}

void Logger::set_level(int level) {
    min_level = level;
    info(T("日志级别已设置为: {level_name}", {{"level_name", LogLevel::name(level)}}), "Logger");
}

void Logger::exception(const QString &exc_type, const QString &exc_msg, const LogMsg &context, bool silent) {
    if (!enabled) return;
    const auto rendered = context.render();
    const auto log_msg = rendered.isEmpty() ? QString("[静默异常] %1: %2").arg(exc_type, exc_msg)
                                            : QString("[静默异常] %1: %2: %3").arg(rendered, exc_type, exc_msg);
    if (silent && log_file_.isOpen() && ready_) {
        write_file(QString("[%1] [SILENT] %2\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"), log_msg), true);
    } else {
        log(LogLevel::Error, log_msg, {});
    }
}

void Logger::exception_with_traceback(const QString &exc_type, const QString &exc_msg, const LogMsg &context) {
    if (!enabled) return;
    const auto rendered = context.render();
    // No interpreter traceback exists; the exception text is all that is available.
    const auto log_msg = rendered.isEmpty() ? QString("[异常] %1: %2\n").arg(exc_type, exc_msg)
                                            : QString("[异常] %1: %2: %3\n").arg(rendered, exc_type, exc_msg);
    log(LogLevel::Error, log_msg, {});
}

void Logger::set_enabled(bool enable) {
    enabled = enable;
    if (enable) info(T("📝 [Logger] 日志已启用"));
    else print(T("🔇 [Logger] 日志已禁用").render());
}

void Logger::set_log_dir(const QString &dir) {
    if (ready_) { warning(T("[Logger] 日志系统已初始化，无法更改日志目录")); return; }
    log_dir = dir;
    print(T("[OK] [Logger] 日志目录已设置为: {log_dir}", {{"log_dir", dir}}).render());
}

void Logger::close() {
    if (!ready_) return;
    info(T("🛑 [Logger] 日志系统关闭"));
    qInstallMessageHandler(nullptr);
    std::lock_guard lock(write_lock_);
    if (log_file_.isOpen()) { log_file_.flush(); log_file_.close(); }
    ready_ = false;
}

Logger &get_logger() { return Logger::instance(); }

void setup_logger(ToolSettingsManager *config_manager) {
    auto &logger = get_logger();
    bool enabled = true;
    QString log_dir;
    int log_level = LogLevel::Warning;
    qint64 retention_days = 7;
    if (config_manager) {
        enabled = config_manager->get_log_enabled();
        log_dir = config_manager->get_log_dir();
        static const QHash<QString, int> level_map = {{"DEBUG", LogLevel::Debug}, {"INFO", LogLevel::Info}, {"WARNING", LogLevel::Warning}, {"ERROR", LogLevel::Error}};
        log_level = level_map.value(config_manager->get_log_level(), LogLevel::Warning);
        retention_days = config_manager->get_log_retention_days();
    } else {
        log_dir = get_log_dir();
    }
    logger.setup(enabled, log_dir);
    logger.set_level(log_level);
    logger.set_console_level(log_level);
    if (retention_days > 0) cleanup_old_logs(log_dir, retention_days);
}

namespace {
void trim_fixed_log(const QString &path, qint64 max_bytes) {
    QFile file(path);
    const auto size = file.size();
    if (size <= max_bytes) return;
    if (!file.open(QIODevice::ReadOnly)) {
        get_logger().print(T("[WARN] [日志截断] 处理 {name} 失败: {e}", {{"name", QFileInfo(path).fileName()}, {"e", file.errorString()}}).render());
        return;
    }
    file.seek(std::max<qint64>(0, size - max_bytes / 2));
    file.readLine();
    const auto tail = file.readAll();
    file.close();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        get_logger().print(T("[WARN] [日志截断] 处理 {name} 失败: {e}", {{"name", QFileInfo(path).fileName()}, {"e", file.errorString()}}).render());
        return;
    }
    file.write(QString("[日志截断: 文件超过 %1 KB 上限，已保留末尾内容]\n\n").arg(max_bytes / 1024).toUtf8());
    file.write(tail);
    get_logger().print(T("✂️ [日志截断] {name} 超过 {max_kb} KB，已截断", {{"name", QFileInfo(path).fileName()}, {"max_kb", max_bytes / 1024}}).render());
}
}

void cleanup_old_logs(const QString &log_dir, qint64 retention_days) {
    if (retention_days <= 0) return;
    QDir dir(log_dir);
    if (!dir.exists()) return;
    const auto cutoff = QDateTime::currentDateTime().addDays(-retention_days);
    // Only this build's files: the Python version cleans up its own.
    static const QRegularExpression pattern("^" + QRegularExpression::escape(RUNTIME_LOG_PREFIX) + R"((\d{8})\.log$)");
    int deleted_count = 0;
    for (const auto &info : dir.entryInfoList(QDir::Files)) {
        const auto match = pattern.match(info.fileName());
        if (!match.hasMatch()) continue;
        const auto file_date = QDate::fromString(match.captured(1), "yyyyMMdd");
        if (!file_date.isValid()) continue;
        // <= keeps exactly retention_days files.
        if (QDateTime(file_date, QTime(0, 0)) <= cutoff && QFile::remove(info.absoluteFilePath())) ++deleted_count;
    }
    if (deleted_count > 0)
        get_logger().print(T("🗑️ [日志清理] 已删除 {deleted_count} 个过期日志文件（保留 {retention_days} 天）",
                             {{"deleted_count", deleted_count}, {"retention_days", retention_days}}).render());
    constexpr qint64 max_fixed_log_bytes = 1024 * 1024;
    for (const auto &name : {CRASH_LOG_NAME, FAULT_LOG_NAME}) {
        const auto path = dir.filePath(name);
        if (QFile::exists(path)) trim_fixed_log(path, max_fixed_log_bytes);
    }
}

QString exception_text(const std::exception &e) {
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = utf8(QByteArrayView(e.what()));
    return utf8.hasError() ? QString::fromLocal8Bit(e.what()) : text;
}

void log_exception(const std::exception &e, const LogMsg &context, bool silent) {
    get_logger().exception(QString::fromLatin1(typeid(e).name()), exception_text(e), context, silent);
}
void log_exception(const QString &exc_type, const QString &exc_msg, const LogMsg &context, bool silent) {
    get_logger().exception(exc_type, exc_msg, context, silent);
}
void log_exception_full(const std::exception &e, const LogMsg &context) {
    get_logger().exception_with_traceback(QString::fromLatin1(typeid(e).name()), exception_text(e), context);
}
void log_debug(const LogMsg &message, const QString &module) {
    auto &logger = get_logger();
    if (logger.min_level <= LogLevel::Debug) logger.debug(message, module);
}
void log_info(const LogMsg &message, const QString &module) { get_logger().info(message, module); }
void log_warning(const LogMsg &message, const QString &module) { get_logger().warning(message, module); }
void log_error(const LogMsg &message, const QString &module) { get_logger().error(message, module); }
void set_log_level(int level) { get_logger().set_level(level); }
void set_console_log_level(int level) { get_logger().set_console_level(level); }
}
