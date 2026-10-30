#pragma once
#include "canvas/canvas_host.h"
#include "core/shortcut_manager.h"
#include "tools/action.h"
#include "ui/selection_info/controller.h"
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QWidget>
#include <functional>
#include <memory>

namespace jietuba {
class CanvasScene;
class CanvasView;
class InitialSelectionGate;
class MagnifierOverlay;
class MaskOverlayWidget;
class SelectionInfoPanel;
class SelectionOverlayWidget;
class SystemCursor;
class Toolbar;
class ToolSettingsManager;
class ScreenshotWindow;

// Capture window shortcuts.
class ScreenshotShortcutHandler : public ShortcutHandler {
public:
    explicit ScreenshotShortcutHandler(ScreenshotWindow *window);
    QString handler_name() const override { return "ScreenshotWindow"; }
    // The capture window and the dialogs it parents; other windows shown over the capture keep their keys.
    bool owns_window(const QWidget *candidate) const override;
    // The manager filters before dialogs get keys, so a dialog blocking the capture
    // window (colour, save, toolbar layout) keeps the keyboard: Esc there must not end
    // the capture and Enter must not confirm it.
    bool is_active() override;
    // A global pin hotkey pressed during a capture acts like the in-app pin key;
    // RegisterHotKey eats the key before the window sees it. Unlike the in-app key it
    // also pins while editing text: handle_pin leaves editing first.
    void pin_from_global_hotkey();
    // The middle button runs through the same chain as keys.
    bool handle_mouse(QMouseEvent *event) override;
    // Key-only branches (text editing passthrough, Esc, cursor nudge, the fixed C and
    // Enter) see Key_unknown for middle-button events, so only binding branches apply.
    bool handle_key(QInputEvent *event) override;
    ScreenshotWindow *window;
private:
    bool match(QInputEvent *event, const QString &cfg_key) const;
    bool pin_if_confirmed();
    // Restores the last capture region, only with no drawing tool active. The region is
    // stored in absolute virtual desktop (scene) coordinates; outside the current
    // desktop (monitors rearranged) it quietly does nothing.
    bool restore_last_region();
    std::vector<std::pair<QString, QString>> tool_shortcuts_;  // (config key, tool id)
    QHash<QString, KeyBinding> bindings_;
    QHash<QString, MouseBinding> mouse_bindings_;
    QHash<int, std::pair<int, int>> move_keys_;
};

// The capture hub: canvas, toolbar, selection panels and mask. One window per capture:
// ending the capture deletes it.
class ScreenshotWindow : public QWidget, public CanvasHost, public SelectionInfoHost, public ActionHost {
    Q_OBJECT
public:
    explicit ScreenshotWindow(ToolSettingsManager *config_manager = nullptr, const QImage &prefetched_image = QImage(),
                              const std::optional<QRectF> &prefetched_rect = std::nullopt, std::shared_ptr<SystemCursor> prefetched_cursor = nullptr);
    ~ScreenshotWindow() override;
    // Ends the capture: releases the scene and image now and deletes the window later.
    void cleanup_and_close() override;
    QObject *hand_over() override;
    // The capture is open (from construction until cleanup_and_close, or until handed over).
    bool session_active() const { return session_active_ && !handover_; }
    void update_toolbar_position() override;
    void pin_from_global_hotkey();
    bool is_text_editing() const;
    // The text being typed, in screen coordinates; empty when none.
    QRect active_text_screen_rect() const;
    // The capture action whose mouse binding matches this event and gesture.
    std::optional<QString> matching_capture_mouse_action(QInputEvent *event, const QString &gesture) const;
    void start_gif_record_mode();
    void start_long_screenshot_mode();

    // CanvasHost
    Toolbar *canvas_toolbar() const override { return toolbar; }
    MagnifierOverlay *canvas_magnifier_overlay() const override { return magnifier_overlay; }
    bool has_capture_right_click_handler() const override { return true; }
    void handle_capture_right_click(QMouseEvent *event) override;
    std::optional<bool> matches_capture_double_click(QMouseEvent *event) const override;
    bool has_double_click_handler() const override { return true; }
    bool handle_double_click(QMouseEvent *event) override;
    bool has_confirm_handler() const override { return true; }
    void handle_confirm() override;
    CanvasView *canvas_view() const override { return view; }

    // SelectionInfoHost
    bool selection_info_closing() const override { return is_closing; }
    void set_exclude_from_capture(bool exclude) override;
    std::shared_ptr<SystemCursor> capture_cursor() const override { return capture_cursor_; }
    void replace_background(const QImage &image) override;

    ToolSettingsManager *config_manager;
    bool is_closing = false;
    QImage original_image;
    double virtual_x = 0, virtual_y = 0, virtual_width = 0, virtual_height = 0;
    CanvasScene *scene = nullptr;
    CanvasView *view = nullptr;
    QString smart_selection_mode = "off";
    bool smart_selection_enabled = false;
    Toolbar *toolbar = nullptr;
    std::unique_ptr<ActionTools> action_handler;
    MaskOverlayWidget *mask_overlay = nullptr;
    SelectionOverlayWidget *selection_overlay = nullptr;
    SelectionInfoPanel *info_panel = nullptr;
    std::unique_ptr<SelectionInfoController> info_controller;
    MagnifierOverlay *magnifier_overlay = nullptr;
signals:
    void session_ended();

protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    // Routes through cleanup_and_close unless that already ran.
    void closeEvent(QCloseEvent *event) override;

private:
    CanvasView *create_canvas_view(CanvasScene *scene);
    bool has_capture_mouse_gesture(const QString &gesture) const;
    // Old configuration objects without the mode setting fall back to window level, the default.
    QString configured_smart_selection_mode() const;
    void begin_session_ui();
    void show_prepared_session(CanvasView *prepared_view);
    void teardown_session();
    void release_session_resources();
    // Toolbar -> this window. Action buttons go through wrappers, since the action
    // handler is released when the capture ends.
    void connect_toolbar_signals();
    // Scene -> this window and toolbar -> the view's controller.
    void connect_scene_signals();
    void safe_activate_and_focus();
    void on_selection_dragging_changed(bool is_dragging);
    void on_selection_confirmed();
    void init_smart_selection_at_cursor();
    void init_magnifier_at_cursor();
    void on_tool_changed(const QString &tool_id);
    void on_watermark_requested();
    void on_watermark_style_changed(const QString &key, const QVariant &value);
    void on_watermark_text_changed(const QString &template_text);
    // Not undoable: a watermark present from the start must not go with the first Ctrl+Z.
    void add_auto_watermark();
    // Leaving without a selection is not a capture and changes nothing remembered.
    void remember_watermark();
    void on_number_next_changed(int next_value);
    void on_number_style_changed(const QString &style);
    void on_color_changed(const QColor &color);
    void on_stroke_width_changed(int width);
    void on_opacity_changed(int opacity_int);
    void on_arrow_style_changed(const QString &style);
    void on_mosaic_style_changed(const QString &style);
    void on_mosaic_block_size_changed(int block_size);
    void on_line_style_changed(const QString &style);
    QVariantMap capture_arrow_state(QGraphicsItem *item) const;
    void on_undo();
    void on_redo();

    bool session_active_ = false;
    // Set by hand_over; deleted with the scene.
    QObject *handover_ = nullptr;
    bool fullscreen_crosshair_ = false;
    InitialSelectionGate *initial_selection_gate_ = nullptr;
    std::shared_ptr<SystemCursor> capture_cursor_;
    std::unique_ptr<ScreenshotShortcutHandler> shortcut_handler_;
};
}
