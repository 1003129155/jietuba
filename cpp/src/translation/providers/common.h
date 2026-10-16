#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <optional>

namespace jietuba {
// The response body as JSON; nullopt when it does not parse.
inline std::optional<QJsonValue> parse_json_body(const QByteArray &body) {
    QJsonParseError error{};
    const auto doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError) return std::nullopt;
    if (doc.isArray()) return QJsonValue(doc.array());
    if (doc.isObject()) return QJsonValue(doc.object());
    return std::nullopt;
}

// An error body as an object; empty when absent or not an object.
inline QJsonObject error_body_object(const QByteArray &body) {
    const auto parsed = body.isEmpty() ? std::nullopt : parse_json_body(body);
    return parsed && parsed->isObject() ? parsed->toObject() : QJsonObject();
}

// Python str(value or ""): falsy values become empty, numbers and strings their text.
inline QString json_text(const QJsonValue &value) {
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? "True" : QString();
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number == 0) return {};
        if (number == static_cast<qint64>(number)) return QString::number(static_cast<qint64>(number));
        return QString::number(number);
    }
    return {};
}

// Python truthiness of a JSON value.
inline bool json_truthy(const QJsonValue &value) {
    switch (value.type()) {
    case QJsonValue::Bool: return value.toBool();
    case QJsonValue::Double: return value.toDouble() != 0;
    case QJsonValue::String: return !value.toString().isEmpty();
    case QJsonValue::Array: return !value.toArray().isEmpty();
    case QJsonValue::Object: return !value.toObject().isEmpty();
    default: return false;
    }
}

// str(a or b or ...): the text of the first truthy value, or fallback.
inline QString first_text(std::initializer_list<QJsonValue> values, const QString &fallback = {}) {
    for (const auto &value : values)
        if (json_truthy(value)) return json_text(value);
    return fallback;
}

inline QByteArray compact_json(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
inline QByteArray compact_json(const QJsonArray &array) { return QJsonDocument(array).toJson(QJsonDocument::Compact); }
}
