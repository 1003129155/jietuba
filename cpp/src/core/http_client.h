#pragma once
#include <QByteArray>
#include <QString>
#include <utility>
#include <vector>

namespace jietuba {
// One blocking HTTP exchange through the system's HTTP stack (WinHTTP, using the
// system proxy settings), with urllib's outcome classes: a status of 400 or more is an
// HTTP error carrying status, reason and body; timeouts and other network failures are
// told apart.
struct HttpResponse {
    enum Kind { Ok, HttpError, Timeout, NetworkError } kind = NetworkError;
    int status = 0;
    QString reason;     // status text for Ok/HttpError, failure description otherwise
    QByteArray body;
};

using HttpHeaders = std::vector<std::pair<QString, QString>>;

// Aborts the exchange running on a thread that holds it in an HttpCancellationScope;
// later exchanges there fail at once. The reference's urllib cannot be interrupted and
// waits for its timeout instead. cancel() may be called from any thread.
class HttpCancellation {
public:
    HttpCancellation();
    ~HttpCancellation();
    HttpCancellation(const HttpCancellation &) = delete;
    HttpCancellation &operator=(const HttpCancellation &) = delete;
    void cancel();
    bool cancelled() const;
    // The manual-reset event that cancel() signals.
    void *native_event() const { return event_; }
private:
    void *event_;
};

// http_request on the current thread watches cancellation while this lives.
class HttpCancellationScope {
public:
    explicit HttpCancellationScope(HttpCancellation *cancellation);
    ~HttpCancellationScope();
    HttpCancellationScope(const HttpCancellationScope &) = delete;
    HttpCancellationScope &operator=(const HttpCancellationScope &) = delete;
private:
    HttpCancellation *previous_;
};

// timeout_seconds applies to resolving, connecting, sending and each receive. A cancelled
// exchange is a network error ([WinError 12017]).
HttpResponse http_request(const QString &method, const QString &url, const HttpHeaders &headers = {}, const QByteArray &body = {},
                          double timeout_seconds = 30);

// urllib.parse.urlencode: UTF-8, spaces as '+', everything but letters, digits and "_.-~" escaped.
QString python_urlencode(const std::vector<std::pair<QString, QString>> &query);
}
