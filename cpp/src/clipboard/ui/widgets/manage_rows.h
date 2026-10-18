#pragma once
#include <QStyledItemDelegate>

// Row painting of the manage window's group list (left) and item list (middle).
namespace jietuba::clipboard {
constexpr int ICON_ROLE = Qt::UserRole + 1;
constexpr int TAG_ROLE = Qt::UserRole + 2;
constexpr int THUMB_ROLE = Qt::UserRole + 3;
constexpr int META_ROLE = Qt::UserRole + 4;
// Full text searched but not shown: the whole text, or a file item's full path.
constexpr int SEARCH_ROLE = Qt::UserRole + 5;
// The group type (group_kind_style's KIND_*), which sets the colour of the icon tile and the tag.
constexpr int KIND_ROLE = Qt::UserRole + 6;

// A tinted icon tile and the name; quick-launch, hidden and filter groups carry a small tag of the same colour on the right.
class GroupRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

// One line with an icon or thumbnail on the left and the date on the right, like the clipboard panel.
class ContentRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

void clear_thumbnail_cache();
}
