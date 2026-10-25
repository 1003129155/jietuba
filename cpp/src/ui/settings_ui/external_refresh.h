#pragma once
#include "core/logger.h"
#include <QObject>
#include <QThread>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

namespace jietuba {
// Queries own their inputs; only apply runs on the widget thread. A newer request supersedes
// an in-flight result, and destroying the owner disconnects delivery without waiting for I/O.
template <typename Result> class ExternalRefresh : public QObject {
public:
    ExternalRefresh(QObject *owner, std::function<Result()> query, std::function<void(const Result &)> apply)
        : QObject(owner), query_(std::move(query)), apply_(std::move(apply)) {}

    void request() {
        if (running_) {
            again_ = true;
            return;
        }
        if (queued_) return;
        queued_ = true;
        // Two event-loop turns: showing a window queues its activation, which is delivered after
        // the first turn's posted calls, so the request it makes joins this one instead of
        // superseding a query that has just started.
        QTimer::singleShot(0, this, [this] {
            QTimer::singleShot(0, this, [this] {
                queued_ = false;
                start();
            });
        });
    }

private:
    void start() {
        running_ = true;
        auto result = std::make_shared<std::optional<Result>>();
        auto error = std::make_shared<QString>();
        auto *thread = QThread::create([query = query_, result, error] {
            try {
                result->emplace(query());
            } catch (const std::exception &e) {
                *error = QString::fromUtf8(e.what());
            } catch (...) {
                *error = "unknown error";
            }
        });
        connect(thread, &QThread::finished, this, [this, result, error] {
            running_ = false;
            if (again_) {
                again_ = false;
                request();
                return;
            }
            if (*result) apply_(**result);
            else log_warning("Settings refresh failed: " + *error, "Settings");
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    std::function<Result()> query_;
    std::function<void(const Result &)> apply_;
    bool queued_ = false;
    bool running_ = false;
    bool again_ = false;
};
}
