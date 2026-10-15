#pragma once
#include "clipboard/controllers/context_menu_controller.h"
#include <QMap>
#include <QPoint>
#include <functional>

class QMenu;
class QWidget;

namespace jietuba::clipboard {
using ActionHandler = std::function<void()>;
// Handler for keys the fixed table does not know (e.g. "move_to_group_3"); empty when none.
using DynamicHandlerResolver = std::function<ActionHandler(const QString &)>;

// Renders MenuAction lists as a QMenu.
class ClipboardActionMenu {
public:
    ClipboardActionMenu(QWidget *parent, const QString &menu_style, std::function<QString(const QString &)> translate, QMap<QString, ActionHandler> action_handlers,
                        QList<DynamicHandlerResolver> dynamic_handler_resolvers = {});
    void show_actions(QWidget *anchor_widget, const QPoint &pos, const QList<MenuAction> &actions);

private:
    void populate_menu(QMenu *menu, const QList<MenuAction> &actions);
    ActionHandler resolve_handler(const QString &action_key) const;
    QString translate_label(const MenuAction &action_data) const;

    QWidget *parent_;
    QString menu_style_;
    std::function<QString(const QString &)> translate_;
    QMap<QString, ActionHandler> action_handlers_;
    QList<DynamicHandlerResolver> dynamic_handler_resolvers_;
};

class ClipboardItemContextMenu : public ClipboardActionMenu {
public:
    using ClipboardActionMenu::ClipboardActionMenu;
    void show(QWidget *anchor_widget, const QPoint &pos, const ContextMenuData &context_data) { show_actions(anchor_widget, pos, context_data.actions); }
};

class ClipboardGroupContextMenu : public ClipboardActionMenu {
public:
    using ClipboardActionMenu::ClipboardActionMenu;
    void show(QWidget *anchor_widget, const QPoint &pos, const QList<MenuAction> &actions) { show_actions(anchor_widget, pos, actions); }
};
}
