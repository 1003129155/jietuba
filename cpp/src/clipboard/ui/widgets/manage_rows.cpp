#include "clipboard/ui/widgets/manage_rows.h"
#include "clipboard/ui/widgets/reorder_list.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/theme.h"
#include <QCache>
#include <QPainter>
#include <QPixmap>
#include <optional>

namespace jietuba::clipboard {
namespace {
QFont font_of(const QFont &base, int pixel_size, QFont::Weight weight = QFont::Normal) {
    QFont font(base);
    font.setFamilies(FONT_FAMILIES());
    font.setPixelSize(dialog_scaled(pixel_size));
    font.setWeight(weight);
    return font;
}

// As the settings navigation draws its rows: a neutral wash for hover and the current row, which also gets the bar.
void paint_row_background(QPainter *painter, const QStyleOptionViewItem &option, const QRect &rect) {
    const auto &tokens = get_ui_theme()->tokens();
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    if (!(selected || hovered)) return;
    const int radius = dialog_scaled(4);
    painter->setPen(Qt::NoPen);
    painter->setBrush(css_color(tokens.item_hover));
    painter->drawRoundedRect(QRectF(rect), radius, radius);
    if (selected) {
        const int bar_height = std::min(rect.height() - dialog_scaled(16), dialog_scaled(16));
        const QRectF bar(rect.left(), rect.center().y() - bar_height / 2.0, dialog_scaled(3), bar_height);
        painter->setBrush(css_color(ACCENT));
        painter->drawRoundedRect(bar, bar.width() / 2, bar.width() / 2);
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

QSize GroupRowDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const { return QSize(option.rect.width(), dialog_scaled(36)); }

void GroupRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const auto &tokens = get_ui_theme()->tokens();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    if (const auto *view = qobject_cast<const ReorderListWidget *>(parent()); view && !view->is_row_droppable(index.row())) painter->setOpacity(0.35);
    const QRect rect = option.rect.adjusted(0, dialog_scaled(2), 0, -dialog_scaled(2));
    paint_row_background(painter, option, rect);

    // The icon in the place and size of a navigation icon, without a tile.
    const int icon_box = dialog_scaled(20);
    const QRectF icon_rect(rect.left() + dialog_scaled(14), rect.center().y() - icon_box / 2.0, icon_box, icon_box);
    painter->setFont(font_of(option.font, 15));
    painter->setPen(css_color(tokens.fg));
    painter->drawText(icon_rect, Qt::AlignCenter, index.data(ICON_ROLE).toString());
    int x = static_cast<int>(icon_rect.right()) + dialog_scaled(12);

    int right = rect.right() - dialog_scaled(10);
    const QString tag = index.data(TAG_ROLE).toString();
    if (!tag.isEmpty()) {
        // The settings search's page tag.
        const auto &tone = feature_tone("neutral");
        const QFont tag_font = font_of(option.font, 12);
        const QFontMetrics metrics(tag_font);
        const int pad = dialog_scaled(6);
        const int tag_w = metrics.horizontalAdvance(tag) + pad * 2;
        const int tag_h = metrics.height() + dialog_scaled(4);
        const QRectF tag_rect(right - tag_w, rect.center().y() - tag_h / 2.0, tag_w, tag_h);
        painter->setPen(Qt::NoPen);
        painter->setBrush(css_color(tokens.is_dark() ? tone.dark_bg : tone.light_bg));
        painter->drawRoundedRect(tag_rect, dialog_scaled(5), dialog_scaled(5));
        painter->setFont(tag_font);
        painter->setPen(css_color(tokens.is_dark() ? tone.dark_fg : tone.light_fg));
        painter->drawText(tag_rect, Qt::AlignCenter, tag);
        right = static_cast<int>(tag_rect.left()) - dialog_scaled(6);
    }

    // Same weight when selected: a bold face would shift the name by a fraction of a pixel.
    const QFont name_font = font_of(option.font, 14, QFont::Normal);
    painter->setFont(name_font);
    painter->setPen(css_color(tokens.fg));
    const QRect name_rect(x, rect.top(), std::max(0, right - x), rect.height());
    const QString name = QFontMetrics(name_font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, name_rect.width());
    painter->drawText(name_rect, Qt::AlignVCenter | Qt::AlignLeft, name);
    painter->restore();
}

QSize ContentRowDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const { return QSize(option.rect.width(), dialog_scaled(36)); }

void ContentRowDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const auto &tokens = get_ui_theme()->tokens();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRect rect = option.rect.adjusted(0, dialog_scaled(1), 0, -dialog_scaled(1));
    paint_row_background(painter, option, rect);

    const QRect inner = rect.adjusted(dialog_scaled(14), 0, -dialog_scaled(12), 0);
    if (index.row() < index.model()->rowCount() - 1) {
        painter->setPen(QPen(css_color(tokens.divider), 1));
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
        painter->setFont(font_of(option.font, 14));
        painter->setPen(css_color(tokens.fg));
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
        painter->setPen(css_color(tokens.fg_muted));
        painter->drawText(QRect(right - meta_w, inner.top(), meta_w, inner.height()), Qt::AlignVCenter | Qt::AlignRight, meta);
        right -= meta_w + dialog_scaled(10);
    }

    const QFont text_font = font_of(option.font, 14);
    const int text_w = std::max(0, right - x);
    painter->setFont(text_font);
    painter->setPen(css_color(tokens.fg));
    const QString text = QFontMetrics(text_font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, text_w);
    painter->drawText(QRect(x, inner.top(), text_w, inner.height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    painter->restore();
}
}
