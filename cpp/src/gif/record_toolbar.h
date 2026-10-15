#pragma once
#include "gif/video_settings.h"
#include <QColor>
#include <QFont>
#include <QMap>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QWidget>
#include <optional>
#include <vector>

class QDialog;
class QHBoxLayout;
class QInputDialog;
class QLabel;
class QMenu;
class QPushButton;

// The recording toolbar: frame rate, record, pause, drawing tools, undo / redo, drag and close.
// Drawing tool buttons sit on the toolbar itself; a click opens that tool's settings panel.
namespace jietuba {
class ArrowSettingsPanel;
class ClickMenuButton;
class PaintSettingsPanel;
class ShapeSettingsPanel;
class TextSettingsPanel;
class ToolController;
class ToolSettingsManager;

class RecordToolbar : public QWidget {
    Q_OBJECT
public:
    static constexpr int BASE_HEIGHT = 48;
    static constexpr int BASE_BTN = 32;
    static constexpr int BASE_ICON = 20;
    static constexpr int BASE_SEP_HEIGHT = 24;
    static constexpr int BASE_HANDLE_WIDTH = 28;
    static constexpr int BASE_HANDLE_ICON = 18;
    explicit RecordToolbar(QWidget *parent = nullptr);
    ~RecordToolbar() override;

    // Recomputes the toolbar and panel sizes; recording state and tool choice stay.
    void apply_scale();
    // The recording rectangle, for panel placement.
    void set_record_rect(const QRect &rect) { record_rect_ = rect; }
    // Whether the toolbar sits below the recording area; settings panels reuse it.
    void set_toolbar_side(bool below);
    // Repositions every visible panel (after the window moves).
    void reposition_panels();
    // The tallest settings panel plus its gap, reserved when placing the toolbar.
    int get_max_panel_height() const;
    // External highlight (for example when Esc leaves drawing).
    void highlight_tool(const std::optional<QString> &tool_id);
    // Syncs the current colour and width from the tool controller into the panels.
    void sync_from_controller(const ToolController &tool_controller);
    // Sets the stroke width shown by every panel without emitting.
    void set_stroke_width(int width);
    // Sets the opacity shown by every panel without emitting.
    void set_opacity(int opacity_255);
    void set_elapsed(double seconds);
    // Restores the initial state for re-recording.
    void reset_state();
    int get_current_fps() const { return mode_ == "mp4" ? video_options_.fps : gif_fps_; }
    QString get_recording_mode() const { return mode_; }
    RecordingOptions get_video_options() const { return video_options_; }
    int get_start_delay() const { return start_delay_; }
    void show_video_result(const QString &path);
    void show_video_copied();
    // Startup, pause hand-over and muxing do not allow repeated clicks or encoder changes.
    void set_busy(const QString &text, bool allow_stop = false);
    void set_video_ready();
    void set_video_paused(bool paused);
    // Hides the toolbar and every panel.
    void hide();

    PaintSettingsPanel *paint_panel = nullptr;
    ShapeSettingsPanel *shape_panel = nullptr;
    ArrowSettingsPanel *arrow_panel = nullptr;
    TextSettingsPanel *text_panel = nullptr;
    // The canvas view's wheel handling reaches the text panel under this name.
    TextSettingsPanel *text_menu = nullptr;
signals:
    void fps_changed(int fps);
    void mode_changed(const QString &mode);
    void record_start();
    void record_stop();
    void pause_toggled(bool paused);
    void move_requested(const QPoint &delta);
    void drag_ended();
    void close_requested();
    void video_open_requested();
    void video_folder_requested();
    void video_copy_requested();
    void video_rerecord_requested();
    void tool_selected(const QString &tool_id);
    void undo_requested();
    void redo_requested();
    void color_changed(const QColor &color);
    void width_changed(int width);
    // 0-255.
    void opacity_changed(int opacity);
    // Leave drawing (back to pass-through).
    void deactivate_requested();
    // single / double / bar.
    void arrow_style_changed(const QString &style);
    // solid / dashed / dashed_dense.
    void line_style_changed(const QString &style);
    void font_changed(const QFont &font);
    void background_changed(bool enabled, const QColor &color, int opacity);
    void outline_changed(bool enabled, const QColor &color, double width);
    // The colour's alpha is the opacity.
    void shadow_changed(bool enabled, const QColor &color);
protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QString time_label_qss() const;
    std::vector<QWidget *> unique_panels() const;
    // Sets the icon and remembers its SVG: icons are rasterised per size and redrawn on scale changes.
    void set_button_icon(QPushButton *button, const QString &svg);
    void apply_scale_sizes();
    void build_ui();
    // The drawing tools' settings panels, shared with the screenshot UI.
    void init_settings_panels();
    void load_saved_settings();
    void on_arrow_style_changed(const QString &style);
    void on_line_style_changed(const QString &style);
    void on_text_font_changed(const QFont &font);
    void on_text_background_changed(bool enabled, const QColor &color, int opacity);
    void on_text_outline_changed(bool enabled, const QColor &color, double width);
    void on_text_shadow_changed(bool enabled, const QColor &color);
    // Places the panel away from the drawing area: below the toolbar when the toolbar is
    // below the recording area, above when above; flips when the preferred side does not fit.
    void position_panel(QWidget *panel);
    void show_panel_for_tool(const QString &tool_id);
    void hide_all_panels();
    void on_tool_clicked(const QString &tool_id);
    void update_tool_checked();
    void close_option_menu();
    void popup_options(QMenu *menu, QWidget *anchor);
    void show_delay_menu();
    void set_start_delay(int seconds);
    void show_audio_menu();
    void save_quick_option(std::optional<bool> system_audio, std::optional<bool> cursor);
    void on_mode_selected(const QString &mode);
    void refresh_recording_options();
    // Only saves encoder parameters; the recorder neither previews nor caches MP4 frames.
    void show_video_settings();
    void on_record_clicked();
    void on_pause_clicked();
    void on_fps_selected(const QVariant &fps);

    bool recording_ = false;
    bool paused_ = false;
    std::optional<QPoint> drag_pos_;
    std::optional<QString> current_tool_;
    std::vector<std::pair<QString, QPushButton *>> tool_buttons_;
    QRect record_rect_;
    bool toolbar_below_record_rect_ = true;
    ToolSettingsManager *config_;
    QString mode_;
    RecordingOptions video_options_;
    QPointer<QDialog> video_dialog_;
    QPointer<QInputDialog> fps_dialog_;
    QPointer<QMenu> option_menu_;
    int start_delay_ = 0;
    int gif_fps_ = 16;
    std::vector<std::pair<QString, QVariant>> gif_fps_pairs_;
    QMap<QString, QWidget *> panel_map_;

    QWidget *container_ = nullptr;
    QWidget *result_container_ = nullptr;
    QHBoxLayout *row_layout_ = nullptr;
    QHBoxLayout *result_layout_ = nullptr;
    std::vector<std::pair<QPushButton *, QString>> button_svgs_;
    std::vector<QWidget *> separators_;
    QPushButton *time_label_ = nullptr;
    ClickMenuButton *mode_btn_ = nullptr;
    ClickMenuButton *fps_btn_ = nullptr;
    QPushButton *video_settings_btn_ = nullptr;
    QPushButton *record_btn_ = nullptr;
    QPushButton *pause_btn_ = nullptr;
    QPushButton *audio_btn_ = nullptr;
    QPushButton *cursor_btn_ = nullptr;
    QPushButton *undo_btn_ = nullptr;
    QPushButton *redo_btn_ = nullptr;
    QLabel *move_handle_ = nullptr;
    QPushButton *close_btn_ = nullptr;
    QLabel *result_label_ = nullptr;
    std::vector<QPushButton *> result_buttons_;
};
}
