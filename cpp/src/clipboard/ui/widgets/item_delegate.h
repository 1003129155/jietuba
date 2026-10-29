#pragma once
#include "clipboard/ui/theme/themes.h"
#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QStyledItemDelegate>
#include <algorithm>
#include <optional>

class QListWidget;

namespace jietuba::clipboard {
struct ClipboardItem;
// QListWidgetItem roles: the item id and the ClipboardItem itself.
constexpr int ROLE_ITEM_ID = Qt::UserRole;
constexpr int ROLE_ITEM_DATA = Qt::UserRole + 1;

// Image rows are this many text lines tall, by the image size setting; the default is a text row's height.
int image_row_lines(const QString &image_size);

// The line icon of a kind, an SVG under svg/, looked up by (content_type, sub_kind). A sub kind without its own icon,
// such as a file the backend could not sort, gets its content type's: for files the generic file icon.
QString kind_icon(const QString &content_type, const QString &sub_kind);
// The source as shown: the program's file name without ".exe".
QString row_source_text(const QString &source_app);
// The time column: HH:mm for today, MM-dd for any other day.
QString row_time_text(const QDateTime &created_at, const QDate &today = QDate::currentDate());

// Paints clipboard rows directly with QPainter, so opening and scrolling a long list
// creates no widgets per row.
class ClipboardItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    ClipboardItemDelegate(QListWidget *parent, const Theme &theme, int font_size = 15, bool show_metadata = true, int line_height_padding = 8,
                          bool show_shortcuts = true, const QString &image_size = "small", int display_lines = 1);

    void set_theme(const Theme &theme);
    void set_font_size(int size);
    void set_display_lines(int lines) { display_lines_ = std::clamp(lines, 1, 5); }
    // Follows the toolbar and panel scale: fonts rebuilt, sizes computed while painting.
    void apply_scale() { rebuild_font_cache(); }
    // Shows or hides the time at the end of the first line.
    void set_show_metadata(bool show) { show_metadata_ = show; }
    void set_image_size(const QString &size) { image_row_lines_ = image_row_lines(size); }
    void set_show_shortcuts(bool show) { show_shortcuts_ = show; }
    // The direct pick keys, labelled at the start of the first rows in order.
    void set_pick_keys(const QString &keys) { shortcut_labels_ = labels_for(keys); }
    // The selection manager's cursor and marks, drawn as described in paint().
    void set_cursor_id(std::optional<qint64> item_id) { cursor_id_ = item_id; }
    std::optional<qint64> cursor_id() const { return cursor_id_; }
    void set_marked_ids(const QList<qint64> &item_ids);
    // The pick key label of a row; empty when it has none: shortcuts off, past the pick keys, or while rows are marked.
    QString key_label(int row) const;
    // The time text of a row; empty when it is turned off or the item has no time.
    QString meta_text(const ClipboardItem &item) const;
    // The circle at the start of a one-line text row while anything is marked: filled with a check when marked, hollow otherwise.
    QRectF mark_circle_rect(const QRect &row_rect) const;
    static void clear_thumb_cache();

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    struct Colors {
        QColor card_fill, cursor_fill, marked_fill, accent, selected, text_primary, text_secondary, text_tertiary;
    };
    // Horizontal layout of a row, from its left edge; paint() and sizeHint() share it so wrapping matches what is drawn.
    struct Columns {
        int circle_x = 0;
        int icon_x = 0, icon_width = 0;
        int key_x = 0, key_width = 0;
        int text_x = 0, text_width = 1;
        // Lines after the first also run under the pin and time.
        int rest_width = 1;
        int pin_x = 0, pin_size = 0;
        int meta_x = 0, meta_width = 0;
        QString meta_text, key_text;
    };
    static QStringList labels_for(const QString &keys);
    QStringList text_lines(const ClipboardItem &item, int width, int rest_width) const;
    Columns columns(const ClipboardItem &item, int row, int row_width, int row_height) const;
    int icon_width(const ClipboardItem &item, int row_height) const;
    int thumbnail_height(int row_height) const;
    int line_height() const;
    int vertical_padding() const;
    QRectF mark_circle_at(const QRect &row_rect, double center_y) const;
    void paint_mark_circle(QPainter *painter, const QRectF &circle, bool marked) const;
    // An SVG tinted with the theme's secondary text color, cached by path, size and pixel ratio.
    const QPixmap &glyph(const QString &path, int size, qreal ratio) const;
    int font_size() const;
    void rebuild_color_cache();
    void rebuild_font_cache();
    static std::optional<QPixmap> get_thumbnail(const QString &data_url);

    Theme theme_;
    int display_lines_;
    int font_size_;
    bool show_metadata_;
    int line_height_padding_;
    bool show_shortcuts_;
    QStringList shortcut_labels_;
    std::optional<qint64> cursor_id_;
    QSet<qint64> marked_ids_;
    int image_row_lines_;
    Colors colors_;
    QFont content_font_, meta_font_;
    QFontMetrics content_fm_{QFont()};
    QFontMetrics meta_fm_{QFont()};
    int glyph_size_ = 16, key_width_ = 12, pin_size_ = 13;
    mutable QHash<QString, QPixmap> glyph_cache_;
};
}
