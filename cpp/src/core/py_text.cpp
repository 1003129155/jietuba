#include "core/py_text.h"
#include <QList>
#include <algorithm>
#include <iterator>
#include <string>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
QList<char32_t> code_points(const QString &text) { return code_points_of(text); }

QString from_code_points(const QList<char32_t> &points, qsizetype start, qsizetype end) {
    if (start >= end) return {};
    return QString::fromUcs4(points.constData() + start, end - start);
}

QString one(char32_t c) { return QString::fromUcs4(&c, 1); }

// Case data of the reference interpreter's Unicode version (scripts/generate_python_case_tables.py),
// so case conversions match Python rather than Qt's newer tables.
struct CaseMapping {
    char32_t code;
    char32_t mapped[3];
};
struct CodeRange {
    char32_t first, last;
};
#include "core/py_case_tables.inc"

template <size_t N> const CaseMapping *find_mapping(const CaseMapping (&table)[N], char32_t c) {
    const auto *it = std::lower_bound(std::begin(table), std::end(table), c, [](const CaseMapping &entry, char32_t code) { return entry.code < code; });
    return it != std::end(table) && it->code == c ? it : nullptr;
}

template <size_t N> bool in_ranges(const CodeRange (&table)[N], char32_t c) {
    const auto *it = std::upper_bound(std::begin(table), std::end(table), c, [](char32_t code, const CodeRange &range) { return code < range.first; });
    return it != std::begin(table) && c <= std::prev(it)->last;
}

template <size_t N> void append_mapped(QString &out, const CaseMapping (&table)[N], char32_t c) {
    const auto *entry = find_mapping(table, c);
    if (!entry) {
        out += one(c);
        return;
    }
    for (char32_t mapped : entry->mapped)
        if (mapped) out += one(mapped);
}

bool is_upper(char32_t c) { return in_ranges(UPPERCASE_RANGES, c); }
bool is_lower(char32_t c) { return in_ranges(LOWERCASE_RANGES, c); }
bool is_cased(char32_t c) { return in_ranges(CASED_RANGES, c); }
bool is_case_ignorable(char32_t c) { return in_ranges(CASE_IGNORABLE_RANGES, c); }

// Capital sigma lowers to final sigma after a cased letter and not before one, skipping
// case-ignorable characters on both sides.
char32_t lowered_sigma(const QList<char32_t> &points, qsizetype i) {
    qsizetype j = i - 1;
    while (j >= 0 && is_case_ignorable(points[j])) --j;
    bool final_sigma = j >= 0 && is_cased(points[j]);
    if (final_sigma && i + 1 < points.size()) {
        j = i + 1;
        while (j < points.size() && is_case_ignorable(points[j])) ++j;
        final_sigma = j == points.size() || !is_cased(points[j]);
    }
    return final_sigma ? 0x3C2 : 0x3C3;
}

void append_lower(QString &out, const QList<char32_t> &points, qsizetype i) {
    if (points[i] == 0x3A3) out += one(lowered_sigma(points, i));
    else append_mapped(out, LOWER_MAPPINGS, points[i]);
}

// Windows path separators.
bool is_sep(QChar c) { return c == u'\\' || c == u'/'; }

std::pair<QString, QString> splitdrive(const QString &p) {
    if (p.size() >= 2) {
        QString normp = p;
        normp.replace(u'/', u'\\');
        if (normp.left(2) == "\\\\") {
            const qsizetype start = normp.left(8).toUpper() == "\\\\?\\UNC\\" ? 8 : 2;
            const qsizetype index = normp.indexOf(u'\\', start);
            if (index == -1) return {p, {}};
            const qsizetype index2 = normp.indexOf(u'\\', index + 1);
            if (index2 == -1) return {p, {}};
            return {p.left(index2), p.mid(index2)};
        }
        if (normp.mid(1, 1) == ":") return {p.left(2), p.mid(2)};
    }
    return {{}, p};
}

std::pair<QString, QString> split(const QString &path) {
    auto [drive, p] = splitdrive(path);
    qsizetype i = p.size();
    while (i && !is_sep(p[i - 1])) --i;
    QString head = p.left(i);
    const QString tail = p.mid(i);
    qsizetype end = head.size();
    while (end && is_sep(head[end - 1])) --end;
    if (end) head = head.left(end);
    return {drive + head, tail};
}
}

QList<char32_t> code_points_of(const QString &text) {
    const auto points = text.toUcs4();
    return QList<char32_t>(points.begin(), points.end());
}

bool py_isspace(char32_t c) {
    switch (c) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20:
    case 0x85: case 0xA0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

QString py_lstrip(const QString &text) {
    const auto points = code_points(text);
    qsizetype start = 0;
    while (start < points.size() && py_isspace(points[start])) ++start;
    return from_code_points(points, start, points.size());
}

QString py_rstrip(const QString &text) {
    const auto points = code_points(text);
    qsizetype end = points.size();
    while (end > 0 && py_isspace(points[end - 1])) --end;
    return from_code_points(points, 0, end);
}

QString py_strip(const QString &text) { return py_rstrip(py_lstrip(text)); }

QString py_strip_chars(const QString &text, const QString &chars) {
    const auto points = code_points(text);
    const auto strip = code_points(chars);
    qsizetype start = 0, end = points.size();
    while (start < end && strip.contains(points[start])) ++start;
    while (end > start && strip.contains(points[end - 1])) --end;
    return from_code_points(points, start, end);
}

qsizetype py_len(const QString &text) { return text.toUcs4().size(); }

std::optional<qint64> py_int(const QString &text) {
    const auto points = code_points_of(py_strip(text));
    qsizetype i = 0;
    bool negative = false;
    if (i < points.size() && (points[i] == U'+' || points[i] == U'-')) negative = points[i++] == U'-';
    constexpr qint64 max = std::numeric_limits<qint64>::max();
    qint64 value = 0;
    bool saturated = false, after_digit = false;
    for (; i < points.size(); ++i) {
        const char32_t c = points[i];
        if (c == U'_') {
            if (!after_digit) return std::nullopt;
            after_digit = false;
            continue;
        }
        if (QChar::category(c) != QChar::Number_DecimalDigit) return std::nullopt;
        const int digit = QChar::digitValue(c);
        if (value > (max - digit) / 10) saturated = true;
        else if (!saturated) value = value * 10 + digit;
        after_digit = true;
    }
    if (!after_digit) return std::nullopt;
    if (saturated) return negative ? std::numeric_limits<qint64>::min() : max;
    return negative ? -value : value;
}

QString py_slice(const QString &text, qsizetype start, qsizetype end) {
    const auto points = code_points(text);
    const qsizetype size = points.size();
    auto clamp = [size](qsizetype index) {
        if (index < 0) index += size;
        return std::clamp<qsizetype>(index, 0, size);
    };
    return from_code_points(points, clamp(start), clamp(end));
}

QString py_upper(const QString &text) {
    QString out;
    for (char32_t c : code_points(text)) append_mapped(out, UPPER_MAPPINGS, c);
    return out;
}

QString py_lower(const QString &text) {
    const auto points = code_points(text);
    QString out;
    for (qsizetype i = 0; i < points.size(); ++i) append_lower(out, points, i);
    return out;
}

QString py_title(const QString &text) {
    const auto points = code_points(text);
    QString out;
    bool previous_is_cased = false;
    for (qsizetype i = 0; i < points.size(); ++i) {
        if (previous_is_cased) append_lower(out, points, i);
        else append_mapped(out, TITLE_MAPPINGS, points[i]);
        previous_is_cased = is_cased(points[i]);
    }
    return out;
}

QString py_swapcase(const QString &text) {
    const auto points = code_points(text);
    QString out;
    for (qsizetype i = 0; i < points.size(); ++i) {
        if (is_upper(points[i])) append_lower(out, points, i);
        else if (is_lower(points[i])) append_mapped(out, UPPER_MAPPINGS, points[i]);
        else out += one(points[i]);
    }
    return out;
}

QString py_casefold(const QString &text) {
    QString out;
    for (char32_t c : code_points(text)) append_mapped(out, FOLD_MAPPINGS, c);
    return out;
}

QString py_basename(const QString &path) { return split(path).second; }

QString py_dirname(const QString &path) { return split(path).first; }

QString py_path_join(const QString &first, const QString &second) {
    auto [result_drive, result_path] = splitdrive(first);
    const auto [p_drive, p_path] = splitdrive(second);
    bool relative = true;
    if (!p_path.isEmpty() && is_sep(p_path[0])) {
        if (!p_drive.isEmpty() || result_drive.isEmpty()) result_drive = p_drive;
        result_path = p_path;
        relative = false;
    } else if (!p_drive.isEmpty() && p_drive != result_drive) {
        if (p_drive.toLower() != result_drive.toLower()) {
            result_drive = p_drive;
            result_path = p_path;
            relative = false;
        } else {
            result_drive = p_drive;
        }
    }
    if (relative) {
        if (!result_path.isEmpty() && !is_sep(result_path.back())) result_path += u'\\';
        result_path += p_path;
    }
    if (!result_path.isEmpty() && !is_sep(result_path[0]) && !result_drive.isEmpty() && !result_drive.endsWith(u':'))
        return result_drive + u'\\' + result_path;
    return result_drive + result_path;
}

QString py_normpath(const QString &input) {
    QString path = input;
    path.replace(u'/', u'\\');
    auto [prefix, rest] = splitdrive(path);
    if (rest.startsWith(u'\\')) {
        prefix += u'\\';
        qsizetype skip = 0;
        while (skip < rest.size() && rest[skip] == u'\\') ++skip;
        rest = rest.mid(skip);
    }
    QStringList comps = rest.split(u'\\');
    qsizetype i = 0;
    while (i < comps.size()) {
        if (comps[i].isEmpty() || comps[i] == ".") {
            comps.removeAt(i);
        } else if (comps[i] == "..") {
            if (i > 0 && comps[i - 1] != "..") {
                comps.remove(i - 1, 2);
                --i;
            } else if (i == 0 && prefix.endsWith(u'\\')) {
                comps.removeAt(i);
            } else {
                ++i;
            }
        } else {
            ++i;
        }
    }
    if (prefix.isEmpty() && comps.isEmpty()) comps.append(".");
    return prefix + comps.join(u'\\');
}

QString py_normcase(const QString &path) {
    if (path.isEmpty()) return path;
    QString text = path;
    text.replace(u'/', u'\\');
    const auto source = reinterpret_cast<LPCWSTR>(text.utf16());
    const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source, static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0) return text.toLower();
    std::wstring out(static_cast<size_t>(needed), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, source, static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr, 0);
    return QString::fromWCharArray(out.data(), needed);
}
}
