#include "clipboard/ui/widgets/item_delegate.h"
#include "clipboard/core/models.h"
#include "core/logger.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/icons.h"
#include <QHash>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QTextLayout>
#include <cmath>

namespace jietuba::clipboard {
namespace {
// Sizes at 100%; pixels follow the toolbar and panel scale.
// List edge to the row's highlight, and the highlight to its content.
constexpr int ROW_MARGIN = 6;
constexpr int ROW_PADDING_LEFT = 10;
constexpr int ROW_PADDING_RIGHT = 8;
constexpr int COLUMN_GAP = 10;
// Between the key, the icon and the text, which read as one unit.
constexpr int INLINE_GAP = 8;
// Above and below the text lines together, including the gap between two cards. Showing several lines,
// items get more room around them than their lines have between them, so an item's second line does not read as the next item.
constexpr int ROW_PADDING_VERTICAL = 16;
constexpr int ROW_PADDING_VERTICAL_MULTI_LINE = 24;
constexpr int ROW_GAP = 4;
constexpr int HIGHLIGHT_RADIUS = 6;
constexpr int MARK_CIRCLE_RADIUS = 7;
// Every row sits on a card of the text color this faint: enough to part the rows, little enough that the cursor stays the one that stands out.
constexpr int CARD_ALPHA = 8;
constexpr int CURSOR_ALPHA = 60;
// A thumbnail fills its card's height but for this margin, and is this many times as wide.
constexpr int THUMBNAIL_INSET = 3;
constexpr double THUMBNAIL_ASPECT = 1.6;

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

bool has_thumbnail(const ClipboardItem &item) { return item.content_type == "image" && item.thumbnail && !item.thumbnail->isEmpty(); }

struct KindIcon {
    const char *content_type;
    const char *sub_kind;
    const char *icon;
};
// The empty sub kind is the content type's own icon. File sub kinds are the backend's categories of the files a record holds.
constexpr KindIcon KIND_ICONS[] = {
    {"text", "", "clipboard_text.svg"},
    {"text", "url", "clipboard_link.svg"},
    {"text", "path", "clipboard_folder.svg"},
    {"image", "", "clipboard_image.svg"},
    {"file", "", "clipboard_file.svg"},
    {"file", "folder", "clipboard_folder.svg"},
    {"file", "image", "clipboard_file_image.svg"},
    {"file", "spreadsheet", "clipboard_file_spreadsheet.svg"},
    {"file", "document", "clipboard_file_document.svg"},
    {"file", "presentation", "clipboard_file_presentation.svg"},
    {"file", "pdf", "clipboard_file_pdf.svg"},
    {"file", "markdown", "clipboard_file_markdown.svg"},
    {"file", "text", "clipboard_file_text.svg"},
    {"file", "archive", "clipboard_file_archive.svg"},
    {"file", "audio", "clipboard_file_audio.svg"},
    {"file", "video", "clipboard_file_video.svg"},
};
}

QString kind_icon(const QString &content_type, const QString &sub_kind) {
    const char *fallback = "clipboard_text.svg";
    for (const auto &entry : KIND_ICONS) {
        if (content_type != QLatin1String(entry.content_type)) continue;
        if (sub_kind == QLatin1String(entry.sub_kind)) return QString::fromLatin1(entry.icon);
        if (!*entry.sub_kind) fallback = entry.icon;
    }
    return QString::fromLatin1(fallback);
}

int image_row_lines(const QString &image_size) {
    if (image_size == "medium") return 2;
    if (image_size == "large") return 3;
    return 1;
}

QString row_source_text(const QString &source_app) {
    QString name = source_app.trimmed();
    if (name.endsWith(".exe", Qt::CaseInsensitive)) name.chop(4);
    return name;
}

QString row_time_text(const QDateTime &created_at, const QDate &today) {
    return created_at.date() == today ? created_at.time().toString("HH:mm") : created_at.date().toString("MM-dd");
}

ClipboardItemDelegate::ClipboardItemDelegate(QListWidget *parent, const Theme &theme, int font_size, bool show_metadata, int line_height_padding,
                                             bool show_shortcuts, const QString &image_size, int display_lines)
    : QStyledItemDelegate(parent), theme_(theme), display_lines_(std::clamp(display_lines, 1, 5)), font_size_(font_size), show_metadata_(show_metadata),
      line_height_padding_(line_height_padding), show_shortcuts_(show_shortcuts), shortcut_labels_(labels_for("123456789abcdefghijklmnopqrstuvwxyz")),
      image_row_lines_(image_row_lines(image_size)) {
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

int ClipboardItemDelegate::line_height() const { return static_cast<int>(font_size() * 1.4); }

int ClipboardItemDelegate::vertical_padding() const {
    return scaled(display_lines_ > 1 ? ROW_PADDING_VERTICAL_MULTI_LINE : ROW_PADDING_VERTICAL);
}

QString ClipboardItemDelegate::key_label(int row) const {
    // While rows are marked the keys paste nothing, and the circles take their place.
    if (!show_shortcuts_ || !marked_ids_.isEmpty() || row < 0 || row >= shortcut_labels_.size()) return {};
    return shortcut_labels_[row];
}

QString ClipboardItemDelegate::meta_text(const ClipboardItem &item) const {
    // The source would cost the text a third of the line; the preview shows it.
    if (!show_metadata_ || !item.created_at.isValid()) return {};
    return row_time_text(item.created_at);
}

int ClipboardItemDelegate::thumbnail_height(int row_height) const { return std::max(1, row_height - scaled(ROW_GAP) - 2 * scaled(THUMBNAIL_INSET)); }

int ClipboardItemDelegate::icon_width(const ClipboardItem &item, int row_height) const {
    if (has_thumbnail(item)) return qRound(thumbnail_height(row_height) * THUMBNAIL_ASPECT);
    if (item.color || item.content_type == "file") return glyph_size_;
    // Plain text, most of the list, gets no icon; only the kinds an icon tells apart do.
    return kind_icon(item.content_type, item.sub_kind) == QLatin1String("clipboard_text.svg") ? 0 : glyph_size_;
}

ClipboardItemDelegate::Columns ClipboardItemDelegate::columns(const ClipboardItem &item, int row, int row_width, int row_height) const {
    Columns c;
    const int inline_gap = scaled(INLINE_GAP);
    int left = scaled(ROW_MARGIN) + scaled(ROW_PADDING_LEFT);
    const int full_right = row_width - scaled(ROW_MARGIN) - scaled(ROW_PADDING_RIGHT);
    int right = full_right;
    // Left to right: the pick key, or while rows are marked the circle, then the icon, then the text.
    // Rows past the pick keys give the key's room to the text.
    if (!marked_ids_.isEmpty()) {
        c.circle_x = left;
        left += 2 * scaled(MARK_CIRCLE_RADIUS) + inline_gap;
    } else if (c.key_text = key_label(row); !c.key_text.isEmpty()) {
        c.key_x = left;
        c.key_width = key_width_;
        left += key_width_ + inline_gap;
    }
    c.icon_width = icon_width(item, row_height);
    c.icon_x = left;
    if (c.icon_width > 0) left += c.icon_width + inline_gap;
    c.text_x = left;
    // Right to left on the first line: time, then pin.
    c.meta_text = meta_text(item);
    if (!c.meta_text.isEmpty()) {
        c.meta_width = meta_fm_.horizontalAdvance(c.meta_text);
        c.meta_x = right - c.meta_width;
        right = c.meta_x - scaled(COLUMN_GAP);
    }
    if (item.is_pinned) {
        c.pin_size = pin_size_;
        c.pin_x = right - pin_size_;
        right = c.pin_x - inline_gap;
    }
    c.text_width = std::max(1, right - left);
    c.rest_width = std::max(1, full_right - left);
    return c;
}

QRectF ClipboardItemDelegate::mark_circle_at(const QRect &row_rect, double center_y) const {
    const double radius = scaled_f(MARK_CIRCLE_RADIUS);
    const double left = row_rect.left() + scaled(ROW_MARGIN) + scaled(ROW_PADDING_LEFT);
    return QRectF(left, center_y - radius, radius * 2, radius * 2);
}

QRectF ClipboardItemDelegate::mark_circle_rect(const QRect &row_rect) const {
    return mark_circle_at(row_rect, row_rect.top() + (vertical_padding() + line_height()) / 2.0);
}

void ClipboardItemDelegate::paint_mark_circle(QPainter *painter, const QRectF &circle, bool marked) const {
    const double radius = circle.width() / 2;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (!marked) {
        painter->setPen(QPen(colors_.text_tertiary, scaled_f(1.4)));
        painter->setBrush(Qt::NoBrush);
        painter->drawEllipse(circle.adjusted(0.7, 0.7, -0.7, -0.7));
        painter->restore();
        return;
    }
    painter->setPen(Qt::NoPen);
    painter->setBrush(colors_.selected);
    painter->drawEllipse(circle);
    QPen tick(Qt::white, radius / 4.5);
    tick.setCapStyle(Qt::RoundCap);
    tick.setJoinStyle(Qt::RoundJoin);
    painter->setPen(tick);
    painter->setBrush(Qt::NoBrush);
    const QPointF c = circle.center();
    QPainterPath path(QPointF(c.x() - radius * 0.45, c.y() + radius * 0.02));
    path.lineTo(c.x() - radius * 0.1, c.y() + radius * 0.35);
    path.lineTo(c.x() + radius * 0.48, c.y() - radius * 0.33);
    painter->drawPath(path);
    painter->restore();
}

const QPixmap &ClipboardItemDelegate::glyph(const QString &path, int size, qreal ratio) const {
    const QString key = QString("%1|%2|%3").arg(path).arg(size).arg(ratio);
    if (const auto found = glyph_cache_.constFind(key); found != glyph_cache_.constEnd()) return *found;
    // Rendered and tinted at the device size: a larger copy scaled down blurs the thin strokes.
    const int device_size = qRound(size * ratio);
    QPixmap pixmap = ResourceManager::get_icon(path, device_size).pixmap(QSize(device_size, device_size), 1.0);
    if (!pixmap.isNull()) {
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), colors_.text_secondary);
    }
    pixmap.setDevicePixelRatio(ratio);
    return *glyph_cache_.insert(key, pixmap);
}

QStringList ClipboardItemDelegate::text_lines(const ClipboardItem &item, int width, int rest_width) const {
    width = std::max(1, width);
    rest_width = std::max(1, rest_width);
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
            const int line_width = lines.isEmpty() ? width : rest_width;
            if (line.isValid()) line.setLineWidth(line_width);
            lines.append(line.isValid() ? paragraphs[paragraph].mid(line.textStart(), line.textLength()) : QString());
            if (lines.size() == display_lines_) {
                const bool more = (line.isValid() && line.textStart() + line.textLength() < paragraphs[paragraph].size()) ||
                                  paragraph + 1 < paragraphs.size() || source.size() > 4096;
                if (more) lines.last() = content_fm_.elidedText(lines.last() + QChar(0x2026), Qt::ElideRight, line_width);
                layout.endLayout();
                return lines;
            }
            if (!line.isValid() || line.textStart() + line.textLength() >= paragraphs[paragraph].size()) break;
        } while (true);
        layout.endLayout();
    }
    return lines;
}

QStringList ClipboardItemDelegate::labels_for(const QString &keys) {
    // Key caps show letters as capitals, like the keys they stand for.
    QStringList labels;
    for (char32_t key : keys.toUcs4()) labels.append(QString::fromUcs4(&key, 1).toUpper());
    return labels;
}

void ClipboardItemDelegate::set_marked_ids(const QList<qint64> &item_ids) { marked_ids_ = QSet<qint64>(item_ids.begin(), item_ids.end()); }

void ClipboardItemDelegate::rebuild_color_cache() {
    const auto &c = theme_.colors;
    // The cursor is an accent wash; marks use the theme's stronger highlight so the two never read alike.
    colors_.card_fill = hex_to_qcolor(c.text_primary, CARD_ALPHA);
    colors_.cursor_fill = hex_to_qcolor(c.accent_primary, CURSOR_ALPHA);
    colors_.marked_fill = parse_rgba(c.bg_selected_highlight);
    colors_.accent = hex_to_qcolor(c.accent_primary);
    colors_.selected = hex_to_qcolor(c.border_selected);
    colors_.text_primary = hex_to_qcolor(c.text_primary);
    colors_.text_secondary = hex_to_qcolor(c.text_secondary);
    colors_.text_tertiary = hex_to_qcolor(c.text_tertiary);
    glyph_cache_.clear();
}

void ClipboardItemDelegate::clear_thumb_cache() { thumb_cache().clear(); }

void ClipboardItemDelegate::rebuild_font_cache() {
    const int size = font_size();
    content_font_ = QFont();
    content_font_.setPixelSize(size);
    content_fm_ = QFontMetrics(content_font_);
    meta_font_ = QFont();
    meta_font_.setPixelSize(std::max(scaled(11), qRound(size * 0.78)));
    meta_fm_ = QFontMetrics(meta_font_);
    glyph_size_ = std::max(scaled(16), qRound(size * 0.95));
    pin_size_ = std::max(scaled(13), qRound(size * 0.8));
    // Wide enough for any label, so every row's text starts at the same place.
    key_width_ = std::max(meta_fm_.horizontalAdvance(QStringLiteral("M")), meta_fm_.horizontalAdvance(QStringLiteral("W")));
    glyph_cache_.clear();
}

void ClipboardItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    const QVariant data = index.data(ROLE_ITEM_DATA);
    if (!data.isValid() || !data.canConvert<ClipboardItem>()) return;
    const auto item = data.value<ClipboardItem>();
    const qint64 item_id = index.data(ROLE_ITEM_ID).toLongLong();
    const bool is_cursor = cursor_id_ && item_id == *cursor_id_;
    const bool is_marked = marked_ids_.contains(item_id);
    const auto &cc = colors_;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRect rect = option.rect;
    const auto cols = columns(item, index.row(), rect.width(), rect.height());
    const auto lines = text_lines(item, cols.text_width, cols.rest_width);
    const bool is_image = item.content_type == "image";
    const int line_h = line_height();
    const qreal ratio = painter->device()->devicePixelRatioF();
    // Images centre their line against the thumbnail; text rows keep the first line at the top so extra lines hang below it.
    const double center_y = rect.top() + (is_image ? rect.height() : vertical_padding() + line_h) / 2.0;
    const auto at = [&](int x) { return rect.left() + x; };
    // Icons land on whole device pixels; a half-pixel offset would resample them soft.
    const auto snap = [ratio](double value) { return std::round(value * ratio) / ratio; };

    // ---- Card ----
    // Every row is a faint card; the cursor's is an accent wash with a bar at its start, a mark's a stronger wash,
    // and a marked row under the cursor adds an outline.
    const QRectF card = QRectF(rect).adjusted(scaled(ROW_MARGIN), scaled(ROW_GAP) / 2.0, -scaled(ROW_MARGIN), -scaled(ROW_GAP) / 2.0);
    painter->setPen(Qt::NoPen);
    painter->setBrush(is_marked ? cc.marked_fill : is_cursor ? cc.cursor_fill : cc.card_fill);
    painter->drawRoundedRect(card, scaled(HIGHLIGHT_RADIUS), scaled(HIGHLIGHT_RADIUS));
    if (is_cursor && !is_marked) {
        const double bar_height = std::min(card.height() - scaled(10), scaled_f(18));
        painter->setBrush(cc.accent);
        painter->drawRoundedRect(QRectF(card.left() + scaled_f(1), card.center().y() - bar_height / 2, scaled_f(3), bar_height), scaled_f(1.5), scaled_f(1.5));
    }
    if (is_marked && is_cursor) {
        painter->setPen(QPen(cc.selected, 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), scaled(HIGHLIGHT_RADIUS), scaled(HIGHLIGHT_RADIUS));
    }

    // ---- Mark circle ----
    if (!marked_ids_.isEmpty()) {
        QRectF circle = mark_circle_at(rect, center_y);
        circle.moveLeft(at(cols.circle_x));
        paint_mark_circle(painter, circle, is_marked);
    }

    // ---- Icon slot ----
    if (cols.icon_width > 0) {
        bool drew_thumbnail = false;
        if (has_thumbnail(item)) {
            if (const auto pixmap = get_thumbnail(*item.thumbnail); pixmap && !pixmap->isNull()) {
                const QSize fitted = pixmap->size().scaled(cols.icon_width, thumbnail_height(rect.height()), Qt::KeepAspectRatio);
                const QRectF target(at(cols.icon_x), center_y - fitted.height() / 2.0, fitted.width(), fitted.height());
                QPainterPath clip;
                clip.addRoundedRect(target, scaled_f(2), scaled_f(2));
                painter->save();
                painter->setClipPath(clip);
                painter->drawPixmap(target, *pixmap, QRectF(pixmap->rect()));
                painter->restore();
                QColor outline = cc.text_primary;
                outline.setAlpha(40);
                painter->setPen(QPen(outline, 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(target.adjusted(0.5, 0.5, -0.5, -0.5), scaled_f(2), scaled_f(2));
                drew_thumbnail = true;
            }
        }
        if (item.color) {
            const double side = scaled(14);
            const QRectF swatch(at(cols.icon_x) + (cols.icon_width - side) / 2.0, center_y - side / 2.0, side, side);
            QColor outline = cc.text_primary;
            outline.setAlpha(70);
            painter->setPen(QPen(outline, 1));
            painter->setBrush(*item.color);
            painter->drawRoundedRect(swatch.adjusted(0.5, 0.5, -0.5, -0.5), scaled_f(3), scaled_f(3));
        } else if (!drew_thumbnail) {
            const QPixmap &icon = glyph(ResourceManager::get_icon_path(kind_icon(item.content_type, item.sub_kind)), glyph_size_, ratio);
            painter->drawPixmap(QRectF(snap(at(cols.icon_x) + (cols.icon_width - glyph_size_) / 2.0), snap(center_y - glyph_size_ / 2.0), glyph_size_, glyph_size_),
                                icon, QRectF(icon.rect()));
        }
    }

    // ---- Text ----
    painter->setFont(content_font_);
    painter->setPen(cc.text_primary);
    const double first_line_top = is_image ? center_y - line_h / 2.0 : rect.top() + vertical_padding() / 2.0;
    for (int line = 0; line < lines.size(); ++line)
        painter->drawText(QRectF(at(cols.text_x), first_line_top + line * line_h, line == 0 ? cols.text_width : cols.rest_width, line_h),
                          Qt::AlignVCenter | Qt::AlignLeft, lines[line]);

    // ---- Pin ----
    if (cols.pin_size > 0) {
        const QPixmap &pin = glyph(fluent_icon_path(FluentIcon::Pin), cols.pin_size, ratio);
        painter->drawPixmap(QRectF(snap(at(cols.pin_x)), snap(center_y - cols.pin_size / 2.0), cols.pin_size, cols.pin_size), pin, QRectF(pin.rect()));
    }

    // ---- Time ----
    if (!cols.meta_text.isEmpty()) {
        painter->setFont(meta_font_);
        painter->setPen(cc.text_tertiary);
        const int meta_height = meta_fm_.height();
        painter->drawText(QRectF(at(cols.meta_x), center_y - meta_height / 2.0, cols.meta_width + 2, meta_height), Qt::AlignVCenter | Qt::AlignLeft, cols.meta_text);
    }

    // ---- Pick key ----
    if (!cols.key_text.isEmpty()) {
        painter->setFont(meta_font_);
        painter->setPen(is_cursor ? cc.text_primary : cc.text_tertiary);
        const int key_height = meta_fm_.height();
        painter->drawText(QRectF(at(cols.key_x), center_y - key_height / 2.0, cols.key_width, key_height), Qt::AlignCenter, cols.key_text);
    }

    painter->restore();
}

QSize ClipboardItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
    int height = line_height() + vertical_padding();
    const QVariant data = index.data(ROLE_ITEM_DATA);
    if (data.canConvert<ClipboardItem>()) {
        const auto item = data.value<ClipboardItem>();
        if (has_thumbnail(item)) {
            height = line_height() * image_row_lines_ + vertical_padding();
        } else if (item.content_type != "image" && display_lines_ > 1) {
            // One line is one line however wide the row is; only wrapping needs the text's width.
            const auto *list = qobject_cast<QListWidget *>(parent());
            const int width = list && list->viewport()->width() > 0 ? list->viewport()->width() : option.rect.width();
            const auto cols = columns(item, index.row(), width > 0 ? width : 300, 0);
            height = line_height() * static_cast<int>(text_lines(item, cols.text_width, cols.rest_width).size()) + vertical_padding();
        }
    }
    return QSize(option.rect.width() > 0 ? option.rect.width() : 300, height);
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
