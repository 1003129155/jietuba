#pragma once
#include "clipboard/ui/theme/themes.h"
#include <QColor>
#include <QFont>
#include <QFontMetrics>
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

// Image rows span this many text rows, by the image size setting.
double image_row_span(const QString &image_size);

// Paints clipboard rows directly with QPainter, so opening and scrolling a long list
// creates no widgets per row.
class ClipboardItemDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    ClipboardItemDelegate(QListWidget *parent, const Theme &theme, int font_size = 15, int window_opacity = 0, bool show_metadata = true,
                          int line_height_padding = 8, bool show_shortcuts = true, const QString &image_size = "small", int display_lines = 1);

    void set_theme(const Theme &theme);
    void set_font_size(int size);
    void set_display_lines(int lines) { display_lines_ = std::clamp(lines, 1, 5); }
    // Follows the toolbar and panel scale: fonts rebuilt, sizes computed while painting.
    void apply_scale() { rebuild_font_cache(); }
    void set_window_opacity(int opacity);
    void set_show_metadata(bool show) { show_metadata_ = show; }
    void set_image_size(const QString &size) { image_row_span_ = image_row_span(size); }
    void set_show_shortcuts(bool show) { show_shortcuts_ = show; }
    // The direct pick keys, labelled on the first rows in order.
    void set_pick_keys(const QString &keys) { shortcut_labels_ = labels_for(keys); }
    void set_hide_file_icon(bool hide) { hide_file_icon_ = hide; }
    // The selection manager's cursor and marks, drawn as described in paint().
    void set_cursor_id(std::optional<qint64> item_id) { cursor_id_ = item_id; }
    std::optional<qint64> cursor_id() const { return cursor_id_; }
    void set_marked_ids(const QList<qint64> &item_ids);
    // The circle in the left column of a row while anything is marked: filled with a check when marked, hollow otherwise.
    QRectF mark_circle_rect(const QRect &row_rect) const;
    static void clear_thumb_cache();

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    struct Colors {
        QColor bg_even, bg_odd, bg_selected, cursor_tint, cursor_outline, border_bottom, border_selected, text_primary, text_tertiary, shortcut_text;
    };
    static QStringList labels_for(const QString &keys);
    QStringList text_lines(const ClipboardItem &item, int width) const;
    int text_width(const ClipboardItem &item, int row, int width) const;
    // Whether the row has the left column: the shortcut label, or the mark circle while anything is marked.
    bool has_left_column(int row) const;
    void paint_mark_circle(QPainter *painter, const QRect &row_rect, bool marked) const;
    int font_size() const;
    void rebuild_color_cache();
    void rebuild_font_cache();
    static std::optional<QPixmap> get_thumbnail(const QString &data_url);

    Theme theme_;
    int display_lines_;
    int font_size_;
    int window_opacity_;
    bool show_metadata_;
    int line_height_padding_;
    bool show_shortcuts_;
    QStringList shortcut_labels_;
    bool hide_file_icon_ = false;
    std::optional<qint64> cursor_id_;
    QSet<qint64> marked_ids_;
    double image_row_span_;
    Colors colors_;
    QFont shortcut_font_, icon_font_, content_font_, meta_font_;
    QFontMetrics shortcut_fm_{QFont()};
    QFontMetrics content_fm_{QFont()};
};
}
