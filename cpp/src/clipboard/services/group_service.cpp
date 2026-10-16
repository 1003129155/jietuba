#include "clipboard/services/group_service.h"
#include "core/py_text.h"

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

QString get_default_group_icon(bool is_file_group, bool is_hidden) {
    if (is_hidden) return HIDDEN_GROUP_ICON();
    return is_file_group ? QUICK_LAUNCH_GROUP_ICON() : GENERAL_GROUP_ICON();
}

QString get_group_display_icon(const std::optional<QString> &custom_icon, bool is_file_group, bool is_hidden) {
    if (custom_icon && !custom_icon->isEmpty()) return *custom_icon;
    return get_default_group_icon(is_file_group, is_hidden);
}

QString get_toggled_default_group_icon(const QString &current_icon, bool is_file_group, bool is_hidden) {
    const bool is_default = current_icon == HIDDEN_GROUP_ICON() || current_icon == GENERAL_GROUP_ICON() || current_icon == QUICK_LAUNCH_GROUP_ICON();
    // A custom icon stays; an empty field takes the hidden icon.
    if (is_hidden && !is_default && py_strip(current_icon).isEmpty()) return HIDDEN_GROUP_ICON();
    if (is_hidden && current_icon == QUICK_LAUNCH_GROUP_ICON()) return HIDDEN_GROUP_ICON();
    if (is_hidden && current_icon == GENERAL_GROUP_ICON()) return HIDDEN_GROUP_ICON();
    if (!is_hidden && current_icon == HIDDEN_GROUP_ICON()) return GENERAL_GROUP_ICON();
    if (is_file_group && current_icon == GENERAL_GROUP_ICON()) return QUICK_LAUNCH_GROUP_ICON();
    if (!is_file_group && current_icon == QUICK_LAUNCH_GROUP_ICON()) return GENERAL_GROUP_ICON();
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
