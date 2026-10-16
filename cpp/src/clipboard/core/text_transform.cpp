#include "clipboard/core/text_transform.h"
#include "core/py_text.h"
#include <QList>
#include <QStringList>

namespace jietuba::clipboard {
namespace {
bool is_sentence_end(char32_t c) { return c == U'.' || c == U'!' || c == U'?'; }

QString one(char32_t c) { return QString::fromUcs4(&c, 1); }

QString quote_sql_value(const QString &value) {
    QString escaped = value;
    escaped.replace("'", "''");
    return "'" + escaped + "'";
}

QStringList parse_sql_values(const QString &text) {
    QStringList values;
    QString current;
    for (char32_t c : text.toUcs4()) {
        if (py_isspace(c) || c == U',' || c == U';') {
            if (!current.isEmpty()) values.append(current);
            current.clear();
        } else {
            current += one(c);
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
        if (py_len(candidate) <= max_line_length || current_line.isEmpty()) {
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

QString to_uppercase(const QString &text) { return py_upper(text); }

QString to_lowercase(const QString &text) { return py_lower(text); }

QString capitalize_words(const QString &text) { return py_title(text); }

QString capitalize_sentences(const QString &text) {
    const auto points = code_points_of(text);
    QString result;
    QList<char32_t> segment;
    auto flush_segment = [&] {
        if (!segment.isEmpty()) {
            result += py_upper(one(segment.front()));
            if (segment.size() > 1) result += QString::fromUcs4(segment.constData() + 1, segment.size() - 1);
        }
        segment.clear();
    };
    qsizetype i = 0;
    while (i < points.size()) {
        if (!is_sentence_end(points[i])) {
            segment.append(points[i++]);
            continue;
        }
        flush_segment();
        const qsizetype start = i;
        while (i < points.size() && is_sentence_end(points[i])) ++i;
        while (i < points.size() && py_isspace(points[i])) ++i;
        result += QString::fromUcs4(points.constData() + start, i - start);
    }
    flush_segment();
    return result;
}

QString toggle_case(const QString &text) { return py_swapcase(text); }

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
    for (char32_t c : text.toUcs4())
        if (!py_isspace(c)) result += one(c);
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
