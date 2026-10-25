#include "core/platform_utils.h"
#include <QDir>
#include <QCoreApplication>
#include "core/constants.h"
#include "core/crash_handler.h"
#include "core/logger.h"
#include <QFileInfo>
#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QTimer>
#include <cmath>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#endif

namespace jietuba {
namespace {
QTimer *trim_timer = nullptr;
std::function<bool()> trim_busy;
std::function<void()> trim_override;
std::function<bool(const QStringList &, const QString &)> copy_override;

void trim_when_idle() {
    if (trim_busy && trim_busy()) {
        trim_timer->start();
        return;
    }
    // Emptying the working set takes longer as it grows (tens of ms at a few hundred MB),
    // so it runs on a worker thread and the UI thread does not wait for it.
    std::thread([trim = trim_override ? trim_override : std::function<void()>(&trim_working_set)] {
#ifdef Q_OS_WIN
        SetThreadDescription(GetCurrentThread(), L"TrimWorkingSet");
#endif
        run_thread_guarded("TrimWorkingSet", trim);
    }).detach();
}

#ifdef Q_OS_WIN
HWND to_hwnd(quintptr hwnd) { return reinterpret_cast<HWND>(hwnd); }

QString win32_error(DWORD code) { return QString("[WinError %1]").arg(code); }
#endif
}

void trim_working_set() {
#ifdef Q_OS_WIN
    if (!SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1)))
        log_exception(win32_error(GetLastError()), T("释放工作集"));
#endif
}

void set_trim_busy_check(std::function<bool()> predicate) { trim_busy = std::move(predicate); }

void set_trim_for_test(std::function<void()> trim) { trim_override = std::move(trim); }

namespace {
#ifdef Q_OS_WIN
bool shell_copy(const QStringList &sources, const QString &folder) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOperation *operation = nullptr;
    IShellItem *destination = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation));
    if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION | FOF_NOCONFIRMMKDIR);
    if (SUCCEEDED(hr))
        hr = SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(folder).utf16()), nullptr, IID_PPV_ARGS(&destination));
    for (const QString &source : sources) {
        if (FAILED(hr)) break;
        IShellItem *item = nullptr;
        hr = SHCreateItemFromParsingName(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(source).utf16()), nullptr, IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr)) hr = operation->CopyItem(item, destination, nullptr, nullptr);
        if (item) item->Release();
    }
    BOOL aborted = FALSE;
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    if (SUCCEEDED(hr)) operation->GetAnyOperationsAborted(&aborted);
    if (destination) destination->Release();
    if (operation) operation->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) log_exception(QString("IFileOperation failed: 0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0')), T("复制到文件夹"));
    return SUCCEEDED(hr) && !aborted;
}
#endif
}

void copy_into_folder(const QStringList &sources, const QString &folder, std::function<void(bool, const QStringList &)> done) {
    std::function<bool(const QStringList &, const QString &)> copy = copy_override;
#ifdef Q_OS_WIN
    if (!copy) copy = &shell_copy;
#endif
    std::thread([sources, folder, copy = std::move(copy), done = std::move(done)]() mutable {
#ifdef Q_OS_WIN
        SetThreadDescription(GetCurrentThread(), L"CopyIntoFolder");
#endif
        QStringList present, missing;
        for (const QString &source : sources) (QFileInfo::exists(source) ? present : missing).append(source);
        bool ok = present.isEmpty();
        if (!present.isEmpty()) run_thread_guarded("CopyIntoFolder", [&] { ok = copy && copy(present, folder); });
        // done goes to the main thread whole, so what it holds is released there.
        if (done && QCoreApplication::instance())
            QMetaObject::invokeMethod(QCoreApplication::instance(), [done = std::move(done), ok, missing] { done(ok, missing); }, Qt::QueuedConnection);
    }).detach();
}

void set_copy_into_folder_for_test(std::function<bool(const QStringList &, const QString &)> copy) { copy_override = std::move(copy); }

void request_trim_working_set(int delay_ms) {
    if (!trim_timer) {
        trim_timer = new QTimer;
        trim_timer->setSingleShot(true);
        QObject::connect(trim_timer, &QTimer::timeout, &trim_when_idle);
    }
    trim_timer->start(delay_ms);
}

void set_dpi_awareness() {
#ifdef Q_OS_WIN
    const HRESULT hr = SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    if (FAILED(hr)) {
        log_exception(QString("HRESULT 0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0')), "SetProcessDpiAwareness");
        if (!SetProcessDPIAware()) log_exception(win32_error(GetLastError()), "SetProcessDPIAware");
    }
#endif
}

double get_system_dpi(double fallback) {
    if (!std::isfinite(fallback) || fallback <= 0) fallback = 96.0;
#ifdef Q_OS_WIN
    // GetDpiForSystem exists from Windows 10 1607; older systems use the device context.
    using GetDpiForSystemFn = UINT(WINAPI *)();
    if (auto *user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto fn = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"))) {
            const UINT dpi = fn();
            if (dpi > 0) return static_cast<double>(dpi);
        }
    }
    if (HDC dc = GetDC(nullptr)) {
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        if (dpi > 0) return static_cast<double>(dpi);
    }
#endif
    return fallback;
}

void set_app_user_model_id(const QString &app_id) {
#ifdef Q_OS_WIN
    const HRESULT hr = SetCurrentProcessExplicitAppUserModelID(reinterpret_cast<PCWSTR>(app_id.utf16()));
    if (FAILED(hr)) log_exception(QString("HRESULT 0x%1").arg(static_cast<quint32>(hr), 8, 16, QChar('0')), "SetAppUserModelID");
#else
    Q_UNUSED(app_id);
#endif
}

std::optional<std::pair<quint64, QString>> get_process_identity(qint64 pid) {
    if (pid <= 0) return std::nullopt;
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!handle) return std::nullopt;
    std::optional<std::pair<quint64, QString>> result;
    FILETIME creation{}, exit_time{}, kernel_time{}, user_time{};
    if (GetProcessTimes(handle, &creation, &exit_time, &kernel_time, &user_time)) {
        const quint64 create_time = (static_cast<quint64>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
        std::wstring buffer(32768, L'\0');
        DWORD size = static_cast<DWORD>(buffer.size());
        QString image_name;
        if (QueryFullProcessImageNameW(handle, 0, buffer.data(), &size))
            image_name = QFileInfo(QString::fromWCharArray(buffer.data(), static_cast<int>(size))).fileName();
        result = std::make_pair(create_time, image_name);
    }
    CloseHandle(handle);
    return result;
#else
    return std::nullopt;
#endif
}

bool terminate_process_by_pid(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return false;
    const bool ok = TerminateProcess(handle, 0) != FALSE;
    CloseHandle(handle);
    return ok;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

QStringList recognized_game_executables(const QString &children_key) {
    QStringList paths;
#ifdef Q_OS_WIN
    HKEY children = nullptr;
    const std::wstring key = children_key.isEmpty() ? std::wstring(L"System\\GameConfigStore\\Children") : children_key.toStdWString();
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &children) != ERROR_SUCCESS) return paths;
    std::wstring path(32768, L'\0');
    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD name_size = static_cast<DWORD>(std::size(name));
        const LONG status = RegEnumKeyExW(children, index, name, &name_size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        DWORD bytes = static_cast<DWORD>(path.size() * sizeof(wchar_t));
        if (RegGetValueW(children, name, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, path.data(), &bytes) == ERROR_SUCCESS && bytes > sizeof(wchar_t))
            paths << QString::fromWCharArray(path.data(), static_cast<int>(bytes / sizeof(wchar_t)) - 1);
    }
    RegCloseKey(children);
#endif
    return paths;
}

bool is_game_in_foreground() {
#ifdef Q_OS_WIN
    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN) return true;
    // A recognized game counts in a window too: keys pressed in it are meant for the game.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    const bool named = QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
    CloseHandle(process);
    if (!named) return false;
    const QString exe = QDir::fromNativeSeparators(QString::fromWCharArray(buffer.data(), static_cast<int>(size)));
    // Not cached: Windows lists a game only after it first runs, and checks follow foreground switches.
    for (const auto &game : recognized_game_executables())
        if (QDir::fromNativeSeparators(game).compare(exe, Qt::CaseInsensitive) == 0) return true;
    return false;
#else
    return false;
#endif
}

bool set_window_exclude_from_capture(quintptr hwnd, bool exclude) {
#ifdef Q_OS_WIN
    // Earlier versions take the value as WDA_MONITOR, which records the window as a black box.
    if (exclude && QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows10_2004) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    return SetWindowDisplayAffinity(to_hwnd(hwnd), exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
#else
    Q_UNUSED(hwnd); Q_UNUSED(exclude);
    return false;
#endif
}

void set_thread_cursor(Qt::CursorShape shape) {
#ifdef Q_OS_WIN
    switch (shape) {
    case Qt::ArrowCursor: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); break;
    case Qt::CrossCursor: SetCursor(LoadCursorW(nullptr, IDC_CROSS)); break;
    case Qt::BlankCursor: SetCursor(nullptr); break;
    default: break;
    }
#else
    Q_UNUSED(shape);
#endif
}

bool set_window_topmost(quintptr hwnd, bool topmost) {
#ifdef Q_OS_WIN
    return SetWindowPos(to_hwnd(hwnd), topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE) != FALSE;
#else
    Q_UNUSED(hwnd); Q_UNUSED(topmost);
    return false;
#endif
}

bool set_window_click_through(quintptr hwnd, bool enabled) {
#ifdef Q_OS_WIN
    const LONG_PTR style = GetWindowLongPtrW(to_hwnd(hwnd), GWL_EXSTYLE);
    const LONG_PTR new_style = enabled ? (style | WS_EX_TRANSPARENT) : (style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    if (new_style == style) return true;
    SetLastError(0);
    // Returns the previous value, which may itself be 0; LastError tells failure apart.
    return SetWindowLongPtrW(to_hwnd(hwnd), GWL_EXSTYLE, new_style) != 0 || GetLastError() == 0;
#else
    Q_UNUSED(hwnd); Q_UNUSED(enabled);
    return false;
#endif
}

bool supports_window_corner_preference() {
#ifdef Q_OS_WIN
    // Windows 11 (build 22000) rounds top-level windows and allows opting out per window.
    using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
    static const DWORD build = [] {
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof info;
        if (auto *ntdll = GetModuleHandleW(L"ntdll.dll"))
            if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&info);
        return info.dwBuildNumber;
    }();
    return build >= 22000;
#else
    return false;
#endif
}

bool set_window_rounded_corners(quintptr hwnd, bool rounded) {
    if (!supports_window_corner_preference()) return false;
#ifdef Q_OS_WIN
    const int value = rounded ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    return DwmSetWindowAttribute(to_hwnd(hwnd), DWMWA_WINDOW_CORNER_PREFERENCE, &value, sizeof value) == S_OK;
#else
    Q_UNUSED(hwnd); Q_UNUSED(rounded);
    return false;
#endif
}

void disable_native_frame(quintptr hwnd) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() != "windows") return;
    const std::pair<DWORD, DWORD> attributes[] = {
        {2, 1},            // DWMWA_NCRENDERING_POLICY = DWMNCRP_DISABLED
        {33, 1},           // DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_DONOTROUND
        {34, 0xFFFFFFFE},  // DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE
    };
    for (const auto &[attribute, value] : attributes) {
        DWORD native_value = value;
        // Windows 10 rejects the last two; the first still applies.
        DwmSetWindowAttribute(to_hwnd(hwnd), attribute, &native_value, sizeof native_value);
    }
#else
    Q_UNUSED(hwnd);
#endif
}

long long get_last_error() {
#ifdef Q_OS_WIN
    return static_cast<long long>(GetLastError());
#else
    return -1;
#endif
}

bool get_autostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const bool found = RegQueryValueExW(key, reinterpret_cast<LPCWSTR>(AUTOSTART_APP_NAME.utf16()), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

std::optional<QString> get_autostart_program() {
    const auto *name = reinterpret_cast<LPCWSTR>(AUTOSTART_APP_NAME.utf16());
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return std::nullopt;
    std::wstring data(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, name, RRF_RT_REG_SZ, nullptr, data.data(), &size) != ERROR_SUCCESS) return std::nullopt;
    // Of a quoted command line with arguments only the program counts.
    const QString command = QString::fromWCharArray(data.c_str()).trimmed();
    return command.startsWith('"') ? command.mid(1, command.indexOf('"', 1) - 1) : command;
}

bool startup_approved_disabled(const wchar_t *approved_key, const QString &name) {
    BYTE data[16] = {};
    DWORD size = sizeof(data);
    const auto status = RegGetValueW(HKEY_CURRENT_USER, approved_key, reinterpret_cast<LPCWSTR>(name.utf16()), RRF_RT_REG_BINARY, nullptr, data, &size);
    return status == ERROR_SUCCESS && size > 0 && (data[0] & 1) != 0;
}

bool autostart_disabled_by_system() { return get_autostart_program() && startup_approved_disabled(STARTUP_APPROVED_REG_KEY, AUTOSTART_APP_NAME); }

void set_autostart(bool enabled) {
    // The Run entry is shared with the Python version; tests never write it.
    if (!test_sandbox_dir().isEmpty()) return;
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_REG_KEY, 0, KEY_SET_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        log_exception(QString("RegOpenKeyEx failed: %1").arg(status), T("设置开机自启"));
        return;
    }
    const auto *name = reinterpret_cast<LPCWSTR>(AUTOSTART_APP_NAME.utf16());
    if (enabled) {
        const QString exe_path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(exe_path.utf16()), static_cast<DWORD>((exe_path.size() + 1) * sizeof(wchar_t)));
        if (status == ERROR_SUCCESS) log_info(T("已写入开机自启注册表项: {exe_path}", {{"exe_path", exe_path}}), "page6");
        else log_exception(QString("RegSetValueEx failed: %1").arg(status), T("设置开机自启"));
    } else {
        // A missing value is fine.
        status = RegDeleteValueW(key, name);
        if (status == ERROR_SUCCESS) log_info(T("已删除开机自启注册表项"), "page6");
        else if (status != ERROR_FILE_NOT_FOUND) log_exception(QString("RegDeleteValue failed: %1").arg(status), T("设置开机自启"));
    }
    RegCloseKey(key);
}
}
