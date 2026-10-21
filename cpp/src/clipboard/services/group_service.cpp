#include "clipboard/services/group_service.h"
#include <QHash>

namespace jietuba::clipboard {
const QString &GENERAL_GROUP_ICON() {
    static const QString icon = QString::fromUtf8("📁");
    return icon;
}

const QString &QUICK_LAUNCH_GROUP_ICON() {
    static const QString icon = QString::fromUtf8("⚡");
    return icon;
}

const QString &HIDDEN_GROUP_ICON() {
    static const QString icon = QString::fromUtf8("🙈");
    return icon;
}

namespace {
// Default icon of a filter group by the kind its rule selects; a rule on the source app alone has its own.
const QHash<QString, QString> &filter_kind_icons() {
    static const QHash<QString, QString> icons = {
        {"text", QString::fromUtf8("📝")}, {"image", QString::fromUtf8("🖼️")}, {"file", QString::fromUtf8("📄")},
        {SUB_KIND_URL, QString::fromUtf8("🔗")}, {SUB_KIND_COLOR, QString::fromUtf8("🎨")}, {SUB_KIND_PATH, QString::fromUtf8("📂")},
    };
    return icons;
}

const QString &APP_FILTER_ICON() {
    static const QString icon = QString::fromUtf8("🧩");
    return icon;
}

QString filter_group_icon(const std::optional<FilterRule> &filter_rule) {
    const auto kind = filter_rule ? filter_rule->kind : std::nullopt;
    if (!kind) return filter_rule && filter_rule->app ? APP_FILTER_ICON() : filter_kind_icons().value("text");
    return filter_kind_icons().value(*kind, filter_kind_icons().value("text"));
}

// Every icon some group type uses as its default.
QSet<QString> default_icons() {
    QSet<QString> icons = {GENERAL_GROUP_ICON(), QUICK_LAUNCH_GROUP_ICON(), HIDDEN_GROUP_ICON(), APP_FILTER_ICON()};
    for (const QString &icon : filter_kind_icons()) icons.insert(icon);
    return icons;
}
}

QString get_default_group_icon(int group_type, const std::optional<FilterRule> &filter_rule) {
    if (group_type == GroupType::FILE) return QUICK_LAUNCH_GROUP_ICON();
    if (group_type == GroupType::HIDDEN) return HIDDEN_GROUP_ICON();
    if (group_type == GroupType::FILTER) return filter_group_icon(filter_rule);
    return GENERAL_GROUP_ICON();
}

QString get_group_display_icon(const std::optional<QString> &custom_icon, int group_type, const std::optional<FilterRule> &filter_rule) {
    if (custom_icon && !custom_icon->isEmpty()) return *custom_icon;
    return get_default_group_icon(group_type, filter_rule);
}

QString get_toggled_default_group_icon(const QString &current_icon, int group_type, const std::optional<FilterRule> &filter_rule) {
    if (current_icon.trimmed().isEmpty() || default_icons().contains(current_icon)) return get_default_group_icon(group_type, filter_rule);
    return current_icon;
}

bool group_name_exists(const QList<Group> &groups, const QString &name, std::optional<qint64> exclude_group_id) {
    for (const auto &group : groups) {
        if (exclude_group_id && group.id == *exclude_group_id) continue;
        if (group.name == name) return true;
    }
    return false;
}

QString make_unique_group_name(const QString &base_name, const QSet<QString> &used_names) {
    if (!used_names.contains(base_name)) return base_name;
    for (int index = 1;; ++index) {
        const QString candidate = base_name + QString(" (%1)").arg(index);
        if (!used_names.contains(candidate)) return candidate;
    }
}

QString build_delete_group_confirm_message(const std::function<QString(const char *)> &translate) {
    return translate("Are you sure you want to delete this group?") + "\n" + translate("Items in the group will be moved back to clipboard history.");
}
}
