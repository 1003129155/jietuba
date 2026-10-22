#include "clipboard/core/text_transform.h"
#include <QStringList>

namespace jietuba::clipboard {
namespace {
bool is_sentence_end(QChar c) { return c == u'.' || c == u'!' || c == u'?'; }

QString one(char32_t c) { return QString::fromUcs4(&c, 1); }

bool is_cased(char32_t c) { return QChar::isUpper(c) || QChar::isLower(c) || QChar::isTitleCase(c); }

bool is_apostrophe(char32_t c) { return c == U'\'' || c == 0x2019; }

QString upper_first(const QString &text) {
    const qsizetype n = text.size() > 1 && text[0].isHighSurrogate() && text[1].isLowSurrogate() ? 2 : 1;
    return text.left(n).toUpper() + text.mid(n);
}

QString quote_sql_value(const QString &value) {
    QString escaped = value;
    escaped.replace("'", "''");
    return "'" + escaped + "'";
}

QStringList parse_sql_values(const QString &text) {
    QStringList values;
    QString current;
    for (QChar c : text) {
        if (c.isSpace() || c == u',' || c == u';') {
            if (!current.isEmpty()) values.append(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.isEmpty()) values.append(current);
    return values;
}

QString format_sql_values(const QStringList &values, int max_line_length) {
    if (max_line_length <= 0) throw std::invalid_argument("max_line_length 必须大于 0");
    if (values.isEmpty()) return {};
    QStringList lines;
    QString current_line;
    for (qsizetype index = 0; index < values.size(); ++index) {
        const bool is_last = index == values.size() - 1;
        const QString quoted = quote_sql_value(values[index]);
        const QString token = is_last ? quoted : quoted + ",";
        const QString candidate = current_line + (current_line.isEmpty() ? "" : " ") + token;
        if (candidate.size() <= max_line_length || current_line.isEmpty()) {
            current_line = candidate;
            continue;
        }
        lines.append(current_line);
        current_line = token;
    }
    lines.append(current_line);
    return lines.join("\n");
}
}

QString to_uppercase(const QString &text) { return text.toUpper(); }

QString to_lowercase(const QString &text) { return text.toLower(); }

QString capitalize_words(const QString &text) {
    QString result;
    result.reserve(text.size());
    // Whether the previous character belongs to a word, so a letter here is not its first one.
    bool in_word = false;
    for (char32_t c : text.toUcs4()) {
        if (is_cased(c)) {
            result += in_word ? one(c).toLower() : one(QChar::toTitleCase(c));
            in_word = true;
            continue;
        }
        result += one(c);
        if (QChar::isNumber(c)) in_word = true;
        else if (!QChar::isMark(c) && !is_apostrophe(c)) in_word = false;
    }
    return result;
}

QString capitalize_sentences(const QString &text) {
    QString result;
    qsizetype i = 0;
    while (i < text.size()) {
        const qsizetype start = i;
        while (i < text.size() && !is_sentence_end(text[i])) ++i;
        if (i > start) result += upper_first(text.mid(start, i - start));
        const qsizetype end_start = i;
        while (i < text.size() && is_sentence_end(text[i])) ++i;
        while (i < text.size() && text[i].isSpace()) ++i;
        result += text.mid(end_start, i - end_start);
    }
    return result;
}

QString toggle_case(const QString &text) {
    QString result;
    result.reserve(text.size());
    for (char32_t c : text.toUcs4()) {
        if (QChar::isUpper(c)) result += one(c).toLower();
        else if (QChar::isLower(c)) result += one(c).toUpper();
        else result += one(c);
    }
    return result;
}

QString to_sql_in_clause(const QString &text, int max_line_length) { return format_sql_values(parse_sql_values(text), max_line_length); }

QString remove_line_breaks(const QString &text) {
    QString result;
    bool in_break = false;
    for (QChar c : text) {
        if (c == u'\r' || c == u'\n') {
            if (!in_break) result += u' ';
            in_break = true;
        } else {
            result += c;
            in_break = false;
        }
    }
    return result;
}

QString remove_all_whitespace(const QString &text) {
    QString result;
    for (QChar c : text)
        if (!c.isSpace()) result += c;
    return result;
}

QString extract_digits(const QString &text) {
    QString result;
    for (QChar c : text)
        if (c >= u'0' && c <= u'9') result += c;
    return result;
}

QString extract_letters(const QString &text) {
    QString result;
    for (QChar c : text)
        if ((c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z')) result += c;
    return result;
}

QString append_current_time(const QString &text, const QDateTime &now) {
    const QString stamp = now.toString("yyyy-MM-dd HH:mm:ss");
    if (!text.isEmpty() && !text.endsWith(u'\n') && !text.endsWith(u' ') && !text.endsWith(u'\t')) return text + " " + stamp;
    return text + stamp;
}

const std::map<QString, TextTransform> &TRANSFORM_REGISTRY() {
    static const std::map<QString, TextTransform> registry{
        {"transform_uppercase", to_uppercase},
        {"transform_lowercase", to_lowercase},
        {"transform_capitalize_words", capitalize_words},
        {"transform_capitalize_sentences", capitalize_sentences},
        {"transform_toggle_case", toggle_case},
        {"transform_sql_in", [](const QString &text) { return to_sql_in_clause(text); }},
        {"transform_remove_linebreaks", remove_line_breaks},
        {"transform_remove_whitespace", remove_all_whitespace},
        {"transform_extract_digits", extract_digits},
        {"transform_extract_letters", extract_letters},
        {"transform_append_time", [](const QString &text) { return append_current_time(text); }},
    };
    return registry;
}
}
