#pragma once
#include "clipboard/core/models.h"
#include <QLabel>
#include <QPoint>
#include <QRect>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <optional>

namespace jietuba {
class TextEdit;
}

namespace jietuba::clipboard {
class ClipboardManager;

// Top-left of a floating panel beside avoid_rect (usually the clipboard window), kept on
// screen. prefer_side "left" / "right" / "auto" (right first); the other side when the
// preferred one is too small, the roomier side when neither fits. Without avoid_rect
// the panel goes right of pos, or left when that does not fit.
QPoint side_position(const QSize &size, const QPoint &pos, const std::optional<QRect> &avoid_rect = std::nullopt, const QString &prefer_side = "auto",
                     std::optional<int> gap = std::nullopt);

// Hover preview of an item: full text, file list or the original image. One instance.
class PreviewPopup : public QWidget {
    Q_OBJECT
public:
    static PreviewPopup *instance();
    // Follows the toolbar and panel scale; a visible preview picks it up when shown again.
    void apply_scale();
    void set_manager(ClipboardManager *manager) { manager_ = manager; }
    // Disabled also drops anything shown or pending.
    void set_display_enabled(bool enabled);
    // delay_ms 0 shows at once.
    void show_preview(const std::optional<ClipboardItem> &item, const QPoint &pos, int delay_ms = 5, const QString &prefer_side = "auto",
                      const std::optional<QRect> &avoid_rect = std::nullopt);
    void hide_preview();
    // Stops everything when the clipboard window closes.
    void force_cleanup();

    QLabel *title_label = nullptr;
    TextEdit *content_widget = nullptr;
    QLabel *image_label = nullptr;
    QLabel *color_label = nullptr;

private:
    PreviewPopup();
    void setup_ui();
    void do_show();
    void show_text_preview(const ClipboardItem &item);
    void show_file_preview(const ClipboardItem &item);
    // False when neither the stored image nor the thumbnail can be shown.
    bool show_image_preview(const ClipboardItem &item);
    void adjust_content_size();
    void clear_pending();

    QVBoxLayout *layout_ = nullptr;
    ClipboardManager *manager_ = nullptr;
    std::optional<qint64> current_item_id_;
    // A parentless top-level: late key or hover events can still arrive after the
    // clipboard window hides, so showing is gated instead of relying on one hide().
    bool display_enabled_ = false;
    QTimer show_timer_;
    std::optional<ClipboardItem> pending_item_;
    QPoint pending_pos_;
    QString pending_prefer_side_ = "auto";
    std::optional<QRect> pending_avoid_rect_;
};
}
