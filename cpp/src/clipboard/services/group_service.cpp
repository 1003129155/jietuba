#include "clipboard/services/group_service.h"
#include "clipboard/core/kind_catalog.h"
#include "clipboard/core/manager.h"
#include "settings/tool_settings.h"

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
const QString &APP_FILTER_ICON() {
    static const QString icon = QString::fromUtf8("🧩");
    return icon;
}

// Default icon of a filter group by the kind its rule selects; a rule on the source app alone has its own.
QString filter_group_icon(const std::optional<FilterRule> &filter_rule) {
    const QString text_icon = find_kind("text")->filter_icon;
    const auto kind = filter_rule ? filter_rule->kind : std::nullopt;
    if (!kind) return filter_rule && filter_rule->app ? APP_FILTER_ICON() : text_icon;
    const KindInfo *info = find_kind(*kind);
    return info ? info->filter_icon : text_icon;
}

// Every icon some group type uses as its default; only kinds the form offers can be a group's.
QSet<QString> default_icons() {
    QSet<QString> icons = {GENERAL_GROUP_ICON(), QUICK_LAUNCH_GROUP_ICON(), HIDDEN_GROUP_ICON(), APP_FILTER_ICON()};
    for (const KindInfo *kind : offered_kinds()) icons.insert(kind->filter_icon);
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

void seed_starter_groups(ClipboardManager &manager, ToolSettingsManager *config, const std::function<QString(const char *)> &translate) {
    if (config->get_clipboard_starter_groups_seeded()) return;
    // Someone who already made groups knows the feature; an unreadable database is retried next start.
    if (manager.get_groups().isEmpty()) {
        if (!manager.create_group(translate("Favorites"), std::nullopt, QString::fromUtf8("⭐"))) return;
        manager.create_group(translate("Image"), std::nullopt, std::nullopt, static_cast<int>(GroupType::FILTER), FilterRule::of_kind("image"));
    }
    config->set_clipboard_starter_groups_seeded(true);
}
}
