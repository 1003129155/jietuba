#pragma once
#include "clipboard/core/models.h"
#include <QList>
#include <QString>
#include <optional>

namespace jietuba::clipboard {
class ClipboardController;

// One menu entry; children make a submenu.
struct MenuAction {
    QString label;
    QString key;
    bool enabled = true;
    bool checkable = false;
    bool checked = false;
    bool is_separator = false;
    QList<MenuAction> children;
    bool translate_label = true;
};

struct ContextMenuData {
    qint64 item_id = 0;
    QList<MenuAction> actions;
};

// Text items can always be quick-edited; the title is kept on save.
bool is_quick_editable(const ClipboardItem &item);

// Which entries the item, group and multi-select menus show, from the controller's state.
class ClipboardContextMenuController {
public:
    explicit ClipboardContextMenuController(ClipboardController *controller) : controller_(controller) {}
    // nullopt when item_id does not exist.
    std::optional<ContextMenuData> build_context_menu_data(qint64 item_id);
    QList<MenuAction> build_group_context_menu_data(qint64 group_id);
    // nullopt when none of the ids is in the loaded list.
    std::optional<ContextMenuData> build_marked_context_menu_data(const QList<qint64> &item_ids);

private:
    QList<Group> item_context_menu_groups(const ClipboardItem &clipboard_item);
    ClipboardController *controller_;
};

// Drops leading, trailing and repeated separators and empty submenus.
QList<MenuAction> normalize_menu_actions(QList<MenuAction> actions);
}
