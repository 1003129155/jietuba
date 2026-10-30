#pragma once
#include <QColor>
#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <optional>

namespace jietuba::clipboard {
// Group.group_type values. A filter group stores no items; it shows the history its filter_rule selects.
enum class GroupType : int { NORMAL = 0, FILE = 1, HIDDEN = 2, FILTER = 3 };
inline bool operator==(int value, GroupType type) { return value == static_cast<int>(type); }
inline bool operator!=(int value, GroupType type) { return value != static_cast<int>(type); }

// ClipboardItem.sub_kind values: what a whole text item is. Empty for other text and non-text items.
inline const QString SUB_KIND_URL = QStringLiteral("url");
inline const QString SUB_KIND_COLOR = QStringLiteral("color");
inline const QString SUB_KIND_PATH = QStringLiteral("path");

// A source app in a FilterRule standing for every build of this application.
inline const QString OWN_SOURCE_APP = QStringLiteral("jietuba");

// What a filter group, or the history's type filter, selects; both parts must match when set.
struct FilterRule {
    // "text", "image", "file", a text sub-kind, or "file:" and a file sub-kind; "text" and "file" include every sub-kind.
    std::optional<QString> kind;
    // Source exe name, matched without case.
    std::optional<QString> app;

    static FilterRule of_kind(const QString &kind) { return {kind, std::nullopt}; }
    // None for anything but a JSON object.
    static std::optional<FilterRule> from_json(const QJsonValue &value);
    QByteArray to_json() const;
    bool operator==(const FilterRule &other) const { return kind == other.kind && app == other.app; }
    bool operator!=(const FilterRule &other) const { return !(*this == other); }
};

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
    QString sub_kind;
    // The color a color item's text names; none when it is not a color or names one the parser cannot read.
    std::optional<QColor> color;
    QDateTime created_at;
    QDateTime updated_at;

    // From the native layer's JSON (attribute names of the binding's ClipboardItem).
    static ClipboardItem from_json(const QJsonObject &object);
    QString display_text() const;
    QString icon() const;
};

// The paths a file item holds or a path item's text names; empty for other items.
QStringList item_paths(const ClipboardItem &item);
// Whether a group of this type stores items of `content_type` and text `sub_kind`; the rule lives in the Rust core.
bool group_accepts(int group_type, const QString &content_type, const QString &sub_kind = {});

struct Group {
    qint64 id = 0;
    QString name;
    std::optional<QString> color;
    std::optional<QString> icon;
    int group_type = static_cast<int>(GroupType::NORMAL);
    // Set on filter groups; none when the stored rule cannot be read.
    std::optional<FilterRule> filter_rule;

    static Group from_json(const QJsonObject &object);
    bool is_filter() const { return group_type == GroupType::FILTER; }
    bool accepts(const ClipboardItem &item) const { return group_accepts(group_type, item.content_type, item.sub_kind); }
};
}

Q_DECLARE_METATYPE(jietuba::clipboard::ClipboardItem)
