#pragma once
#include "clipboard/core/models.h"
#include <QList>
#include <QSet>
#include <QString>
#include <functional>
#include <optional>

// Group icons, name checks and the delete confirmation, shared by the manage window and forms.
namespace jietuba::clipboard {
const QString &GENERAL_GROUP_ICON();
const QString &QUICK_LAUNCH_GROUP_ICON();
const QString &HIDDEN_GROUP_ICON();

QString get_default_group_icon(bool is_file_group, bool is_hidden = false);
// The custom icon when set, otherwise the type's default.
QString get_group_display_icon(const std::optional<QString> &custom_icon, bool is_file_group, bool is_hidden = false);
// Switches between default icons when the type changes, leaving custom icons alone.
QString get_toggled_default_group_icon(const QString &current_icon, bool is_file_group, bool is_hidden = false);
bool group_name_exists(const QList<Group> &groups, const QString &name, std::optional<qint64> exclude_group_id = std::nullopt);
QString make_unique_group_name(const QString &base_name, const QSet<QString> &used_names);
QString build_delete_group_confirm_message(const std::function<QString(const char *)> &translate);
}
