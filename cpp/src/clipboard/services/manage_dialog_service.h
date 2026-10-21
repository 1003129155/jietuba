#pragma once
#include "clipboard/core/models.h"
#include <QString>
#include <optional>

// The manage window's save actions for groups, text items and file items.
namespace jietuba::clipboard {
class ClipboardManager;

struct SaveResult {
    bool success = false;
    QString action;  // "created" or "updated"
    QString error;   // empty on success
    std::optional<qint64> entity_id;
};

SaveResult save_group(ClipboardManager &manager, std::optional<qint64> editing_group_id, const QString &name, const std::optional<QString> &icon, int group_type,
                      const std::optional<FilterRule> &filter_rule = std::nullopt);
SaveResult save_text_content(ClipboardManager &manager, std::optional<qint64> editing_item_id, qint64 selected_group_id, const QString &content,
                             const std::optional<QString> &title);
SaveResult save_file_content(ClipboardManager &manager, std::optional<qint64> editing_item_id, qint64 selected_group_id, const QString &path,
                             const std::optional<QString> &title);
}
