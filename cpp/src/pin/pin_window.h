#pragma once
#include "pin/pin_context_menu.h"
#include "pin/pin_controls.h"
#include "pin/pin_hover.h"
#include "pin/pin_image_transform.h"
#include "pin/pin_ocr_manager.h"
#include "pin/pin_thumbnail.h"
#include "pin/pin_translation.h"
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPointer>
#include <QRubberBand>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>

class QGraphicsItem;
class QInputEvent;

namespace jietuba {
class PinBorderOverlay;
class PinCanvas;
class PinCanvasView;
class PinClickThroughBar;
class PinToolbar;
class ToolSettingsManager;

// Wheel and settings never take opacity below this, so a pin cannot vanish.
constexpr double MIN_OPACITY = 0.15;
constexpr double MAX_SCALE = 4.0;
// The shorter window side never shrinks below this many pixels.
constexpr int MIN_SIDE = 50;
// An image larger than its screen is pinned at this share of the screen, leaving a margin to grab.
constexpr double AUTO_SHRINK_RATIO = 0.95;
// A band this wide (at 96 DPI) along the edges resizes instead of moving.
constexpr int RESIZE_MARGIN = 6;

// A pin: a frameless topmost image window that can be dragged, zoomed and edited.
// The window manages geometry and children only; PinCanvasView renders, PinCanvas
// routes tools, and the helpers own OCR, thumbnails, buttons, hover, menu and translation.
class PinWindow : public QWidget {
    Q_OBJECT
public:
    // image is the clean selection background; drawing_items are the capture's
    // annotations (vector data), moved by selection_offset; number_next syncs the counter.
    PinWindow(const QImage &image, const QPoint &position, ToolSettingsManager *config_manager, const QList<QGraphicsItem *> &drawing_items = {},
              const std::optional<QPoint> &selection_offset = std::nullopt, std::optional<int> number_next = std::nullopt);
    ~PinWindow() override;

    OCRTextLayer *ocr_text_layer() const { return ocr_mgr_ ? ocr_mgr_->ocr_text_layer : nullptr; }
    PinOCRManager *ocr_manager() const { return ocr_mgr_.get(); }
    bool ocr_has_result() const { return ocr_mgr_ && ocr_mgr_->has_result(); }
    bool text_selection_enabled() const { return ocr_mgr_ && ocr_mgr_->text_selection_enabled(); }
    bool thumbnail_mode() const { return thumbnail_ && thumbnail_->active(); }
    bool is_closed() const { return is_closed_; }
    bool is_dragging() const { return is_dragging_; }
    bool click_through() const { return click_through_; }
    bool locked() const { return locked_; }
    QSize orig_size() const { return orig_size_; }
    const QPixmap &base_pixmap() const { return base_pixmap_; }
    PinControlButtons *control_buttons() const { return control_buttons_.get(); }
    QPushButton *close_button() const { return control_buttons_ ? control_buttons_->close_button : nullptr; }
    QPushButton *toolbar_toggle_button() const { return control_buttons_ ? control_buttons_->toolbar_toggle_button : nullptr; }

    void update_button_positions();
    // Children created later stack above and would cover the top-right buttons; called after creating them.
    void raise_control_buttons();
    // Applies saved appearance settings to an open pin.
    void refresh_appearance();
    void start_window_drag(const QPoint &global_pos);
    void update_window_drag(const QPoint &global_pos);
    void end_window_drag();
    void update_display();
    void update_view_transform();
    void refresh_background_for_scale();
    // The whole window.
    QRectF content_rect() const { return QRectF(rect()); }
    // +, = and - adjust what the mouse wheel bindings would, for keyboards.
    bool handle_mouse_alternative_key(QInputEvent *event);
    void toggle_thumbnail_mode();
    void toggle_toolbar();
    // Only PinHoverControls::sync calls these two.
    void show_toolbar();
    void hide_toolbar();
    void on_translate_clicked();
    void show_context_menu(const QPoint &global_pos);
    void toggle_stay_on_top();
    void toggle_lock();
    // Locked pins block, per settings, moving and scaling, mouse closing and key closing;
    // the context menu stays available and unlocks too.
    void set_locked(bool locked);
    // Whether locking blocks this kind: move (with zoom and resize), click_close, key_close.
    bool lock_blocks(const QString &kind) const;
    void show_locked_hint();
    // Shortcuts, mouse gestures and the hover close button; the context menu's Close is
    // an explicit action and closes even when locked.
    void close_unless_locked(const QString &kind = "key_close");
    void toggle_click_through();
    // Clicks and wheel go to the window below; moving, opacity and leaving go through the bar above.
    void set_click_through(bool enabled);
    // The click-through bar's slider; kept on this pin for the next click-through.
    void set_click_through_opacity(double opacity);
    void toggle_border_effect();
    void toggle_text_selection();
    // Back to 100% of the original, keeping rotation and flips and the top-left corner.
    void reset_to_original_size();
    void rotate_image_cw();
    void rotate_image_ccw();
    void flip_image_horizontal();
    void flip_image_vertical();
    void reset_image_transform();
    QImage get_current_image();
    // Leaves editing for func and restores the tool afterwards.
    void with_edit_paused(const std::function<void()> &func);
    void save_image();
    void copy_to_clipboard();
    // All recognized text; recognizes first when there is no result yet.
    void copy_all_text();
    void close_window();
    void cleanup();

    ToolSettingsManager *config_manager;
    QList<QGraphicsItem *> drawing_items;
    QPoint selection_offset;
    bool border_enabled;
    int corner = 0;
    int border_width = 2;
    QColor border_color;
    bool is_editing = false;
    double scale_factor = 1.0;
    PinCanvas *canvas = nullptr;
    QPointer<PinCanvasView> view;
    PinToolbar *toolbar = nullptr;
    std::unique_ptr<PinHoverControls> hover_controls;
    PinBorderOverlay *border_overlay = nullptr;
signals:
    void closed();

protected:
    void showEvent(QShowEvent *event) override;
    // The click-through bar is a separate window and does not hide with the pin.
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    // The view is the only renderer.
    void paintEvent(QPaintEvent *) override {}
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void setup_control_buttons();
    QVariant pin_setting(const QString &key) const;
    double opacity_setting(const QString &key) const;
    // Images larger than their screen shrink to 95% of it, centred; others keep their size at position.
    QRect initial_geometry(const QPoint &position, const QSize &size);
    void apply_window_corners();
    // The edge or corner hot zone under pos as (sx, sy) in -1/0/1; nullopt when resizing is not possible.
    std::optional<std::pair<int, int>> resize_edge_at(const QPoint &pos);
    // Cursor changes and edge dragging; true when the event was handled.
    bool handle_border_resize(QObject *watched, QEvent *event);
    void show_resize_cursor(QWidget *widget, const std::optional<std::pair<int, int>> &edge);
    // Keeps the aspect ratio; the edge (corner) opposite the dragged one stays put.
    void update_border_resize(const QPoint &global_pos);
    void end_border_resize() { resize_edge_.reset(); }
    void sync_click_through_bar();
    void adjust_mouse_value(const QString &action, int delta);
    // The 100% window size; width and height swap after 90/270 degree rotation.
    QSize display_base_size() const;
    // Scales to scale with the window point at fraction (fx, fy) held at anchor (default: where it is now).
    void scale_around(double scale, double fx, double fy, std::optional<QPointF> anchor = std::nullopt);
    bool blocked_by_modal() const;
    std::optional<QString> matching_mouse_action(QMouseEvent *event, const QString &gesture);
    void run_mouse_action(const QString &action, const std::optional<QPoint> &position = std::nullopt);
    void finish_mouse_click();
    bool handle_mouse_gesture(QMouseEvent *event);
    void apply_smooth_scaling();
    void show_zoom_percent();
    // Top-left hint shared by zoom and opacity percentages.
    void show_hint_label(const QString &text);
    void start_ocr_for_translation();
    void on_ocr_translation_finished(bool success, const QString &result);
    void copy_ocr_text(bool success, const QString &result);
    void set_window_opacity(double opacity);

    bool square_corners_ = false;
    bool is_closed_ = false;
    bool is_dragging_ = false;
    QPoint drag_start_pos_;
    QPoint drag_start_window_pos_;
    struct MouseGesture {
        Qt::MouseButton button;
        QPoint start;
        std::optional<QString> action;
        bool region;
    };
    std::optional<MouseGesture> mouse_gesture_;
    std::optional<Qt::MouseButton> mouse_swallow_release_;
    QRubberBand *mouse_band_ = nullptr;
    QTimer *mouse_click_timer_ = nullptr;
    std::optional<QString> pending_mouse_action_;
    std::optional<QPoint> pending_mouse_position_;
    bool locked_ = false;
    bool click_through_ = false;
    double click_through_opacity_;
    std::optional<double> opacity_before_click_through_;
    PinClickThroughBar *click_through_bar_ = nullptr;
    std::optional<std::pair<int, int>> resize_edge_;
    QPoint resize_start_pos_;
    QRect resize_start_geometry_;
    QPointer<QWidget> resize_cursor_owner_;
    QSize orig_size_;
    QPixmap base_pixmap_;
    double view_scale_x_ = 1.0, view_scale_y_ = 1.0;
    std::optional<std::pair<int, int>> last_background_scale_size_;
    QTimer *scale_timer_ = nullptr;
    bool is_scaling_ = false;
    double win_opacity_;
    QLabel *zoom_label_ = nullptr;
    QTimer *zoom_hide_timer_ = nullptr;
    bool cross_tool_selection_enabled_ = false;
    std::unique_ptr<PinControlButtons> control_buttons_;
    std::unique_ptr<PinContextMenu> context_menu_;
    std::unique_ptr<PinTranslationHelper> translation_helper_;
    std::unique_ptr<PinOCRManager> ocr_mgr_;
    std::unique_ptr<PinThumbnailMode> thumbnail_;
    std::unique_ptr<PinImageTransform> image_transform_;
};
}
