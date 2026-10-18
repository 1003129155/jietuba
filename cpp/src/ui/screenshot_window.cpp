#include "ui/screenshot_window.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/background_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/undo.h"
#include "canvas/view.h"
#include "capture/capture_service.h"
#include "capture/system_cursor.h"
#include "core/i18n.h"
#include "core/last_capture_region.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/qt_utils.h"
#include "core/ui_scale.h"
#include "pin/pin_manager.h"
#include "settings/tool_settings.h"
#include "stitch/scroll_window.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include "tools/mosaic.h"
#include "tools/number.h"
#include "tools/watermark.h"
#include "ui/dialogs.h"
#include "ui/initial_selection.h"
#include "ui/magnifier.h"
#include "ui/mask_overlay.h"
#include "ui/arrow_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/selection_info/panel.h"
#include "ui/selection_overlay.h"
#include "ui/toolbar.h"
#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QGraphicsPixmapItem>
#include "gif/record_window.h"
#include <QGraphicsTextItem>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>

namespace jietuba {
namespace {
QString window_tr(const char *text) { return i18n_tr(text, "ScreenshotWindow"); }

// Milliseconds since the timer started, as a "{:.1f}ms" timing entry.
QString timing_entry(const QString &name, double ms) { return QString("%1=%2ms").arg(name, python_fixed(ms, 1)); }
}

ScreenshotShortcutHandler::ScreenshotShortcutHandler(ScreenshotWindow *window) : window(window) {
    // Read once; in-app shortcuts do not change during a capture window's lifetime.
    QStringList bound_keys = {
        "inapp_confirm", "inapp_pin", "inapp_undo", "inapp_redo",
        "inapp_delete", "inapp_restore_last_region",
        "inapp_zoom_in", "inapp_zoom_out", "inapp_translate",
        "inapp_text_recognize",
    };
    for (const auto &entry : ANNOTATION_TOOL_SHORTCUTS()) {
        tool_shortcuts_.emplace_back(entry.config_key, entry.tool_id);
        bound_keys.append(entry.config_key);
    }
    bindings_ = load_inapp_bindings(bound_keys);
    mouse_bindings_ = load_inapp_mouse_bindings(bound_keys);
    move_keys_ = load_move_keys();
}

bool ScreenshotShortcutHandler::is_active() {
    auto *w = window;
    return w && !w->is_closing && w->isVisible() && blocking_modal(w) == nullptr;
}

bool ScreenshotShortcutHandler::match(QInputEvent *event, const QString &cfg_key) const { return match_inapp_binding(event, cfg_key, bindings_, mouse_bindings_); }

bool ScreenshotShortcutHandler::pin_if_confirmed() {
    auto *w = window;
    if (w->scene && w->scene->selection_model->is_confirmed()) {
        w->action_handler->handle_pin();
        return true;
    }
    return false;
}

void ScreenshotShortcutHandler::pin_from_global_hotkey() {
    if (is_active()) pin_if_confirmed();
}

bool ScreenshotShortcutHandler::handle_mouse(QMouseEvent *event) {
    auto *w = window;
    if (event->button() == Qt::MiddleButton) {
        auto *view = w->view;
        auto action = w->matching_capture_mouse_action(event, "middle");
        // While editing text only close applies, like Esc.
        if (action && *action != "close" && w->is_text_editing()) action.reset();
        if (action && view && view->viewport()->rect().contains(view->viewport()->mapFromGlobal(event->globalPosition().toPoint())) &&
            w->action_handler->handle_capture_action(*action))
            return true;
    }
    return handle_key(event);
}

bool ScreenshotShortcutHandler::handle_key(QInputEvent *event) {
    auto *w = window;
    const int key = event_key(event);
    if (w->view) w->view->invalidate_double_click_candidate();
    const bool is_text_editing = w->is_text_editing();
    // While editing text some keys belong to the text item.
    if (is_text_editing) {
        if (key == Qt::Key_Return || key == Qt::Key_Enter) return false;
        if (key == Qt::Key_C || key == Qt::Key_D) return false;
        if ((key == Qt::Key_Z || key == Qt::Key_Y) && event->modifiers() == Qt::ControlModifier) return false;
    }
    // Esc is fixed.
    if (key == Qt::Key_Escape) {
        w->cleanup_and_close();
        return true;
    }
    if (match(event, "inapp_restore_last_region") && restore_last_region()) return true;
    if (match(event, "inapp_confirm")) {
        if (w->scene && w->scene->selection_model->is_confirmed()) {
            w->action_handler->handle_confirm();
            return true;
        }
    }
    if (match(event, "inapp_pin") && pin_if_confirmed()) return true;
    if (match(event, "inapp_undo")) {
        if (w->scene && w->scene->undo_stack->canUndo()) w->scene->undo_stack->undo();
        return true;
    }
    if (match(event, "inapp_redo")) {
        if (w->scene && w->scene->undo_stack->canRedo()) w->scene->undo_stack->redo();
        return true;
    }
    // While editing text this key (Delete by default) deletes characters, so the event
    // is only consumed when it deletes the selected item.
    if (match(event, "inapp_delete")) {
        if (!is_text_editing) {
            if (w->view && w->view->smart_edit_controller) w->view->smart_edit_controller->delete_selected();
            return true;
        }
    }
    if (match(event, "inapp_translate")) {
        if (w->scene && w->scene->selection_model->is_confirmed()) {
            if (w->toolbar) emit w->toolbar->screenshot_translate_clicked();
            return true;
        }
    }
    if (match(event, "inapp_text_recognize")) {
        if (w->scene && w->scene->selection_model->is_confirmed()) {
            if (w->toolbar) emit w->toolbar->text_recognize_clicked();
            return true;
        }
    }
    // Magnifier zoom is a configurable capture action, ahead of tool keys.
    auto *mo = w->magnifier_overlay;
    if (match(event, "inapp_zoom_in")) {
        if (mo && mo->cursor_scene_pos && mo->should_render()) {
            mo->adjust_zoom(1);
            return true;
        }
    }
    if (match(event, "inapp_zoom_out")) {
        if (mo && mo->cursor_scene_pos && mo->should_render()) {
            mo->adjust_zoom(-1);
            return true;
        }
    }
    // Annotation tools once the selection is confirmed; auto-repeat is consumed without re-activating.
    if (!is_text_editing && w->scene && w->scene->selection_model->is_confirmed()) {
        for (const auto &[cfg_key, tool_id] : tool_shortcuts_) {
            if (!match(event, cfg_key)) continue;
            if (!event_is_auto_repeat(event) && w->toolbar) w->toolbar->select_tool(tool_id, false);
            return true;
        }
    }
    // Cursor nudge, after configured actions and tools.
    if (!is_text_editing && move_keys_.contains(key) && event->modifiers() == Qt::NoModifier) {
        const auto delta = move_keys_.value(key);
        const auto p = QCursor::pos();
        QCursor::setPos(p.x() + delta.first, p.y() + delta.second);
        return true;
    }
    // Colour format cycling on a lone Shift; fixed, paired with C.
    if (key == Qt::Key_Shift && !event_is_auto_repeat(event)) {
        if (mo && mo->cursor_scene_pos && mo->should_render()) {
            mo->cycle_color_format();
            return true;
        }
    }
    // Colour copy on a lone C; fixed.
    if (key == Qt::Key_C && event->modifiers() == Qt::NoModifier) {
        if (mo && mo->cursor_scene_pos && mo->should_render() && mo->copy_color_info()) {
            w->cleanup_and_close();
            return true;
        }
    }
    // Enter confirms (fixed); mouse action settings do not change keyboard behaviour.
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        if (w->scene && w->scene->selection_model->is_confirmed()) {
            if (!event_is_auto_repeat(event)) w->action_handler->handle_confirm();
            return true;
        }
    }
    return false;
}

bool ScreenshotShortcutHandler::restore_last_region() {
    auto *w = window;
    if (!w->scene) return false;
    if (w->scene->tool_controller->current_tool_id() != "cursor") return false;
    const auto absolute = get_last_region();
    if (!absolute) return false;
    // With a secondary screen left of or above the primary the desktop origin is negative.
    const QRect virtual_bounds(static_cast<int>(python_round(w->virtual_x)), static_cast<int>(python_round(w->virtual_y)),
                               static_cast<int>(python_round(w->virtual_width)), static_cast<int>(python_round(w->virtual_height)));
    if (!virtual_bounds.contains(*absolute)) return false;
    w->scene->preset_selection(QRectF(*absolute));
    return true;
}

ScreenshotWindow::ScreenshotWindow(ToolSettingsManager *config, const QImage &prefetched_image, const std::optional<QRectF> &prefetched_rect,
                                   std::shared_ptr<SystemCursor> prefetched_cursor)
    : QWidget(nullptr), config_manager(config ? config : get_tool_settings_manager()) {
    // One window per capture; cleanup_and_close() deletes it. The reference keeps one
    // window and rebinds a new scene to it each time, saving about 250 ms of building in
    // Python; built natively the window takes about 30 ms, so that rebinding is not carried over.
    QElapsedTimer clock;
    clock.start();
    QStringList timings;
    // 1. The screen image: prefetched on a background thread when available, else captured now.
    QRectF rect;
    if (!prefetched_image.isNull() && prefetched_rect) {
        original_image = prefetched_image;
        rect = *prefetched_rect;
        capture_cursor_ = std::move(prefetched_cursor);
        timings.append(timing_entry(QString::fromUtf8("截屏"), 0.0));
    } else {
        CaptureService capture_service;
        capture_cursor_ = config_manager->get_app_setting("capture_include_cursor", false).toBool() ? SystemCursor::grab() : nullptr;
        std::tie(original_image, rect) = capture_service.capture_all_screens(capture_cursor_);
        timings.append(timing_entry(QString::fromUtf8("截屏"), clock.nsecsElapsed() / 1e6));
    }
    const double t1 = clock.nsecsElapsed() / 1e6;
    virtual_x = rect.x();
    virtual_y = rect.y();
    virtual_width = rect.width();
    virtual_height = rect.height();
    log_debug(T("虚拟桌面: {width}x{height} at ({x}, {y})", {{"width", virtual_width}, {"height", virtual_height}, {"x", virtual_x}, {"y", virtual_y}}),
              "ScreenshotWindow");
    log_debug(T("图像尺寸: {width}x{height}", {{"width", original_image.width()}, {"height", original_image.height()}}), "ScreenshotWindow");

    // 2. Window flags and geometry, before any child widget.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setGeometry(static_cast<int>(virtual_x), static_cast<int>(virtual_y), static_cast<int>(virtual_width), static_cast<int>(virtual_height));

    // 3. Scene and view.
    scene = new CanvasScene(original_image, rect, true);
    view = create_canvas_view(scene);
    view->setGeometry(0, 0, static_cast<int>(virtual_width), static_cast<int>(virtual_height));
    const double t2 = clock.nsecsElapsed() / 1e6;
    timings.append(timing_entry("Scene+View", t2 - t1));
    smart_selection_mode = configured_smart_selection_mode();
    smart_selection_enabled = smart_selection_mode != "off";
    view->enable_smart_selection(smart_selection_mode);
    view->smart_selection_animated = config_manager->get_smart_selection_animation();

    // 4. Toolbar, shown after the selection is confirmed.
    toolbar = new Toolbar(this);
    toolbar->hide();
    const double t3 = clock.nsecsElapsed() / 1e6;
    timings.append(timing_entry("Toolbar", t3 - t2));

    // 5. Toolbar button logic.
    action_handler = std::make_unique<ActionTools>(scene, config_manager, this);

    // 6. The mask over the whole window, above the view.
    mask_overlay = new MaskOverlayWidget(this, scene->selection_model);
    mask_overlay->setGeometry(0, 0, static_cast<int>(virtual_width), static_cast<int>(virtual_height));
    // 6.1 Selection decoration above the mask, or frame and handles outside the selection get dimmed.
    selection_overlay = new SelectionOverlayWidget(this, scene->selection_item, scene->selection_model);
    selection_overlay->setGeometry(0, 0, static_cast<int>(virtual_width), static_cast<int>(virtual_height));
    selection_overlay->raise();
    view->set_fullscreen_crosshair(config_manager->get_app_setting("capture_fullscreen_crosshair", false).toBool(), mask_overlay);
    const double t4 = clock.nsecsElapsed() / 1e6;
    timings.append(timing_entry("Action+Mask", t4 - t3));

    // 6.5 Selection info panel: coordinates and size plus rounded corners, ratio lock and more.
    info_panel = new SelectionInfoPanel(this, view);
    info_controller = std::make_unique<SelectionInfoController>(info_panel, scene->selection_model, scene->selection_item, &action_handler->export_service,
                                                                mask_overlay, this, config_manager);
    const double t5 = clock.nsecsElapsed() / 1e6;
    timings.append(timing_entry("InfoPanel", t5 - t4));

    // 7. Pointer magnifier with colour information.
    magnifier_overlay = new MagnifierOverlay(this, scene, view, config_manager);
    update_toolbar_position();
    connect_toolbar_signals();
    connect_scene_signals();
    const double t6 = clock.nsecsElapsed() / 1e6;
    timings.append(timing_entry(QString::fromUtf8("Magnifier+信号"), t6 - t5));
    timings.append(timing_entry(QString::fromUtf8("总计"), t6));
    log_debug(T("[计时] 初始化完成 | {parts}", {{"parts", timings.join(" | ")}}), "ScreenshotWindow");
    begin_session_ui();
}

ScreenshotWindow::~ScreenshotWindow() {
    // Deleted with the capture still open (application exit): the scene is not a child
    // and pins wait to come back to the topmost band.
    try {
        teardown_session();
    } catch (const std::exception &e) {
        log_exception(e, T("销毁截图窗口"));
    }
}

void ScreenshotWindow::begin_session_ui() {
    // Pin windows stay out of the topmost band during the capture, so equal TopMost windows do not fight over Z order.
    PinManager::instance()->suppress_topmost();
    shortcut_handler_ = std::make_unique<ScreenshotShortcutHandler>(this);
    ShortcutManager::instance()->register_handler(shortcut_handler_.get());
    session_active_ = true;
    initial_selection_gate_ = new InitialSelectionGate(this);
    connect(initial_selection_gate_, &InitialSelectionGate::ready, this, &ScreenshotWindow::show_prepared_session);
    initial_selection_gate_->start(view);
}

void ScreenshotWindow::show_prepared_session(CanvasView *prepared_view) {
    if (is_closing || !session_active_ || view != prepared_view) return;
    show();
    activateWindow();
    raise();
    setFocus();
    // Windows focus stealing prevention can ignore the first activation.
    QTimer::singleShot(50, this, &ScreenshotWindow::safe_activate_and_focus);
}

CanvasView *ScreenshotWindow::create_canvas_view(CanvasScene *new_scene) {
    return new CanvasView(new_scene, this, has_capture_mouse_gesture("doubleleft"), config_manager->get_cross_tool_selection_enabled());
}

bool ScreenshotWindow::has_capture_mouse_gesture(const QString &gesture) const {
    for (const auto &entry : CAPTURE_MOUSE_ACTIONS())
        if (get_capture_mouse_binding(config_manager, entry.key).split('+').last() == gesture) return true;
    return false;
}

std::optional<QString> ScreenshotWindow::matching_capture_mouse_action(QInputEvent *event, const QString &gesture) const {
    for (const auto &entry : CAPTURE_MOUSE_ACTIONS())
        if (mouse_gesture_binding_matches(get_capture_mouse_binding(config_manager, entry.key), event, gesture)) return QString(entry.key);
    return std::nullopt;
}

std::optional<bool> ScreenshotWindow::matches_capture_double_click(QMouseEvent *event) const { return matching_capture_mouse_action(event, "doubleleft").has_value(); }

void ScreenshotWindow::handle_capture_right_click(QMouseEvent *event) {
    const auto action = matching_capture_mouse_action(event, "right");
    if (!action || !action_handler) return;
    // Like the middle button: only close applies while editing text.
    if (*action != "close" && is_text_editing()) return;
    action_handler->handle_capture_action(*action);
}

QString ScreenshotWindow::configured_smart_selection_mode() const {
    const auto mode = config_manager->get_smart_selection_mode().toLower();
    if (SMART_SELECTION_MODES.contains(mode)) return mode;
    return config_manager->get_smart_selection() ? "window" : "off";
}

void ScreenshotWindow::teardown_session() {
    if (!session_active_) return;
    session_active_ = false;
    is_closing = true;
    if (initial_selection_gate_) initial_selection_gate_->cancel();
    hide();
    try {
        release_session_resources();
    } catch (...) {
        // Copying may have hidden the window early; the input hub must hear the session
        // end even when cleanup fails, or quick capture stays blocked.
        emit session_ended();
        throw;
    }
    emit session_ended();
}

void ScreenshotWindow::release_session_resources() {
    remember_watermark();
    // Visible to capture APIs again.
    if (exclude_from_capture_set) {
        set_exclude_from_capture(false);
        exclude_from_capture_set = false;
    }
    if (shortcut_handler_) {
        ShortcutManager::instance()->unregister_handler(shortcut_handler_.get());
        shortcut_handler_->window = nullptr;
        // The handler may be the one dispatching this key (Esc), so it is destroyed only after the dispatch returns.
        QTimer::singleShot(0, qApp, [handler = std::shared_ptr<ScreenshotShortcutHandler>(shortcut_handler_.release())] {});
    }
    PinManager::instance()->restore_topmost();
    // The toolbar goes with this window; nothing it emits meanwhile may reach the released scene.
    if (toolbar) toolbar->blockSignals(true);
    original_image = QImage();
    capture_cursor_.reset();
    if (info_controller) {
        info_controller->cleanup();
        info_controller.reset();
    }
    // View and controller leave the scene before it is destroyed.
    if (view) view->cleanup();
    if (scene) {
        disconnect(scene, nullptr, this, nullptr);
        disconnect(scene->selection_model, nullptr, this, nullptr);
        if (scene->background) {
            scene->background->release_image_cache();
            scene->background->setPixmap(QPixmap());
        }
        scene->tool_controller.reset();
        scene->undo_stack->clear();
        // Large pixmaps go now; deleteLater destroys the rest.
        for (auto *item : scene->items())
            if (auto *pixmap_item = dynamic_cast<QGraphicsPixmapItem *>(item)) pixmap_item->setPixmap(QPixmap());
        scene->deleteLater();
        scene = nullptr;
    }
    if (view) {
        view->setScene(nullptr);
        view->setParent(nullptr);
        view->deleteLater();
        view = nullptr;
    }
    // action_handler stays until the window is deleted: the action ending the capture
    // (recognition, code scanning) is usually still running on it.
    log_info(T("截图会话资源释放完成"), "ScreenshotWindow");
}

void ScreenshotWindow::set_exclude_from_capture(bool exclude) {
    // WDA_EXCLUDEFROMCAPTURE (Windows 10 2004+): capture APIs skip this window while it stays visible on screen.
    const auto hwnd = static_cast<quintptr>(winId());
    if (set_window_exclude_from_capture(hwnd, exclude))
        log_debug(T("SetWindowDisplayAffinity({mode}) 成功", {{"mode", QString(exclude ? "EXCLUDE" : "NONE")}}), "ScreenshotWindow");
    else
        log_debug(T("SetWindowDisplayAffinity 失败, GetLastError={error_code}", {{"error_code", get_last_error()}}), "ScreenshotWindow");
}

void ScreenshotWindow::replace_background(const QImage &image) {
    original_image = image;
    scene->background->update_image(image);
}

void ScreenshotWindow::connect_toolbar_signals() {
    connect(toolbar, &Toolbar::tool_changed, this, &ScreenshotWindow::on_tool_changed);
    connect(toolbar, &Toolbar::color_changed, this, &ScreenshotWindow::on_color_changed);
    connect(toolbar, &Toolbar::stroke_width_changed, this, &ScreenshotWindow::on_stroke_width_changed);
    connect(toolbar, &Toolbar::opacity_changed, this, &ScreenshotWindow::on_opacity_changed);
    connect(toolbar, &Toolbar::arrow_style_changed, this, &ScreenshotWindow::on_arrow_style_changed);
    connect(toolbar, &Toolbar::line_style_changed, this, &ScreenshotWindow::on_line_style_changed);
    connect(toolbar, &Toolbar::number_next_changed, this, &ScreenshotWindow::on_number_next_changed);
    connect(toolbar, &Toolbar::number_style_changed, this, &ScreenshotWindow::on_number_style_changed);
    connect(toolbar, &Toolbar::mosaic_style_changed, this, &ScreenshotWindow::on_mosaic_style_changed);
    connect(toolbar, &Toolbar::mosaic_block_size_changed, this, &ScreenshotWindow::on_mosaic_block_size_changed);
    connect(toolbar, &Toolbar::watermark_requested, this, &ScreenshotWindow::on_watermark_requested);
    connect(toolbar, &Toolbar::watermark_style_changed, this, &ScreenshotWindow::on_watermark_style_changed);
    connect(toolbar, &Toolbar::watermark_text_changed, this, &ScreenshotWindow::on_watermark_text_changed);
    connect(toolbar, &Toolbar::undo_clicked, this, &ScreenshotWindow::on_undo);
    connect(toolbar, &Toolbar::redo_clicked, this, &ScreenshotWindow::on_redo);
    connect(toolbar, &Toolbar::confirm_clicked, this, &ScreenshotWindow::handle_confirm);
    connect(toolbar, &Toolbar::cancel_clicked, this, &ScreenshotWindow::cleanup_and_close);
    connect(toolbar, &Toolbar::copy_clicked, this, [this] {
        if (action_handler) action_handler->handle_copy();
    });
    connect(toolbar, &Toolbar::save_clicked, this, [this] {
        if (action_handler) action_handler->handle_save();
    });
    connect(toolbar, &Toolbar::pin_clicked, this, [this] {
        if (action_handler) action_handler->handle_pin();
    });
    connect(toolbar, &Toolbar::long_screenshot_clicked, this, &ScreenshotWindow::start_long_screenshot_mode);
    connect(toolbar, &Toolbar::screenshot_translate_clicked, this, [this] {
        if (action_handler) action_handler->handle_screenshot_translate();
    });
    connect(toolbar, &Toolbar::text_recognize_clicked, this, [this] {
        if (action_handler) action_handler->handle_text_recognize();
    });
    connect(toolbar, &Toolbar::scan_code_clicked, this, [this] {
        if (action_handler) action_handler->handle_scan_code();
    });
    connect(toolbar, &Toolbar::gif_record_clicked, this, &ScreenshotWindow::start_gif_record_mode);
}

void ScreenshotWindow::connect_scene_signals() {
    connect(scene, &CanvasScene::selectionConfirmed, this, &ScreenshotWindow::on_selection_confirmed);
    connect(scene->selection_model, &SelectionModel::rectChanged, this, [this](const QRectF &) { update_toolbar_position(); });
    connect(scene->selection_model, &SelectionModel::draggingChanged, this, &ScreenshotWindow::on_selection_dragging_changed);
    // The automatic watermark is added once per capture: after the user clears it and
    // selects again it does not come back by itself.
    if (!WatermarkTool::auto_template(scene->tool_settings_manager).isEmpty())
        connect(scene, &CanvasScene::selectionConfirmed, this, &ScreenshotWindow::add_auto_watermark, Qt::SingleShotConnection);
    // Text tool signals to the view's controller.
    if (view->smart_edit_controller) {
        auto *controller = view->smart_edit_controller.get();
        connect(toolbar, &Toolbar::text_font_changed, view, [controller](const QFont &font) { controller->on_text_font_changed(font); });
        connect(toolbar, &Toolbar::color_changed, view, [controller](const QColor &color) { controller->on_text_color_changed(color); });
        connect(toolbar, &Toolbar::text_background_changed, view,
                [controller](bool enabled, const QColor &color, int opacity) { controller->on_text_background_changed(enabled, color, opacity); });
        connect(toolbar, &Toolbar::text_outline_changed, view,
                [controller](bool enabled, const QColor &color, double width) { controller->on_text_outline_changed(enabled, color, width); });
        connect(toolbar, &Toolbar::text_shadow_changed, view, [controller](bool enabled, const QColor &color) { controller->on_text_shadow_changed(enabled, color); });
    }
}

void ScreenshotWindow::handle_confirm() {
    if (action_handler) action_handler->handle_confirm();
}

bool ScreenshotWindow::handle_double_click(QMouseEvent *event) {
    if (!action_handler) return false;
    const auto action = matching_capture_mouse_action(event, "doubleleft");
    return action && action_handler->handle_capture_action(*action);
}

void ScreenshotWindow::pin_from_global_hotkey() {
    if (shortcut_handler_) shortcut_handler_->pin_from_global_hotkey();
}

void ScreenshotWindow::safe_activate_and_focus() {
    if (is_closing || !isVisible()) return;
    activateWindow();
    setFocus();
}

void ScreenshotWindow::on_selection_dragging_changed(bool is_dragging) {
    if (is_closing || !toolbar) return;
    if (is_dragging) {
        // Hidden while dragging to reduce repaints, its panel too.
        toolbar->hide();
        if (toolbar->paint_panel && toolbar->paint_panel->isVisible()) toolbar->paint_panel->hide();
    } else if (scene->selection_model->is_confirmed()) {
        toolbar->show();
        toolbar->raise();
        update_toolbar_position();
    }
}

void ScreenshotWindow::on_selection_confirmed() {
    if (is_closing) return;
    toolbar->show();
    // Raised only, not activated.
    toolbar->raise();
    update_toolbar_position();
    if (magnifier_overlay) magnifier_overlay->refresh();
    // The capture window keeps focus.
    activateWindow();
    setFocus();
}

void ScreenshotWindow::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (is_closing) return;
    if (smart_selection_enabled) init_smart_selection_at_cursor();
    init_magnifier_at_cursor();
}

void ScreenshotWindow::init_smart_selection_at_cursor() {
    if (scene->selection_model->is_confirmed()) return;
    const auto cursor_pos = QCursor::pos();
    const QPointF scene_pos(cursor_pos.x(), cursor_pos.y());
    const auto smart_rect = view->get_smart_selection_rect(scene_pos);
    if (smart_rect.isEmpty()) return;
    scene->selection_model->activate();
    // The first appearance has nothing to tween from.
    view->apply_smart_selection_rect(smart_rect, false);
    log_debug(T("智能选区初始化: 鼠标位置({x}, {y}) -> 选区{rect}", {{"x", cursor_pos.x()}, {"y", cursor_pos.y()}, {"rect", py_repr(smart_rect)}}), "ScreenshotWindow");
}

void ScreenshotWindow::init_magnifier_at_cursor() {
    if (!magnifier_overlay) return;
    const auto cursor_pos = QCursor::pos();
    magnifier_overlay->update_cursor(QPointF(cursor_pos.x(), cursor_pos.y()));
    log_debug(T("放大镜初始化: 位置({x}, {y})", {{"x", cursor_pos.x()}, {"y", cursor_pos.y()}}), "ScreenshotWindow");
}

void ScreenshotWindow::resizeEvent(QResizeEvent *event) {
    if (is_closing) return;
    view->setGeometry(rect());
    mask_overlay->setGeometry(rect());
    selection_overlay->setGeometry(rect());
    if (scene->selection_model->is_confirmed()) update_toolbar_position();
    QWidget::resizeEvent(event);
}

void ScreenshotWindow::update_toolbar_position() {
    if (is_closing) return;
    if (!toolbar || !toolbar->isVisible()) return;
    const auto rect = scene->selection_model->rect();
    if (rect.isEmpty()) return;
    // Scene to view coordinates; the view converts to global.
    const auto view_rect = view->mapFromScene(rect).boundingRect();
    // The reference passes this QRect itself, whose right()/bottom() are its last pixel; QRectF(view_rect)
    // would move them out by one.
    toolbar->position_near_rect(QRectF(QPointF(view_rect.topLeft()), QPointF(view_rect.bottomRight())), view);
    // A visible panel moves without being shown again.
    auto *paint_menu = toolbar->paint_panel;
    if (paint_menu && paint_menu->isVisible()) {
        const auto toolbar_pos = toolbar->pos();
        int menu_x = toolbar_pos.x();
        int menu_y = toolbar_pos.y() + toolbar->height() + 5;
        if (auto *screen = QApplication::screenAt(toolbar_pos)) {
            const auto screen_rect = screen->geometry();
            if (menu_y + paint_menu->height() > screen_rect.y() + screen_rect.height()) menu_y = toolbar_pos.y() - paint_menu->height() - 5;
            if (menu_x + paint_menu->width() > screen_rect.x() + screen_rect.width()) menu_x = screen_rect.x() + screen_rect.width() - paint_menu->width() - 5;
        }
        if (paint_menu->pos().x() != menu_x || paint_menu->pos().y() != menu_y) paint_menu->move(menu_x, menu_y);
    }
}

void ScreenshotWindow::cleanup_and_close() {
    if (!session_active_) return;
    // Deferred: the caller (a key dispatch, an action) is usually still running on this window.
    deleteLater();
    teardown_session();
    // Debounced: rapid captures trigger only the last one, avoiding page-fault storms.
    request_trim_working_set(1500);
}

void ScreenshotWindow::keyPressEvent(QKeyEvent *event) {
    // Shortcuts go through the ShortcutManager; this only passes text-editing keys on.
    QWidget::keyPressEvent(event);
}

bool ScreenshotWindow::is_text_editing() const {
    if (!scene) return false;
    auto *focus_item = dynamic_cast<QGraphicsTextItem *>(scene->focusItem());
    return focus_item && focus_item->hasFocus() && (focus_item->textInteractionFlags() & Qt::TextEditorInteraction);
}

void ScreenshotWindow::on_tool_changed(const QString &tool_id) {
    scene->activate_tool(tool_id);
    // The tool's settings are now in its context; sync them to the toolbar with signals
    // blocked so the setters do not loop back. Rebuilt in full on every activation, so
    // a cross-tool projection never leaks into a later tool switch.
    const auto &ctx = scene->tool_controller->ctx;
    const bool blocked = toolbar->blockSignals(true);
    toolbar->restore_active_tool_state(tool_id, &ctx, scene);
    toolbar->blockSignals(blocked);
    // Focus back to the view so shortcuts keep working.
    QTimer::singleShot(0, view, [view = QPointer<CanvasView>(view)] {
        if (view) view->setFocus();
    });
    if (magnifier_overlay) magnifier_overlay->refresh();
}

void ScreenshotWindow::on_watermark_requested() {
    auto *item = WatermarkTool::add(scene);
    toolbar->show_watermark(item->template_text(), item->created_at());
}

void ScreenshotWindow::on_watermark_style_changed(const QString &key, const QVariant &value) { WatermarkTool::change_style(scene, {{key, value}}); }

void ScreenshotWindow::on_watermark_text_changed(const QString &template_text) {
    if (auto *item = WatermarkTool::change_text(scene, template_text)) toolbar->show_watermark(item->template_text(), item->created_at());
}

void ScreenshotWindow::add_auto_watermark() { WatermarkTool::add(scene, false); }

void ScreenshotWindow::remember_watermark() {
    if (scene && scene->selection_model->is_confirmed()) WatermarkTool::remember(scene);
}

void ScreenshotWindow::on_number_next_changed(int next_value) { NumberTool::set_next_number_and_refresh(scene, next_value); }

void ScreenshotWindow::on_number_style_changed(const QString &style) { NumberTool::apply_style_change(style, view, scene->undo_stack); }

void ScreenshotWindow::on_color_changed(const QColor &color) {
    if (view && view->apply_cross_tool_selection_style(color).has_value()) return;
    scene->update_style(color);
}

void ScreenshotWindow::on_stroke_width_changed(int width) {
    if (view && view->apply_cross_tool_selection_style(std::nullopt, width).has_value()) return;
    const auto *ctx = scene->tool_controller ? &scene->tool_controller->ctx : nullptr;
    const double prev_width = ctx ? std::max(1.0, ctx->stroke_width) : width;
    scene->update_style(std::nullopt, width);
    const double new_width = ctx ? std::max(1.0, ctx->stroke_width) : width;
    if (view && prev_width > 0) {
        const double scale = new_width / prev_width;
        if (std::abs(scale - 1.0) > 1e-6) view->apply_size_change_to_selection(scale);
    }
    if (view && view->cursor_manager) view->cursor_manager->update_tool_cursor_size(width);
}

void ScreenshotWindow::on_opacity_changed(int opacity_int) {
    // 0-255 -> 0.0-1.0
    const double opacity = opacity_int / 255.0;
    if (view && view->apply_cross_tool_selection_style(std::nullopt, std::nullopt, opacity).has_value()) return;
    scene->update_style(std::nullopt, std::nullopt, opacity);
    if (view) view->apply_opacity_change_to_selection(opacity);
}

void ScreenshotWindow::on_arrow_style_changed(const QString &style) {
    if (!view || !view->smart_edit_controller) return;
    auto *arrow = dynamic_cast<ArrowItem *>(view->smart_edit_controller->selected_item());
    if (!arrow) return;
    const auto old_state = capture_arrow_state(arrow);
    arrow->set_arrow_style(style);
    const auto new_state = capture_arrow_state(arrow);
    if (scene->undo_stack) scene->undo_stack->push(new EditItemCommand(ItemReference(arrow), old_state, new_state, QString::fromUtf8("修改箭头样式")));
    arrow->update();
    log_debug(T("箭头样式已更新: {style}", {{"style", style}}), "ScreenshotWindow");
}

void ScreenshotWindow::on_mosaic_style_changed(const QString &style) {
    if (MosaicTool::apply_style_change(style, view)) log_debug(T("马赛克种类已更新: {style}", {{"style", style}}), "ScreenshotWindow");
}

void ScreenshotWindow::on_mosaic_block_size_changed(int block_size) {
    if (MosaicTool::apply_block_size_change(block_size, view)) log_debug(T("马赛克粒度已更新: {size}", {{"size", block_size}}), "ScreenshotWindow");
}

void ScreenshotWindow::on_line_style_changed(const QString &style) {
    log_debug(LogMsg(QString("line style change -> %1").arg(style)), "ScreenshotWindow");
    if (view) {
        view->apply_line_style_change_to_selection(style);
        log_debug(T("线条样式已更新: {style}", {{"style", style}}), "ScreenshotWindow");
    }
}

QVariantMap ScreenshotWindow::capture_arrow_state(QGraphicsItem *item) const {
    QVariantMap state;
    if (auto *arrow = dynamic_cast<ArrowItem *>(item)) {
        state["start"] = arrow->start_pos;
        state["end"] = arrow->end_pos;
        state["control"] = arrow->_control_pos;
        state["control_modified"] = arrow->_control_modified;
        state["arrow_style"] = arrow->_arrow_style;
    }
    return state;
}

void ScreenshotWindow::on_undo() {
    if (scene->undo_stack->canUndo()) scene->undo_stack->undo();
}

void ScreenshotWindow::on_redo() {
    if (scene->undo_stack->canRedo()) scene->undo_stack->redo();
}

void ScreenshotWindow::start_gif_record_mode() {
    log_info(T("启动GIF录制模式"), "ScreenshotWindow");
    if (!scene->selection_model->is_confirmed()) {
        show_modeless_warning_dialog(this, window_tr("Warning"), window_tr("Please select a valid capture area first."));
        return;
    }
    const auto selection_rect = scene->selection_model->rect();
    const int real_x = static_cast<int>(selection_rect.x()), real_y = static_cast<int>(selection_rect.y());
    const int real_width = static_cast<int>(selection_rect.width()), real_height = static_cast<int>(selection_rect.height());
    const QRect capture_rect(real_x, real_y, real_width, real_height);
    log_debug(T("GIF录制区域: x={x}, y={y}, w={w}, h={h}", {{"x", real_x}, {"y", real_y}, {"w", real_width}, {"h", real_height}}), "ScreenshotWindow");
    // An older recording window is a singleton: closed first, or kept with its session while it is busy.
    if (auto old_win = active_gif_window()) {
        // Recording or muxing keeps the original session so two encoders never run at once.
        if (old_win->video_busy()) {
            cleanup_and_close();
            return;
        }
        old_win->close_all();
        active_gif_window() = nullptr;
    }
    auto *gif_win = new GifRecordWindow(capture_rect);
    log_debug(T("GifRecordWindow已创建, overlay visible={visible}", {{"visible", gif_win->overlay_visible() ? QString("True") : QString("False")}}), "ScreenshotWindow");
    // Held globally so it outlives the screenshot window; QPointer clears itself when it is destroyed.
    active_gif_window() = gif_win;
    log_info(T("GIF录制窗口已启动"), "ScreenshotWindow");
    cleanup_and_close();
}

void ScreenshotWindow::start_long_screenshot_mode() {
    log_info(T("启动长截图模式"), "ScreenshotWindow");
    if (!scene->selection_model->is_confirmed()) {
        show_modeless_warning_dialog(this, window_tr("Warning"), window_tr("Please select a valid capture area first."));
        return;
    }
    const auto selection_rect = scene->selection_model->rect();
    log_debug(T("selection_rect（场景坐标）: x={x}, y={y}, w={w}, h={h}",
                {{"x", selection_rect.x()}, {"y", selection_rect.y()}, {"w", selection_rect.width()}, {"h", selection_rect.height()}}),
              "LongScreenshot");
    log_debug(T("virtual偏移: x={x}, y={y}", {{"x", virtual_x}, {"y", virtual_y}}), "LongScreenshot");
    // Scene coordinates are already global screen coordinates (the background keeps the
    // desktop offset), so the virtual offset must not be added again.
    const int real_x = static_cast<int>(selection_rect.x()), real_y = static_cast<int>(selection_rect.y());
    const int real_width = static_cast<int>(selection_rect.width()), real_height = static_cast<int>(selection_rect.height());
    const QRect capture_rect(real_x, real_y, real_width, real_height);
    log_debug(T("选中区域（屏幕坐标）: x={x}, y={y}, w={w}, h={h}", {{"x", real_x}, {"y", real_y}, {"w", real_width}, {"h", real_height}}), "LongScreenshot");
    // A separate top-level, independent of this window.
    auto *scroll_window = new ScrollCaptureWindow(capture_rect, nullptr, config_manager);
    auto *app = QApplication::instance();
    if (auto *old_scroll = qobject_cast<QWidget *>(app->property("_scroll_window").value<QObject *>())) old_scroll->close();
    app->setProperty("_scroll_window", QVariant::fromValue<QObject *>(scroll_window));
    connect(scroll_window, &QObject::destroyed, app, [app] {
        app->setProperty("_scroll_window", QVariant());
        request_trim_working_set(1000);
    });
    log_debug(T("长截图窗口创建完成，准备显示"), "LongScreenshot");
    scroll_window->show();
    scroll_window->raise();
    scroll_window->activateWindow();
    log_info(T("滚动截图窗口已显示并激活"), "LongScreenshot");
    log_debug(T("释放截图窗口内存"), "LongScreenshot");
    cleanup_and_close();
}

void ScreenshotWindow::closeEvent(QCloseEvent *event) {
    if (!is_closing) cleanup_and_close();
    QWidget::closeEvent(event);
}
}
