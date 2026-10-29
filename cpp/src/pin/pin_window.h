#pragma once
#include "core/drag_out.h"
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
struct AnimationSource;
class PinAnimation;
class PinBorderOverlay;
class PinCanvas;
class PinCanvasView;
class PinClickThroughBar;
class PinDisplayScaler;
class PinFileCard;
class PinFileSelection;
class PinTextContent;
class PinTextMode;
class PinToolbar;
class ToolSettingsManager;
class WaitBar;

// Wheel and settings never take opacity below this, so a pin cannot vanish.
constexpr double MIN_OPACITY = 0.15;
constexpr double MAX_SCALE = 4.0;
// The shorter window side never shrinks below this many pixels.
constexpr int MIN_SIDE = 30;
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

    // Null until recognition has started.
    PinTextSelection *text_selection() const { return ocr_mgr_ ? ocr_mgr_->text_selection() : nullptr; }
    PinOCRManager *ocr_manager() const { return ocr_mgr_.get(); }
    bool ocr_has_result() const { return ocr_mgr_ && ocr_mgr_->has_result(); }
    bool text_selection_enabled() const { return ocr_mgr_ && ocr_mgr_->text_selection_enabled(); }
    bool thumbnail_mode() const { return thumbnail_ && thumbnail_->active(); }
    // Shown as editable text instead of the image.
    bool text_mode() const;
    // Pinned from text: its image is the text drawn on a page, drawn afresh at each zoom.
    bool text_only() const { return text_content_ != nullptr; }
    PinTextContent *text_content() const { return text_content_.get(); }
    PinTextMode *text_mode_controller() const { return text_mode_.get(); }
    bool is_closed() const { return is_closed_; }
    bool is_dragging() const { return is_dragging_; }
    bool click_through() const { return click_through_; }
    bool locked() const { return locked_; }
    // The pin or its toolbar is the active window: keyboard shortcuts go to this pin.
    bool has_keyboard_focus() const;
    // Lock colour, else the theme colour with keyboard focus and grey without.
    QColor frame_color() const;
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
    void toggle_text_mode();
    void leave_text_mode();
    // In text mode only: the text takes the keyboard.
    void edit_text();
    // Makes a pin made with content's first image a text pin: its text is selectable from the start,
    // without recognizing it.
    void set_text_content(std::unique_ptr<PinTextContent> content);
    // Shows or hides a text pin's formatting; the pin keeps its zoom and fits the text laid out anew,
    // once that is done. Until then text_formatted() tells the state asked for.
    void toggle_text_formatting();
    bool text_formatted() const;
    // Plays source over the first frame the pin was made from.
    void open_animation(AnimationSource source);
    PinAnimation *animation() const { return animation_; }
    // A frame of the animation in place of the image.
    void show_animation_frame(const QImage &frame);
    // The pin was made from card's image, which it draws afresh at each zoom; its menu acts on the files.
    void open_files(const PinFileCard &card);
    const PinFileCard *file_card() const { return file_card_.get(); }
    // The file whose card is under global_pos, -1 elsewhere.
    int file_at(const QPoint &global_pos) const;
    PinFileSelection *file_selection() const { return file_selection_.get(); }
    // A click that stayed where it was pressed, on a file card.
    void click_file(const QPoint &global_pos, Qt::KeyboardModifiers modifiers);
    // A double click: opens the file under global_pos; false off the cards.
    bool open_file_at(const QPoint &global_pos);
    // Annotations and the toolbar: not on an animation, whose frames would replace what is drawn under them.
    bool annotatable() const { return !animation_; }
    // Text recognition, text selection, text mode and translation: not for an animation or a file list.
    bool recognizable() const { return !animation_ && !file_card_; }
    // Annotations stay in the scene, covered by the image; drawing shows them again.
    bool annotations_hidden() const { return annotations_hidden_; }
    void set_annotations_hidden(bool hidden);
    // With a hint, as the shortcut and menu do.
    void toggle_annotations_hidden();
    // The image as the view shows it, without annotations; the painter is in image pixels.
    void paint_background(QPainter *painter) const;
    // Leaves the drawing tool, unchecking it on the toolbar.
    void stop_drawing();
    // Top-left hint shared by zoom, opacity and other short messages.
    void show_hint_label(const QString &text, int duration_ms = 1000);
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
    // Maps the pixels of a get_current_image() result of that size to the image's pixels,
    // undoing the pixel ratio, rotation and flips.
    QTransform from_current_image(const QSize &size) const;
    // Leaves editing for func and restores the tool afterwards.
    void with_edit_paused(const std::function<void()> &func);
    // Calls then once smart erase on the pin has its pixels, right away when it has; never after the pin is gone.
    void when_pixels_ready(std::function<void()> then);
    void save_image();
    void save_image_with_system_dialog();
    // A text pin copies its selected text, or all of it.
    void copy_to_clipboard();
    void copy_image();
    // A text pin's text without its formatting.
    void copy_plain_text();
    // The animation as a file, with its bytes for programs that paste data.
    void copy_animation();
    void copy_current_frame();
    // All recognized text; recognizes first when there is no result yet.
    void copy_all_text();
    // What dragging the content out from global_pos carries: what Copy would copy, but the highlighted text when the
    // drag starts on it, and on a file card the selected files when it starts on one of them, another file alone, or,
    // off the cards, every file. The preview shows what goes, held at the point it was grabbed.
    struct DragOut {
        DragOutContent content;
        QPixmap preview;
        QPoint hot_spot;
    };
    DragOut prepare_drag_out(const QPoint &global_pos);
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
    void start_drag_out(const QPoint &global_pos);
    // The view's scene_rect, or all of it when empty, shrunk to fit the preview size.
    QPixmap drag_preview(const QPoint &global_pos, const QRectF &scene_rect, QPoint *hot_spot);
    void show_zoom_percent();
    void start_ocr_for_translation();
    void on_ocr_translation_finished(bool success, const QString &result);
    // With html the clipboard gets the formatted text as well.
    void copy_ocr_text(bool success, const QString &result, const QString &html = {});
    void set_window_opacity(double opacity);
    // The animation's own file, in its format.
    void save_animation();
    // The current frame at its own size, scaled by the view as it draws.
    void show_frame_unscaled();
    // Stepping to a frame the animation reads up to again: a bar of how far, once it takes a while.
    void show_seek_progress(int done, int total);
    void place_seek_bar();
    // A new image of another size, keeping the zoom, the top-left corner and the annotations where they
    // are; pixmap is the image made into one already, when it is not null.
    void replace_base_image(const QImage &image, const QPixmap &pixmap = {});
    void relay_out_text();
    void draw_text_page_on_demand();
    // The background as exported, at the original's resolution; null when what is shown is that already.
    QPixmap original_background(double device_pixel_ratio) const;

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
        bool drag_out;
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
    // Counts the backgrounds shown, so an image scaled for an earlier one is dropped.
    quint64 display_ticket_ = 0;
    std::shared_ptr<PinDisplayScaler> display_scaler_;
    double win_opacity_;
    QLabel *zoom_label_ = nullptr;
    QTimer *zoom_hide_timer_ = nullptr;
    bool cross_tool_selection_enabled_ = false;
    std::unique_ptr<PinControlButtons> control_buttons_;
    std::unique_ptr<PinContextMenu> context_menu_;
    std::unique_ptr<PinTranslationHelper> translation_helper_;
    std::unique_ptr<PinOCRManager> ocr_mgr_;
    std::unique_ptr<PinThumbnailMode> thumbnail_;
    std::unique_ptr<PinTextMode> text_mode_;
    std::unique_ptr<PinTextContent> text_content_;
    // Counts the contents shown, so a page drawn for an earlier one is dropped.
    quint64 text_generation_ = 0;
    std::optional<bool> wanted_formatting_;
    bool formatting_pending_ = false;
    std::unique_ptr<PinImageTransform> image_transform_;
    bool annotations_hidden_ = false;
    PinAnimation *animation_ = nullptr;
    WaitBar *seek_bar_ = nullptr;
    std::unique_ptr<PinFileCard> file_card_;
    std::unique_ptr<PinFileSelection> file_selection_;
};
}
