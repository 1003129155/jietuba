#pragma once
#include "clipboard/ui/theme/themes.h"
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QStyledItemDelegate>
#include <optional>

class QListWidget;

namespace jietuba::clipboard {
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
    ClipboardItemDelegate(QListWidget *parent, const Theme &theme, int display_lines = 1, int window_opacity = 0, bool show_metadata = true,
                          int line_height_padding = 8, bool show_shortcuts = true, const QString &image_size = "small");

    void set_theme(const Theme &theme);
    void set_display_lines(int lines);
    // Follows the toolbar and panel scale: fonts rebuilt, sizes computed while painting.
    void apply_scale() { rebuild_font_cache(); }
    void set_window_opacity(int opacity);
    void set_show_metadata(bool show) { show_metadata_ = show; }
    void set_image_size(const QString &size) { image_row_span_ = image_row_span(size); }
    void set_show_shortcuts(bool show) { show_shortcuts_ = show; }
    // The direct pick keys, labelled on the first rows in order.
    void set_pick_keys(const QString &keys) { shortcut_labels_ = labels_for(keys); }
    void set_hide_file_icon(bool hide) { hide_file_icon_ = hide; }
    void set_highlighted_id(std::optional<qint64> item_id) { highlighted_id_ = item_id; }
    std::optional<qint64> highlighted_id() const { return highlighted_id_; }
    void set_marked_ids(const QList<qint64> &item_ids);
    static void clear_thumb_cache();

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    struct Colors {
        QColor bg_even, bg_odd, bg_selected, border_bottom, border_selected, text_primary, text_tertiary, shortcut_text;
    };
    static QStringList labels_for(const QString &keys);
    int font_size() const;
    void rebuild_color_cache();
    void rebuild_font_cache();
    static std::optional<QPixmap> get_thumbnail(const QString &data_url);

    Theme theme_;
    int display_lines_;
    int window_opacity_;
    bool show_metadata_;
    int line_height_padding_;
    bool show_shortcuts_;
    QStringList shortcut_labels_;
    bool hide_file_icon_ = false;
    std::optional<qint64> highlighted_id_;
    QSet<qint64> marked_ids_;
    double image_row_span_;
    Colors colors_;
    QFont shortcut_font_, icon_font_, content_font_, meta_font_;
    QFontMetrics shortcut_fm_{QFont()};
    QFontMetrics content_fm_{QFont()};
};
}
