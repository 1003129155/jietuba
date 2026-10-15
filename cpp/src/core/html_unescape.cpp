#include "core/html_unescape.h"
#include <QHash>
#include <algorithm>

namespace jietuba {
namespace {
struct HtmlEntity {
    const char *name;
    const char *value;
};
struct InvalidCharref {
    char32_t number;
    char32_t replacement;
};
#include "core/html_entities.inc"

const QHash<QString, QString> &entities() {
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> result;
        for (const auto &entity : kHtml5Entities) result.insert(QString::fromUtf8(entity.name), QString::fromUtf8(entity.value));
        return result;
    }();
    return table;
}

QString from_code_point(char32_t code_point) { return QString::fromUcs4(&code_point, 1); }

bool is_ascii_digit(QChar c) { return c >= '0' && c <= '9'; }
bool is_hex_digit(QChar c) { return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

QString replace_numeric(const QString &digits, int base) {
    // References may have any number of digits; everything past 0x10FFFF is clamped
    // so it falls into the U+FFFD case below.
    qsizetype first = 0;
    while (first < digits.size() - 1 && digits.at(first) == '0') ++first;
    const auto significant = digits.mid(first);
    const qulonglong number = significant.size() > 10 ? 0x110000 : significant.toULongLong(nullptr, base);
    const char32_t num = static_cast<char32_t>(std::min<qulonglong>(number, 0x110000));
    for (const auto &entry : kInvalidCharrefs)
        if (entry.number == num) return from_code_point(entry.replacement);
    if ((num >= 0xD800 && num <= 0xDFFF) || num > 0x10FFFF) return QString(QChar(0xFFFD));
    if (std::find(std::begin(kInvalidCodepoints), std::end(kInvalidCodepoints), num) != std::end(kInvalidCodepoints)) return {};
    return from_code_point(num);
}

QString replace_named(const QString &name) {
    const auto &table = entities();
    const auto found = table.constFind(name);
    if (found != table.constEnd()) return *found;
    for (qsizetype x = name.size() - 1; x > 1; --x) {
        const auto prefix = table.constFind(name.left(x));
        if (prefix != table.constEnd()) return *prefix + name.mid(x);
    }
    return "&" + name;
}
}

QString html_unescape(const QString &text) {
    if (!text.contains('&')) return text;
    QString result;
    result.reserve(text.size());
    qsizetype i = 0;
    const qsizetype n = text.size();
    while (i < n) {
        const QChar c = text.at(i);
        if (c != '&' || i + 1 >= n) {
            result += c;
            ++i;
            continue;
        }
        const qsizetype start = i + 1;
        qsizetype end = start;
        QString replacement;
        bool matched = false;
        if (text.at(start) == '#') {
            const bool hex = start + 1 < n && (text.at(start + 1) == 'x' || text.at(start + 1) == 'X');
            const qsizetype digits_start = start + (hex ? 2 : 1);
            end = digits_start;
            while (end < n && (hex ? is_hex_digit(text.at(end)) : is_ascii_digit(text.at(end)))) ++end;
            if (end > digits_start) {
                matched = true;
                replacement = replace_numeric(text.mid(digits_start, end - digits_start), hex ? 16 : 10);
                if (end < n && text.at(end) == ';') ++end;
            }
        } else {
            static const QString stops = QStringLiteral("\t\n\f <&#;");
            while (end < n && end - start < 32 && !stops.contains(text.at(end))) ++end;
            if (end > start) {
                matched = true;
                if (end < n && text.at(end) == ';') ++end;
                replacement = replace_named(text.mid(start, end - start));
            }
        }
        if (!matched) {
            result += c;
            ++i;
            continue;
        }
        result += replacement;
        i = end;
    }
    return result;
}
}
