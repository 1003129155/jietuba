#pragma once
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantAnimation>
#include <functional>
#include <vector>

class QScrollArea;
class QTimer;
class QWidget;

namespace jietuba {
class NavigationInterface;

// The second navigation level of the settings window: lists the groups of the pages under their
// items, keeps the current one in step with the scroll position of the page shown and scrolls to
// a group on request.
//
// Which pages are unfolded: the page shown unfolds itself and folds again when left, unless the
// user unfolded it with its arrow, which then stays until the user folds it. A page the user
// folded while on it stays folded until it is opened again.
class PageOutline : public QObject {
    Q_OBJECT
public:
    PageOutline(NavigationInterface *navigation, QObject *parent = nullptr);
    // The window's side: the scroll area of a page by route, built first when needed; and opening a
    // page as a click on its navigation item does.
    std::function<QScrollArea *(const QString &route)> page_of;
    std::function<void(const QString &route)> open_page;

    // Follows the page now shown; a page without a scroll area has no outline.
    void set_page(QScrollArea *scroll, const QString &route_key);
    // Reads the groups again at once; also runs, coalesced, whenever the page's layout changes.
    void refresh();
    // Scrolls to the group with a short animation; the highlight stays on it meanwhile.
    void scroll_to_group(int index);
    // Marks the group holding target as current until the page is scrolled again.
    void select_group_of(QWidget *target);
    // Scrolls to the group holding target, like a click on its navigation entry.
    void scroll_to_group_of(QWidget *target);
    int group_count() const { return static_cast<int>(groups_.size()); }
    int current_group() const { return current_; }
    bool animating() const { return animation_.state() == QAbstractAnimation::Running; }
    // The pages the user unfolded by hand.
    const QSet<QString> &manually_unfolded() const { return manual_; }
    // Where the group's top sits after scrolling to it, at 100% scale.
    static constexpr int TOP_MARGIN = 12;
    // How far below that line a group's top may still be to count as current: its title has reached the top
    // while the group above shows only its last few pixels.
    static constexpr int REACH = 40;
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    struct Group {
        QPointer<QWidget> widget;
        QString title;
    };
    // The visible groups of a page, top to bottom (a page never shown keeps its building order).
    static std::vector<Group> groups_of(QScrollArea *scroll);
    // Fills groups with the visible groups of root and watched with every group, hidden or not.
    static void collect(QWidget *root, std::vector<Group> &groups, std::vector<QPointer<QWidget>> &watched);
    int group_top(const Group &group) const;
    int group_index_of(const QWidget *target) const;
    int target_value(int index) const;
    int index_at_scroll() const;
    void update_current();
    void schedule_refresh();
    void stop_animation();
    void visit(const QString &route_key);
    void toggle(const QString &route_key);
    void sub_item_clicked(const QString &route_key, int index);

    NavigationInterface *navigation_;
    QPointer<QScrollArea> scroll_;
    QString route_;
    std::vector<Group> groups_;
    // Groups whose showing or hiding changes the list, hidden ones included.
    std::vector<QPointer<QWidget>> watched_;
    QMetaObject::Connection value_connection_;
    int current_ = -1;
    // The group chosen by a click or a search jump; kept until the page is scrolled by other means.
    int locked_ = -1;
    bool scrolling_by_outline_ = false;
    // Unfolded by the user's arrow, or by opening the page.
    QSet<QString> manual_, automatic_;
    // The group clicked under another page, scrolled to once that page is shown.
    QString pending_title_;
    int animation_from_ = 0;
    int animation_group_ = -1;
    QTimer *refresh_timer_;
    QVariantAnimation animation_;
};
}
