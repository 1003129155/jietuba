#include "clipboard/services/manage_dialog_service.h"
#include "clipboard/core/manager.h"
#include "clipboard/services/file_payload_service.h"

namespace jietuba::clipboard {
namespace {
SaveResult save_content(ClipboardManager &manager, std::optional<qint64> editing_item_id, qint64 selected_group_id, const QString &content,
                        const QString &content_type, const std::optional<QString> &title) {
    if (!editing_item_id) {
        const auto item_id = manager.add_item(content, content_type, title);
        if (!item_id || !*item_id) return {false, "created", "add_content_failed", std::nullopt};
        if (!manager.move_to_group(*item_id, selected_group_id)) return {false, "created", "move_to_group_failed", item_id};
        return {true, "created", {}, item_id};
    }
    if (manager.update_item(*editing_item_id, content, title)) return {true, "updated", {}, editing_item_id};
    return {false, "updated", "update_content_failed", editing_item_id};
}
}

SaveResult save_group(ClipboardManager &manager, std::optional<qint64> editing_group_id, const QString &name, const std::optional<QString> &icon, int group_type) {
    if (!editing_group_id) {
        const auto group_id = manager.create_group(name, std::nullopt, icon, group_type);
        if (group_id && *group_id) return {true, "created", {}, group_id};
        return {false, "created", "create_group_failed", std::nullopt};
    }
    if (manager.update_group(*editing_group_id, name, std::nullopt, icon, group_type)) return {true, "updated", {}, editing_group_id};
    return {false, "updated", "update_group_failed", std::nullopt};
}

SaveResult save_text_content(ClipboardManager &manager, std::optional<qint64> editing_item_id, qint64 selected_group_id, const QString &content,
                             const std::optional<QString> &title) {
    return save_content(manager, editing_item_id, selected_group_id, content, "text", title);
}

SaveResult save_file_content(ClipboardManager &manager, std::optional<qint64> editing_item_id, qint64 selected_group_id, const QString &path,
                             const std::optional<QString> &title) {
    return save_content(manager, editing_item_id, selected_group_id, build_file_payload(path), "file", title);
}
}
