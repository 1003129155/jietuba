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
class NavIndicator;

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
    // Unfolds or folds the item's sub-items, as a FoldBox does once the window is shown. The
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
    // The item's sub-items are about to be read and one of them made current: until it is cleared with
    // an empty route, neither the item nor the bar marks a row, so the mark goes straight to the group
    // instead of stopping at the item first.
    void setSubItemPending(const QString &route_key);
    int currentSubItem() const { return current_sub_; }
    // Whether an unfold or fold animation is running.
    bool animating() const;
    // Where the current row's bar is drawn now, in the scrolled list's coordinates; empty when the
    // current row is not in that list or not on show.
    QRectF indicatorRect() const;
signals:
    // The arrow of an item was pressed, or its row where that only folds or unfolds it; the owner
    // decides what to unfold.
    void arrowClicked(const QString &route_key);
    void subItemClicked(const QString &route_key, int index);
private:
    void apply_icon(const QString &route_key);
    // Brings the widget into view once no list is moving, as their heights change meanwhile.
    void request_visible(QWidget *widget);
    void flush_visible();
    void mark_sub_active();
    NavSubList *list_of(const QString &route_key) const { return lists_.value(route_key); }
    // Opens or closes the list, sized first to the width it is shown at.
    void fold(NavSubList *list, bool open, bool animate);
    QMap<QString, QPushButton *> items_;
    QMap<QString, IconSource> icons_;
    QMap<QString, bool> expanded_;
    QMap<QString, NavSubList *> lists_;
    QString current_;
    QString current_sub_route_;
    int current_sub_ = -1;
    QString pending_sub_route_;
    QPointer<QWidget> pending_visible_;
    QButtonGroup *group_;
    QScrollArea *scroll_;
    NavIndicator *indicator_ = nullptr;
    QVBoxLayout *layout_, *top_, *bottom_;
    int expand_width_ = 0, minimum_expand_width_ = 0;
};
}
