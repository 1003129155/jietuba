#pragma once
#include <QDateTime>
#include <QString>
#include <functional>
#include <map>
#include <stdexcept>

// Pure text transforms behind the Special Paste menu.
namespace jietuba::clipboard {
QString to_uppercase(const QString &text);
QString to_lowercase(const QString &text);
// Upper-cases the first letter of each word and lower-cases the rest. Digits and combining
// marks belong to the word, and an apostrophe inside a word does not start a new one.
QString capitalize_words(const QString &text);
// Upper-cases the first letter after each run of . ! ? and following whitespace.
QString capitalize_sentences(const QString &text);
QString toggle_case(const QString &text);
// Values split on whitespace, commas or semicolons, single-quoted with quotes doubled,
// wrapped only between values so lines stay within max_line_length where possible.
// Throws std::invalid_argument when max_line_length <= 0.
QString to_sql_in_clause(const QString &text, int max_line_length = 80);
QString remove_line_breaks(const QString &text);
QString remove_all_whitespace(const QString &text);
QString extract_digits(const QString &text);
QString extract_letters(const QString &text);
// Appends " YYYY-MM-DD HH:MM:SS" (no space after a trailing newline, space or tab).
QString append_current_time(const QString &text, const QDateTime &now = QDateTime::currentDateTime());

using TextTransform = std::function<QString(const QString &)>;
// Transform key -> function; labels live with the menu.
const std::map<QString, TextTransform> &TRANSFORM_REGISTRY();
}
