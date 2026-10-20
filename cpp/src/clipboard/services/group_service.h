#pragma once
#include "clipboard/core/models.h"
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

// Group icons, name checks and the delete confirmation, shared by the manage window and forms.
namespace jietuba::clipboard {
const QString &GENERAL_GROUP_ICON();
const QString &QUICK_LAUNCH_GROUP_ICON();
const QString &HIDDEN_GROUP_ICON();

// group_type is a GroupType value; a filter group's icons follow the kind its rule selects.
QString get_default_group_icon(int group_type, const std::optional<FilterRule> &filter_rule = std::nullopt);
// The icons suggested at the top of the icon picker for a group type; the first is the type's default.
QStringList get_recommended_group_icons(int group_type, const std::optional<FilterRule> &filter_rule = std::nullopt);
// The custom icon when set, otherwise the type's default.
QString get_group_display_icon(const std::optional<QString> &custom_icon, int group_type, const std::optional<FilterRule> &filter_rule = std::nullopt);
// An empty icon or another type's default icon becomes the new type's default; a custom icon stays.
QString get_toggled_default_group_icon(const QString &current_icon, int group_type, const std::optional<FilterRule> &filter_rule = std::nullopt);
bool group_name_exists(const QList<Group> &groups, const QString &name, std::optional<qint64> exclude_group_id = std::nullopt);
QString make_unique_group_name(const QString &base_name, const QSet<QString> &used_names);
QString build_delete_group_confirm_message(const std::function<QString(const char *)> &translate);
}
