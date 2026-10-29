#include "annotation/screen_annotation.h"
#include "annotation/annotation_toolbar.h"
#include "annotation/annotation_view.h"
#include "annotation/cursor_highlight.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/drawing_items.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/undo.h"
#include "clipboard/controllers/paste_keystroke.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "pin/pin_canvas.h"
#include "pin/pin_manager.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include "tools/number.h"
#include <QApplication>
#include <QCursor>
#include <QGraphicsTextItem>
#include <QScreen>
#include <QSet>
#include <algorithm>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

namespace jietuba {
namespace {
// The capture tools the layers have: no mosaic, which needs the captured image, and no spotlight or
// watermark, which follow a capture selection.
const QSet<QString> &annotation_tools() {
    static const QSet<QString> tools = {"pen", "highlighter", "arrow", "number", "rect", "ellipse", "text", "eraser"};
    return tools;
}

void end_text_editing(AnnotationView *view) {
    if (auto *item = view->get_active_text_item()) view->end_text_edit(item);
}
}

ScreenAnnotationShortcutHandler::ScreenAnnotationShortcutHandler(ScreenAnnotation *owner) : owner_(owner) {
    // Read once per session, like the capture window.
    QStringList keys = {"inapp_undo", "inapp_redo", "inapp_delete"};
    for (const auto &entry : ANNOTATION_TOOL_SHORTCUTS()) {
        const QString tool_id = entry.tool_id;
        if (tool_id != "cursor" && !annotation_tools().contains(tool_id)) continue;
        tool_shortcuts_.emplace_back(entry.config_key, tool_id);
        keys.append(entry.config_key);
    }
    bindings_ = load_inapp_bindings(keys);
    mouse_bindings_ = load_inapp_mouse_bindings(keys);
}

bool ScreenAnnotationShortcutHandler::owns_window(const QWidget *window) const { return owner_->owns_window(window); }

bool ScreenAnnotationShortcutHandler::is_active() { return owner_->mode() == ScreenAnnotation::Mode::Drawing; }

bool ScreenAnnotationShortcutHandler::match(QInputEvent *event, const QString &cfg_key) const {
    return match_inapp_binding(event, cfg_key, bindings_, mouse_bindings_);
}

bool ScreenAnnotationShortcutHandler::handle_key(QInputEvent *event) {
    auto *view = owner_->active_view();
    const int key = event_key(event);
    // Esc is fixed: it ends text editing as well, since leaving the tool commits the text.
    if (key == Qt::Key_Escape) {
        owner_->select_tool("cursor");
        return true;
    }
    // While editing text the keys below belong to the text item.
    if (view && view->is_text_editing()) return false;
    if (match(event, "inapp_undo")) {
        owner_->undo();
        return true;
    }
    if (match(event, "inapp_redo")) {
        owner_->redo();
        return true;
    }
    if (match(event, "inapp_delete")) {
        if (view && view->smart_edit_controller) view->smart_edit_controller->delete_selected();
        return true;
    }
    for (const auto &[cfg_key, tool_id] : tool_shortcuts_) {
        if (!match(event, cfg_key)) continue;
        if (!event_is_auto_repeat(event)) owner_->select_tool(tool_id);
        return true;
    }
    return false;
}

ScreenAnnotation::ScreenAnnotation(QObject *parent) : QObject(parent), highlight_(new CursorHighlight(this)) {}

ScreenAnnotation::~ScreenAnnotation() {
    // The owner is usually being destroyed too (application exit); it must not hear of the mode change.
    blockSignals(true);
    stop();
    discard_views();
    // No event loop may follow (application exit), so the windows go now instead of deleteLater.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void ScreenAnnotation::toggle() {
    if (active()) stop();
    else start();
}

void ScreenAnnotation::start() {
    if (active()) return;
    log_info(T("屏幕标注开始"), "Annotation");
    toolbar_ = new AnnotationToolbar(this);
    connect_toolbar();
    // Layers kept from the last session come back, matched to the screens there are now.
    if (!keep_drawings()) discard_views();
    for (auto *view : std::vector<AnnotationView *>(views_)) {
        if (!view->target_screen()) remove_view(view);
        else view->fit_screen();
    }
    for (auto *screen : QGuiApplication::screens())
        if (std::none_of(views_.begin(), views_.end(), [screen](AnnotationView *view) { return view->target_screen() == screen; })) add_view(screen);
    session_connections_.push_back(connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *screen) {
        add_view(screen);
        raise_toolbar();
    }));
    session_connections_.push_back(connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *screen) {
        for (auto *view : views_) {
            if (view->target_screen() != screen) continue;
            remove_view(view);
            break;
        }
        if (toolbar_) toolbar_->keep_on_screen();
    }));
    session_connections_.push_back(connect(qApp, &QGuiApplication::applicationStateChanged, this, &ScreenAnnotation::on_application_state_changed));
    session_connections_.push_back(connect(PinManager::instance(), &PinManager::pin_created, this, &ScreenAnnotation::on_pin_created));
    shortcuts_ = std::make_unique<ScreenAnnotationShortcutHandler>(this);
    ShortcutManager::instance()->register_handler(shortcuts_.get());
    set_mode(Mode::PassThrough);
    set_cursor_highlight(get_tool_settings_manager()->get_screen_annotation_cursor_highlight(), false);
    sync_effect_buttons();
    for_each_view([](AnnotationView *view) { view->show(); });
    toolbar_->place_at_start();
    toolbar_->show();
    raise_toolbar();
    // Pass-through, the pen, or the tool the last session used (screen_annotation_start_tool).
    auto *config = get_tool_settings_manager();
    const QString start_tool = config->get_screen_annotation_start_tool();
    if (start_tool == "pen") select_tool("pen");
    else if (start_tool == "last") select_tool(config->get_screen_annotation_last_tool());
}

void ScreenAnnotation::stop() {
    if (!active()) return;
    if (mode_ == Mode::Drawing) enter_pass_through(true);
    board_ = false;
    if (highlight_) highlight_->set_enabled(false);
    for_each_view([](AnnotationView *view) { view->set_board(std::nullopt); });
    for (const auto &connection : session_connections_) disconnect(connection);
    session_connections_.clear();
    if (shortcuts_) {
        ShortcutManager::instance()->unregister_handler(shortcuts_.get());
        shortcuts_.reset();
    }
    // Kept layers wait hidden with their undo steps; a hidden window holds no backing store.
    if (keep_drawings()) for_each_view([](AnnotationView *view) { view->hide(); });
    else discard_views();
    active_view_ = nullptr;
    toolbar_->remember_position();
    toolbar_->close_with_floating_windows();
    toolbar_ = nullptr;
    set_mode(Mode::Off);
    log_info(T("屏幕标注结束"), "Annotation");
    request_trim_working_set();
}

bool ScreenAnnotation::keep_drawings() const { return get_tool_settings_manager()->get_screen_annotation_keep_drawings(); }

void ScreenAnnotation::discard_views() {
    // deleteLater: the exit button and Esc run inside these windows' own event handlers.
    for (auto *view : views_) {
        view->hide();
        view->deleteLater();
    }
    views_.clear();
    if (highlight_) highlight_->set_views(views_);
    stack_index_.clear();
    history_.clear();
    redo_.clear();
}

void ScreenAnnotation::add_view(QScreen *screen) {
    auto *view = new AnnotationView(screen, this);
    views_.push_back(view);
    if (highlight_) highlight_->set_views(views_);
    stack_index_[view] = view->undo_stack()->index();
    // The layer as context: its scene clears the stack while being destroyed, after this disconnects.
    connect(view->undo_stack(), &QUndoStack::indexChanged, view, [this, view](int index) { on_stack_index_changed(view, index); });
    connect(view, &AnnotationView::pressed, this, [this](AnnotationView *pressed_view) {
        active_view_ = pressed_view;
        sync_toolbar_style();
        raise_toolbar();
    });
    connect(screen, &QScreen::geometryChanged, view, [this, view] {
        view->fit_screen();
        if (toolbar_) toolbar_->keep_on_screen();
    });
    const QRect geometry = screen->geometry();
    log_debug(T("屏幕标注图层: {geometry}", {{"geometry", QString("%1,%2 %3x%4").arg(geometry.x()).arg(geometry.y()).arg(geometry.width()).arg(geometry.height())}}),
              "Annotation");
    if (mode_ == Mode::Off) return;
    // A screen plugged in during the session joins in the current mode.
    if (mode_ == Mode::Drawing && toolbar_ && toolbar_->current_tool) view->activate_tool(*toolbar_->current_tool);
    if (mode_ != Mode::Hidden) view->show();
    apply_board();
}

void ScreenAnnotation::remove_view(AnnotationView *view) {
    views_.erase(std::remove(views_.begin(), views_.end(), view), views_.end());
    if (highlight_) highlight_->set_views(views_);
    stack_index_.remove(view);
    // Steps of the removed screen go; steps shared with other screens keep those screens.
    for (auto *steps : {&history_, &redo_}) {
        for (auto &step : *steps) step.erase(std::remove(step.begin(), step.end(), view), step.end());
        steps->erase(std::remove_if(steps->begin(), steps->end(), [](const auto &step) { return step.empty(); }), steps->end());
    }
    if (active_view_ == view) active_view_ = nullptr;
    view->hide();
    view->deleteLater();
}

void ScreenAnnotation::connect_toolbar() {
    auto *tb = toolbar_;
    connect(tb, &Toolbar::tool_changed, this, &ScreenAnnotation::on_tool_changed);
    connect(tb, &AnnotationToolbar::pointer_clicked, this, [this] { select_tool("cursor"); });
    connect(tb, &AnnotationToolbar::hide_clicked, this, [this] { set_drawings_visible(mode_ == Mode::Hidden); });
    connect(tb, &AnnotationToolbar::clear_clicked, this, &ScreenAnnotation::clear_drawings);
    connect(tb, &AnnotationToolbar::board_clicked, this, &ScreenAnnotation::toggle_board);
    connect(tb, &AnnotationToolbar::highlight_clicked, this, [this] { set_cursor_highlight(!cursor_highlight_enabled()); });
    // Queued: the button that ends the session belongs to the toolbar being closed.
    connect(tb, &AnnotationToolbar::exit_clicked, this, &ScreenAnnotation::stop, Qt::QueuedConnection);
    connect(tb, &Toolbar::undo_clicked, this, &ScreenAnnotation::undo);
    connect(tb, &Toolbar::redo_clicked, this, &ScreenAnnotation::redo);
    // Style changes go to every layer, with the capture window's rules for selected drawings.
    connect(tb, &Toolbar::color_changed, this, [this](const QColor &color) {
        for_each_view([&color](AnnotationView *view) {
            if (view->apply_cross_tool_selection_style(color).has_value()) return;
            view->scene_obj()->update_style(color);
            if (view->smart_edit_controller) view->smart_edit_controller->on_text_color_changed(color);
        });
    });
    connect(tb, &Toolbar::stroke_width_changed, this, [this](int width) {
        for_each_view([width](AnnotationView *view) {
            if (view->apply_cross_tool_selection_style(std::nullopt, width).has_value()) return;
            const auto &ctx = view->tool_controller()->ctx;
            const double previous = std::max(1.0, ctx.stroke_width);
            view->scene_obj()->update_style(std::nullopt, width);
            const double now = std::max(1.0, ctx.stroke_width);
            if (std::abs(now / previous - 1.0) > 1e-6) view->apply_size_change_to_selection(now / previous);
            if (view->cursor_manager) view->cursor_manager->update_tool_cursor_size(width);
        });
    });
    connect(tb, &Toolbar::opacity_changed, this, [this](int value) {
        const double opacity = value / 255.0;
        for_each_view([opacity](AnnotationView *view) {
            if (view->apply_cross_tool_selection_style(std::nullopt, std::nullopt, opacity).has_value()) return;
            view->scene_obj()->update_style(std::nullopt, std::nullopt, opacity);
            view->apply_opacity_change_to_selection(opacity);
        });
    });
    connect(tb, &Toolbar::arrow_style_changed, this, [this](const QString &style) {
        for_each_view([&style](AnnotationView *view) {
            auto *arrow = view->smart_edit_controller ? dynamic_cast<ArrowItem *>(view->smart_edit_controller->selected_item()) : nullptr;
            if (!arrow) return;
            const auto old_state = PinCanvas::capture_arrow_state(arrow);
            arrow->set_arrow_style(style);
            view->undo_stack()->push(new EditItemCommand(ItemReference(arrow), old_state, PinCanvas::capture_arrow_state(arrow), QString::fromUtf8("修改箭头样式")));
            arrow->update();
        });
    });
    connect(tb, &Toolbar::line_style_changed, this, [this](const QString &style) {
        for_each_view([&style](AnnotationView *view) { view->apply_line_style_change_to_selection(style); });
    });
    connect(tb, &Toolbar::highlighter_mode_changed, this, [this](const QString &) {
        for_each_view([](AnnotationView *view) {
            if (view->cursor_manager) view->cursor_manager->set_tool_cursor("highlighter", true);
        });
    });
    connect(tb, &Toolbar::number_next_changed, this, [this](int value) {
        for_each_view([value](AnnotationView *view) { NumberTool::set_next_number_and_refresh(view->scene_obj(), value); });
    });
    connect(tb, &Toolbar::number_style_changed, this, [this](const QString &style) {
        for_each_view([&style](AnnotationView *view) { NumberTool::apply_style_change(style, view, view->undo_stack()); });
    });
    connect(tb, &Toolbar::text_font_changed, this, [this](const QFont &font) {
        for_each_view([&font](AnnotationView *view) {
            if (view->smart_edit_controller) view->smart_edit_controller->on_text_font_changed(font);
        });
    });
    connect(tb, &Toolbar::text_outline_changed, this, [this](bool enabled, const QColor &color, double width) {
        for_each_view([&](AnnotationView *view) {
            if (view->smart_edit_controller) view->smart_edit_controller->on_text_outline_changed(enabled, color, width);
        });
    });
    connect(tb, &Toolbar::text_shadow_changed, this, [this](bool enabled, const QColor &color) {
        for_each_view([&](AnnotationView *view) {
            if (view->smart_edit_controller) view->smart_edit_controller->on_text_shadow_changed(enabled, color);
        });
    });
    connect(tb, &Toolbar::text_background_changed, this, [this](bool enabled, const QColor &color, int opacity) {
        for_each_view([&](AnnotationView *view) {
            if (view->smart_edit_controller) view->smart_edit_controller->on_text_background_changed(enabled, color, opacity);
        });
    });
}

void ScreenAnnotation::select_tool(const QString &tool_id) {
    if (!toolbar_) return;
    // The toolbar owns the current tool; its tool_changed drives the layers.
    if (tool_id == "cursor" || annotation_tools().contains(tool_id)) toolbar_->select_tool(tool_id);
}

void ScreenAnnotation::on_tool_changed(const QString &tool_id) {
    if (!active()) return;
    if (tool_id == "cursor" || !annotation_tools().contains(tool_id)) {
        enter_pass_through(true);
    } else {
        enter_drawing(tool_id);
        get_tool_settings_manager()->set_screen_annotation_last_tool(tool_id);
    }
    toolbar_->prepare_floating_windows();
}

void ScreenAnnotation::enter_drawing(const QString &tool_id) {
    if (mode_ == Mode::Hidden) for_each_view([](AnnotationView *view) { view->show(); });
    if (mode_ != Mode::Drawing) remember_foreground();
    // Each layer leaves pass-through and activates itself; the one under the pointer takes the keys.
    for_each_view([&tool_id](AnnotationView *view) { view->activate_tool(tool_id); });
    set_mode(Mode::Drawing);
    active_view_ = view_under_cursor();
    if (active_view_) active_view_->activateWindow();
    sync_toolbar_style();
    raise_toolbar();
    log_debug(T("屏幕标注工具: {tool_id}", {{"tool_id", tool_id}}), "Annotation");
}

void ScreenAnnotation::enter_pass_through(bool restore_focus) {
    const bool was_drawing = mode_ == Mode::Drawing;
    if (mode_ == Mode::Hidden) for_each_view([](AnnotationView *view) { view->show(); });
    // Leaving the tool also commits text being edited.
    for_each_view([](AnnotationView *view) { view->activate_tool("cursor"); });
    set_mode(Mode::PassThrough);
    if (!was_drawing) return;
    if (restore_focus) restore_foreground();
    previous_foreground_ = 0;
    raise_toolbar();
}

void ScreenAnnotation::set_cursor_highlight(bool enabled, bool remember) {
    if (!active()) return;
    if (remember) get_tool_settings_manager()->set_screen_annotation_cursor_highlight(enabled);
    highlight_->set_views(views_);
    highlight_->set_enabled(enabled);
    sync_effect_buttons();
}

bool ScreenAnnotation::cursor_highlight_enabled() const { return highlight_ && highlight_->enabled(); }

void ScreenAnnotation::toggle_board() {
    if (!active()) return;
    board_ = !board_;
    if (board_ && mode_ != Mode::Drawing) select_tool("pen");
    apply_board();
}

void ScreenAnnotation::apply_board() {
    const bool shown = board_ && active();
    const std::optional<QColor> color = shown ? std::optional<QColor>(QColor(Qt::white)) : std::nullopt;
    bool added = false;
    for_each_view([&](AnnotationView *view) {
        added = added || (shown && !view->board());
        view->set_board(color);
    });
    if (added) stack_boards();
    sync_effect_buttons();
}

void ScreenAnnotation::stack_boards() {
    std::vector<HWND> boards;
    for (auto *view : views_)
        if (auto *board = view->board_window()) boards.push_back(reinterpret_cast<HWND>(board->winId()));
    // The board covers other programs and the windows of this one that are not kept on top. Those kept
    // on top, the pins, the layers and the toolbar among them, stay above it in their order.
    std::vector<HWND> above;
    const DWORD process = GetCurrentProcessId();
    for (HWND hwnd = GetTopWindow(nullptr); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner != process || !IsWindowVisible(hwnd) || !(GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) continue;
        if (std::find(boards.begin(), boards.end(), hwnd) == boards.end()) above.push_back(hwnd);
    }
    // From the top down, each window right below the one before it.
    above.insert(above.end(), boards.begin(), boards.end());
    HWND after = HWND_TOPMOST;
    for (HWND hwnd : above) {
        SetWindowPos(hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        after = hwnd;
    }
}

void ScreenAnnotation::on_pin_created() {
    // Drawing goes on over a pin made meanwhile, as over the pins already there: the layers come back
    // above it and keep the keys.
    if (mode_ != Mode::Drawing) return;
    for (auto *view : views_)
        if (view->isVisible()) SetWindowPos(reinterpret_cast<HWND>(view->winId()), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (auto *view = active_view()) view->activateWindow();
    raise_toolbar();
}

void ScreenAnnotation::sync_effect_buttons() {
    if (toolbar_) toolbar_->set_effect_checks(board_, cursor_highlight_enabled());
}

void ScreenAnnotation::set_drawings_visible(bool visible) {
    if (!active()) return;
    if (!visible) {
        if (mode_ == Mode::Hidden) return;
        if (mode_ == Mode::Drawing) toolbar_->select_tool("cursor");
        for_each_view([](AnnotationView *view) { view->hide(); });
        set_mode(Mode::Hidden);
        return;
    }
    if (mode_ != Mode::Hidden) return;
    for_each_view([](AnnotationView *view) { view->show(); });
    set_mode(Mode::PassThrough);
    raise_toolbar();
}

void ScreenAnnotation::clear_drawings() {
    if (!active()) return;
    // Settled first: committing text being edited pushes a step of its own.
    for_each_view([](AnnotationView *view) {
        end_text_editing(view);
        if (view->smart_edit_controller) view->smart_edit_controller->clear_selection();
    });
    std::vector<AnnotationView *> changed;
    coordinating_ = true;
    for (auto *view : views_) {
        auto *scene = view->scene_obj();
        std::vector<ItemReference> items;
        // Annotations only (top-level tracked items); tool helpers such as the brush indicator stay.
        for (auto *item : scene->items()) {
            if (item->parentItem() || !scene->is_drawing(item)) continue;
            if (auto *tracked = dynamic_cast<DrawingItemMixin *>(item)) items.emplace_back(tracked);
        }
        if (items.empty()) continue;
        view->undo_stack()->push_command(new BatchRemoveCommand(scene, std::move(items), "Clear Drawings"));
        stack_index_[view] = view->undo_stack()->index();
        changed.push_back(view);
    }
    coordinating_ = false;
    if (changed.empty()) return;
    history_.push_back(changed);
    redo_.clear();
    log_debug(T("屏幕标注已清空: {count} 个屏幕", {{"count", static_cast<int>(changed.size())}}), "Annotation");
}

void ScreenAnnotation::undo() {
    for_each_view(end_text_editing);
    if (history_.empty()) return;
    const auto step = history_.back();
    history_.pop_back();
    coordinating_ = true;
    for (auto *view : step) {
        view->undo_stack()->undo();
        stack_index_[view] = view->undo_stack()->index();
    }
    coordinating_ = false;
    redo_.push_back(step);
}

void ScreenAnnotation::redo() {
    for_each_view(end_text_editing);
    if (redo_.empty()) return;
    const auto step = redo_.back();
    redo_.pop_back();
    coordinating_ = true;
    for (auto *view : step) {
        view->undo_stack()->redo();
        stack_index_[view] = view->undo_stack()->index();
    }
    coordinating_ = false;
    history_.push_back(step);
}

void ScreenAnnotation::on_stack_index_changed(AnnotationView *view, int index) {
    const int previous = stack_index_.value(view, 0);
    stack_index_[view] = index;
    if (coordinating_ || !active()) return;
    if (index > previous) {
        // Drawn on this layer; a new step drops what could be redone, as in one stack.
        for (int i = previous; i < index; ++i) history_.push_back({view});
        redo_.clear();
        return;
    }
    // Undone by the layer itself: those steps leave the history.
    int remaining = previous - index;
    for (auto it = history_.end(); remaining > 0 && it != history_.begin();) {
        --it;
        if (it->size() == 1 && it->front() == view) {
            redo_.push_back(*it);
            it = history_.erase(it);
            --remaining;
        }
    }
}

void ScreenAnnotation::prepare_for_capture() {
    if (!active()) return;
    if (mode_ == Mode::Drawing) toolbar_->select_tool("cursor");
    // The light around the pointer is not part of a screenshot; the setting stays for the next session.
    if (cursor_highlight_enabled()) set_cursor_highlight(false, false);
    // Painted now rather than on the next event loop, then one composition later, so the screen
    // read by the capture no longer has the hit layer.
    bool painted = false;
    for (auto *view : views_) {
        if (!view->isVisible()) continue;
        view->viewport()->repaint();
        painted = true;
    }
    if (painted) DwmFlush();
}

void ScreenAnnotation::set_mode(Mode mode) {
    mode_ = mode;
    if (toolbar_) toolbar_->set_mode_checks(mode == Mode::PassThrough, mode == Mode::Hidden);
    emit mode_changed(mode);
}

void ScreenAnnotation::sync_toolbar_style() {
    auto *view = active_view();
    if (!toolbar_ || !view || !view->tool_controller()) return;
    const auto tool_id = view->tool_controller()->current_tool_id();
    if (tool_id == "cursor") return;
    // Signals blocked, so the setters do not come back as style changes.
    const auto &ctx = view->tool_controller()->ctx;
    const bool blocked = toolbar_->blockSignals(true);
    toolbar_->set_current_color(ctx.color);
    toolbar_->set_stroke_width(static_cast<int>(ctx.stroke_width));
    toolbar_->set_opacity(static_cast<int>(ctx.opacity * 255));
    if (tool_id == "number") toolbar_->set_number_next_value(NumberTool::get_next_number(view->scene_obj()));
    toolbar_->blockSignals(blocked);
}

void ScreenAnnotation::raise_toolbar() {
    if (!toolbar_ || !toolbar_->isVisible()) return;
    // A layer that leaves pass-through, or is pressed to draw on, comes to the top of the topmost
    // windows and its hit layer takes the toolbar's clicks. Restacked without activation, so the
    // layer keeps the keys.
    const auto to_top = [](QWidget *window) {
        SetWindowPos(reinterpret_cast<HWND>(window->winId()), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    for (auto *window : toolbar_->floating_windows())
        if (window->isVisible()) to_top(window);
    to_top(toolbar_);
}

void ScreenAnnotation::remember_foreground() {
    const auto foreground = clipboard::get_foreground_hwnd();
    if (!foreground || owns_window(QWidget::find(static_cast<WId>(foreground)))) return;
    previous_foreground_ = foreground;
}

void ScreenAnnotation::restore_foreground() {
    const auto hwnd = std::exchange(previous_foreground_, 0);
    if (!clipboard::is_alive(hwnd) || IsIconic(reinterpret_cast<HWND>(hwnd))) return;
    // Only from a layer: a window the user switched to meanwhile keeps the focus.
    if (!owns_window(QWidget::find(static_cast<WId>(clipboard::get_foreground_hwnd())))) return;
    clipboard::set_foreground_window(hwnd);
}

void ScreenAnnotation::on_application_state_changed(Qt::ApplicationState state) {
    // Another program came to the front (Alt+Tab, the taskbar): it is used as is, so the layers let
    // the mouse through and the focus is not taken back.
    if (state == Qt::ApplicationActive || mode_ != Mode::Drawing) return;
    previous_foreground_ = 0;
    toolbar_->select_tool("cursor");
}

AnnotationView *ScreenAnnotation::view_under_cursor() const {
    auto *screen = QGuiApplication::screenAt(QCursor::pos());
    for (auto *view : views_)
        if (view->target_screen() == screen) return view;
    return views_.empty() ? nullptr : views_.front();
}

AnnotationView *ScreenAnnotation::active_view() const {
    if (active_view_) return active_view_;
    return view_under_cursor();
}

bool ScreenAnnotation::owns_window(const QWidget *widget) const {
    if (!widget || !active()) return false;
    auto *window = widget->window();
    if (auto *view = dynamic_cast<AnnotationView *>(window); view && std::find(views_.begin(), views_.end(), view) != views_.end()) return true;
    if (!toolbar_) return false;
    if (window == toolbar_) return true;
    const auto floating = toolbar_->floating_windows();
    return std::find(floating.begin(), floating.end(), window) != floating.end();
}
}
