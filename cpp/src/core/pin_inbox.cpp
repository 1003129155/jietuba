#include "core/pin_inbox.h"
#include <QFileInfo>
#include <iterator>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
constexpr wchar_t WINDOW_CLASS[] = L"jietuba.PinRequest";
// Marks a WM_COPYDATA message as a pin request.
constexpr ULONG_PTR REQUEST_MAGIC = 0x4A544250;
// Large enough for the longest request parse_pin_request takes.
constexpr DWORD MAX_PAYLOAD = 4 * 1024 * 1024;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams));
    } else if (message == WM_COPYDATA) {
        auto *inbox = reinterpret_cast<PinInbox *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        const auto *data = reinterpret_cast<COPYDATASTRUCT *>(lparam);
        if (!inbox || !data || data->dwData != REQUEST_MAGIC || !data->lpData || data->cbData > MAX_PAYLOAD) return FALSE;
        return inbox->receive(QByteArray(static_cast<const char *>(data->lpData), static_cast<qsizetype>(data->cbData))) ? TRUE : FALSE;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::wstring mutex_name(const QString &instance_name) { return (QStringLiteral("Local\\jietuba.primary.") + instance_name).toStdWString(); }
}

QString own_instance_name() {
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return QFileInfo(QString::fromWCharArray(path, static_cast<int>(length))).fileName().toLower();
}

PinInbox::PinInbox(const QString &instance_name) {
    WNDCLASSEXW window_class = {sizeof(window_class)};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = WINDOW_CLASS;
    // A second inbox in one process finds the class registered already.
    RegisterClassExW(&window_class);
    const std::wstring name = instance_name.toStdWString();
    HWND window = CreateWindowExW(0, WINDOW_CLASS, name.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, window_class.hInstance, this);
    window_ = window;
    // An instance running as administrator still takes requests from the user's ordinary processes.
    if (window) ChangeWindowMessageFilterEx(window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
}

PinInbox::~PinInbox() {
    if (window_) DestroyWindow(static_cast<HWND>(window_));
}

void PinInbox::set_handler(std::function<void(const QStringList &)> handler) {
    handler_ = std::move(handler);
    auto pending = std::exchange(pending_, {});
    if (handler_)
        for (const auto &files : pending) handler_(files);
}

bool PinInbox::receive(const QByteArray &payload) {
    const auto request = parse_pin_request(payload);
    if (!request) return false;
    if (!seen_.remember(request->id)) return true;
    if (handler_) handler_(request->files);
    else pending_.append(request->files);
    return true;
}

SendResult send_pin_request(const QString &instance_name, const PinRequest &request, unsigned timeout_ms) {
    const std::wstring name = instance_name.toStdWString();
    HWND window = FindWindowExW(HWND_MESSAGE, nullptr, WINDOW_CLASS, name.c_str());
    if (!window) return SendResult::NoWindow;
    // So the instance may bring a window of its own forward in answer.
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id) AllowSetForegroundWindow(process_id);
    QByteArray payload = pack_pin_request(request);
    COPYDATASTRUCT data = {REQUEST_MAGIC, static_cast<DWORD>(payload.size()), payload.data()};
    DWORD_PTR answer = 0;
    if (!SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, timeout_ms, &answer)) return SendResult::Failed;
    return answer ? SendResult::Delivered : SendResult::Failed;
}

PrimaryInstance::PrimaryInstance(const QString &instance_name) : name_(instance_name) {}

PrimaryInstance::~PrimaryInstance() {
    release();
    if (mutex_) CloseHandle(static_cast<HANDLE>(mutex_));
}

bool PrimaryInstance::try_claim(unsigned wait_ms) {
    if (held_) return true;
    if (!mutex_) mutex_ = CreateMutexW(nullptr, FALSE, mutex_name(name_).c_str());
    // No handle: the mutex belongs to a process this one may not open.
    if (!mutex_) return false;
    const DWORD result = WaitForSingleObject(static_cast<HANDLE>(mutex_), wait_ms);
    // An abandoned mutex is the holder having ended, which is what is waited for.
    held_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    return held_;
}

void PrimaryInstance::release() {
    if (held_ && mutex_) ReleaseMutex(static_cast<HANDLE>(mutex_));
    held_ = false;
}

PinRoute route_pin_request(PrimaryInstance &primary, const QStringList &files, const PinDelivery &delivery, const QString &instance_name) {
    const PinRequest request{new_pin_request_id(), files};
    const ULONGLONG deadline = GetTickCount64() + delivery.deadline_ms;
    bool elevated_started = false;
    while (GetTickCount64() < deadline) {
        if (send_pin_request(instance_name, request, delivery.send_timeout_ms) == SendResult::Delivered) return PinRoute::Delivered;
        if (primary.try_claim(0)) {
            if (!delivery.start_elevated || elevated_started) return PinRoute::Primary;
            // The elevated instance claims the mutex itself, so it is let go first.
            elevated_started = true;
            primary.release();
            if (delivery.start_elevated()) continue;
            if (primary.try_claim(0)) return PinRoute::Primary;
        }
        Sleep(delivery.poll_ms);
    }
    return PinRoute::TimedOut;
}
}
