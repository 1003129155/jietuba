#include "clipboard/ui/widgets/manage_rows.h"
#include "clipboard/ui/group_kind_style.h"
#include "clipboard/ui/widgets/reorder_list.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/theme.h"
#include <QCache>
#include <QPainter>
#include <QPixmap>
#include <optional>

namespace jietuba::clipboard {
namespace {
QFont font_of(const QFont &base, int pixel_size, QFont::Weight weight = QFont::Normal) {
    QFont font(base);
    font.setPixelSize(dialog_scaled(pixel_size));
    font.setWeight(weight);
    return font;
}

void paint_row_background(QPainter *painter, const QStyleOptionViewItem &option, const QRect &rect) {
    const auto &tokens = get_ui_theme()->tokens();
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    if (!(selected || hovered)) return;
    const int radius = dialog_scaled(8);
    painter->setPen(Qt::NoPen);
    if (selected) {
        painter->setBrush(css_color(tokens.accent_soft));
    } else {
        QColor hover = css_color(tokens.accent_soft);
        hover.setAlphaF(0.45f);
        painter->setBrush(hover);
    }
    painter->drawRoundedRect(QRectF(rect), radius, radius);
    if (selected) {
        const int bar_height = std::min(rect.height() - dialog_scaled(16), dialog_scaled(18));
        const QRectF bar(rect.left(), rect.center().y() - bar_height / 2.0, dialog_scaled(3), bar_height);
        painter->setBrush(css_color(tokens.accent));
        painter->drawRoundedRect(bar, dialog_scaled_f(1.5), dialog_scaled_f(1.5));
    }
}

QCache<QString, std::optional<QPixmap>> &thumbnail_cache() {
    static QCache<QString, std::optional<QPixmap>> cache(256);
    return cache;
}

// An image item's data-URL thumbnail, decoded once per URL.
std::optional<QPixmap> thumbnail(const QString &data_url) {
    auto &cache = thumbnail_cache();
    if (const auto *cached = cache.object(data_url)) return *cached;
    std::optional<QPixmap> result;
    if (data_url.startsWith("data:image") && data_url.contains(u',')) {
        QPixmap pixmap;
        pixmap.loadFromData(QByteArray::fromBase64(data_url.mid(data_url.indexOf(u',') + 1).toLatin1()));
        if (!pixmap.isNull()) result = pixmap;
    }
    cache.insert(data_url, new std::optional<QPixmap>(result));
    return result;
}
}

void clear_thumbnail_cache() { thumbnail_cache().clear(); }

QSize GroupRowDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const { return QSize(option.rect.width(), dialog_scaled(38)); }

void GroupRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const auto &tokens = get_ui_theme()->tokens();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (const auto *view = qobject_cast<const ReorderListWidget *>(parent()); view && !view->is_row_droppable(index.row())) painter->setOpacity(0.35);
    const QRect rect = option.rect.adjusted(dialog_scaled(4), dialog_scaled(2), -dialog_scaled(4), -dialog_scaled(2));
    paint_row_background(painter, option, rect);

    const QString kind = index.data(KIND_ROLE).toString().isEmpty() ? KIND_NORMAL() : index.data(KIND_ROLE).toString();
    const bool selected = option.state & QStyle::State_Selected;
    const int tile_size = dialog_scaled(26);
    const QRectF tile(rect.left() + dialog_scaled(8), rect.center().y() - tile_size / 2.0, tile_size, tile_size);
    painter->setPen(Qt::NoPen);
    painter->setBrush(kind_tint(kind, selected ? 0.20 : 0.15, tokens));
    painter->drawRoundedRect(tile, dialog_scaled(8), dialog_scaled(8));
    painter->setFont(font_of(option.font, 15));
    painter->setPen(css_color(tokens.text));
    painter->drawText(tile, Qt::AlignCenter, index.data(ICON_ROLE).toString());
    int x = static_cast<int>(tile.right()) + dialog_scaled(10);

    int right = rect.right() - dialog_scaled(10);
    const QString tag = index.data(TAG_ROLE).toString();
    if (!tag.isEmpty()) {
        const QFont tag_font = font_of(option.font, 11);
        const QFontMetrics metrics(tag_font);
        const int pad = dialog_scaled(6);
        const int tag_w = metrics.horizontalAdvance(tag) + pad * 2;
        const int tag_h = metrics.height() + dialog_scaled(2);
        const QRectF tag_rect(right - tag_w, rect.center().y() - tag_h / 2.0, tag_w, tag_h);
        painter->setPen(Qt::NoPen);
        painter->setBrush(kind_tint(kind, 0.16, tokens));
        painter->drawRoundedRect(tag_rect, tag_h / 2.0, tag_h / 2.0);
        painter->setFont(tag_font);
        painter->setPen(kind_text_color(kind, tokens));
        painter->drawText(tag_rect, Qt::AlignCenter, tag);
        right = static_cast<int>(tag_rect.left()) - dialog_scaled(6);
    }

    const QFont name_font = font_of(option.font, 13, selected ? QFont::DemiBold : QFont::Normal);
    painter->setFont(name_font);
    painter->setPen(css_color(tokens.text));
    const QRect name_rect(x, rect.top(), std::max(0, right - x), rect.height());
    const QString name = QFontMetrics(name_font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, name_rect.width());
    painter->drawText(name_rect, Qt::AlignVCenter | Qt::AlignLeft, name);
    painter->restore();
}

QSize ContentRowDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const { return QSize(option.rect.width(), dialog_scaled(34)); }

void ContentRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const auto &tokens = get_ui_theme()->tokens();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRect rect = option.rect.adjusted(dialog_scaled(6), dialog_scaled(1), -dialog_scaled(6), -dialog_scaled(1));
    paint_row_background(painter, option, rect);

    const QRect inner = rect.adjusted(dialog_scaled(14), 0, -dialog_scaled(12), 0);
    if (index.row() < index.model()->rowCount() - 1) {
        QPen pen(css_color(tokens.border), 1);
        pen.setStyle(Qt::DashLine);
        painter->setPen(pen);
        const double y = option.rect.bottom() + 0.5;
        painter->drawLine(QPointF(inner.left(), y), QPointF(inner.right(), y));
    }
    int x = inner.left();

    const auto image = thumbnail(index.data(THUMB_ROLE).toString());
    const QString icon = index.data(ICON_ROLE).toString();
    if (image) {
        const int side = inner.height() - dialog_scaled(8);
        const QPixmap scaled_pixmap = image->scaled(side * 2, side * 2, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QRect target(x, inner.top() + (inner.height() - side) / 2, side, side);
        const QSize fitted = scaled_pixmap.size().scaled(target.size(), Qt::KeepAspectRatio);
        painter->drawPixmap(QRect(target.left(), target.top() + (side - fitted.height()) / 2, fitted.width(), fitted.height()), scaled_pixmap);
        x += side + dialog_scaled(8);
    } else if (!icon.isEmpty()) {
        painter->setFont(font_of(option.font, 13));
        painter->setPen(css_color(tokens.text));
        const int icon_w = dialog_scaled(20);
        painter->drawText(QRect(x, inner.top(), icon_w, inner.height()), Qt::AlignCenter, icon);
        x += icon_w + dialog_scaled(6);
    }

    int right = inner.right();
    const QString meta = index.data(META_ROLE).toString();
    if (!meta.isEmpty()) {
        const QFont meta_font = font_of(option.font, 12);
        const int meta_w = QFontMetrics(meta_font).horizontalAdvance(meta);
        painter->setFont(meta_font);
        painter->setPen(css_color(tokens.text_muted));
        painter->drawText(QRect(right - meta_w, inner.top(), meta_w, inner.height()), Qt::AlignVCenter | Qt::AlignRight, meta);
        right -= meta_w + dialog_scaled(10);
    }

    const QFont text_font = font_of(option.font, 13);
    const int text_w = std::max(0, right - x);
    painter->setFont(text_font);
    painter->setPen(css_color(tokens.text));
    const QString text = QFontMetrics(text_font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, text_w);
    painter->drawText(QRect(x, inner.top(), text_w, inner.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    painter->restore();
}
}
