#include "clipboard/controllers/context_menu_controller.h"
#include "clipboard/controllers/clipboard_controller.h"
#include "settings/special_paste.h"
#include "settings/tool_settings.h"
#include <QSet>
#include <functional>

namespace jietuba::clipboard {
namespace {
struct ItemMenuState {
    ClipboardItem clipboard_item;
    std::optional<qint64> current_group_id;
    QList<Group> visible_groups;
    bool can_move_up = false;
    bool can_move_down = false;
    bool is_image_item() const { return clipboard_item.content_type == "image"; }
    bool is_file_item() const { return clipboard_item.content_type == "file"; }
    bool in_group() const { return current_group_id.has_value(); }
};

struct ItemMenuSpec {
    QString key;
    QString label;
    bool is_separator = false;
    std::function<bool(const ItemMenuState &)> predicate = [](const ItemMenuState &) { return true; };
    std::function<QString(const ItemMenuState &)> label_builder;
    std::function<bool(const ItemMenuState &)> enabled_builder;
    std::function<QList<MenuAction>(const ItemMenuState &)> children_builder;
};

struct GroupMenuState {
    bool can_move_up;
    bool can_move_down;
};

struct GroupMenuSpec {
    QString key;
    QString label;
    bool is_separator = false;
    std::function<bool(const GroupMenuState &)> enabled_builder;
};

MenuAction separator(const QString &key) {
    MenuAction action;
    action.key = key;
    action.is_separator = true;
    return action;
}

MenuAction entry(const QString &label, const QString &key, bool enabled = true) {
    MenuAction action;
    action.label = label;
    action.key = key;
    action.enabled = enabled;
    return action;
}

QList<MenuAction> move_group_menu_children(const ItemMenuState &state) {
    QList<MenuAction> children;
    for (const auto &group : state.visible_groups) {
        MenuAction action = entry(group.icon && !group.icon->isEmpty() ? *group.icon + " " + group.name : group.name, QString("move_to_group_%1").arg(group.id));
        action.translate_label = false;
        children.append(action);
    }
    if (state.in_group()) {
        children.append(separator("sep_remove_from_group"));
        children.append(entry("Remove from Group", "remove_from_group"));
    }
    return normalize_menu_actions(children);
}

// The Special Paste submenu: enabled options for this content type, a separator between option groups.
QList<MenuAction> special_paste_menu_children(const ItemMenuState &state) {
    QList<MenuAction> children;
    QString previous_group;
    bool has_previous = false;
    for (const auto &[key, enabled] : special_paste::load(get_tool_settings_manager())) {
        const auto &option = special_paste::OPTIONS_BY_KEY().at(key);
        if (!enabled || option.content_type != state.clipboard_item.content_type) continue;
        if (!children.isEmpty() && (!has_previous || option.group != previous_group)) children.append(separator("sep_sp_" + key));
        children.append(entry(option.label, key));
        previous_group = option.group;
        has_previous = true;
    }
    return children;
}

bool is_text_item(const ItemMenuState &state) { return state.clipboard_item.content_type == "text"; }

const QList<ItemMenuSpec> &item_menu_specs() {
    static const QList<ItemMenuSpec> specs = [] {
        QList<ItemMenuSpec> list;
        list.append(ItemMenuSpec{"paste", "Paste"});
        ItemMenuSpec special{"special_paste_menu", "Special Paste"};
        special.predicate = [](const ItemMenuState &state) { return is_text_item(state) || state.is_file_item(); };
        special.children_builder = special_paste_menu_children;
        list.append(special);
        list.append(ItemMenuSpec{"sep_after_paste", {}, true});
        ItemMenuSpec pin{"pin_image", "Pin as Overlay"};
        pin.predicate = [](const ItemMenuState &state) { return state.is_image_item(); };
        list.append(pin);
        ItemMenuSpec save{"save_image_as", "Save as"};
        save.predicate = [](const ItemMenuState &state) { return state.is_image_item(); };
        list.append(save);
        ItemMenuSpec toggle{"toggle_pin"};
        toggle.predicate = [](const ItemMenuState &state) { return !state.is_image_item(); };
        toggle.label_builder = [](const ItemMenuState &state) { return QString(state.clipboard_item.is_pinned ? "Unpin" : "Pin"); };
        list.append(toggle);
        ItemMenuSpec location{"open_file_location", "Open File Location"};
        location.predicate = [](const ItemMenuState &state) { return state.is_file_item(); };
        list.append(location);
        list.append(ItemMenuSpec{"sep_before_groups", {}, true});
        ItemMenuSpec move{"move_group_menu", "Move to Group"};
        move.predicate = [](const ItemMenuState &state) { return !state.visible_groups.isEmpty(); };
        move.children_builder = move_group_menu_children;
        list.append(move);
        list.append(ItemMenuSpec{"sep_before_order", {}, true});
        ItemMenuSpec quick{"quick_edit_item", "Quick Edit"};
        quick.predicate = [](const ItemMenuState &state) { return is_quick_editable(state.clipboard_item); };
        list.append(quick);
        ItemMenuSpec edit{"edit_item", "Edit"};
        edit.predicate = [](const ItemMenuState &state) { return state.in_group(); };
        list.append(edit);
        ItemMenuSpec up{"move_item_up", "Move Up"};
        up.predicate = [](const ItemMenuState &state) { return state.in_group(); };
        up.enabled_builder = [](const ItemMenuState &state) { return state.can_move_up; };
        list.append(up);
        ItemMenuSpec down{"move_item_down", "Move Down"};
        down.predicate = [](const ItemMenuState &state) { return state.in_group(); };
        down.enabled_builder = [](const ItemMenuState &state) { return state.can_move_down; };
        list.append(down);
        list.append(ItemMenuSpec{"sep_before_delete", {}, true});
        list.append(ItemMenuSpec{"delete_item", "Delete"});
        return list;
    }();
    return specs;
}

const QList<GroupMenuSpec> &group_menu_specs() {
    static const QList<GroupMenuSpec> specs{
        {"edit_group", "Edit"},
        {"move_group_up", "Move Up", false, [](const GroupMenuState &state) { return state.can_move_up; }},
        {"move_group_down", "Move Down", false, [](const GroupMenuState &state) { return state.can_move_down; }},
        {"sep_before_delete", {}, true},
        {"delete_group", "Delete Group"},
    };
    return specs;
}
}

bool is_quick_editable(const ClipboardItem &item) { return item.content_type == "text"; }

QList<MenuAction> normalize_menu_actions(QList<MenuAction> actions) {
    QList<MenuAction> normalized;
    for (auto &action : actions) {
        if (!action.children.isEmpty()) {
            action.children = normalize_menu_actions(action.children);
            if (action.children.isEmpty() && !action.is_separator) continue;
        }
        if (action.is_separator && (normalized.isEmpty() || normalized.back().is_separator)) continue;
        normalized.append(action);
    }
    if (!normalized.isEmpty() && normalized.back().is_separator) normalized.removeLast();
    return normalized;
}

QList<Group> ClipboardContextMenuController::item_context_menu_groups(const ClipboardItem &clipboard_item) {
    const bool is_file_item = clipboard_item.content_type == "file";
    QList<Group> groups;
    for (const auto &group : controller_->get_groups())
        if (group.group_type != GroupType::HIDDEN && (group.group_type == GroupType::NORMAL || is_file_item)) groups.append(group);
    return groups;
}

std::optional<ContextMenuData> ClipboardContextMenuController::build_context_menu_data(qint64 item_id) {
    const auto clipboard_item = controller_->get_item(item_id);
    if (!clipboard_item) return std::nullopt;

    ItemMenuState state;
    state.clipboard_item = *clipboard_item;
    state.current_group_id = controller_->current_group_id;
    if (state.current_group_id) std::tie(state.can_move_up, state.can_move_down) = controller_->get_item_move_state(clipboard_item->id, state.current_group_id);
    state.visible_groups = item_context_menu_groups(*clipboard_item);

    QList<MenuAction> actions;
    for (const auto &spec : item_menu_specs()) {
        if (!spec.predicate(state)) continue;
        if (spec.is_separator) {
            actions.append(separator(spec.key));
            continue;
        }
        QList<MenuAction> children;
        if (spec.children_builder) {
            children = spec.children_builder(state);
            if (children.isEmpty()) continue;
        }
        MenuAction action = entry(spec.label_builder ? spec.label_builder(state) : spec.label, spec.key, spec.enabled_builder ? spec.enabled_builder(state) : true);
        action.children = children;
        actions.append(action);
    }
    return ContextMenuData{item_id, normalize_menu_actions(actions)};
}

QList<MenuAction> ClipboardContextMenuController::build_group_context_menu_data(qint64 group_id) {
    const auto [can_up, can_down] = controller_->get_group_move_state(group_id);
    const GroupMenuState state{can_up, can_down};
    QList<MenuAction> actions;
    for (const auto &spec : group_menu_specs()) {
        if (spec.is_separator) {
            actions.append(separator(spec.key));
            continue;
        }
        actions.append(entry(spec.label, spec.key, spec.enabled_builder ? spec.enabled_builder(state) : true));
    }
    return normalize_menu_actions(actions);
}

std::optional<ContextMenuData> ClipboardContextMenuController::build_marked_context_menu_data(const QList<qint64> &item_ids) {
    QList<ClipboardItem> items;
    for (qint64 item_id : item_ids)
        if (const auto item = controller_->loaded_item(item_id)) items.append(*item);
    if (items.isEmpty()) return std::nullopt;
    QSet<QString> kinds;
    for (const auto &item : items) kinds.insert(item.content_type);

    QList<MenuAction> actions;
    if (kinds == QSet<QString>{"image"}) {
        actions.append(entry("Stitch Vertically and Paste", "paste_marked_vertical"));
        actions.append(entry("Stitch Horizontally and Paste", "paste_marked_horizontal"));
    } else {
        actions.append(entry("Paste Selected", "paste_marked"));
        if (kinds.contains("text") || kinds.contains("file")) actions.append(entry("Paste Selected as Plain Text", "paste_marked_plain"));
    }

    ItemMenuState state;
    state.clipboard_item = items.front();
    state.current_group_id = controller_->current_group_id;
    for (const auto &group : controller_->get_groups())
        if (group.group_type != GroupType::HIDDEN && (group.group_type == GroupType::NORMAL || kinds == QSet<QString>{"file"})) state.visible_groups.append(group);
    const auto move_children = move_group_menu_children(state);
    actions.append(separator("sep_before_groups"));
    if (!move_children.isEmpty()) {
        MenuAction move = entry("Move to Group", "move_group_menu");
        move.children = move_children;
        actions.append(move);
    }
    actions.append(separator("sep_before_delete"));
    actions.append(entry("Clear Selection", "clear_marks"));
    actions.append(entry("Delete Selected", "delete_marked"));
    return ContextMenuData{items.front().id, normalize_menu_actions(actions)};
}
}
