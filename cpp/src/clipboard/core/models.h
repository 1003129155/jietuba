#pragma once
#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <optional>

namespace jietuba::clipboard {
// Group.group_type values. A filter group stores no items; it shows one type of the history.
enum class GroupType : int { NORMAL = 0, FILE = 1, HIDDEN = 2, FILTER_TEXT = 3, FILTER_IMAGE = 4, FILTER_FILE = 5 };
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

// The content type ("text", "image" or "file") a filter group shows; none for other group types.
std::optional<QString> filter_content_type(int group_type);
// The filter group type that shows `content_type`, the inverse of filter_content_type.
std::optional<GroupType> filter_group_type(const QString &content_type);
// Whether a group of this type stores items of `content_type`; the rule lives in the Rust core.
bool group_accepts(int group_type, const QString &content_type);

struct Group {
    qint64 id = 0;
    QString name;
    std::optional<QString> color;
    std::optional<QString> icon;
    int group_type = static_cast<int>(GroupType::NORMAL);

    static Group from_json(const QJsonObject &object);
    std::optional<QString> filter_content_type() const { return jietuba::clipboard::filter_content_type(group_type); }
    bool is_filter() const { return filter_content_type().has_value(); }
    bool accepts(const QString &content_type) const { return group_accepts(group_type, content_type); }
};
}

Q_DECLARE_METATYPE(jietuba::clipboard::ClipboardItem)
