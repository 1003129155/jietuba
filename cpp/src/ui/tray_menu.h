#pragma once
#include <QMenu>

// The system tray context menu.
namespace jietuba {
class MainApp;

// Right releases do not trigger items.
class CustomTrayMenu : public QMenu {
    Q_OBJECT
public:
    using QMenu::QMenu;
protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
};

QMenu *create_tray_menu(MainApp *app);
}
