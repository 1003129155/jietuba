#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <optional>

namespace jietuba::clipboard {
// Group.group_type values.
enum class GroupType : int { NORMAL = 0, FILE = 1, HIDDEN = 2 };
inline bool operator==(int value, GroupType type) { return value == static_cast<int>(type); }
inline bool operator!=(int value, GroupType type) { return value != static_cast<int>(type); }

// One history record. Optional fields are None in the database when unset; the
// timestamps are invalid when the record carries 0.
struct ClipboardItem {
    qint64 id = 0;
    QString content;
    QString content_type;
    std::optional<QString> title;
    std::optional<QString> html_content;
    std::optional<QString> image_id;
    std::optional<QString> thumbnail;
    bool is_pinned = false;
    qint64 paste_count = 0;
    std::optional<QString> source_app;
    QDateTime created_at;
    QDateTime updated_at;

    // From the native layer's JSON (attribute names of the binding's ClipboardItem).
    static ClipboardItem from_json(const QJsonObject &object);
    QString display_text() const;
    QString icon() const;
};

struct Group {
    qint64 id = 0;
    QString name;
    std::optional<QString> color;
    std::optional<QString> icon;
    int group_type = static_cast<int>(GroupType::NORMAL);

    static Group from_json(const QJsonObject &object);
};
}

Q_DECLARE_METATYPE(jietuba::clipboard::ClipboardItem)
