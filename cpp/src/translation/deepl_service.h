#pragma once
#include <QString>
#include <optional>

namespace jietuba {
// DeepLService::translate's outcome.
struct DeepLResult {
    bool success = false;
    QString translated_text;
    QString detected_source_lang;
    QString error;
};

// The DeepL API.
class DeepLService {
public:
    static inline const QString API_URL_FREE = "https://api-free.deepl.com/v2/translate";
    static inline const QString API_URL_PRO = "https://api.deepl.com/v2/translate";
    // use_pro selects the paid API.
    explicit DeepLService(const QString &api_key, bool use_pro = false) : api_key(api_key), api_url(use_pro ? API_URL_PRO : API_URL_FREE) {}
    // split_sentences: "0" none, "1" automatic (keeps line breaks), "nonewlines" ignores line breaks.
    DeepLResult translate(const QString &text, const QString &target_lang = "ZH", const std::optional<QString> &source_lang = std::nullopt,
                          const QString &split_sentences = "1", bool preserve_formatting = true, int timeout = 30);
    QString api_key;
    QString api_url;
private:
    static QString parse_http_error(int code, const QString &reason);
};
}
