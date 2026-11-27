#pragma once
#include "core/http_client.h"
#include "translation/provider.h"
#include <functional>
#include <vector>

namespace jietuba {
struct ParagraphTranslations {
    // Same size and order as the texts; empty where a text has no translation.
    QStringList translations;
    // Texts left without a translation, and why the first of them failed.
    int failed = 0;
    QString error;
    // The key, the quota or the language pair rules out every text: nothing more was sent.
    bool fatal = false;
    bool cancelled = false;
    // UTF-32 length of the texts the service translated, for the quota bookkeeping.
    qint64 translated_chars = 0;
};

// Translates the paragraphs of one capture through one service. A capture has tens to
// hundreds of them, so the cost is the number of round trips and how many run at once:
// equal texts are sent once, a request carries as many texts as batch_limits() allows,
// and up to parallel_requests requests are in flight, spaced by request_interval_ms.
//
// providers are instances of the same service, one per request that may be in flight.
// prototype carries the target language, the options and the timeout; its text is ignored.
// settled is called once for every text that is not blank when its request is over, with
// an empty translation for one that failed: from any of the threads, one call at a time.
//
// A text that fails keeps an empty translation and the others still arrive. A rate limit
// halves how many requests run at once, down to one a second; a network failure sends the
// rest one at a time. A request that fails again at the slowest pace ends the run with
// what is there.
ParagraphTranslations translate_paragraphs(const QStringList &texts, const std::vector<TranslationProvider *> &providers, const TranslationRequest &prototype,
                                           HttpCancellation *cancellation = nullptr,
                                           const std::function<void(int index, const QString &translation)> &settled = {});
}
