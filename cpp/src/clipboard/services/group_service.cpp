#include "clipboard/services/group_service.h"
#include "core/py_text.h"
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
// Default icon of each GroupType value; 3, 4 and 5 are the text, image and file filter groups.
const QString &filter_group_icon(GroupType type) {
    static const QString text = QString::fromUtf8("📝");
    static const QString image = QString::fromUtf8("🖼️");
    static const QString file = QString::fromUtf8("📄");
    return type == GroupType::FILTER_TEXT ? text : type == GroupType::FILTER_IMAGE ? image : file;
}

constexpr GroupType ALL_TYPES[] = {GroupType::NORMAL, GroupType::FILE, GroupType::HIDDEN, GroupType::FILTER_TEXT, GroupType::FILTER_IMAGE, GroupType::FILTER_FILE};
}

QString get_default_group_icon(int group_type) {
    if (group_type == GroupType::FILE) return QUICK_LAUNCH_GROUP_ICON();
    if (group_type == GroupType::HIDDEN) return HIDDEN_GROUP_ICON();
    if (group_type == GroupType::FILTER_TEXT || group_type == GroupType::FILTER_IMAGE || group_type == GroupType::FILTER_FILE)
        return filter_group_icon(static_cast<GroupType>(group_type));
    return GENERAL_GROUP_ICON();
}

QStringList get_recommended_group_icons(int group_type) {
    // Keyed by GroupType value.
    static const QHash<int, QStringList> icons = {
        {0, {QString::fromUtf8("📁"), QString::fromUtf8("📌"), QString::fromUtf8("⭐"), QString::fromUtf8("📝"), QString::fromUtf8("💼"), QString::fromUtf8("🏠"), QString::fromUtf8("🎯"), QString::fromUtf8("🔖")}},
        {1, {QString::fromUtf8("⚡"), QString::fromUtf8("🚀"), QString::fromUtf8("🎮"), QString::fromUtf8("💻"), QString::fromUtf8("🎧"), QString::fromUtf8("🧰"), QString::fromUtf8("🌐"), QString::fromUtf8("📂")}},
        {2, {QString::fromUtf8("🙈"), QString::fromUtf8("🔒"), QString::fromUtf8("🕶️"), QString::fromUtf8("🤫"), QString::fromUtf8("🗝️"), QString::fromUtf8("🛡️"), QString::fromUtf8("👻"), QString::fromUtf8("🌙")}},
        {3, {QString::fromUtf8("📝"), QString::fromUtf8("📄"), QString::fromUtf8("🔗"), QString::fromUtf8("🧾"), QString::fromUtf8("🔖"), QString::fromUtf8("💬"), QString::fromUtf8("✏️"), QString::fromUtf8("📋")}},
        {4, {QString::fromUtf8("🖼️"), QString::fromUtf8("📷"), QString::fromUtf8("🎨"), QString::fromUtf8("📸"), QString::fromUtf8("🌄"), QString::fromUtf8("🖌️"), QString::fromUtf8("🧩"), QString::fromUtf8("🎞️")}},
        {5, {QString::fromUtf8("📄"), QString::fromUtf8("📎"), QString::fromUtf8("🗂️"), QString::fromUtf8("📦"), QString::fromUtf8("📑"), QString::fromUtf8("🗃️"), QString::fromUtf8("💾"), QString::fromUtf8("📂")}},
    };
    return icons.value(group_type, icons.value(0));
}

QString get_group_display_icon(const std::optional<QString> &custom_icon, int group_type) {
    if (custom_icon && !custom_icon->isEmpty()) return *custom_icon;
    return get_default_group_icon(group_type);
}

QString get_toggled_default_group_icon(const QString &current_icon, int group_type) {
    if (py_strip(current_icon).isEmpty()) return get_default_group_icon(group_type);
    for (const GroupType type : ALL_TYPES)
        if (current_icon == get_default_group_icon(static_cast<int>(type))) return get_default_group_icon(group_type);
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
