#include "clipboard/core/models.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/native.h"
#include "core/py_text.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

namespace jietuba::clipboard {
namespace {
std::optional<QString> optional_text(const QJsonObject &object, const char *key) {
    const auto value = object.value(QLatin1String(key));
    if (!value.isString()) return std::nullopt;
    return value.toString();
}

QDateTime timestamp(const QJsonObject &object, const char *key) {
    const qint64 seconds = object.value(QLatin1String(key)).toInteger();
    return seconds ? QDateTime::fromSecsSinceEpoch(seconds) : QDateTime();
}

QString tr_file() { return QCoreApplication::translate("ClipboardWindow", "File"); }
}

ClipboardItem ClipboardItem::from_json(const QJsonObject &object) {
    ClipboardItem item;
    item.id = object.value("id").toInteger();
    item.content = object.value("content").toString();
    item.content_type = object.value("content_type").toString();
    item.title = optional_text(object, "title");
    item.html_content = optional_text(object, "html_content");
    item.image_id = optional_text(object, "image_id");
    item.thumbnail = optional_text(object, "thumbnail");
    item.is_pinned = object.value("is_pinned").toBool();
    item.paste_count = object.value("paste_count").toInteger();
    item.source_app = optional_text(object, "source_app");
    item.created_at = timestamp(object, "created_at");
    item.updated_at = timestamp(object, "updated_at");
    return item;
}

QString ClipboardItem::display_text() const {
    if (title && !title->isEmpty()) return *title;

    if (content_type == "text") {
        QString text = content;
        text.replace(u'\n', u' ');
        text = py_strip(text);
        return py_len(text) > 100 ? py_slice(text, 0, 100) + "..." : text;
    }
    if (content_type == "image") return py_strip_chars(content, "[]");
    if (content_type == "file") {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(content.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            log_exception(error.error != QJsonParseError::NoError ? "JSONDecodeError" : "AttributeError",
                          error.error != QJsonParseError::NoError ? error.errorString() : "object has no attribute 'get'", T("解析文件类型显示文本"));
            return tr_file();
        }
        const auto files = document.object().value("files").toArray();
        QStringList names;
        for (const auto &file : files) {
            if (!file.isString()) {
                log_exception("TypeError", "expected str, bytes or os.PathLike object", T("解析文件类型显示文本"));
                return tr_file();
            }
            names.append(py_basename(file.toString()));
        }
        if (names.size() == 1) return names.front();
        if (names.size() > 1) return names.join(", ");
        return tr_file();
    }
    return py_slice(content, 0, 50);
}

QString ClipboardItem::icon() const {
    if (content_type == "file") return QString::fromUtf8("📁");
    if (content_type == "image") return QString::fromUtf8("📷");
    return {};
}

std::optional<QString> filter_content_type(int group_type) {
    char buffer[16] = {};
    if (!jb_clipboard_filter_content_type(group_type, buffer, sizeof buffer)) return std::nullopt;
    return QString::fromUtf8(buffer);
}

std::optional<GroupType> filter_group_type(const QString &content_type) {
    for (const auto group_type : {GroupType::FILTER_TEXT, GroupType::FILTER_IMAGE, GroupType::FILTER_FILE})
        if (filter_content_type(static_cast<int>(group_type)) == content_type) return group_type;
    return std::nullopt;
}

bool group_accepts(int group_type, const QString &content_type) { return jb_clipboard_group_accepts(group_type, content_type.toUtf8().constData()); }

Group Group::from_json(const QJsonObject &object) {
    Group group;
    group.id = object.value("id").toInteger();
    group.name = object.value("name").toString();
    group.color = optional_text(object, "color");
    group.icon = optional_text(object, "icon");
    group.group_type = static_cast<int>(object.value("group_type").toInteger());
    return group;
}
}
