#include "core/word_boundaries.h"
#include <QTextBoundaryFinder>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jietuba {
namespace {
#ifdef Q_OS_WIN
// The ICU of Windows 10 1903 and later, which finds Chinese and Japanese words from a dictionary.
struct SystemIcu {
    using Open = void *(*)(int type, const char *locale, const char16_t *text, int32_t length, int *status);
    using Next = int32_t (*)(void *iterator);
    using Close = void (*)(void *iterator);
    Open open = nullptr;
    Next next = nullptr;
    Close close = nullptr;
};

const SystemIcu &system_icu() {
    static const SystemIcu icu = [] {
        SystemIcu loaded;
        if (const HMODULE module = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            loaded.open = reinterpret_cast<SystemIcu::Open>(GetProcAddress(module, "ubrk_open"));
            loaded.next = reinterpret_cast<SystemIcu::Next>(GetProcAddress(module, "ubrk_next"));
            loaded.close = reinterpret_cast<SystemIcu::Close>(GetProcAddress(module, "ubrk_close"));
            if (!loaded.open || !loaded.next || !loaded.close) loaded = {};
        }
        return loaded;
    }();
    return icu;
}

constexpr int UBRK_WORD = 1, UBRK_DONE = -1;

bool icu_boundaries(const QString &text, QList<int> &boundaries) {
    const auto &icu = system_icu();
    if (!icu.open) return false;
    int status = 0;
    // The root locale: the dictionary covers Chinese and Japanese whatever the system language.
    void *iterator = icu.open(UBRK_WORD, "", reinterpret_cast<const char16_t *>(text.utf16()), static_cast<int32_t>(text.size()), &status);
    // Positive codes are errors, negative ones warnings.
    if (!iterator || status > 0) {
        if (iterator) icu.close(iterator);
        return false;
    }
    boundaries = {0};
    for (int32_t position = icu.next(iterator); position != UBRK_DONE; position = icu.next(iterator)) boundaries.append(position);
    icu.close(iterator);
    return true;
}
#endif

bool has_letter_or_number(QStringView piece) {
    return std::any_of(piece.begin(), piece.end(), [](QChar c) { return c.isLetterOrNumber(); });
}
}

QList<int> word_boundaries(const QString &text) {
    if (text.isEmpty()) return {0};
#ifdef Q_OS_WIN
    if (QList<int> boundaries; icu_boundaries(text, boundaries)) return boundaries;
#endif
    return punctuation_word_boundaries(text);
}

QList<int> punctuation_word_boundaries(const QString &text) {
    if (text.isEmpty()) return {0};
    // Unicode's word breaks keep "it's", "3.14" and "snake_case" whole; the pieces with letters or
    // digits that they leave side by side, as Chinese characters one by one, join into one word.
    QList<int> breaks = {0};
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    for (auto position = finder.toNextBoundary(); position != -1; position = finder.toNextBoundary()) breaks.append(static_cast<int>(position));
    QList<int> boundaries = {0};
    for (int i = 1; i + 1 < breaks.size(); ++i) {
        const QStringView before = QStringView(text).mid(breaks[i - 1], breaks[i] - breaks[i - 1]);
        const QStringView after = QStringView(text).mid(breaks[i], breaks[i + 1] - breaks[i]);
        if (!(has_letter_or_number(before) && has_letter_or_number(after))) boundaries.append(breaks[i]);
    }
    boundaries.append(static_cast<int>(text.size()));
    return boundaries;
}
}
