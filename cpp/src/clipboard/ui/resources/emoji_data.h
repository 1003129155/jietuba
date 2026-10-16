#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <utility>

// Emoji groups for the group icon picker, from the prebuilt svg/emoji_groups.json.
namespace jietuba::clipboard {
// (group order, emojis by group), loaded once.
std::pair<QStringList, QHash<QString, QStringList>> get_emoji_groups();
// The emoji shown on a group's tab.
QString get_group_icon(const QString &group_name);
}
