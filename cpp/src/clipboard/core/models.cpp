#include "clipboard/core/models.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/native.h"
#include "core/text_utils.h"
#include <QFileInfo>
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
    item.sub_kind = object.value("sub_kind").toString();
    uint8_t rgba[4] = {};
    if (item.sub_kind == SUB_KIND_COLOR && jb_clipboard_parse_color(item.content.toUtf8().constData(), rgba)) item.color = QColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    item.created_at = timestamp(object, "created_at");
    item.updated_at = timestamp(object, "updated_at");
    return item;
}

QString ClipboardItem::display_text() const {
    if (title && !title->isEmpty()) return *title;

    if (content_type == "text") {
        QString text = content;
        text.replace(u'\n', u' ');
        text = text.trimmed();
        const QString head = left_chars(text, 100);
        return head.size() < text.size() ? head + "..." : text;
    }
    if (content_type == "image") {
        qsizetype start = 0, end = content.size();
        while (start < end && (content[start] == u'[' || content[start] == u']')) ++start;
        while (end > start && (content[end - 1] == u'[' || content[end - 1] == u']')) --end;
        return content.mid(start, end - start);
    }
    if (content_type == "file") {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(content.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            log_exception(error.error != QJsonParseError::NoError ? error.errorString() : "content is not a JSON object", T("解析文件类型显示文本"));
            return tr_file();
        }
        const auto files = document.object().value("files").toArray();
        QStringList names;
        for (const auto &file : files) {
            if (!file.isString()) {
                log_exception("file entry is not a string", T("解析文件类型显示文本"));
                return tr_file();
            }
            names.append(QFileInfo(file.toString()).fileName());
        }
        if (names.size() == 1) return names.front();
        if (names.size() > 1) return names.join(", ");
        return tr_file();
    }
    return left_chars(content, 50);
}

QString ClipboardItem::icon() const {
    if (content_type == "file") return QString::fromUtf8("📁");
    if (content_type == "image") return QString::fromUtf8("📷");
    return {};
}

std::optional<FilterRule> FilterRule::from_json(const QJsonValue &value) {
    if (!value.isObject()) return std::nullopt;
    const auto object = value.toObject();
    return FilterRule{optional_text(object, "kind"), optional_text(object, "app")};
}

QByteArray FilterRule::to_json() const {
    QJsonObject object;
    if (kind) object.insert("kind", *kind);
    if (app) object.insert("app", *app);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QStringList item_paths(const ClipboardItem &item) {
    QStringList paths;
    if (item.content_type == "file") {
        for (const auto &value : QJsonDocument::fromJson(item.content.toUtf8()).object().value("files").toArray())
            if (!value.toString().isEmpty()) paths.append(value.toString());
    } else if (item.sub_kind == SUB_KIND_PATH) {
        char *json = jb_clipboard_text_paths(item.content.toUtf8().constData());
        if (!json) return paths;
        for (const auto &value : QJsonDocument::fromJson(json).array()) paths.append(value.toString());
        jb_clipboard_text_free(json);
    }
    return paths;
}

bool group_accepts(int group_type, const QString &content_type, const QString &sub_kind) {
    return jb_clipboard_group_accepts(group_type, content_type.toUtf8().constData(), sub_kind.toUtf8().constData());
}

Group Group::from_json(const QJsonObject &object) {
    Group group;
    group.id = object.value("id").toInteger();
    group.name = object.value("name").toString();
    group.color = optional_text(object, "color");
    group.icon = optional_text(object, "icon");
    group.group_type = static_cast<int>(object.value("group_type").toInteger());
    group.filter_rule = FilterRule::from_json(object.value("filter_rule"));
    return group;
}
}
