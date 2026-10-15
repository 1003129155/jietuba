#pragma once
#include <QList>
#include <QMetaObject>
#include <QObject>

class QLayout;
class QWidget;

namespace jietuba {
// Disconnects without warning when the connection is already gone.
inline void safe_disconnect(QMetaObject::Connection &connection) {
    if (connection) QObject::disconnect(connection);
    connection = {};
}
// Widgets in a layout, in layout order.
QList<QWidget *> layout_widgets(QLayout *layout);
// The visible modal window blocking input to widget's window, or nullptr. With
// widget == nullptr this answers for a new parentless top-level window.
QWidget *blocking_modal(QWidget *widget = nullptr);
// Closes this process's open popups (menus, combo lists), submenus included.
void close_active_popups();
}
