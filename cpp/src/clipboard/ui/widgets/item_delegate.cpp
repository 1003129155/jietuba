#include "clipboard/ui/widgets/item_delegate.h"
#include "clipboard/core/models.h"
#include "core/logger.h"
#include "core/ui_scale.h"
#include <QHash>
#include <QListWidget>
#include <QPainter>
#include <QPen>
#include <QTextLayout>

namespace jietuba::clipboard {
namespace {
// Width of the shortcut badge area (wider with the colon).
constexpr int SHORTCUT_BADGE_WIDTH = 26;
constexpr int MIN_IMAGE_ROW_HEIGHT = 42;

QColor hex_to_qcolor(QString hex_color, int alpha = 255) {
    while (hex_color.startsWith(u'#')) hex_color.remove(0, 1);
    if (hex_color.size() == 3) hex_color = QString(hex_color[0]) + hex_color[0] + hex_color[1] + hex_color[1] + hex_color[2] + hex_color[2];
    return QColor(hex_color.mid(0, 2).toInt(nullptr, 16), hex_color.mid(2, 2).toInt(nullptr, 16), hex_color.mid(4, 2).toInt(nullptr, 16), alpha);
}

// "rgba(r, g, b, a)" with a from 0 to 1.
QColor parse_rgba(const QString &rgba_str) {
    QString inner = rgba_str.trimmed();
    if (inner.startsWith("rgba(")) inner = inner.mid(5);
    if (inner.endsWith(")")) inner.chop(1);
    const auto parts = inner.split(u',');
    bool ok_r = false, ok_g = false, ok_b = false, ok_a = false;
    if (parts.size() < 4) return QColor(0, 0, 0, 40);
    const int r = parts[0].trimmed().toInt(&ok_r), g = parts[1].trimmed().toInt(&ok_g), b = parts[2].trimmed().toInt(&ok_b);
    const double a = parts[3].trimmed().toDouble(&ok_a);
    if (!ok_r || !ok_g || !ok_b || !ok_a) return QColor(0, 0, 0, 40);
    return QColor(r, g, b, static_cast<int>(a * 255));
}

QHash<QString, std::optional<QPixmap>> &thumb_cache() {
    static QHash<QString, std::optional<QPixmap>> cache;
    return cache;
}
}

double image_row_span(const QString &image_size) {
    if (image_size == "medium") return 1.5;
    if (image_size == "large") return 2;
    return 1;
}

ClipboardItemDelegate::ClipboardItemDelegate(QListWidget *parent, const Theme &theme, int font_size, int window_opacity, bool show_metadata,
                                             int line_height_padding, bool show_shortcuts, const QString &image_size, int display_lines)
    : QStyledItemDelegate(parent), theme_(theme), display_lines_(std::clamp(display_lines, 1, 5)), font_size_(font_size), window_opacity_(window_opacity), show_metadata_(show_metadata),
      line_height_padding_(line_height_padding), show_shortcuts_(show_shortcuts), shortcut_labels_(labels_for("123456789abcdefghijklmnopqrstuvwxyz")),
      image_row_span_(image_row_span(image_size)) {
    rebuild_color_cache();
    rebuild_font_cache();
}

void ClipboardItemDelegate::set_theme(const Theme &theme) {
    theme_ = theme;
    rebuild_color_cache();
}

void ClipboardItemDelegate::set_font_size(int size) {
    font_size_ = size;
    rebuild_font_cache();
}

int ClipboardItemDelegate::font_size() const {
    // The font size setting is in pixels at 100%, times the panel scale.
    return scaled(font_size_ >= 10 ? font_size_ : 15);
}

int ClipboardItemDelegate::text_width(const ClipboardItem &item, int row, int width) const {
    const int left = show_shortcuts_ && row < shortcut_labels_.size() ? scaled(SHORTCUT_BADGE_WIDTH + 4) : scaled(12);
    const int icon = item.color ? scaled(54) : !item.icon().isEmpty() && !(hide_file_icon_ && item.content_type == "file") ? scaled(24) : 0;
    return std::max(1, width - 1 - scaled(5) - left - icon - (item.is_pinned ? scaled(24) : 0) - (marked_ids_.contains(item.id) ? scaled(20) : 0));
}

QStringList ClipboardItemDelegate::text_lines(const ClipboardItem &item, int width) const {
    width = std::max(1, width);
    if (display_lines_ == 1 || item.content_type == "image") return {content_fm_.elidedText(item.display_text(), Qt::ElideRight, width)};
    const QString source = item.title && !item.title->isEmpty() ? *item.title : item.content_type == "text" ? item.content : item.display_text();
    // Bound layout work even for very large clipboard records.
    QString text = source.left(4096);
    text.replace("\r\n", "\n");
    text.replace(u'\r', u'\n');
    const auto paragraphs = text.split(u'\n');
    QStringList lines;
    for (int paragraph = 0; paragraph < paragraphs.size(); ++paragraph) {
        QTextLayout layout(paragraphs[paragraph], content_font_);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        layout.beginLayout();
        do {
            auto line = layout.createLine();
            if (line.isValid()) line.setLineWidth(width);
            lines.append(line.isValid() ? paragraphs[paragraph].mid(line.textStart(), line.textLength()) : QString());
            if (lines.size() == display_lines_) {
                const bool more = (line.isValid() && line.textStart() + line.textLength() < paragraphs[paragraph].size()) ||
                                  paragraph + 1 < paragraphs.size() || source.size() > 4096;
                if (more) lines.last() = content_fm_.elidedText(lines.last() + QChar(0x2026), Qt::ElideRight, width);
                layout.endLayout();
                return lines;
            }
            if (!line.isValid() || line.textStart() + line.textLength() >= paragraphs[paragraph].size()) break;
        } while (true);
        layout.endLayout();
    }
    return lines;
}

void ClipboardItemDelegate::set_window_opacity(int opacity) {
    window_opacity_ = opacity;
    rebuild_color_cache();
}

QStringList ClipboardItemDelegate::labels_for(const QString &keys) {
    // With a full-width colon.
    QStringList labels;
    for (char32_t key : keys.toUcs4()) labels.append(QString::fromUcs4(&key, 1) + QString::fromUtf8("："));
    return labels;
}

void ClipboardItemDelegate::set_marked_ids(const QList<qint64> &item_ids) { marked_ids_ = QSet<qint64>(item_ids.begin(), item_ids.end()); }

void ClipboardItemDelegate::rebuild_color_cache() {
    const auto &c = theme_.colors;
    const int alpha = 255 - static_cast<int>(window_opacity_ * 2.55);
    colors_.bg_even = hex_to_qcolor(c.bg_primary, alpha);
    colors_.bg_odd = hex_to_qcolor(c.bg_secondary, alpha);
    colors_.bg_selected = parse_rgba(c.bg_selected_highlight);
    colors_.border_bottom = hex_to_qcolor(c.border_primary, 128);
    colors_.border_selected = hex_to_qcolor(c.border_selected);
    colors_.text_primary = hex_to_qcolor(c.text_primary);
    colors_.text_tertiary = hex_to_qcolor(c.text_tertiary);
    colors_.shortcut_text = hex_to_qcolor(c.shortcut_key_color);
}

void ClipboardItemDelegate::clear_thumb_cache() { thumb_cache().clear(); }

void ClipboardItemDelegate::rebuild_font_cache() {
    const int size = font_size();
    shortcut_font_ = QFont();
    shortcut_font_.setPixelSize(std::max(scaled(10), size - 1));
    shortcut_font_.setBold(true);
    shortcut_fm_ = QFontMetrics(shortcut_font_);
    icon_font_ = QFont();
    icon_font_.setPixelSize(scaled(16));
    content_font_ = QFont();
    content_font_.setPixelSize(size);
    content_fm_ = QFontMetrics(content_font_);
    meta_font_ = QFont();
    meta_font_.setPixelSize(scaled(10));
}

void ClipboardItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);

    const QRect rect = option.rect;
    const QVariant data = index.data(ROLE_ITEM_DATA);
    const qint64 item_id = index.data(ROLE_ITEM_ID).toLongLong();
    const int row = index.row();
    if (!data.isValid() || !data.canConvert<ClipboardItem>()) {
        painter->restore();
        return;
    }
    const auto item_data = data.value<ClipboardItem>();
    const bool is_selected = highlighted_id_ && item_id == *highlighted_id_;
    const bool is_marked = marked_ids_.contains(item_id);
    const auto &cc = colors_;

    // ---- Background ----
    if (is_selected || is_marked) painter->fillRect(rect, cc.bg_selected);
    else if (row % 2 == 0) painter->fillRect(rect, cc.bg_even);
    else painter->fillRect(rect, cc.bg_odd);

    // ---- Left bar (selection indicator) ----
    // With a multiple selection the bar marks only selected rows; hovered and keyboard rows only change background, so the selection stands out.
    if (is_marked || (is_selected && marked_ids_.isEmpty())) painter->fillRect(QRect(rect.left(), rect.top(), scaled(3), rect.height()), cc.border_selected);

    // ---- Bottom separator ----
    QPen pen(cc.border_bottom);
    pen.setWidth(1);
    painter->setPen(pen);
    painter->drawLine(rect.left(), rect.bottom(), rect.right(), rect.bottom());

    // ---- Shortcut badge (the first rows a direct pick key reaches, fixed area on the left) ----
    const int shortcut_badge_width = show_shortcuts_ ? scaled(SHORTCUT_BADGE_WIDTH) : 0;
    const bool has_badge = show_shortcuts_ && row < shortcut_labels_.size();
    if (has_badge) {
        const QString &label = shortcut_labels_[row];
        painter->setFont(shortcut_font_);
        painter->setPen(cc.shortcut_text);
        // Centred in the area right of the selection bar (3px).
        const int text_w = shortcut_fm_.horizontalAdvance(label);
        const int badge_area_left = rect.left() + scaled(3);
        const int badge_x = badge_area_left + (shortcut_badge_width - text_w) / 2;
        const int badge_y = rect.top() + (rect.height() + shortcut_fm_.ascent() - shortcut_fm_.descent()) / 2;
        painter->drawText(badge_x, badge_y, label);
    }

    // ---- Content area ----
    // With a badge: its width plus 4px; without: the standard 12px margin.
    const int content_left = has_badge ? rect.left() + shortcut_badge_width + scaled(4) : rect.left() + scaled(12);
    const int content_right = rect.right() - scaled(5);
    const int content_top = rect.top() + 1;
    const int content_width = content_right - content_left;
    const int size = font_size();
    const int line_height = std::max(static_cast<int>(size * 1.4), item_data.color ? scaled(32) : 0);
    int x_offset = content_left;

    // ---- Icon / thumbnail ----
    bool drew_thumbnail = false;
    if (item_data.content_type == "image" && item_data.thumbnail && !item_data.thumbnail->isEmpty()) {
        drew_thumbnail = true;
        if (const auto pixmap = get_thumbnail(*item_data.thumbnail)) {
            // Scaled to fit a square of the row height (above the separator), left aligned and vertically centred.
            const int side = rect.height() - 2;
            const QSize fitted = pixmap->size().scaled(side, side, Qt::KeepAspectRatio);
            const QRect thumb_rect(x_offset, rect.top() + 1 + (side - fitted.height()) / 2, fitted.width(), fitted.height());
            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter->drawPixmap(thumb_rect, *pixmap);
            x_offset += fitted.width() + scaled(6);
        }
    }
    if (item_data.color) {
        const int swatch_width = scaled(44), swatch_height = scaled(28);
        const QRectF swatch(x_offset + scaled(2), content_top + (line_height - swatch_height) / 2.0, swatch_width, swatch_height);
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor border = cc.text_primary;
        border.setAlpha(110);
        painter->setPen(QPen(border, 1));
        painter->setBrush(*item_data.color);
        painter->drawRoundedRect(swatch.adjusted(0.5, 0.5, -0.5, -0.5), scaled(3), scaled(3));
        x_offset += swatch_width + scaled(10);
    } else if (!drew_thumbnail && !item_data.icon().isEmpty() && !(hide_file_icon_ && item_data.content_type == "file")) {
        painter->setFont(icon_font_);
        painter->setPen(cc.text_primary);
        painter->drawText(QRect(x_offset, content_top, scaled(24), line_height), Qt::AlignVCenter, item_data.icon());
        x_offset += scaled(24);
    }

    // ---- Text ----
    painter->setFont(content_font_);
    painter->setPen(cc.text_primary);
    const QRect text_rect(x_offset, content_top, content_right - x_offset, line_height);
    // Room on the right for the pin mark and the selection check.
    const int pin_width = item_data.is_pinned ? scaled(24) : 0;
    const int mark_width = is_marked ? scaled(20) : 0;
    const auto lines = text_lines(item_data, text_rect.width() - pin_width - mark_width);
    for (int line = 0; line < lines.size(); ++line)
        painter->drawText(text_rect.translated(0, line * line_height), Qt::AlignVCenter, lines[line]);

    // ---- Pin mark ----
    if (item_data.is_pinned)
        painter->drawText(QRect(content_right - pin_width, content_top, pin_width, line_height), Qt::AlignVCenter | Qt::AlignRight, QString::fromUtf8("📌"));

    // ---- Selection check ----
    if (is_marked) {
        painter->setPen(cc.border_selected);
        painter->drawText(QRect(content_right - pin_width - mark_width, content_top, mark_width, line_height), Qt::AlignVCenter | Qt::AlignRight,
                          QString::fromUtf8("✓"));
    }

    // ---- Second line: metadata ----
    if (show_metadata_) {
        painter->setFont(meta_font_);
        painter->setPen(cc.text_tertiary);
        QStringList meta_parts;
        if (item_data.source_app && !item_data.source_app->isEmpty()) meta_parts.append(*item_data.source_app);
        if (item_data.created_at.isValid()) meta_parts.append(item_data.created_at.toString("MM-dd HH:mm"));
        const QRect meta_rect(content_left, content_top + line_height * lines.size(), content_width, scaled(16));
        painter->drawText(meta_rect, Qt::AlignRight | Qt::AlignVCenter, meta_parts.join(QString::fromUtf8(" · ")));
    }

    painter->restore();
}

QSize ClipboardItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    // Content rows plus a 1px separator above and below.
    int content_height = static_cast<int>(font_size() * 1.4) + 2;
    if (show_metadata_) content_height += scaled(16);
    const QVariant data = index.data(ROLE_ITEM_DATA);
    if (data.canConvert<ClipboardItem>()) {
        const auto item_data = data.value<ClipboardItem>();
        if (item_data.content_type != "image") {
            const auto *list = qobject_cast<QListWidget *>(parent());
            const int width = list && list->viewport()->width() > 0 ? list->viewport()->width() : option.rect.width();
            const int lines = text_lines(item_data, text_width(item_data, index.row(), width > 0 ? width : 300)).size();
            content_height = std::max(static_cast<int>(font_size() * 1.4), item_data.color ? scaled(32) : 0) * lines + 2 + (show_metadata_ ? scaled(16) : 0);
        }
        if (item_data.content_type == "image" && item_data.thumbnail && !item_data.thumbnail->isEmpty())
            content_height = std::max(static_cast<int>(content_height * image_row_span_), scaled(MIN_IMAGE_ROW_HEIGHT));
    }
    return QSize(option.rect.width() > 0 ? option.rect.width() : 300, content_height);
}

std::optional<QPixmap> ClipboardItemDelegate::get_thumbnail(const QString &data_url) {
    auto &cache = thumb_cache();
    if (const auto found = cache.constFind(data_url); found != cache.constEnd()) return *found;
    std::optional<QPixmap> result;
    if (data_url.startsWith("data:image") && data_url.contains(u',')) {
        QPixmap pixmap;
        pixmap.loadFromData(QByteArray::fromBase64(data_url.mid(data_url.indexOf(u',') + 1).toLatin1()));
        result = pixmap;
    }
    cache.insert(data_url, result);
    return result;
}
}
