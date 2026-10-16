#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <limits>
#include <optional>

// Python str semantics where Qt differs: lengths and slices count code points, not
// UTF-16 units, and whitespace is exactly what str.isspace() accepts.
namespace jietuba {
// The code points of text (QString::toUcs4 gives uint).
QList<char32_t> code_points_of(const QString &text);
bool py_isspace(char32_t code_point);
// str.strip() / lstrip() / rstrip() without arguments.
QString py_strip(const QString &text);
QString py_lstrip(const QString &text);
QString py_rstrip(const QString &text);
// str.strip(chars).
QString py_strip_chars(const QString &text, const QString &chars);
// len(text).
qsizetype py_len(const QString &text);
// int(text) in base 10: surrounding whitespace, a sign, any Unicode decimal digits and
// single underscores between digits; nullopt where Python raises ValueError. Values beyond
// qint64 saturate.
std::optional<qint64> py_int(const QString &text);
// text[start:end] with Python's clamping; negative indices count from the end.
QString py_slice(const QString &text, qsizetype start, qsizetype end = std::numeric_limits<qsizetype>::max());
// str.upper(), lower(), title(), swapcase() and casefold() over whole code points.
QString py_upper(const QString &text);
QString py_lower(const QString &text);
QString py_title(const QString &text);
QString py_swapcase(const QString &text);
QString py_casefold(const QString &text);
// os.path.basename / dirname / join / normpath / normcase on Windows paths.
QString py_basename(const QString &path);
QString py_dirname(const QString &path);
QString py_path_join(const QString &first, const QString &second);
QString py_normpath(const QString &path);
QString py_normcase(const QString &path);
}
