#include "clipboard/ui/menus/action_menu.h"
#include <QMenu>
#include <QWidget>

namespace jietuba::clipboard {
ClipboardActionMenu::ClipboardActionMenu(QWidget *parent, const QString &menu_style, std::function<QString(const QString &)> translate,
                                         QMap<QString, ActionHandler> action_handlers, QList<DynamicHandlerResolver> dynamic_handler_resolvers)
    : parent_(parent), menu_style_(menu_style), translate_(std::move(translate)), action_handlers_(std::move(action_handlers)),
      dynamic_handler_resolvers_(std::move(dynamic_handler_resolvers)) {}

void ClipboardActionMenu::show_actions(QWidget *anchor_widget, const QPoint &pos, const QList<MenuAction> &actions) {
    if (actions.isEmpty()) return;
    QMenu menu(parent_);
    menu.setStyleSheet(menu_style_);
    populate_menu(&menu, actions);
    menu.exec(anchor_widget->mapToGlobal(pos));
}

void ClipboardActionMenu::populate_menu(QMenu *menu, const QList<MenuAction> &actions) {
    for (const auto &action_data : actions) {
        if (action_data.is_separator) {
            menu->addSeparator();
            continue;
        }
        if (!action_data.children.isEmpty()) {
            QMenu *submenu = menu->addMenu(translate_label(action_data));
            submenu->setStyleSheet(menu_style_);
            populate_menu(submenu, action_data.children);
            submenu->menuAction()->setEnabled(action_data.enabled);
            continue;
        }
        QAction *action = menu->addAction(translate_label(action_data));
        action->setEnabled(action_data.enabled);
        if (action_data.checkable) {
            action->setCheckable(true);
            action->setChecked(action_data.checked);
        }
        if (const auto handler = resolve_handler(action_data.key)) QObject::connect(action, &QAction::triggered, handler);
    }
}

ActionHandler ClipboardActionMenu::resolve_handler(const QString &action_key) const {
    if (const auto found = action_handlers_.constFind(action_key); found != action_handlers_.constEnd() && *found) return *found;
    for (const auto &resolver : dynamic_handler_resolvers_)
        if (auto handler = resolver(action_key)) return handler;
    return {};
}

QString ClipboardActionMenu::translate_label(const MenuAction &action_data) const {
    if (!action_data.translate_label) return action_data.label;
    return translate_(action_data.label);
}
}
