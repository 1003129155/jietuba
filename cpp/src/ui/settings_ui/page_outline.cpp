#include "ui/settings_ui/page_outline.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/navigation.h"
#include "ui/motion.h"
#include "ui/settings_ui/components.h"
#include <QEvent>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <algorithm>
#include <limits>

namespace jietuba {
namespace {
// Hidden by code, not merely because an ancestor is.
bool hidden_on_purpose(const QWidget *widget) { return widget->isHidden() && widget->testAttribute(Qt::WA_WState_ExplicitShowHide); }

// The title label of a group container, or null for any other widget.
const QLabel *group_label(const QWidget *widget) {
    if (const auto *group = qobject_cast<const SettingCardGroup *>(widget)) return group->titleLabel;
    if (const auto *section = qobject_cast<const SectionCard *>(widget)) return section->titleLabel;
    return nullptr;
}

int top_of(const QWidget *widget, const QWidget *content) { return widget->mapTo(content, QPoint(0, 0)).y(); }
}

PageOutline::PageOutline(NavigationInterface *navigation, QObject *parent) : QObject(parent), navigation_(navigation), refresh_timer_(new QTimer(this)) {
    refresh_timer_->setSingleShot(true);
    refresh_timer_->setInterval(0);
    connect(refresh_timer_, &QTimer::timeout, this, &PageOutline::refresh);
    animation_.setStartValue(0.0);
    animation_.setEndValue(1.0);
    animation_.setEasingCurve(motion::decelerate());
    connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &progress) {
        if (!scroll_ || animation_group_ < 0 || animation_group_ >= group_count()) return;
        // The target is read each step: a page still settling moves its groups.
        const int end = target_value(animation_group_);
        scrolling_by_outline_ = true;
        scroll_->verticalScrollBar()->setValue(animation_from_ + qRound((end - animation_from_) * progress.toReal()));
        scrolling_by_outline_ = false;
    });
    connect(&animation_, &QAbstractAnimation::finished, this, [this] { update_current(); });
    connect(navigation_, &NavigationInterface::subItemClicked, this, &PageOutline::sub_item_clicked);
    connect(navigation_, &NavigationInterface::arrowClicked, this, &PageOutline::toggle);
}

void PageOutline::set_page(QScrollArea *scroll, const QString &route_key) {
    route_ = route_key;
    // Another page's groups are known once it is laid out; until then its own row stays unmarked too.
    if (!scroll || route_key.isEmpty()) navigation_->setSubItemPending({});
    else if (scroll != scroll_.data()) navigation_->setSubItemPending(route_key);
    unfold_only(route_);
    if (scroll == scroll_.data()) {
        schedule_refresh();
        return;
    }
    stop_animation();
    disconnect(value_connection_);
    if (scroll_ && scroll_->widget()) scroll_->widget()->removeEventFilter(this);
    for (const auto &widget : watched_)
        if (widget) widget->removeEventFilter(this);
    watched_.clear();
    groups_.clear();
    scroll_ = scroll;
    current_ = -1;
    locked_ = -1;
    navigation_->setCurrentSubItem({}, -1);
    if (!scroll_) return;
    value_connection_ = connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (scrolling_by_outline_) return;
        // Scrolled by hand or by the page itself: that overrides a chosen group.
        stop_animation();
        locked_ = -1;
        update_current();
    });
    if (scroll_->widget()) scroll_->widget()->installEventFilter(this);
    schedule_refresh();
}

void PageOutline::unfold_only(const QString &route_key) {
    if (!unfolded_.isEmpty() && unfolded_ != route_key) navigation_->setExpanded(unfolded_, false);
    unfolded_ = route_key;
    if (!route_key.isEmpty() && !navigation_->isExpanded(route_key)) navigation_->setExpanded(route_key, true);
}

void PageOutline::toggle(const QString &route_key) {
    if (navigation_->isExpanded(route_key)) {
        navigation_->setExpanded(route_key, false);
        if (unfolded_ == route_key) unfolded_.clear();
        return;
    }
    if (route_key == route_) {
        refresh();
    } else {
        // Its groups are read from the page, which is built for this first.
        QScrollArea *scroll = page_of ? page_of(route_key) : nullptr;
        QStringList titles;
        if (scroll)
            for (const auto &group : groups_of(scroll)) titles << group.title;
        navigation_->setExpandable(route_key, !titles.isEmpty());
        navigation_->setSubItems(route_key, titles);
    }
    if (navigation_->subItemCount(route_key) == 0) return;
    unfold_only(route_key);
}

void PageOutline::sub_item_clicked(const QString &route_key, int index) {
    if (route_key == route_) {
        scroll_to_group(index);
        return;
    }
    pending_title_ = navigation_->subItemTitles(route_key).value(index);
    if (open_page) open_page(route_key);
    QTimer::singleShot(0, this, [this] {
        // The page is laid out by now.
        refresh();
        const QString title = std::exchange(pending_title_, QString());
        for (int i = 0; i < group_count(); ++i)
            if (groups_[i].title == title) {
                scroll_to_group(i);
                return;
            }
    });
}

void PageOutline::collect(QWidget *root, std::vector<Group> &groups, std::vector<QPointer<QWidget>> &watched) {
    for (QObject *object : root->children()) {
        auto *child = qobject_cast<QWidget *>(object);
        if (!child || child->isWindow()) continue;
        if (const QLabel *label = group_label(child)) {
            watched.emplace_back(child);
            if (hidden_on_purpose(child)) continue;
            const QString title = label->text().simplified();
            if (!title.isEmpty()) groups.push_back({child, title});
            continue;
        }
        if (!hidden_on_purpose(child)) collect(child, groups, watched);
    }
}

std::vector<PageOutline::Group> PageOutline::groups_of(QScrollArea *scroll) {
    std::vector<Group> groups;
    std::vector<QPointer<QWidget>> unused;
    if (!scroll || !scroll->widget()) return groups;
    collect(scroll->widget(), groups, unused);
    QWidget *content = scroll->widget();
    // Child order is not page order where sibling pages were shown in turn; positions are.
    std::stable_sort(groups.begin(), groups.end(), [content](const Group &a, const Group &b) { return top_of(a.widget, content) < top_of(b.widget, content); });
    return groups;
}

int PageOutline::group_top(const Group &group) const {
    if (!group.widget || !scroll_ || !scroll_->widget()) return std::numeric_limits<int>::max();
    return top_of(group.widget, scroll_->widget());
}

void PageOutline::refresh() {
    refresh_timer_->stop();
    std::vector<Group> found;
    std::vector<QPointer<QWidget>> watched;
    if (scroll_ && scroll_->widget()) {
        collect(scroll_->widget(), found, watched);
        // A tab's pages are siblings, so child order is not page order.
        std::stable_sort(found.begin(), found.end(), [this](const Group &a, const Group &b) { return group_top(a) < group_top(b); });
    }
    for (const auto &widget : watched)
        if (widget) widget->installEventFilter(this);
    watched_ = std::move(watched);
    bool same = found.size() == groups_.size();
    for (size_t i = 0; same && i < found.size(); ++i) same = found[i].widget == groups_[i].widget && found[i].title == groups_[i].title;
    QStringList titles;
    for (const auto &group : found) titles << group.title;
    if (!same || (!route_.isEmpty() && navigation_->subItemTitles(route_) != titles)) {
        groups_ = std::move(found);
        if (!route_.isEmpty()) {
            navigation_->setExpandable(route_, !titles.isEmpty());
            navigation_->setSubItems(route_, titles);
        }
        if (locked_ >= group_count()) locked_ = -1;
        current_ = -1;
    }
    // A group clicked under another page is the chosen one from the start, not group one on the way.
    if (!pending_title_.isEmpty())
        for (int i = 0; i < group_count(); ++i)
            if (groups_[i].title == pending_title_) locked_ = i;
    update_current();
    navigation_->setSubItemPending({});
}

void PageOutline::schedule_refresh() {
    if (!refresh_timer_->isActive()) refresh_timer_->start();
}

int PageOutline::index_at_scroll() const {
    if (groups_.empty() || !scroll_) return -1;
    const auto *bar = scroll_->verticalScrollBar();
    if (bar->maximum() > 0 && bar->value() >= bar->maximum()) return group_count() - 1;
    // The last group whose top has scrolled up near the line where a chosen group is put.
    const int line = bar->value() + dialog_scaled(TOP_MARGIN) + dialog_scaled(REACH);
    int index = 0;
    for (int i = 0; i < group_count(); ++i)
        if (group_top(groups_[i]) <= line) index = i;
    return index;
}

void PageOutline::update_current() {
    const int index = locked_ >= 0 && locked_ < group_count() ? locked_ : index_at_scroll();
    if (index == current_ && navigation_->currentSubItem() == index && navigation_->currentSubRoute() == (index < 0 ? QString() : route_)) return;
    current_ = index;
    navigation_->setCurrentSubItem(route_, index);
}

int PageOutline::target_value(int index) const {
    const auto *bar = scroll_->verticalScrollBar();
    return std::clamp(group_top(groups_[index]) - dialog_scaled(TOP_MARGIN), bar->minimum(), bar->maximum());
}

void PageOutline::scroll_to_group(int index) {
    if (!scroll_ || index < 0 || index >= group_count()) return;
    stop_animation();
    locked_ = index;
    update_current();
    const int start = scroll_->verticalScrollBar()->value();
    if (start == target_value(index)) return;
    animation_from_ = start;
    animation_group_ = index;
    animation_.setDuration(std::max(1, motion::duration(motion::SLOW_MS)));
    animation_.start();
}

int PageOutline::group_index_of(const QWidget *target) const {
    for (int i = 0; i < group_count(); ++i) {
        const QWidget *group = groups_[i].widget;
        if (group && (group == target || group->isAncestorOf(target))) return i;
    }
    return -1;
}

void PageOutline::select_group_of(QWidget *target) {
    if (!target) return;
    refresh();
    const int index = group_index_of(target);
    if (index < 0) return;
    stop_animation();
    locked_ = index;
    update_current();
}

void PageOutline::scroll_to_group_of(QWidget *target) {
    if (!target) return;
    refresh();
    scroll_to_group(group_index_of(target));
}

void PageOutline::stop_animation() {
    if (animation_.state() == QAbstractAnimation::Running) animation_.stop();
}

bool PageOutline::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::LayoutRequest:
    case QEvent::Resize:
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
        schedule_refresh();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
}
