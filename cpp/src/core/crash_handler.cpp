#include "core/crash_handler.h"
#include "core/constants.h"
#include "core/logger.h"
#include "core/native.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>
#include <exception>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#endif

namespace jietuba {
namespace {
QString log_dir() { return get_log_dir(); }

void ensure_log_dir() { QDir().mkpath(log_dir()); }

void append(const QString &name, const QString &text) {
    QFile file(QDir(log_dir()).filePath(name));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write(text.toUtf8());
}

[[noreturn]] void terminate_handler() {
    QString message = "std::terminate\n";
    if (const auto current = std::current_exception()) {
        try { std::rethrow_exception(current); }
        catch (const std::exception &e) { message = QString("%1: %2\n").arg(QString::fromLatin1(typeid(e).name()), exception_text(e)); }
        catch (...) { message = "unknown exception\n"; }
    }
    write_crash(QString::fromUtf8("未处理的异常（主线程）"), message);
    std::abort();
}

void report_rust_panic(const char *report) { write_crash(QStringLiteral("Rust panic"), QString::fromUtf8(report) + '\n'); }

#ifdef Q_OS_WIN
// The faulting thread's stack, most recent call first, as "module!function+offset (file:line)";
// symbols come from the PDBs next to or recorded in each module.
QString native_stack(const CONTEXT *fault_context) {
    if (!fault_context) return {};
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    const bool symbols = SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *fault_context;
    STACKFRAME64 frame{};
#ifdef _M_ARM64
    constexpr DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    constexpr DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
    QString text = "Native stack (most recent call first):\n";
    for (int depth = 0; depth < 64; ++depth) {
        if (!StackWalk64(machine, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
        const DWORD64 address = frame.AddrPC.Offset;
        if (!address) break;
        QString line = QString("  0x%1").arg(address, 0, 16);
        HMODULE module = nullptr;
        wchar_t module_path[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module)
            && GetModuleFileNameW(module, module_path, MAX_PATH))
            line += QString(" %1+0x%2").arg(QFileInfo(QString::fromWCharArray(module_path)).fileName()).arg(address - reinterpret_cast<DWORD64>(module), 0, 16);
        if (symbols) {
            alignas(SYMBOL_INFOW) char buffer[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFOW *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            if (SymFromAddrW(process, address, &displacement, symbol)) line += QString(" %1+0x%2").arg(QString::fromWCharArray(symbol->Name)).arg(displacement, 0, 16);
            IMAGEHLP_LINEW64 source{};
            source.SizeOfStruct = sizeof(source);
            DWORD column = 0;
            if (SymGetLineFromAddrW64(process, address, &column, &source)) line += QString(" (%1:%2)").arg(QString::fromWCharArray(source.FileName)).arg(source.LineNumber);
        }
        text += line + "\n";
    }
    if (symbols) SymCleanup(process);
    return text;
}

// The filter installed before ours: the C++ runtime's, which turns an uncaught C++ exception into std::terminate.
LPTOP_LEVEL_EXCEPTION_FILTER runtime_filter = nullptr;
constexpr DWORD MSVC_CPP_EXCEPTION = 0xE06D7363;

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *info) {
    // Last line of defence for native faults; the reference writes faulthandler.log.
    const auto *record = info ? info->ExceptionRecord : nullptr;
    const auto text = QString("Windows fatal exception: code 0x%1 at 0x%2\n")
                          .arg(record ? record->ExceptionCode : 0, 8, 16, QChar('0'))
                          .arg(reinterpret_cast<quintptr>(record ? record->ExceptionAddress : nullptr), 0, 16);
    const auto stack = native_stack(info ? info->ContextRecord : nullptr);
    append(FAULT_LOG_NAME, text + stack);
    std::fputs((text + stack).toLocal8Bit().constData(), stderr);
    // Only through the runtime's filter does terminate_handler see the exception's type and message.
    if (record && record->ExceptionCode == MSVC_CPP_EXCEPTION && runtime_filter) return runtime_filter(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif
}

void write_crash(const QString &tag, const QString &msg) {
    const auto separator = QString(60, '=');
    const auto text = QString("\n%1\n%2:\n%3%1\n").arg(separator, QString::fromUtf8("❌ ") + tag, msg);
    std::fputs(text.toUtf8().constData(), stderr);
    std::fflush(stderr);
    ensure_log_dir();
    append(CRASH_LOG_NAME, QString("[%1] %2\n%3%4\n\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"), tag, msg, separator));
    auto &logger = get_logger();
    if (logger.ready()) logger.error(QString("%1:\n%2").arg(tag, msg), "CRASH");
}

void run_thread_guarded(const QString &name, const std::function<void()> &body) {
    try {
        body();
    } catch (const std::exception &e) {
        write_crash(QString::fromUtf8("未处理的异常（子线程: %1）").arg(name), exception_text(e) + "\n");
    } catch (...) {
        write_crash(QString::fromUtf8("未处理的异常（子线程: %1）").arg(name), "unknown exception\n");
    }
}

void install_crash_hooks() {
    std::set_terminate(&terminate_handler);
    jb_set_panic_handler(&report_rust_panic);
#ifdef Q_OS_WIN
    ensure_log_dir();
    append(FAULT_LOG_NAME, QString("\n--- session %1 ---\n").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")));
    const auto previous = SetUnhandledExceptionFilter(&unhandled_exception_filter);
    // A second install must not chain to itself.
    if (previous != &unhandled_exception_filter) runtime_filter = previous;
#endif
}
}
