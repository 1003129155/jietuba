#include "gif/_widgets.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QScreen>

namespace jietuba {
QString toolbar_style() {
    return QString(R"(
        .QWidget, .QLabel { background: transparent; border: none; color: #2a3243; }
        QPushButton { border: none; border-radius: %1px;
                       color: #2a3243; font-size: %2px; padding: 0; }
        QPushButton:hover { background: rgba(15,23,42,0.07); }
        QPushButton:pressed { background: rgba(15,23,42,0.12); }
        QPushButton:checked { background: rgba(74,124,184,0.16); }
        QPushButton:disabled { color: #9aa3b1; }
    )")
        .arg(scaled(7))
        .arg(scaled(12));
}

void popup_menu(QMenu *menu, QWidget *anchor, bool below) {
    // The menu is always light, and the text colour must sit on ::item: the global
    // style sheet colours QMenu::item by UI theme, which a colour on QMenu alone does
    // not override, giving white text in dark mode.
    menu->setStyleSheet(QString(R"(
        QMenu { background: #fcfdff; color: #2a3243; border: 1px solid #dce2eb;
                 border-radius: %1px; padding: %2px; }
        QMenu::item { padding: %3px %4px; font-size: %5px;
                      border-radius: %6px; color: #2a3243; }
        QMenu::item:selected { background: #e6edf6; }
        QMenu::item:disabled { color: #929baa; }
        QMenu::separator { height: 1px; background: #e1e6ee; margin: %6px; }
    )")
                            .arg(scaled(8))
                            .arg(scaled(6))
                            .arg(scaled(7))
                            .arg(scaled(22))
                            .arg(scaled(12))
                            .arg(scaled(4)));
    const auto size = menu->sizeHint();
    const auto top = anchor->mapToGlobal(QPoint(0, 0));
    auto *screen = QApplication::screenAt(top);
    if (!screen) screen = QApplication::primaryScreen();
    const auto bounds = screen->availableGeometry();
    const int gap = scaled(5);
    const int down = top.y() + anchor->height() + gap, up = top.y() - size.height() - gap;
    int y = below ? down : up;
    if (y < bounds.top() || y + size.height() > bounds.bottom() + 1) y = below ? up : down;
    const int x = std::max(bounds.left(), std::min(top.x(), bounds.right() + 1 - size.width()));
    y = std::max(bounds.top(), std::min(y, bounds.bottom() + 1 - size.height()));
    QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->popup(QPoint(x, y));
}

const QString PROGRESS_BAR_STYLE = R"(
    QProgressBar {
        background: rgba(0, 0, 0, 45);
        border-radius: 7px;
        border: none;
    }
    QProgressBar::chunk {
        background: #2196F3;
        border-radius: 7px;
    }
)";

QIcon svg_icon(const QString &name, int size) { return ResourceManager::get_icon(ResourceManager::get_icon_path(name), size); }

ClickMenuButton::ClickMenuButton(std::vector<std::pair<QString, QVariant>> options, int default_index, QWidget *parent)
    : QPushButton(parent), options_(std::move(options)), current_index_(default_index) {
    update_text();
    setCursor(QCursor(Qt::PointingHandCursor));
    apply_style();
    connect(this, &QPushButton::clicked, this, &ClickMenuButton::show_menu);
}

void ClickMenuButton::apply_style() {
    const QString color = enabled_flag_ ? "#333" : "#bbb";
    setStyleSheet(QString("QPushButton { font-size: %1px; color: %2; padding: %3px %4px; border: none; border-radius: %5px; background: transparent; }"
                          "QPushButton:hover { background: rgba(15,23,42,0.07); }")
                      .arg(scaled(12))
                      .arg(color)
                      .arg(scaled(1))
                      .arg(scaled(5))
                      .arg(scaled(7)));
}

void ClickMenuButton::set_enabled(bool enabled) {
    if (!enabled && menu_) menu_->close();
    enabled_flag_ = enabled;
    setEnabled(enabled);
    apply_style();
}

void ClickMenuButton::show_menu() {
    if (!isEnabled()) return;
    if (menu_) {
        menu_->close();
        return;
    }
    auto *menu = new QMenu(this);
    menu_ = menu;
    connect(menu, &QMenu::aboutToHide, this, [this] { menu_ = nullptr; });
    for (int i = 0; i < static_cast<int>(options_.size()); ++i) {
        auto *action = new QAction(options_[i].first, menu);
        action->setCheckable(true);
        action->setChecked(i == current_index_);
        connect(action, &QAction::triggered, this, [this, i] { on_select(i); });
        menu->addAction(action);
    }
    popup_menu(menu, this, popup_below);
}

void ClickMenuButton::hideEvent(QHideEvent *event) {
    if (menu_) menu_->close();
    QPushButton::hideEvent(event);
}

void ClickMenuButton::on_select(int index) {
    current_index_ = index;
    update_text();
    emit option_selected(options_[index].second);
}
}
