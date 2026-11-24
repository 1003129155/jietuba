#pragma once
#include "ui/fluent_lite/theme.h"
#include <QButtonGroup>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <vector>

namespace jietuba {
enum class NavigationItemPosition { TOP = 0, BOTTOM = 1 };

class NavSubList;

// Route-based sidebar navigation. An item of the top list may unfold a second level of entries
// (sub-items) below it; any number of items can be unfolded at once.
class NavigationInterface : public QWidget {
    Q_OBJECT
public:
    explicit NavigationInterface(QWidget *parent = nullptr);
    // A group separator at the current end of the top list.
    void addSeparator();
    QPushButton *addItem(const QString &route_key, const IconSource &icon, const QString &text, std::function<void()> on_click,
                         NavigationItemPosition position = NavigationItemPosition::TOP, const QString &tooltip = {});
    void setCurrentItem(const QString &route_key);
    void clearCurrentItem();
    void setExpandWidth(int width) { expand_width_ = width; }
    void setMinimumExpandWidth(int width) { minimum_expand_width_ = width; }
    void expand() { if (expand_width_) setMinimumWidth(expand_width_); }
    QPushButton *widget(const QString &route_key) const { return items_.value(route_key); }
    // The scroll area holding the top list.
    QScrollArea *scrollArea() const { return scroll_; }
    Q_INVOKABLE void _apply_theme();

    // Whether the item shows the unfold arrow. Only items of the top list can.
    void setExpandable(const QString &route_key, bool expandable);
    bool isExpandable(const QString &route_key) const;
    // Unfolds or folds the item's sub-items, with a height animation once the window is shown. The
    // state is kept while the item has no sub-items yet, and applies when they are set.
    void setExpanded(const QString &route_key, bool expanded, bool animate = true);
    bool isExpanded(const QString &route_key) const { return expanded_.value(route_key, false); }
    // The entries under an item, replacing its earlier ones; none removes them.
    void setSubItems(const QString &route_key, const QStringList &titles);
    int subItemCount(const QString &route_key) const;
    QPushButton *subItem(const QString &route_key, int index) const;
    QStringList subItemTitles(const QString &route_key) const;
    // The sub-item shown as current; only one in the whole list, none for an index of -1. The item
    // above it then gives up its own highlight.
    void setCurrentSubItem(const QString &route_key, int index);
    const QString &currentSubRoute() const { return current_sub_route_; }
    int currentSubItem() const { return current_sub_; }
    // Whether an unfold or fold animation is running.
    bool animating() const;
signals:
    // The arrow of an item was pressed; the owner decides what to unfold.
    void arrowClicked(const QString &route_key);
    void subItemClicked(const QString &route_key, int index);
private:
    void apply_icon(const QString &route_key);
    // Brings the widget into view once no list is moving, as their heights change meanwhile.
    void request_visible(QWidget *widget);
    void flush_visible();
    // Lays out the rows and updates the scroll range together after the lists' heights changed.
    void settle_layout();
    NavSubList *list_of(const QString &route_key) const { return lists_.value(route_key); }
    QMap<QString, QPushButton *> items_;
    QMap<QString, IconSource> icons_;
    QMap<QString, bool> expanded_;
    QMap<QString, NavSubList *> lists_;
    QString current_;
    QString current_sub_route_;
    int current_sub_ = -1;
    QPointer<QWidget> pending_visible_;
    QButtonGroup *group_;
    QScrollArea *scroll_;
    QVBoxLayout *layout_, *top_, *bottom_;
    int expand_width_ = 0, minimum_expand_width_ = 0;
    bool settle_pending_ = false;
};
}
