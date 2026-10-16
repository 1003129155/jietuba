#include "clipboard/services/file_payload_service.h"
#include "core/py_text.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace jietuba::clipboard {
namespace {
// json.dumps(text, ensure_ascii=False) of one string.
QString json_string(const QString &text) {
    QString out = "\"";
    for (QChar c : text) {
        switch (c.unicode()) {
        case u'"': out += "\\\""; break;
        case u'\\': out += "\\\\"; break;
        case u'\n': out += "\\n"; break;
        case u'\r': out += "\\r"; break;
        case u'\t': out += "\\t"; break;
        case u'\b': out += "\\b"; break;
        case u'\f': out += "\\f"; break;
        default:
            if (c.unicode() < 0x20) out += QString("\\u%1").arg(static_cast<uint>(c.unicode()), 4, 16, QChar(u'0'));
            else out += c;
        }
    }
    return out + "\"";
}
}

QString normalize_file_path(const QString &path) { return py_normpath(py_strip(path)); }

QString build_file_payload(const QString &path) { return "{\"files\": [" + json_string(normalize_file_path(path)) + "]}"; }

QString extract_first_file_path_from_content(const std::optional<QString> &content) {
    const QString raw_content = py_strip(content.value_or(QString()));
    if (raw_content.isEmpty()) return {};

    // json.loads also takes a bare string, number or constant, which QJsonDocument does
    // not; wrapped in an array, any one JSON value parses.
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson("[" + raw_content.toUtf8() + "]", &error);
    const bool python_constant = raw_content == "NaN" || raw_content == "Infinity" || raw_content == "-Infinity";
    if (!python_constant && (error.error != QJsonParseError::NoError || document.array().size() != 1)) return py_normpath(raw_content);
    const QJsonValue data = python_constant ? QJsonValue() : document.array().first();
    if (!data.isObject()) return {};

    const QJsonValue files = data.toObject().value("files");
    // files[0] of a string is its first character.
    if (files.isString()) return files.toString().isEmpty() ? QString() : normalize_file_path(py_slice(files.toString(), 0, 1));
    if (files.toArray().isEmpty()) return {};
    return normalize_file_path(files.toArray().first().toString());
}
}
