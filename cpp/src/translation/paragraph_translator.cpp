#include "translation/paragraph_translator.h"
#include "core/logger.h"
#include <QHash>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace jietuba {
namespace {
using Clock = std::chrono::steady_clock;

// What an LLM is shown of the paragraphs before a batch, each cut to this length.
constexpr int CONTEXT_PARAGRAPHS = 2;
constexpr int CONTEXT_CHARS = 240;
// The pause after a rate limit, and the spacing of single requests once that is not enough.
constexpr int RATE_LIMIT_INTERVAL_MS = 1000;
// Batches that may break the one-to-one contract before any kept it; after that the
// service gets one text per request.
constexpr int BATCH_TRIALS = 2;

// A run of unique texts sent as one request.
struct Unit {
    int first = 0;
    int count = 0;
    bool retried = false;
};

struct Outcome {
    bool ok = false;
    QStringList translations;
    std::optional<TranslationErrorCode> code;
    QString message;
};

bool rules_out_every_text(TranslationErrorCode code) {
    return code == TranslationErrorCode::NOT_CONFIGURED || code == TranslationErrorCode::AUTH_FAILED || code == TranslationErrorCode::QUOTA_EXCEEDED ||
           code == TranslationErrorCode::UNSUPPORTED_LANGUAGE;
}

bool may_pass(TranslationErrorCode code) { return code == TranslationErrorCode::RATE_LIMITED || code == TranslationErrorCode::NETWORK_ERROR; }

class Run {
public:
    Run(const QStringList &texts, const std::vector<TranslationProvider *> &providers, const TranslationRequest &prototype, HttpCancellation *cancellation,
        const std::function<void(int, const QString &)> &settled)
        : providers_(providers), prototype_(prototype), cancellation_(cancellation), settled_(settled) {
        QHash<QString, int> seen;
        owner_.reserve(texts.size());
        for (int index = 0; index < texts.size(); ++index) {
            const auto &text = texts[index];
            if (text.trimmed().isEmpty()) {
                owner_.append(-1);
                continue;
            }
            auto it = seen.find(text);
            if (it == seen.end()) {
                it = seen.insert(text, static_cast<int>(unique_.size()));
                unique_.append(text);
                users_.append(QVector<int>());
            }
            owner_.append(it.value());
            users_[it.value()].append(index);
        }
        translated_.resize(unique_.size());
        const auto limits = providers_.front()->batch_limits();
        int index = 0;
        while (index < unique_.size()) {
            Unit unit{index, 0};
            qsizetype chars = 0;
            while (index < unique_.size() && unit.count < std::max(1, limits.max_texts) &&
                   (unit.count == 0 || limits.max_chars <= 0 || chars + unique_[index].size() <= limits.max_chars)) {
                chars += unique_[index].size();
                ++unit.count;
                ++index;
            }
            queue_.push_back(unit);
        }
        limit_ = std::clamp(providers_.front()->parallel_requests, 1, static_cast<int>(providers_.size()));
        interval_ms_ = std::max(0, providers_.front()->request_interval_ms);
        next_start_ = Clock::now();
    }

    ParagraphTranslations result() {
        const int threads = std::min(limit_, static_cast<int>(queue_.size()));
        std::vector<std::thread> helpers;
        for (int index = 1; index < threads; ++index) helpers.emplace_back([this, index] { work(*providers_[static_cast<size_t>(index)]); });
        if (threads > 0) work(*providers_.front());
        for (auto &helper : helpers) helper.join();

        ParagraphTranslations out;
        out.cancelled = cancelled_;
        out.fatal = fatal_;
        out.error = error_;
        out.translated_chars = translated_chars_;
        out.translations.reserve(owner_.size());
        for (const int unique : owner_) {
            out.translations.append(unique < 0 ? QString() : translated_[unique]);
            if (unique >= 0 && translated_[unique].isEmpty()) ++out.failed;
        }
        return out;
    }

private:
    void work(TranslationProvider &provider) {
        const HttpCancellationScope cancel_scope(cancellation_);
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stop_ || (queue_.empty() ? in_flight_ == 0 : in_flight_ < limit_); });
            if (stop_ || queue_.empty()) break;
            const Unit unit = queue_.front();
            queue_.pop_front();
            if (unit.count > 1 && !batch_usable_) {
                for (int offset = unit.count - 1; offset >= 0; --offset) queue_.push_front({unit.first + offset, 1});
                continue;
            }
            ++in_flight_;
            const int pace = pace_;
            const auto start = std::max(Clock::now(), next_start_);
            next_start_ = start + std::chrono::milliseconds(interval_ms_);
            lock.unlock();
            Outcome outcome;
            if (!wait_until(start)) {
                try {
                    outcome = send(unit, provider);
                } catch (const std::exception &e) {
                    outcome.message = QString::fromUtf8(e.what());
                }
            }
            lock.lock();
            --in_flight_;
            if (cancellation_ && cancellation_->cancelled()) {
                cancelled_ = true;
                stop_ = true;
            } else if (!stop_ || outcome.ok) {
                settle(unit, outcome, pace);
            }
            wake_.notify_all();
        }
        wake_.notify_all();
    }

    // True when the run was cancelled while waiting for its turn.
    bool wait_until(Clock::time_point start) const {
        const auto wait_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(start - Clock::now()).count());
        if (cancellation_) return cancellation_->wait(wait_ms);
        if (wait_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        return false;
    }

    Outcome send(const Unit &unit, TranslationProvider &provider) const {
        Outcome outcome;
        auto request = prototype_;
        if (unit.count == 1) {
            request.text = unique_[unit.first];
            const auto reply = provider.translate(request);
            outcome.ok = reply.success;
            outcome.translations = QStringList{reply.translated_text};
            outcome.code = reply.error_code;
            outcome.message = reply.error_message;
            return outcome;
        }
        // The paragraphs before this batch: the model has no other way to see what its
        // first entries continue. Sources only, so no batch has to wait for another.
        QStringList before;
        for (int index = std::max(0, unit.first - CONTEXT_PARAGRAPHS); index < unit.first; ++index) before << unique_[index].left(CONTEXT_CHARS);
        if (!before.isEmpty()) request.options.insert("context", before.join("\n"));
        const auto reply = provider.translate_batch(unique_.mid(unit.first, unit.count), request);
        outcome.ok = reply.success && reply.translations.size() == unit.count;
        outcome.translations = reply.translations;
        outcome.code = reply.error_code;
        outcome.message = reply.success ? QString("%1 translations for %2 texts").arg(reply.translations.size()).arg(unit.count) : reply.error_message;
        return outcome;
    }

    // Called with the lock held. pace is the pace the request was started at.
    void settle(const Unit &unit, const Outcome &outcome, int pace) {
        if (outcome.ok) {
            if (unit.count > 1) batch_kept_ = true;
            for (int offset = 0; offset < unit.count; ++offset) finish(unit.first + offset, outcome.translations.value(offset).trimmed());
            return;
        }
        const auto code = outcome.code.value_or(TranslationErrorCode::UNKNOWN);
        if (error_.isEmpty()) error_ = outcome.message;
        if (rules_out_every_text(code)) {
            error_ = outcome.message;
            fatal_ = true;
            stop_ = true;
            return;
        }
        if (may_pass(code)) {
            const bool rate_limited = code == TranslationErrorCode::RATE_LIMITED;
            // Refused at a pace that has been lowered since: only sent again.
            if (rate_limited && pace < pace_) {
                queue_.push_front(unit);
                return;
            }
            // A rate limit halves how many run at once, then spaces single requests out; an
            // unreachable service gets one request at a time straight away. At the slowest
            // pace a request is tried once more, and its second failure ends the run.
            const bool can_slow = rate_limited && (limit_ > 1 || interval_ms_ < RATE_LIMIT_INTERVAL_MS);
            if (!can_slow && unit.retried) {
                stop_ = true;
                return;
            }
            if (!rate_limited) limit_ = 1;
            else if (limit_ > 1) limit_ /= 2;
            else interval_ms_ = std::max(interval_ms_, RATE_LIMIT_INTERVAL_MS);
            if (rate_limited) next_start_ = Clock::now() + std::chrono::milliseconds(RATE_LIMIT_INTERVAL_MS);
            ++pace_;
            queue_.push_front({unit.first, unit.count, !can_slow});
            log_warning(QString("translation slowed to %1 at a time: %2").arg(limit_).arg(outcome.message), "ParagraphTranslator");
            return;
        }
        if (unit.count == 1) {
            finish(unit.first, QString());
            return;
        }
        // The reply did not line up with the texts: halves are asked again, down to single
        // texts. A service that never keeps the contract stops being asked for batches.
        if (!batch_kept_ && ++batch_misses_ >= BATCH_TRIALS) batch_usable_ = false;
        const int half = unit.count / 2;
        queue_.push_front({unit.first + half, unit.count - half});
        queue_.push_front({unit.first, half});
        log_warning(QString("batch of %1 split: %2").arg(unit.count).arg(outcome.message), "ParagraphTranslator");
    }

    void finish(int unique, const QString &translation) {
        translated_[unique] = translation;
        if (!translation.isEmpty()) translated_chars_ += unique_[unique].toUcs4().size();
        if (!settled_) return;
        for (const int index : users_[unique]) settled_(index, translation);
    }

    const std::vector<TranslationProvider *> &providers_;
    const TranslationRequest &prototype_;
    HttpCancellation *cancellation_;
    const std::function<void(int, const QString &)> &settled_;

    QStringList unique_;
    // Per text its entry in unique_, -1 for a blank one; per unique text the texts using it.
    QVector<int> owner_;
    QVector<QVector<int>> users_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Unit> queue_;
    QStringList translated_;
    int in_flight_ = 0;
    int limit_ = 1;
    int interval_ms_ = 0;
    // Counts the times the pace was lowered.
    int pace_ = 0;
    Clock::time_point next_start_;
    qint64 translated_chars_ = 0;
    bool batch_usable_ = true;
    bool batch_kept_ = false;
    int batch_misses_ = 0;
    bool stop_ = false;
    bool fatal_ = false;
    bool cancelled_ = false;
    QString error_;
};
}

ParagraphTranslations translate_paragraphs(const QStringList &texts, const std::vector<TranslationProvider *> &providers, const TranslationRequest &prototype,
                                           HttpCancellation *cancellation, const std::function<void(int index, const QString &translation)> &settled) {
    if (providers.empty()) {
        ParagraphTranslations none;
        none.translations.resize(texts.size());
        none.fatal = true;
        return none;
    }
    return Run(texts, providers, prototype, cancellation, settled).result();
}
}
