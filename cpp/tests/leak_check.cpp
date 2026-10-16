// Leak mode (JIETUBA_LEAK_CHECK): the tests run three times. The first pass fills caches and
// singletons; the second and third run with an allocation hook on the debug CRT heap that keeps
// the call stack of every live block. Blocks the second pass allocated that are still alive after
// the third are reported, grouped by the project frame nearest the allocation: state a singleton
// replaces on every pass is freed by the next one, a leak never is. The Rust libraries allocate
// from the process heap, outside the CRT, and are not seen here.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLibraryInfo>
#include <QString>
#include <QThreadPool>
#include <QWidget>
#include <crtdbg.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace jietuba::leak_check {
int run(int argc, char **argv, const std::function<QObject *()> &make, const std::function<int(QObject *, int, char **)> &exec);
}

namespace {
constexpr USHORT MAX_FRAMES = 32;
constexpr int PRINTED_FRAMES = 16;

struct Stack {
    USHORT count = 0;
    void *frames[MAX_FRAMES];
};

// The hook's own bookkeeping comes from a private heap: allocating from the CRT inside its hook recurses.
HANDLE private_heap() {
    static const HANDLE heap = HeapCreate(0, 0, 0);
    return heap;
}

template <class T>
struct PrivateAllocator {
    using value_type = T;
    PrivateAllocator() = default;
    template <class U>
    PrivateAllocator(const PrivateAllocator<U> &) {}
    T *allocate(size_t n) {
        if (void *p = HeapAlloc(private_heap(), 0, n * sizeof(T))) return static_cast<T *>(p);
        throw std::bad_alloc();
    }
    void deallocate(T *p, size_t) { HeapFree(private_heap(), 0, p); }
    template <class U>
    bool operator==(const PrivateAllocator<U> &) const { return true; }
};

using StackMap = std::unordered_map<long, Stack, std::hash<long>, std::equal_to<long>, PrivateAllocator<std::pair<const long, Stack>>>;
StackMap &live_blocks() {
    static StackMap *map = new (HeapAlloc(private_heap(), 0, sizeof(StackMap))) StackMap();
    return *map;
}

SRWLOCK lock = SRWLOCK_INIT;
std::atomic<bool> tracking{false};
thread_local bool inside_hook = false;
thread_local long probed_request = -1;
thread_local bool probing = false;

// The debug heap header ends with the block's size, its request number and four guard bytes.
long request_of(const void *block) {
    return *reinterpret_cast<const long *>(static_cast<const char *>(block) - 8);
}

int __cdecl alloc_hook(int type, void *block, size_t, int use, long request, const unsigned char *, int) {
    if (probing) {
        probed_request = request;
        return TRUE;
    }
    if (!tracking.load(std::memory_order_relaxed) || inside_hook || _BLOCK_TYPE(use) == _CRT_BLOCK) return TRUE;
    inside_hook = true;
    if (type == _HOOK_FREE) {
        if (block) {
            AcquireSRWLockExclusive(&lock);
            live_blocks().erase(request_of(block));
            ReleaseSRWLockExclusive(&lock);
        }
    } else {
        Stack stack;
        stack.count = RtlCaptureStackBackTrace(1, MAX_FRAMES, stack.frames, nullptr);
        AcquireSRWLockExclusive(&lock);
        // A moved block gets the new request number, one grown in place keeps the old; either may be dumped.
        if (type == _HOOK_REALLOC && block) live_blocks()[request_of(block)] = stack;
        live_blocks()[request] = stack;
        ReleaseSRWLockExclusive(&lock);
    }
    inside_hook = false;
    return TRUE;
}

// Confirms the header layout: without it every block would be looked up under a wrong number.
bool hook_understands_heap() {
    probing = true;
    void *block = std::malloc(16);
    const long allocated = probed_request;
    probing = false;
    const bool readable = block && request_of(block) == allocated && *reinterpret_cast<const size_t *>(static_cast<char *>(block) - 16) == 16;
    std::free(block);
    return readable;
}

// The request number the next allocation gets.
long next_request() {
    probing = true;
    void *block = std::malloc(1);
    const long request = probed_request;
    probing = false;
    std::free(block);
    return request;
}

void settle() {
    QThreadPool::globalInstance()->waitForDone();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

// The dump reports a block's "{request} " and its "normal block at ..., N bytes long." as separate messages.
std::vector<std::pair<long, size_t>> *dumped = nullptr;
long dumped_request = -1;
int __cdecl dump_hook(int type, char *message, int *result) {
    if (type != _CRT_WARN || !message || !dumped) return FALSE;
    long request = 0;
    size_t bytes = 0;
    if (const char *brace = std::strchr(message, '{'); brace && std::sscanf(brace, "{%ld}", &request) == 1) {
        dumped_request = request;
    } else if (dumped_request >= 0 && (std::sscanf(message, "normal block at %*[^,], %zu", &bytes) == 1
                                          || std::sscanf(message, "client block at %*[^,], subtype %*x, %zu", &bytes) == 1)) {
        dumped->emplace_back(dumped_request, bytes);
        dumped_request = -1;
    }
    if (result) *result = 0;
    return TRUE;
}

struct Frame {
    std::string module, symbol, file;
    unsigned line = 0;
    // A Qt library or plugin; the platform plugin is not named qt6*.
    bool qt = false;
};

Frame describe(void *address) {
    static const QString plugins = QDir::cleanPath(QLibraryInfo::path(QLibraryInfo::PluginsPath)) + '/';
    Frame frame;
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(address), &module) && GetModuleFileNameA(module, path, MAX_PATH)) {
        const char *name = std::strrchr(path, '\\');
        frame.module = name ? name + 1 : path;
        for (auto &c : frame.module) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        frame.qt = frame.module.rfind("qt6", 0) == 0
            || QDir::fromNativeSeparators(QString::fromLocal8Bit(path)).startsWith(plugins, Qt::CaseInsensitive);
    }
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    const bool found = SymFromAddr(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &displacement, symbol);
    // Without a PDB the nearest export would be named, which misleads; the offset is printed instead.
    IMAGEHLP_MODULE64 info{};
    info.SizeOfStruct = sizeof(info);
    const bool exports_only = !SymGetModuleInfo64(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &info)
        || info.SymType == SymExport || info.SymType == SymNone;
    if (found && !exports_only) {
        frame.symbol = symbol->Name;
    } else if (module) {
        char offset[32];
        std::snprintf(offset, sizeof offset, "+0x%llx", static_cast<unsigned long long>(static_cast<const char *>(address) - reinterpret_cast<const char *>(module)));
        frame.symbol = offset;
    }
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD offset = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), reinterpret_cast<DWORD64>(address), &offset, &line)) {
        frame.file = QDir::fromNativeSeparators(QString::fromLocal8Bit(line.FileName)).toStdString();
        frame.line = line.LineNumber;
    }
    return frame;
}

int report(long second_start, long third_start) {
    std::vector<std::pair<long, size_t>> blocks;
    dumped = &blocks;
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, dump_hook);
    // All live blocks, filtered by request number below: a checkpoint names its newest block by address,
    // which a later allocation can reuse, so dumping since a checkpoint can stop early or run past it.
    _CrtMemDumpAllObjectsSince(nullptr);
    _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, dump_hook);
    dumped = nullptr;

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    const std::string root = QDir::fromNativeSeparators(QStringLiteral(JIETUBA_SOURCE_DIR)).toLower().toStdString() + "/";
    const auto project_file = [&](const Frame &f) {
        std::string file = f.file;
        for (auto &c : file) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return file.rfind(root + "src/", 0) == 0 || (file.rfind(root + "tests/", 0) == 0 && file.find("leak_check") == std::string::npos);
    };

    // Blocks are grouped by the project frame nearest the allocation; one stack is printed per place.
    struct Place {
        std::vector<Frame> sample;
        int nearest = 0;
        size_t blocks = 0, bytes = 0;
        // Qt frames between the allocation and project code, the fewest of any block here: several on
        // every block usually mean a Qt cache filled on the project code's behalf.
        int qt_frames = 1 << 30;
    };
    const auto qt_frames_of = [](const std::vector<Frame> &frames, int nearest) {
        int count = 0;
        for (int i = 0; i < nearest; ++i) count += frames[i].qt;
        return count;
    };
    std::map<std::string, Place> places;
    std::map<std::vector<void *>, std::vector<Frame>> described;
    const auto frames_of = [&](const Stack &stack) -> const std::vector<Frame> & {
        const std::vector<void *> key(stack.frames, stack.frames + stack.count);
        auto [entry, fresh] = described.try_emplace(key);
        if (fresh) for (void *address : key) entry->second.push_back(describe(address));
        return entry->second;
    };
    const auto nearest_of = [&](const std::vector<Frame> &frames) {
        for (int i = 0; i < static_cast<int>(frames.size()); ++i) {
            if (!project_file(frames[i])) continue;
            // main and the pass lambda in it call QTest::qExec: QtTest's own allocations, not the test's.
            return frames[i].symbol != "main" && frames[i].symbol.rfind("`main'", 0) != 0 ? i : -1;
        }
        return -1;
    };
    const auto runtime = [](const Frame &f) {
        return f.module.rfind("ucrtbase", 0) == 0 || f.module.rfind("vcruntime", 0) == 0 || f.module.rfind("msvcp", 0) == 0;
    };
    const auto location = [&](const Frame &f) { return f.file.substr(root.size()) + ":" + std::to_string(f.line) + " " + f.symbol; };

    // A window left open shows as hundreds of blocks below; one line each names the windows themselves.
    const std::map<long, size_t> sizes(blocks.begin(), blocks.end());
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        // The debug heap header before the object holds its size and request number.
        const auto *user = reinterpret_cast<const char *>(widget);
        const long request = request_of(widget);
        const auto size = sizes.find(request);
        const auto found = live_blocks().find(request);
        if (request < second_start || request >= third_start || size == sizes.end() || size->second != *reinterpret_cast<const size_t *>(user - 16)
            || found == live_blocks().end()) continue;
        const auto &frames = frames_of(found->second);
        const int nearest = nearest_of(frames);
        std::printf("LEFT OPEN %s%s, created at %s\n", widget->metaObject()->className(), widget->isVisible() ? " (visible)" : "",
            nearest < 0 ? "Qt" : location(frames[nearest]).c_str());
    }

    size_t untracked = 0, qt_blocks = 0;
    for (const auto &[request, bytes] : blocks) {
        if (request < second_start || request >= third_start) continue;
        const auto found = live_blocks().find(request);
        if (found == live_blocks().end()) {
            ++untracked;
            continue;
        }
        const std::vector<Frame> &frames = frames_of(found->second);
        const int nearest = nearest_of(frames);
        if (nearest < 0) {
            ++qt_blocks;
            continue;
        }
        const Frame &at = frames[nearest];
        Place &place = places[at.file + ":" + std::to_string(at.line) + " " + at.symbol];
        if (!place.blocks) {
            place.sample = frames;
            place.nearest = nearest;
        }
        ++place.blocks;
        place.bytes += bytes;
        place.qt_frames = std::min(place.qt_frames, qt_frames_of(frames, nearest));
    }
    size_t project_blocks = 0, project_bytes = 0, inside_qt = 0;
    for (const auto &[where_key, place] : places) {
        project_blocks += place.blocks;
        project_bytes += place.bytes;
        const int qt_frames = place.qt_frames;
        inside_qt += qt_frames > 2;
        std::printf("LEAK %zu block(s), %zu bytes, at %s %s\n", place.blocks, place.bytes, location(place.sample[place.nearest]).c_str(),
            qt_frames > 2 ? ("(inside Qt, " + std::to_string(qt_frames) + " frames)").c_str() : "(direct)");
        int printed = 0;
        for (int i = 0; i < static_cast<int>(place.sample.size()) && printed < PRINTED_FRAMES; ++i) {
            const Frame &f = place.sample[i];
            if (runtime(f)) continue;
            const std::string where = f.file.empty() ? std::string()
                : (project_file(f) ? f.file.substr(root.size()) : f.file.substr(f.file.rfind('/') + 1)) + ":" + std::to_string(f.line);
            std::printf("    %s %s %s %s\n", i == place.nearest ? ">" : " ", f.module.c_str(), f.symbol.c_str(), where.c_str());
            ++printed;
        }
    }
    const size_t project_places = places.size();
    SymCleanup(GetCurrentProcess());
    if (project_places) {
        std::printf("LEAK CHECK: %zu block(s), %zu bytes, from the second pass outlived the third, in %zu place(s), %zu of them inside Qt;"
            " %zu more from Qt itself, %zu untracked\n", project_blocks, project_bytes, project_places, inside_qt, qt_blocks, untracked);
        return 1;
    }
    std::printf("LEAK CHECK: clean (%zu block(s) from Qt itself, %zu untracked)\n", qt_blocks, untracked);
    return 0;
}

// A debug-CRT assertion or abort must fail the test in the log instead of waiting on a dialog.
const bool crt_reports_to_stderr = [] {
    for (const int type : {_CRT_ASSERT, _CRT_ERROR}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    return true;
}();

// Debug QtTest writes nothing to a redirected stdout, so each pass logs to a file that is copied there.
int run_pass(QObject *object, int argc, char **argv, const std::function<int(QObject *, int, char **)> &exec, int number) {
    std::vector<char *> args(argv, argv + argc);
    const bool own_output = std::none_of(args.begin(), args.end(), [](const char *a) { return std::strcmp(a, "-o") == 0; });
    const QString log = QDir::temp().filePath(QString("jietuba-leak-%1-%2.txt").arg(QCoreApplication::applicationPid()).arg(number));
    QByteArray target = (log + ",txt").toLocal8Bit();
    char option[] = "-o";
    if (own_output) {
        args.push_back(option);
        args.push_back(target.data());
    }
    const int result = exec(object, static_cast<int>(args.size()), args.data());
    if (own_output) {
        QFile file(log);
        if (file.open(QIODevice::ReadOnly)) {
            std::printf("--- leak check pass %d\n", number);
            const QByteArray text = file.readAll();
            std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stdout);
            std::fflush(stdout);
        }
        file.close();
        QFile::remove(log);
    }
    return result;
}
}

namespace jietuba::leak_check {
int run(int argc, char **argv, const std::function<QObject *()> &make, const std::function<int(QObject *, int, char **)> &exec) {
    // One object for every pass: the tests bind process-wide singletons to what it owns, once.
    std::unique_ptr<QObject> object(make());
    const auto pass = [&](int number) {
        const int result = run_pass(object.get(), argc, argv, exec, number);
        settle();
        return result;
    };
    if (const int result = pass(1); result != 0) return result;
    _CrtSetAllocHook(alloc_hook);
    if (!hook_understands_heap()) {
        std::printf("LEAK CHECK: debug heap layout not recognised, not checked\n");
        return 1;
    }
    const long second_start = next_request();
    tracking = true;
    int result = pass(2);
    const long third_start = next_request();
    if (result == 0) result = pass(3);
    // What the test object owns is freed with it, so only blocks nothing owns are left to report.
    object.reset();
    settle();
    tracking = false;
    if (result == 0) result = report(second_start, third_start);
    return result;
}
}
