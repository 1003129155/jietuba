#include "ui/submenu_position.h"
#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QScreen>
#include <algorithm>

namespace jietuba {
QPoint submenu_position(const QSize &size, int y, const std::vector<QRect> &ancestors, const QRect &screen, const std::optional<QRect> &anchor) {
    const auto &parent = ancestors.front();
    const auto edge = anchor.value_or(parent);
    const bool prefer_left = ancestors.size() > 1 && parent.center().x() < ancestors[1].center().x();
    const int directions[2] = {prefer_left ? -1 : 1, prefer_left ? 1 : -1};
    std::optional<std::pair<long long, QPoint>> best;
    for (const int direction : directions) {
        int x = direction < 0 ? edge.left() - size.width() : edge.right() + 1;
        x = std::max(screen.left(), std::min(x, screen.right() + 1 - size.width()));
        const int top = std::max(screen.top(), std::min(y, screen.bottom() + 1 - size.height()));
        const QRect rect(QPoint(x, top), size);
        long long overlap = 0;
        for (const auto &ancestor : ancestors) {
            const auto intersection = rect.intersected(ancestor);
            overlap += static_cast<long long>(intersection.width()) * intersection.height();
        }
        // The first candidate wins ties, as with min() over the list.
        if (!best || overlap < best->first) best = std::make_pair(overlap, rect.topLeft());
    }
    return best->second;
}

namespace {
std::optional<QRect> action_rect(QMenu *menu, QAction *action) {
    const auto rect = menu->actionGeometry(action);
    if (!rect.isValid()) return std::nullopt;
    return QRect(menu->mapToGlobal(rect.topLeft()), rect.size());
}

class SubmenuPositioner : public QObject {
public:
    SubmenuPositioner(QMenu *submenu, QMenu *parent_menu) : QObject(submenu), parent_menu_(parent_menu) {}
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Show) {
            std::vector<QRect> ancestors;
            QWidget *menu = parent_menu_.data();
            while (auto *as_menu = qobject_cast<QMenu *>(menu)) {
                if (!as_menu->isVisible()) break;
                ancestors.push_back(as_menu->geometry());
                menu = as_menu->parentWidget();
            }
            QScreen *screen = ancestors.empty() ? nullptr : QApplication::screenAt(ancestors.front().center());
            auto *submenu = qobject_cast<QMenu *>(watched);
            if (screen && submenu && parent_menu_)
                submenu->move(submenu_position(submenu->size(), submenu->y(), ancestors, screen->availableGeometry(),
                                               action_rect(parent_menu_.data(), submenu->menuAction())));
        }
        return QObject::eventFilter(watched, event);
    }
private:
    QPointer<QMenu> parent_menu_;
};
}

void avoid_submenu_overlap(QMenu *menu) {
    for (auto *action : menu->actions()) {
        auto *submenu = action->menu();
        if (!submenu) continue;
        submenu->installEventFilter(new SubmenuPositioner(submenu, menu));
        avoid_submenu_overlap(submenu);
    }
}
}
