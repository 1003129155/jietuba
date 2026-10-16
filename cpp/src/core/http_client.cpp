#include "core/http_client.h"
#include <QStringList>
#include <algorithm>
#include <memory>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace jietuba {
namespace {
struct InternetHandle {
    HINTERNET handle = nullptr;
    explicit InternetHandle(HINTERNET h) : handle(h) {}
    ~InternetHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
    InternetHandle(const InternetHandle &) = delete;
    InternetHandle &operator=(const InternetHandle &) = delete;
    explicit operator bool() const { return handle != nullptr; }
};

QString winhttp_error_text(DWORD code) {
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                        GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    QString text = length ? QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed() : QString();
    if (buffer) LocalFree(buffer);
    return QString("[WinError %1] %2").arg(code).arg(text);
}

HttpResponse failure(DWORD code) {
    HttpResponse response;
    response.kind = code == ERROR_WINHTTP_TIMEOUT ? HttpResponse::Timeout : HttpResponse::NetworkError;
    response.reason = winhttp_error_text(code);
    return response;
}

thread_local HttpCancellation *current_cancellation = nullptr;

// The exchange runs in WinHTTP's asynchronous mode so that a cancel is just an event and
// only the calling thread touches the handles: WinHTTP forbids any call on a handle once
// another thread has closed it. Each step waits for exactly one completion.
struct Completion {
    HANDLE step = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD error = 0;
    DWORD bytes = 0;
    Completion() = default;
    ~Completion() {
        CloseHandle(step);
        CloseHandle(closed);
    }
    Completion(const Completion &) = delete;
    Completion &operator=(const Completion &) = delete;
};

void CALLBACK on_request_status(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
    auto *completion = reinterpret_cast<Completion *>(context);
    if (!completion) return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        completion->bytes = *static_cast<const DWORD *>(info);
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        completion->bytes = length;
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        completion->error = static_cast<const WINHTTP_ASYNC_RESULT *>(info)->dwError;
        break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        SetEvent(completion->closed);
        return;
    default:
        return;
    }
    SetEvent(completion->step);
}

// Closing the request handle is what stops a pending step. Callbacks end with
// HANDLE_CLOSING, so the completion and the buffers WinHTTP writes into must outlive this.
struct AsyncRequest {
    HINTERNET handle;
    Completion &completion;
    ~AsyncRequest() {
        WinHttpCloseHandle(handle);
        WaitForSingleObject(completion.closed, INFINITE);
    }
};

// The step's error (0 on success). The deadline only backs up WinHTTP's own timeouts.
DWORD wait_step(const Completion &completion, const HttpCancellation *cancellation, DWORD deadline_ms) {
    const HANDLE handles[] = {completion.step, cancellation ? static_cast<HANDLE>(cancellation->native_event()) : nullptr};
    switch (WaitForMultipleObjects(cancellation ? 2 : 1, handles, FALSE, deadline_ms)) {
    case WAIT_OBJECT_0: return completion.error;
    case WAIT_OBJECT_0 + 1: return ERROR_WINHTTP_OPERATION_CANCELLED;
    case WAIT_TIMEOUT: return ERROR_WINHTTP_TIMEOUT;
    default: return GetLastError();
    }
}
}

HttpCancellation::HttpCancellation() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

HttpCancellation::~HttpCancellation() { CloseHandle(static_cast<HANDLE>(event_)); }

void HttpCancellation::cancel() { SetEvent(static_cast<HANDLE>(event_)); }

bool HttpCancellation::cancelled() const { return WaitForSingleObject(static_cast<HANDLE>(event_), 0) == WAIT_OBJECT_0; }

HttpCancellationScope::HttpCancellationScope(HttpCancellation *cancellation) : previous_(current_cancellation) { current_cancellation = cancellation; }

HttpCancellationScope::~HttpCancellationScope() { current_cancellation = previous_; }

HttpResponse http_request(const QString &method, const QString &url, const HttpHeaders &headers, const QByteArray &body, double timeout_seconds) {
    const HttpCancellation *cancellation = current_cancellation;
    if (cancellation && cancellation->cancelled()) return failure(ERROR_WINHTTP_OPERATION_CANCELLED);
    const auto wide_url = url.toStdWString();
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) return failure(GetLastError());
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) + std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    // The same User-Agent the reference's urllib sends.
    InternetHandle session(WinHttpOpen(L"Python-urllib/3.11", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session) return failure(GetLastError());
    const int timeout_ms = static_cast<int>(timeout_seconds * 1000);
    WinHttpSetTimeouts(session.handle, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    // The first step may take resolving, connecting and sending; WinHTTP checks timeouts coarsely.
    const DWORD deadline_ms = timeout_ms > 0 ? static_cast<DWORD>(std::min<qint64>(qint64(timeout_ms) * 3 + 5000, INFINITE - 1)) : INFINITE;
    InternetHandle connection(WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0));
    if (!connection) return failure(GetLastError());
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    std::wstring header_text;
    for (const auto &[name, value] : headers) header_text += (name + ": " + value + "\r\n").toStdWString();
    QByteArray chunk;
    Completion completion;
    HINTERNET handle = WinHttpOpenRequest(connection.handle, method.toStdWString().c_str(), path.empty() ? L"/" : path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!handle) return failure(GetLastError());
    auto context = reinterpret_cast<DWORD_PTR>(&completion);
    if (!WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof context) ||
        WinHttpSetStatusCallback(handle, on_request_status, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        const DWORD error = GetLastError();
        WinHttpCloseHandle(handle);
        return failure(error);
    }
    const AsyncRequest request{handle, completion};
    const auto step = [&](auto &&start) -> DWORD {
        completion.error = 0;
        if (!start()) return GetLastError();
        return wait_step(completion, cancellation, deadline_ms);
    };
    DWORD error = step([&] {
        return WinHttpSendRequest(handle, header_text.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_text.c_str(), header_text.empty() ? 0 : static_cast<DWORD>(-1L),
                                  body.isEmpty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.constData()), static_cast<DWORD>(body.size()),
                                  static_cast<DWORD>(body.size()), context);
    });
    if (!error) error = step([&] { return WinHttpReceiveResponse(handle, nullptr); });
    if (error) return failure(error);
    HttpResponse response;
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    DWORD text_size = 0;
    WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &text_size, WINHTTP_NO_HEADER_INDEX);
    if (text_size) {
        std::wstring text(text_size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &text_size, WINHTTP_NO_HEADER_INDEX))
            response.reason = QString::fromWCharArray(text.c_str(), static_cast<int>(text_size / sizeof(wchar_t)));
    }
    for (;;) {
        if ((error = step([&] { return WinHttpQueryDataAvailable(handle, nullptr); }))) return failure(error);
        const DWORD available = completion.bytes;
        if (available == 0) break;
        chunk.resize(static_cast<qsizetype>(available));
        if ((error = step([&] { return WinHttpReadData(handle, chunk.data(), available, nullptr); }))) return failure(error);
        response.body.append(chunk.constData(), static_cast<qsizetype>(completion.bytes));
    }
    response.kind = response.status >= 400 ? HttpResponse::HttpError : HttpResponse::Ok;
    return response;
}

QString python_urlencode(const std::vector<std::pair<QString, QString>> &query) {
    static const QByteArray safe = "_.-~";
    const auto quote_plus = [](const QString &value) {
        QByteArray out;
        for (const char raw : value.toUtf8()) {
            const auto c = static_cast<unsigned char>(raw);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || safe.contains(static_cast<char>(c))) out.append(static_cast<char>(c));
            else if (c == ' ') out.append('+');
            else out.append(QString("%%1").arg(c, 2, 16, QChar('0')).toUpper().toLatin1());
        }
        return QString::fromLatin1(out);
    };
    QStringList parts;
    for (const auto &[key, value] : query) parts.append(quote_plus(key) + "=" + quote_plus(value));
    return parts.join('&');
}
}
