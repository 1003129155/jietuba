#include "canvas/view.h"
#include "canvas/canvas_host.h"
#include "canvas/handle_overlay.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/selection_item.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/smart_selection_anim.h"
#include "capture/uia_element_finder.h"
#include "capture/window_finder.h"
#include "core/clipboard_utils.h"
#include "core/export.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include "tools/number.h"
#include "ui/arrow_settings_panel.h"
#include "ui/magnifier.h"
#include "ui/mosaic_settings_panel.h"
#include "ui/number_settings_panel.h"
#include "ui/paint_settings_panel.h"
#include "ui/shape_settings_panel.h"
#include "ui/text_settings_panel.h"
#include "ui/toolbar.h"
#include <QApplication>
#include <QCursor>
#include <QGraphicsTextItem>
#include <QMouseEvent>
#include <QSpinBox>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>

namespace jietuba {
namespace {
long long rect_area(const SmartRect &r) { return static_cast<long long>(r[2] - r[0]) * (r[3] - r[1]); }

QString item_type_name(QGraphicsItem *item) {
    if (dynamic_cast<SpotlightItem *>(item)) return "SpotlightItem";
    if (dynamic_cast<StrokeItem *>(item)) return "StrokeItem";
    if (dynamic_cast<RectItem *>(item)) return "RectItem";
    if (dynamic_cast<EllipseItem *>(item)) return "EllipseItem";
    if (dynamic_cast<ArrowItem *>(item)) return "ArrowItem";
    if (dynamic_cast<TextItem *>(item)) return "TextItem";
    if (dynamic_cast<NumberItem *>(item)) return "NumberItem";
    if (dynamic_cast<MosaicItem *>(item)) return "MosaicItem";
    return "QGraphicsItem";
}

DrawingItemMixin *drawing_mixin(QGraphicsItem *item) { return dynamic_cast<DrawingItemMixin *>(item); }
}

void append_level(std::vector<SmartRect> &chain, const SmartRect &rect) {
    if (chain.empty()) {
        chain.push_back(rect);
        return;
    }
    const auto &previous = chain.back();
    if (rect_area(rect) < rect_area(previous)) return;
    bool same = true;
    for (int i = 0; i < 4; ++i)
        if (std::abs(rect[i] - previous[i]) > LEVEL_TOLERANCE_PX) same = false;
    if (same) return;
    chain.push_back(rect);
}

CanvasView::CanvasView(CanvasScene *scene, QWidget *parent, bool confirm, bool cross_tool_select)
    : QGraphicsView(scene, parent), canvas_scene(scene), confirm_on_double_click(confirm) {
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Top-left alignment: no automatic centring on resize, so mapFromScene stays stable.
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setFrameShape(QGraphicsView::NoFrame);
    setContentsMargins(0, 0, 0, 0);
    setViewportMargins(0, 0, 0, 0);
    // Scene uses global screen coordinates; map its top-left to the view origin 1:1.
    resetTransform();
    const auto scene_rect = scene->sceneRect();
    translate(-scene_rect.x(), -scene_rect.y());
    viewport()->setMouseTracking(true);
    setMouseTracking(true);
    smart_selection_anim_ = std::make_unique<SmartSelectionAnimator>([this](const QRectF &rect) { set_selection_rect_now(rect); }, this);
    scene->view = this;
    cursor_manager = std::make_unique<CursorManager>(this);
    smart_edit_controller = std::make_unique<SmartEditController>(canvas_scene);
    smart_edit_controller->cross_tool_select_enabled = cross_tool_select;
    // Scene-to-view signals instead of the scene holding the view.
    auto *cm = cursor_manager.get();
    scene_connections_.push_back(connect(canvas_scene, &CanvasScene::cursor_color_update_requested, this, [cm](const QColor &color) { cm->update_tool_cursor_color(color); }));
    scene_connections_.push_back(connect(canvas_scene, &CanvasScene::cursor_opacity_update_requested, this, [cm] { cm->update_tool_cursor_opacity(); }));
    scene_connections_.push_back(connect(canvas_scene, &CanvasScene::cursor_tool_update_requested, this, &CanvasView::on_cursor_tool_update_requested));
    scene_connections_.push_back(connect(canvas_scene, &CanvasScene::item_auto_select_requested, this, &CanvasView::on_item_auto_select_requested));
    scene_connections_.push_back(connect(canvas_scene, &CanvasScene::editing_cleanup_requested, this, &CanvasView::on_editing_cleanup));
    connect(smart_edit_controller.get(), &SmartEditController::cursor_change_request, this, &CanvasView::on_edit_cursor_change);
    connect(smart_edit_controller.get(), &SmartEditController::selection_changed, this, &CanvasView::on_edit_selection_changed);
    connect(smart_edit_controller.get(), &SmartEditController::tool_switch_requested, this, &CanvasView::on_cross_tool_switch_requested);
    tool_changed_callback_ = canvas_scene->tool_controller->add_tool_changed_callback([this](const QString &id) { on_tool_changed_for_edit(id); });
    if (auto *current_tool = canvas_scene->tool_controller->current_tool) {
        cursor_manager->set_tool_cursor(current_tool->id());
        smart_edit_controller->set_tool(current_tool->id());
    }
    drawing = std::make_unique<DrawingStroke>(this);
    selection_drag = std::make_unique<SelectionDrag>(this);
    text_drag = std::make_unique<TextEdgeDrag>(this);
    item_drag = std::make_unique<ManualItemDrag>(this);
    pending_text_edit = std::make_unique<PendingTextEdit>(this);
    // Handles are drawn on an overlay above the viewport, outside the scene pipeline.
    handle_overlay_ = new HandleOverlayWidget(this);
    auto *layer_editor = smart_edit_controller->layer_editor.get();
    layer_editor->repaint_requested = [this] { request_handles_repaint(); };
    layer_editor->screen_transform = [this] { return viewportTransform(); };
    // Handle positions derive from item geometry, so they follow every scene change
    // rather than relying on each edit path to notify.
    scene_connections_.push_back(connect(canvas_scene, &QGraphicsScene::changed, this, [this](const QList<QRectF> &) { on_scene_changed_for_handles(); }));
}

CanvasView::~CanvasView() { cleanup(); }

QGraphicsItem *CanvasView::selected_drawing_item() const {
    return smart_edit_controller ? smart_edit_controller->selected_item() : nullptr;
}

bool CanvasView::can_show_hover_cursor(QGraphicsItem *item) const {
    return smart_edit_controller && smart_edit_controller->can_show_hover_cursor(item);
}

ToolController *CanvasView::tool_controller() const { return canvas_scene ? canvas_scene->tool_controller.get() : nullptr; }

void CanvasView::request_handles_repaint() {
    if (handle_overlay_) handle_overlay_->refresh();
}

void CanvasView::on_scene_changed_for_handles() {
    // Returns at once when nothing is being edited, so frequent changes cost nothing.
    if (is_closed_) return;
    update_edit_handles();
}

void CanvasView::resizeEvent(QResizeEvent *event) {
    if (is_closed_) return;
    // The overlay covers the viewport; refresh resizes it.
    QGraphicsView::resizeEvent(event);
    request_handles_repaint();
}

void CanvasView::setCursor(const QCursor &cursor) {
    QGraphicsView::setCursor(cursor);
    if (auto *vp = viewport()) vp->setCursor(cursor);
}

void CanvasView::set_fullscreen_crosshair(bool enabled, CrosshairSurface *overlay) {
    if (crosshair_surface_) crosshair_surface_->set_crosshair_position(std::nullopt);
    const bool was_active = crosshair_active_;
    const bool was_enabled = fullscreen_crosshair_;
    crosshair_surface_ = overlay;
    fullscreen_crosshair_ = enabled && overlay;
    if (fullscreen_crosshair_ != was_enabled) {
        // Back to the cross cursor the moment the selection is confirmed, not on the next mouse move.
        if (fullscreen_crosshair_)
            crosshair_confirm_connection_ = connect(canvas_scene, &CanvasScene::selectionConfirmed, this, &CanvasView::sync_fullscreen_crosshair);
        else
            disconnect(crosshair_confirm_connection_);
    }
    crosshair_active_ = false;
    crosshair_inside_ = viewport()->underMouse();
    crosshair_global_pos_ = QCursor::pos();
    if (was_active) setCursor(QCursor(Qt::CrossCursor));
    sync_fullscreen_crosshair();
}

void CanvasView::sync_fullscreen_crosshair() {
    if (!fullscreen_crosshair_ || syncing_crosshair_ || !crosshair_surface_) return;
    syncing_crosshair_ = true;
    auto *vp = viewport();
    const auto shape = vp->cursor().shape();
    const bool selecting = !canvas_scene->selection_model->is_confirmed();
    if (shape == Qt::CrossCursor && selecting) {
        crosshair_active_ = true;
        vp->setCursor(Qt::BlankCursor);
    } else if (shape != Qt::BlankCursor) {
        crosshair_active_ = false;
    } else if (crosshair_active_ && !selecting) {
        // The blank cursor was set for selecting; a confirmed selection gets the cross cursor back.
        crosshair_active_ = false;
        vp->setCursor(Qt::CrossCursor);
    }
    std::optional<QPoint> position;
    if (crosshair_active_ && crosshair_inside_) position = crosshair_surface_->mapFromGlobal(crosshair_global_pos_);
    crosshair_surface_->set_crosshair_position(position);
    syncing_crosshair_ = false;
}

bool CanvasView::viewportEvent(QEvent *event) {
    if (fullscreen_crosshair_) {
        switch (event->type()) {
        case QEvent::MouseMove:
            crosshair_inside_ = true;
            crosshair_global_pos_ = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
            // Painted before the possibly expensive smart-selection work.
            sync_fullscreen_crosshair();
            break;
        case QEvent::Enter:
            crosshair_inside_ = true;
            crosshair_global_pos_ = QCursor::pos();
            sync_fullscreen_crosshair();
            break;
        case QEvent::Leave: case QEvent::Hide:
            crosshair_inside_ = false;
            sync_fullscreen_crosshair();
            break;
        case QEvent::CursorChange:
            // Scene items set viewport cursors too (resize, move, text).
            sync_fullscreen_crosshair();
            break;
        default: break;
        }
    }
    return QGraphicsView::viewportEvent(event);
}

void CanvasView::cleanup() {
    if (is_closed_) return;
    set_fullscreen_crosshair(false);
    item_drag->finish(true);
    is_closed_ = true;
    for (auto &connection : scene_connections_) QObject::disconnect(connection);
    scene_connections_.clear();
    if (canvas_scene && canvas_scene->tool_controller) canvas_scene->tool_controller->remove_tool_changed_callback(tool_changed_callback_);
    if (smart_edit_controller) {
        if (auto *layer_editor = smart_edit_controller->layer_editor.get()) {
            layer_editor->repaint_requested = nullptr;
            layer_editor->screen_transform = nullptr;
        }
        disconnect(smart_edit_controller.get(), nullptr, this, nullptr);
        smart_edit_controller->cleanup();
    }
    if (cursor_manager) {
        cursor_manager->hide_brush_indicator();
        cursor_manager->current_cursor.reset();
        cursor_manager->view = nullptr;
        cursor_manager->scene = nullptr;
    }
    if (window_finder) {
        window_finder->clear();
        window_finder.reset();
    }
    if (element_finder) {
        element_finder->close();
        element_finder.reset();
    }
    if (smart_selection_anim_) smart_selection_anim_->stop();
    if (canvas_scene && canvas_scene->view == this) canvas_scene->view = nullptr;
    canvas_scene = nullptr;
}

void CanvasView::on_cursor_tool_update_requested(const QString &tool_id, bool force) {
    if (is_closed_ || !cursor_manager) return;
    cursor_manager->set_tool_cursor(tool_id, force);
}

void CanvasView::on_item_auto_select_requested(QGraphicsItem *item) {
    if (is_closed_ || !smart_edit_controller) return;
    smart_edit_controller->select_item(item, true);
}

bool CanvasView::can_apply_tool_cursor() const {
    if (!canvas_scene || !canvas_scene->selection_model->is_confirmed()) return true;
    auto *controller = smart_edit_controller.get();
    if (!controller || !controller->selected_item()) return true;
    if (controller->mode == SelectionMode::CLICKING_HANDLE || controller->mode == SelectionMode::DRAGGING_HANDLE
        || controller->mode == SelectionMode::DRAGGING_MOVE)
        return false;
    const auto view_pos = mapFromGlobal(QCursor::pos());
    if (auto *vp = viewport(); vp && !vp->rect().contains(view_pos)) return true;
    const auto scene_pos = mapToScene(view_pos);
    auto *layer_editor = controller->layer_editor.get();
    if (layer_editor && layer_editor->is_editing() && layer_editor->hit_test(scene_pos)) return false;
    auto *selected = controller->selected_item();
    if (selected->contains(selected->mapFromScene(scene_pos))) return false;
    return true;
}

void CanvasView::enterEvent(QEnterEvent *event) {
    if (is_closed_) return;
    // Reapply the cursor after toolbar clicks, and refresh the magnifier now
    // rather than at the first mouse move.
    if (!canvas_scene->selection_model->is_confirmed()) setCursor(QCursor(Qt::CrossCursor));
    else if (cursor_manager && cursor_manager->current_cursor) cursor_manager->apply_cursor(*cursor_manager->current_cursor);
    if (auto *overlay = get_magnifier_overlay()) {
        const auto global = QCursor::pos();
        overlay->update_cursor(QPointF(global.x(), global.y()));
    }
    QGraphicsView::enterEvent(event);
}

void CanvasView::showEvent(QShowEvent *event) {
    if (is_closed_) return;
    // Do not inherit the previous capture's cursor.
    QGraphicsView::showEvent(event);
    if (!canvas_scene->selection_model->is_confirmed()) {
        QPointer<CanvasView> self(this);
        QTimer::singleShot(0, this, [self] { if (self) self->setCursor(QCursor(Qt::CrossCursor)); });
    }
}

void CanvasView::enable_smart_selection(const QString &requested) {
    QString mode = requested.toLower();
    if (mode != "off" && mode != "window" && mode != "element") mode = "off";
    smart_selection_enabled = mode != "off";
    smart_selection_drills_to_elements = mode == "element";
    // Every capture starts at the finest level.
    smart_selection_level_ = 0;
    smart_selection_level_anchor_.reset();
    if (element_finder) {
        element_finder->close();
        element_finder.reset();
    }
    if (smart_selection_enabled) {
        if (!is_smart_selection_available()) {
            log_warning(T("win32gui 未安装，智能选区功能不可用"), "SmartSelect");
            smart_selection_enabled = false;
            smart_selection_drills_to_elements = false;
            return;
        }
        // The scene uses physical screen coordinates, so no offset.
        if (!window_finder) window_finder = std::make_unique<WindowFinder>(0, 0);
        // Window handles are cached before the overlay shows; UIA roots at them so
        // ElementFromPoint never hits the overlay itself.
        window_finder->find_windows();
        log_debug(T("已启用，找到 {window_count} 个窗口", {{"window_count", static_cast<int>(window_finder->windows.size())}}), "SmartSelect");
        if (smart_selection_drills_to_elements) {
            if (is_uia_available()) {
                element_finder = std::make_unique<UIAElementFinder>(this);
                connect(element_finder.get(), &UIAElementFinder::cache_updated, this, &CanvasView::on_uia_cache_updated);
                const auto cursor_pos = QCursor::pos();
                if (const auto hit = window_finder->find_window_at_point_info(cursor_pos.x(), cursor_pos.y()))
                    element_finder->request_refresh_if_needed(hit->hwnd);
                // The rest by exposed area: scanned while the user moves the pointer.
                element_finder->prewarm(window_finder->windows_by_exposed_area());
            } else {
                smart_selection_drills_to_elements = false;
                log_warning(T("comtypes 未安装，已回退为窗口检测"), "SmartSelect");
            }
        }
    } else {
        log_debug(T("已禁用"), "SmartSelect");
        smart_selection_anim_->stop();
        if (window_finder) window_finder->clear();
    }
}

QRectF CanvasView::get_smart_selection_rect(const QPointF &scene_pos) {
    // Not cached: set_rect short-circuits identical rects and hit testing is cheap.
    if (!smart_selection_enabled || !window_finder) return {};
    const int x = static_cast<int>(scene_pos.x()), y = static_cast<int>(scene_pos.y());
    const auto scene_rect = canvas_scene->scene_rect;
    const SmartRect capture_rect = {static_cast<int>(scene_rect.x()), static_cast<int>(scene_rect.y()),
                                    static_cast<int>(scene_rect.x() + scene_rect.width()), static_cast<int>(scene_rect.y() + scene_rect.height())};
    const auto rect = pick_smart_selection_level(smart_selection_chain(x, y, capture_rect));
    if (!rect) return {};
    return QRectF((*rect)[0], (*rect)[1], (*rect)[2] - (*rect)[0], (*rect)[3] - (*rect)[1]).intersected(scene_rect);
}

std::vector<SmartRect> CanvasView::smart_selection_chain(int x, int y, const SmartRect &capture_rect) {
    // control -> window -> the screen under the pointer, which is always the last level.
    std::vector<SmartRect> chain;
    if (const auto hit = window_finder->find_window_at_point_info(x, y)) {
        for (const auto &rect : element_rects(hit->hwnd, hit->rect, x, y, capture_rect)) append_level(chain, rect);
        append_level(chain, hit->rect);
    }
    const auto screen = window_finder->find_monitor_rect_at_point(x, y);
    append_level(chain, {std::max(screen[0], capture_rect[0]), std::max(screen[1], capture_rect[1]),
                         std::min(screen[2], capture_rect[2]), std::min(screen[3], capture_rect[3])});
    return chain;
}

std::vector<SmartRect> CanvasView::element_rects(quintptr hwnd, const SmartRect &window_rect, int x, int y, const SmartRect &capture_rect) {
    // Until a window's scan returns this level is empty and the window stands in;
    // cache_updated tightens it to the control afterwards.
    if (!smart_selection_drills_to_elements || !element_finder) return {};
    const auto min_size = canvas_scene->selection_model->min_size;
    const auto elements = element_finder->elements_at(hwnd, x, y, QSize(static_cast<int>(min_size.width()), static_cast<int>(min_size.height())));
    element_finder->request_refresh_if_needed(hwnd);
    std::vector<SmartRect> rects;
    for (const auto &element : elements) {
        const SmartRect clipped = {std::max({element.rect[0], window_rect[0], capture_rect[0]}),
                                   std::max({element.rect[1], window_rect[1], capture_rect[1]}),
                                   std::min({element.rect[2], window_rect[2], capture_rect[2]}),
                                   std::min({element.rect[3], window_rect[3], capture_rect[3]})};
        // A clipped control may no longer cover the pointer or may fall below the
        // minimum selection; the next level out takes its place.
        if (clipped[0] <= x && x < clipped[2] && clipped[1] <= y && y < clipped[3]
            && clipped[2] - clipped[0] >= min_size.width() && clipped[3] - clipped[1] >= min_size.height())
            rects.push_back(clipped);
    }
    return rects;
}

std::optional<SmartRect> CanvasView::pick_smart_selection_level(const std::vector<SmartRect> &chain) {
    // The level is anchored to the finest rect: moving within one control keeps
    // the wheel-selected level, moving to another control resets it.
    if (chain.empty()) return std::nullopt;
    const auto anchor = chain.front();
    if (smart_selection_level_anchor_ && *smart_selection_level_anchor_ != anchor) smart_selection_level_ = 0;
    smart_selection_level_anchor_ = anchor;
    smart_selection_level_ = std::min(smart_selection_level_, static_cast<int>(chain.size()) - 1);
    return chain[smart_selection_level_];
}

bool CanvasView::adjust_smart_selection_level(int delta, const QPointF &scene_pos) {
    // The wheel belongs to the granularity whenever control detection is on,
    // regardless of how many levels exist under the pointer.
    if (is_closed_ || !smart_selection_drills_to_elements || !canvas_scene || canvas_scene->selection_model->is_confirmed() || selection_drag->active)
        return false;
    smart_selection_level_ = std::max(0, smart_selection_level_ + (delta > 0 ? 1 : -1));
    handle_hover_preview(scene_pos);
    return true;
}

void CanvasView::on_uia_cache_updated(quintptr hwnd) {
    // Not tightened once the user has wheeled out: the late result would replace
    // the finest level and reset the anchor.
    if (is_closed_ || !smart_selection_enabled || !smart_selection_drills_to_elements || smart_selection_level_ > 0 || !window_finder
        || !canvas_scene || canvas_scene->selection_model->is_confirmed() || selection_drag->active)
        return;
    const auto cursor_pos = QCursor::pos();
    const auto hit = window_finder->find_window_at_point_info(cursor_pos.x(), cursor_pos.y());
    if (hit && hit->hwnd == hwnd) handle_hover_preview(QPointF(cursor_pos.x(), cursor_pos.y()));
}

void CanvasView::set_selection_rect_now(const QRectF &rect) {
    // The tween timer may fire once more after the view closed.
    if (is_closed_ || !canvas_scene) return;
    auto copy = rect;
    canvas_scene->selection_model->set_rect(copy);
}

void CanvasView::apply_smart_selection_rect(const QRectF &rect, bool animate) {
    // Without animation for press and first appearance: an interpolated rect matches no window.
    if (animate && smart_selection_animated) smart_selection_anim_->animate_to(canvas_scene->selection_model->rect(), rect);
    else smart_selection_anim_->snap_to(rect);
}

void CanvasView::on_editing_cleanup() {
    item_drag->finish(true);
    if (smart_edit_controller->selected_item()) {
        log_debug(T("取消智能编辑选择"), "CanvasView");
        smart_edit_controller->clear_selection(true);
    }
    cursor_manager->hide_brush_indicator();
}

void CanvasView::on_tool_changed_for_edit(const QString &tool_id) {
    item_drag->finish(true);
    smart_edit_controller->set_tool(tool_id);
    cursor_manager->set_tool_cursor(tool_id);
    if (cursor_manager->current_cursor) setCursor(*cursor_manager->current_cursor);
}

void CanvasView::on_edit_cursor_change(const QString &cursor_type) {
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (cursor_type == "cross") shape = Qt::CrossCursor;
    else if (cursor_type == "move") shape = Qt::SizeAllCursor;
    else if (cursor_type == "resize") shape = Qt::SizeFDiagCursor;
    setCursor(QCursor(shape));
}

void CanvasView::on_cross_tool_switch_requested(const QString &tool_id) {
    // Must finish before select_item(): select_tool -> set_tool clears the selection.
    // Signals are blocked so the host does not activate the tool a second time.
    auto *toolbar = get_active_toolbar();
    if (toolbar) {
        const bool blocked = toolbar->blockSignals(true);
        toolbar->select_tool(tool_id);
        toolbar->blockSignals(blocked);
    }
    if (canvas_scene) canvas_scene->activate_tool(tool_id);
    if (toolbar) {
        const bool blocked = toolbar->blockSignals(true);
        toolbar->restore_active_tool_state(tool_id, canvas_scene ? &canvas_scene->tool_controller->ctx : nullptr, canvas_scene);
        toolbar->blockSignals(blocked);
    }
}

void CanvasView::on_edit_selection_changed(QGraphicsItem *item) {
    if (item) {
        log_debug(T("选中: {item_type}", {{"item_type", item_type_name(item)}}), "SmartEdit");
    } else {
        item_drag->finish(false);
        log_debug(T("取消选择"), "SmartEdit");
        sync_highlighter_panel_mode();
    }
    sync_selection_style_to_toolbar(item);
}

void CanvasView::sync_highlighter_panel_mode() {
    auto *toolbar = get_active_toolbar();
    auto *controller = tool_controller();
    if (!toolbar || !controller) return;
    if (!controller->current_tool || controller->current_tool->id() != "highlighter") return;
    auto *manager = get_tool_settings_manager();
    const auto mode = manager ? manager->get_tool_settings("highlighter")->get("draw_mode", "freehand").toString() : QString("freehand");
    if (toolbar->paint_panel) toolbar->paint_panel->set_highlighter_mode(mode);
}

void CanvasView::refresh_number_toolbar(int next_number) {
    if (auto *toolbar = get_active_toolbar()) toolbar->set_number_next_value(next_number);
}

Toolbar *CanvasView::get_active_toolbar() const {
    auto *host = canvas_host_of(const_cast<CanvasView *>(this));
    return host ? host->canvas_toolbar() : nullptr;
}

void CanvasView::sync_selection_style_to_toolbar(QGraphicsItem *item) {
    auto *toolbar = get_active_toolbar();
    auto *controller_tools = tool_controller();
    if (!toolbar || !controller_tools) return;
    if (!item) {
        toolbar->restore_active_tool_state(controller_tools->current_tool_id(), &controller_tools->ctx, canvas_scene);
        return;
    }
    auto *controller = smart_edit_controller.get();
    const bool is_cross_tool = controller && controller->is_cross_tool_selection();
    toolbar->set_temporary_edit_active(is_cross_tool);
    const bool is_text_item = dynamic_cast<QGraphicsTextItem *>(item) != nullptr;
    const auto width_value = is_text_item ? std::nullopt : extract_selection_width(item);
    const auto opacity_value = extract_selection_opacity(item);
    std::optional<double> width, opacity;
    if (width_value) width = std::max(1.0, *width_value);
    if (opacity_value) opacity = std::max(0.0, std::min(1.0, *opacity_value));
    if ((width || opacity) && !is_cross_tool) {
        controller_tools->update_style(std::nullopt, width, opacity);
        if (width && cursor_manager) cursor_manager->update_tool_cursor_size(static_cast<int>(std::max(1.0, std::nearbyint(controller_tools->ctx.stroke_width))));
    }
    if (width_value) toolbar->set_stroke_width(static_cast<int>(std::nearbyint(*width_value)));
    if (opacity_value) toolbar->set_opacity(static_cast<int>(std::nearbyint(*opacity_value * 255)));
    // Line style follows the owning tool, not the class: highlighter rects and
    // spotlight holes are RectItems too.
    const auto owner_tool_id = controller ? controller->get_item_tool_id(item) : std::nullopt;
    LineStylePanel *line_style_panel = nullptr;
    if (owner_tool_id == "pen" || owner_tool_id == "highlighter") line_style_panel = toolbar->paint_panel;
    else if (owner_tool_id == "rect" || owner_tool_id == "ellipse") line_style_panel = toolbar->shape_panel;
    if (line_style_panel) {
        QPen pen;
        if (auto *stroke = dynamic_cast<QAbstractGraphicsShapeItem *>(item)) pen = stroke->pen();
        const auto pen_style = pen.style();
        QList<double> dash_pattern;
        for (const auto value : pen.dashPattern()) dash_pattern.append(std::nearbyint(value * 10) / 10);
        const bool has_dash = !dash_pattern.isEmpty();
        QString line_style = "solid";
        if (has_dash && dash_pattern.size() >= 2 && dash_pattern[0] == 1.0 && dash_pattern[1] == 2.0) line_style = "dashed_dense";
        else if (pen_style == Qt::DashLine || pen_style == Qt::CustomDashLine || has_dash) line_style = "dashed";
        line_style_panel->set_line_style(line_style);
        if (!is_cross_tool) get_tool_settings_manager()->update_settings(*owner_tool_id, {{"line_style", line_style}});
    }
    show_panel_for_selection(item, toolbar);
}

void CanvasView::show_panel_for_selection(QGraphicsItem *item, Toolbar *toolbar) {
    // Panel defaults first, then this item's state on top; the reverse order
    // would be overwritten by the panel's own refill on show.
    std::optional<QString> tool_id;
    if (dynamic_cast<QGraphicsTextItem *>(item)) tool_id = "text";
    else if (smart_edit_controller) tool_id = smart_edit_controller->get_item_tool_id(item);
    if (!tool_id) return;
    toolbar->show_panel_for_tool(*tool_id);
    if (*tool_id == "text") {
        if (toolbar->text_panel) toolbar->text_panel->set_state_from_item(dynamic_cast<QGraphicsTextItem *>(item));
    } else if (*tool_id == "arrow") {
        if (auto *arrow = dynamic_cast<ArrowItem *>(item); arrow && toolbar->arrow_panel) {
            toolbar->arrow_panel->set_arrow_style(arrow->_arrow_style);
            toolbar->arrow_panel->set_color(arrow->color);
        }
    } else if (*tool_id == "pen" || *tool_id == "highlighter") {
        if (toolbar->paint_panel) {
            toolbar->paint_panel->set_state_from_item(item);
            if (dynamic_cast<RectItem *>(item)) toolbar->paint_panel->set_highlighter_mode("rect");
        }
    } else if (*tool_id == "rect" || *tool_id == "ellipse") {
        if (toolbar->shape_panel) toolbar->shape_panel->set_state_from_item(item);
    } else if (*tool_id == "number") {
        if (auto *number = dynamic_cast<NumberItem *>(item); number && toolbar->number_panel) toolbar->number_panel->set_style(number->style);
    } else if (*tool_id == "mosaic") {
        if (auto *mosaic = dynamic_cast<MosaicItem *>(item); mosaic && toolbar->mosaic_panel) {
            toolbar->mosaic_panel->set_draw_mode(mosaic->fill_mode() ? "rect" : "freehand");
            toolbar->mosaic_panel->set_style(mosaic->smooth() ? "blur" : "pixelate");
            // The panel ignores a click on its highlighted level, so it must show this item's level.
            toolbar->mosaic_panel->set_block_size(mosaic->block_size());
        }
    }
}

std::optional<double> CanvasView::extract_selection_width(QGraphicsItem *item) {
    if (auto *drawing_item = drawing_mixin(item)) return drawing_item->get_stroke_width();
    return std::nullopt;
}

std::optional<double> CanvasView::extract_selection_opacity(QGraphicsItem *item) {
    if (!item) return std::nullopt;
    if (auto *drawing_item = drawing_mixin(item))
        if (const auto result = drawing_item->get_visual_opacity()) return result;
    return std::max(0.0, std::min(1.0, item->opacity()));
}

std::optional<bool> CanvasView::apply_cross_tool_selection_style(const std::optional<QColor> &color, const std::optional<double> &width,
                                                                 const std::optional<double> &opacity) {
    auto *controller = smart_edit_controller.get();
    if (!controller || !controller->is_cross_tool_selection()) return std::nullopt;
    auto *item = controller->selected_item();
    if (!item) return std::nullopt;
    bool applied = false;
    if (width) {
        const auto current_width = extract_selection_width(item);
        auto *drawing_item = drawing_mixin(item);
        if (!current_width || *current_width <= 0 || !drawing_item) return false;
        applied = drawing_item->scale_stroke_width(std::max(1.0, *width) / *current_width);
    }
    if (opacity) applied = update_item_visual_opacity(item, *opacity) || applied;
    if (color) {
        if (dynamic_cast<TextItem *>(item)) {
            // Text colour arrives through SmartEditController's own slot.
            applied = true;
        } else if (auto *stroke = dynamic_cast<StrokeItem *>(item)) {
            QPen pen(stroke->pen());
            QColor new_color(*color);
            new_color.setAlpha(pen.color().alpha());
            pen.setColor(new_color);
            stroke->setPen(pen);
            stroke->update();
            applied = true;
        } else if (auto *rect = dynamic_cast<RectItem *>(item)) {
            QPen pen(rect->pen());
            QColor new_color(*color);
            new_color.setAlpha(pen.color().alpha());
            pen.setColor(new_color);
            rect->setPen(pen);
            if (rect->is_highlighter_rect) {
                QBrush brush(rect->brush());
                QColor brush_color(*color);
                brush_color.setAlpha(brush.color().alpha());
                brush.setColor(brush_color);
                rect->setBrush(brush);
            }
            rect->update();
            applied = true;
        } else if (auto *ellipse = dynamic_cast<EllipseItem *>(item)) {
            QPen pen(ellipse->pen());
            QColor new_color(*color);
            new_color.setAlpha(pen.color().alpha());
            pen.setColor(new_color);
            ellipse->setPen(pen);
            ellipse->update();
            applied = true;
        } else if (auto *arrow = dynamic_cast<ArrowItem *>(item)) {
            QColor new_color(*color);
            new_color.setAlpha(arrow->color.alpha());
            arrow->color = new_color;
            arrow->update_geometry();
            arrow->update();
            applied = true;
        } else if (auto *number = dynamic_cast<NumberItem *>(item)) {
            QColor new_color(*color);
            new_color.setAlpha(number->color.alpha());
            number->color = new_color;
            number->update();
            applied = true;
        } else {
            return false;
        }
    }
    if (applied) {
        auto *editor = controller->layer_editor.get();
        if (editor && editor->is_editing() && !dynamic_cast<TextItem *>(item)) editor->start_edit(item);
        canvas_scene->update();
    }
    return applied;
}

bool CanvasView::route_capture_right_button(QMouseEvent *event, bool dispatch) {
    // Right clicks in the capture window only run mouse actions; an unbound
    // release is swallowed too, so it cannot end a left-button drag.
    if (event->button() != Qt::RightButton) return false;
    auto *host = canvas_host_of(this);
    if (!host || !host->has_capture_right_click_handler()) return false;
    event->accept();
    if (dispatch) host->handle_capture_right_click(event);
    return true;
}

void CanvasView::contextMenuEvent(QContextMenuEvent *event) {
    if (is_closed_) return;
    // The text item's own menu would steal focus and end editing early.
    if (auto *host = canvas_host_of(this); host && host->has_capture_right_click_handler()) {
        event->accept();
        return;
    }
    QGraphicsView::contextMenuEvent(event);
}

void CanvasView::mousePressEvent(QMouseEvent *event) {
    if (is_closed_) return;
    if (route_capture_right_button(event, true)) return;
    const auto scene_pos = mapToScene(event->pos());
    double_click_candidate_.reset();
    pending_text_edit->clear();
    if (!canvas_scene->selection_model->is_confirmed()) {
        selection_drag->begin(scene_pos);
        return;
    }
    // Snapshot the reversible state before routing; it only becomes a double-click
    // candidate once the text and handle branches have passed.
    auto double_click_candidate = build_double_click_candidate(scene_pos, event);
    const auto current_tool_id = canvas_scene->tool_controller->current_tool_id();
    log_debug(T("选区已确认，当前工具: {current_tool_id}", {{"current_tool_id", current_tool_id}}), "CanvasView");
    // Step 0: while editing text, a click outside settles the edit instead of
    // creating new text; a click on other text switches to it in the same click.
    if (is_text_editing()) {
        auto *focus_item = get_active_text_item();
        if (!focus_item) return;
        if (smart_edit_controller->handle_edit_press(scene_pos, event->pos(), event->button(), event->modifiers())) {
            log_debug(T("文字宽度控制点拖拽被处理"), "CanvasView");
            return;
        }
        if (TextEdgeDrag::is_point_on_edge(focus_item, scene_pos)) {
            text_drag->begin(focus_item, scene_pos);
            return;
        }
        if (focus_item->contains(focus_item->mapFromScene(scene_pos))) {
            QGraphicsView::mousePressEvent(event);
            return;
        }
        auto *switch_target = text_switch_target(scene_pos, focus_item, event->modifiers());
        end_text_edit(focus_item);
        if (!switch_target || !switch_target->scene()) return;
        // This click switches text, so it cannot also confirm the capture.
        double_click_candidate.reset();
    }
    // Step 1: handles of the selected item.
    if (smart_edit_controller->handle_edit_press(scene_pos, event->pos(), event->button(), event->modifiers())) {
        log_debug(T("控制点拖拽被处理"), "CanvasView");
        auto *layer_editor = smart_edit_controller->layer_editor.get();
        if (layer_editor->hovered_handle()) setCursor(layer_editor->get_cursor(scene_pos));
        return;
    }
    // Neither text editing nor a handle: rapid +/- clicks never confirm the capture.
    double_click_candidate_ = double_click_candidate;
    // Step 2: selectable items.
    if (smart_edit_controller->handle_press(event->pos(), scene_pos, event->button(), event->modifiers())) {
        log_debug(T("图元选择被处理，阻止绘图"), "CanvasView");
        pending_text_edit->arm(event, scene_pos);
        if (smart_edit_controller->press_requires_manual_dispatch) {
            // Qt would give the gesture to the topmost item; the controller chose one below it.
            item_drag->begin(scene_pos);
            event->accept();
            return;
        }
        QGraphicsView::mousePressEvent(event);
        return;
    }
    // A click that only cleared a selection does not draw.
    if (smart_edit_controller->just_cleared_selection) {
        smart_edit_controller->just_cleared_selection = false;
        log_debug(T("刚清除选择，跳过本次绘图"), "CanvasView");
        return;
    }
    // Step 3: drawing.
    if (current_tool_id != "cursor") {
        log_debug(T("开始绘图"), "CanvasView");
        drawing->begin(scene_pos, event->button());
    } else {
        log_debug(T("cursor工具，传递给Scene"), "CanvasView");
        QGraphicsView::mousePressEvent(event);
    }
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (is_closed_) return;
    // Windows turns the second fast press into a double click; route it as a
    // press so number +/- handles do not lose clicks.
    if (route_capture_right_button(event, true)) return;
    if (!canvas_scene || !canvas_scene->selection_model->is_confirmed()) {
        QGraphicsView::mouseDoubleClickEvent(event);
        return;
    }
    const auto scene_pos = mapToScene(event->pos());
    // Consumed before provisional content could expose handles under the second click.
    if (consume_double_click_candidate(scene_pos, event)) return;
    if (smart_edit_controller->handle_edit_press(scene_pos, event->pos(), event->button(), event->modifiers())) {
        double_click_candidate_.reset();
        log_debug(T("双击→控制点点击被处理"), "CanvasView");
        auto *layer_editor = smart_edit_controller->layer_editor.get();
        if (layer_editor->hovered_handle()) setCursor(layer_editor->get_cursor(scene_pos));
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

std::optional<CanvasView::DoubleClickCandidate> CanvasView::build_double_click_candidate(const QPointF &scene_pos, QMouseEvent *event) {
    if (!confirm_on_double_click || event->button() != Qt::LeftButton) return std::nullopt;
    auto *host = canvas_host_of(this);
    const auto matches = host ? host->matches_capture_double_click(event) : std::nullopt;
    if (matches) {
        if (!*matches) return std::nullopt;
    } else if (event->modifiers() != Qt::NoModifier) {
        return std::nullopt;
    }
    if (selection_drag->active || drawing->active || selection_drag->dragging || text_drag->active) return std::nullopt;
    const auto selection = canvas_scene->selection_model->rect();
    if (selection.isEmpty() || !selection.contains(scene_pos)) return std::nullopt;
    auto *tool = canvas_scene->tool_controller->current_tool;
    auto *stack = canvas_scene->undo_stack;
    return DoubleClickCandidate{scene_pos, selection, tool ? tool->id() : QString(), stack->count(), stack->index(), stack->canRedo(), false};
}

bool CanvasView::consume_double_click_candidate(const QPointF &scene_pos, QMouseEvent *event) {
    const auto candidate = double_click_candidate_;
    double_click_candidate_.reset();
    if (!confirm_on_double_click || !candidate) return false;
    if (event->button() != Qt::LeftButton) return false;
    auto *host = canvas_host_of(this);
    const auto matches = host ? host->matches_capture_double_click(event) : std::nullopt;
    if (matches) {
        if (!*matches) return false;
    } else if (event->modifiers() != Qt::NoModifier) {
        return false;
    }
    if (candidate->dragged || drawing->active || selection_drag->active) return false;
    if (!candidate->selection_rect.contains(scene_pos)) return false;
    if ((scene_pos - candidate->scene_pos).manhattanLength() > QApplication::startDragDistance()) return false;
    auto *tool = canvas_scene->tool_controller->current_tool;
    if ((tool ? tool->id() : QString()) != candidate->tool_id) return false;
    const bool has_handler = host && host->has_double_click_handler();
    const bool has_confirm = host && host->has_confirm_handler();
    if (!has_handler && !has_confirm) return false;
    // The undo stack tells what the first click left, except text, which settles
    // on focus-out; editing here was started by the first click, so end it first.
    if (auto *active_text_item = get_active_text_item()) end_text_edit(active_text_item);
    auto *stack = canvas_scene->undo_stack;
    const QUndoCommand *command_to_undo = nullptr;
    const int old_count = candidate->count, old_index = candidate->index;
    if (stack->index() == old_index && stack->count() == old_count) {
        // The first click created no command.
    } else if (stack->index() == old_index + 1 && stack->count() == (candidate->had_redo ? old_index + 1 : old_count + 1)) {
        command_to_undo = stack->command(old_index);
        const bool reversible = dynamic_cast<const AddItemCommand *>(command_to_undo) || dynamic_cast<const BatchRemoveCommand *>(command_to_undo)
                             || dynamic_cast<const EditItemCommand *>(command_to_undo);
        if (!reversible) return false;
        if (auto *add = dynamic_cast<const AddItemCommand *>(command_to_undo); add && !dynamic_cast<const RemoveItemCommand *>(add)) {
            if (auto *text = add->item().as<TextItem>(); text && !text->toPlainText().trimmed().isEmpty()) return false;
        }
    } else {
        return false;
    }
    if (command_to_undo) {
        stack->undo();
        if (stack->index() != old_index) return false;
    }
    // A crop handle can change the selection without an undo command.
    auto original = candidate->selection_rect;
    canvas_scene->selection_model->set_rect(original);
    canvas_scene->selection_model->stop_dragging();
    selection_drag->dragging = false;
    event->accept();
    if (has_handler) return host->handle_double_click(event);
    host->handle_confirm();
    return true;
}

void CanvasView::mouseMoveEvent(QMouseEvent *event) {
    if (is_closed_) return;
    if (double_click_candidate_ && !double_click_candidate_->dragged) {
        const auto scene_pos = mapToScene(event->pos());
        if ((scene_pos - double_click_candidate_->scene_pos).manhattanLength() > QApplication::startDragDistance()) double_click_candidate_->dragged = true;
    }
    // Stray events after the window closed.
    if (!canvas_scene || !scene()) return;
    const auto scene_pos = mapToScene(event->pos());
    if (selection_drag->active) { selection_drag->perform(scene_pos); return; }
    if (drawing->active) { drawing->perform(scene_pos); return; }
    if (text_drag->active) { handle_text_drag_move(scene_pos); return; }
    if (canvas_scene->selection_model->is_confirmed()) { handle_edit_mode_move(event, scene_pos); return; }
    handle_hover_preview(scene_pos);
}

void CanvasView::handle_text_drag_move(const QPointF &scene_pos) {
    update_magnifier_overlay(scene_pos);
    text_drag->set_cursor(true);
    text_drag->perform(scene_pos);
}

void CanvasView::handle_edit_mode_move(QMouseEvent *event, const QPointF &scene_pos) {
    pending_text_edit->track(event);
    update_magnifier_overlay(scene_pos);
    // With the left button held the user may be selecting text, so no edge hover then.
    const bool is_left_pressed = event->buttons() & Qt::LeftButton;
    if (is_text_editing() && !is_left_pressed) text_drag->update_hover(scene_pos);
    // A manual drag owns the gesture ahead of the top item's handles and hover.
    if (item_drag->active && is_left_pressed) {
        handle_selected_item_drag(event, scene_pos);
        return;
    }
    if (handle_edit_handle_drag(scene_pos)) return;
    if (handle_edit_handle_hover(event, scene_pos)) return;
    if (handle_selected_item_drag(event, scene_pos)) return;
    handle_edit_hover_detection(event, scene_pos);
}

bool CanvasView::handle_edit_handle_drag(const QPointF &scene_pos) {
    if (!smart_edit_controller->handle_edit_move(scene_pos)) return false;
    auto *layer_editor = smart_edit_controller->layer_editor.get();
    if (layer_editor->dragging_handle) setCursor(layer_editor->get_cursor(scene_pos));
    else if (layer_editor->hovered_handle()) setCursor(layer_editor->hovered_handle()->cursor);
    return true;
}

bool CanvasView::handle_edit_handle_hover(QMouseEvent *event, const QPointF &scene_pos) {
    if (!smart_edit_controller->selected_item()) return false;
    auto *layer_editor = smart_edit_controller->layer_editor.get();
    if (!layer_editor->is_editing()) return false;
    const bool is_left_pressed = event->buttons() & Qt::LeftButton;
    if (is_left_pressed && layer_editor->is_moving_item()) return false;
    layer_editor->update_hover(scene_pos);
    if (layer_editor->hovered_handle()) {
        setCursor(layer_editor->get_cursor(scene_pos));
        return true;
    }
    return false;
}

bool CanvasView::handle_selected_item_drag(QMouseEvent *event, const QPointF &scene_pos) {
    auto *selected = smart_edit_controller->selected_item();
    if (!selected) return false;
    const bool is_on_selected_item = selected->contains(selected->mapFromScene(scene_pos));
    const bool is_left_pressed = event->buttons() & Qt::LeftButton;
    if (item_drag->active && is_left_pressed) {
        item_drag->perform(event, scene_pos);
        return true;
    }
    if (is_left_pressed) {
        // Dragging inside text being edited selects text instead of moving the item.
        if (is_text_editing() && dynamic_cast<QGraphicsTextItem *>(selected)) {
            setCursor(QCursor(Qt::IBeamCursor));
            QGraphicsView::mouseMoveEvent(event);
            return true;
        }
        smart_edit_controller->handle_move(event->pos(), scene_pos);
        setCursor(QCursor(Qt::SizeAllCursor));
        QGraphicsView::mouseMoveEvent(event);
        update_edit_handles();
        return true;
    }
    if (is_on_selected_item) {
        // Text being edited sets its own cursor in hoverMoveEvent.
        if (is_text_editing() && dynamic_cast<QGraphicsTextItem *>(selected)) {
            QGraphicsView::mouseMoveEvent(event);
            return true;
        }
        setCursor(QCursor(Qt::CrossCursor));
        QGraphicsView::mouseMoveEvent(event);
        return true;
    }
    return false;
}

void CanvasView::handle_edit_hover_detection(QMouseEvent *event, const QPointF &scene_pos) {
    if (smart_edit_controller->handle_hover(event->pos(), scene_pos)) setCursor(QCursor(Qt::CrossCursor));
    else apply_tool_cursor();
    QGraphicsView::mouseMoveEvent(event);
}

void CanvasView::handle_hover_preview(const QPointF &scene_pos) {
    update_magnifier_overlay(scene_pos);
    auto *model = canvas_scene->selection_model;
    if (smart_selection_enabled) {
        const auto smart_rect = get_smart_selection_rect(scene_pos);
        if (!smart_rect.isEmpty()) {
            model->activate();
            if (!model->is_dragging()) model->start_dragging();
            apply_smart_selection_rect(smart_rect, true);
            return;
        }
    }
    smart_selection_anim_->stop();
    if (model->is_dragging()) model->stop_dragging();
}

void CanvasView::apply_tool_cursor() {
    if (cursor_manager && cursor_manager->current_cursor && cursor_manager->current_tool_id != "cursor") setCursor(*cursor_manager->current_cursor);
}

void CanvasView::leaveEvent(QEvent *event) {
    if (is_closed_) return;
    clear_magnifier_overlay();
    QGraphicsView::leaveEvent(event);
}

void CanvasView::update_edit_handles() {
    // Only the overlay repaints; Qt invalidates the item itself.
    if (!smart_edit_controller) return;
    auto *layer_editor = smart_edit_controller->layer_editor.get();
    if (layer_editor && layer_editor->is_editing())
        if (auto *item = smart_edit_controller->selected_item()) {
            layer_editor->handles = layer_editor->generate_handles(item);
            request_handles_repaint();
        }
}

void CanvasView::mouseReleaseEvent(QMouseEvent *event) {
    if (is_closed_) return;
    if (route_capture_right_button(event, false)) return;
    const auto scene_pos = mapToScene(event->pos());
    if (text_drag->active) { text_drag->end(); return; }
    if (selection_drag->active) { selection_drag->end(); return; }
    if (drawing->active) { drawing->end(scene_pos); return; }
    if (item_drag->active) {
        item_drag->finish(true);
        if (smart_edit_controller->selected_item()) update_edit_handles();
        event->accept();
        return;
    }
    if (smart_edit_controller->handle_edit_release(scene_pos, event->button())) return;
    smart_edit_controller->handle_release(event->pos(), scene_pos, event->button());
    if (smart_edit_controller->selected_item()) update_edit_handles();
    pending_text_edit->settle(event, scene_pos);
    QGraphicsView::mouseReleaseEvent(event);
}

void CanvasView::wheelEvent(QWheelEvent *event) {
    if (is_closed_) return;
    invalidate_double_click_candidate();
    auto *controller_tools = canvas_scene->tool_controller.get();
    auto *current_tool = controller_tools->current_tool;
    const int delta = event->angleDelta().y();
    if (controller_tools->current_tool_id() == "cursor") {
        // With control detection the wheel walks granularity; the magnifier zoom has its own keys.
        if (delta != 0 && adjust_smart_selection_level(delta, mapToScene(event->position().toPoint()))) {
            event->accept();
            return;
        }
        // Pin windows have no magnifier.
        if (auto *overlay = get_magnifier_overlay(); overlay && overlay->cursor_scene_pos && overlay->should_render() && delta != 0) {
            overlay->adjust_zoom(delta > 0 ? 1 : -1);
            event->accept();
            return;
        }
        QGraphicsView::wheelEvent(event);
        return;
    }
    const auto modifiers = event->modifiers();
    auto *toolbar = get_active_toolbar();
    // A temporary cross-tool edit changes only the item, never tool defaults.
    // Shift also means width here, so a number tool's next value is not touched.
    if (smart_edit_controller && smart_edit_controller->is_cross_tool_selection()) {
        auto *item = smart_edit_controller->selected_item();
        if (delta != 0) {
            if (modifiers & Qt::ControlModifier) {
                if (const auto current_opacity = extract_selection_opacity(item)) {
                    const double new_opacity = std::max(0.0, std::min(1.0, *current_opacity + (delta > 0 ? 0.05 : -0.05)));
                    apply_cross_tool_selection_style(std::nullopt, std::nullopt, new_opacity);
                    if (toolbar) toolbar->set_opacity(static_cast<int>(std::nearbyint(new_opacity * 255)));
                }
            } else if (const auto current_width = extract_selection_width(item)) {
                const double next = *current_width + (delta > 0 ? 1.0 : -1.0);
                const double new_width = dynamic_cast<NumberItem *>(item) ? Tool::clamp_width_range(next, NumberTool::MIN_WIDTH, NumberTool::MAX_WIDTH)
                                                                          : Tool::clamp_width_range(next, Tool::MIN_WIDTH, Tool::MAX_WIDTH);
                apply_cross_tool_selection_style(std::nullopt, new_width);
                if (toolbar) toolbar->set_stroke_width(static_cast<int>(std::nearbyint(new_width)));
            }
        }
        event->accept();
        return;
    }
    // Ctrl + wheel: opacity.
    if (modifiers & Qt::ControlModifier) {
        if (delta != 0) {
            const double current_opacity = controller_tools->ctx.opacity;
            const double new_opacity = std::max(0.0, std::min(1.0, current_opacity + (delta > 0 ? 0.05 : -0.05)));
            canvas_scene->update_style(std::nullopt, std::nullopt, new_opacity);
            apply_opacity_change_to_selection(new_opacity);
            if (toolbar) toolbar->set_opacity(static_cast<int>(std::nearbyint(new_opacity * 255)));
        }
        event->accept();
        return;
    }
    // Shift + wheel: the number tool's next value.
    if (current_tool->id() == "number" && (modifiers & Qt::ShiftModifier)) {
        if (delta != 0) {
            const int next_value = NumberTool::adjust_next_number(canvas_scene, delta > 0 ? 1 : -1);
            NumberSequence::set_next_number_and_refresh(canvas_scene, next_value);
        }
        event->accept();
        return;
    }
    if (current_tool->id() == "text") {
        auto *active_text = get_active_text_item();
        QGraphicsTextItem *selected_text = dynamic_cast<QGraphicsTextItem *>(smart_edit_controller->selected_item());
        // Editing or selected text first, then the panel, then 16.
        double current_size = 16;
        if (active_text) current_size = get_text_point_size(active_text);
        else if (selected_text) current_size = get_text_point_size(selected_text);
        else if (toolbar && toolbar->text_panel) current_size = toolbar->text_panel->size_spin->value();
        const double new_size = delta > 0 ? std::min(current_size + 2, 144.0) : std::max(current_size - 2, 8.0);
        apply_text_point_size(active_text, selected_text, new_size);
        if (toolbar && toolbar->text_panel) toolbar->text_panel->size_spin->setValue(static_cast<int>(new_size));
        event->accept();
        return;
    }
    const double current_width = std::max(1.0, controller_tools->ctx.stroke_width);
    // Same range as the slider: the current tool's clamp_width.
    const double new_width = current_tool->clamp_width(current_width + (delta > 0 ? 1.0 : -1.0));
    controller_tools->update_style(std::nullopt, static_cast<double>(static_cast<int>(new_width)));
    cursor_manager->update_tool_cursor_size(static_cast<int>(new_width));
    const double scale = current_width > 0 ? new_width / current_width : 1.0;
    if (std::abs(scale - 1.0) > 1e-6) apply_size_change_to_selection(scale);
    if (toolbar) toolbar->set_stroke_width(static_cast<int>(new_width));
    event->accept();
}

void CanvasView::keyPressEvent(QKeyEvent *event) {
    if (is_closed_) return;
    // Only text editing keys stay here; window shortcuts (Esc, Enter, Ctrl+Z/Y,
    // Ctrl+C/D) bubble up to the screenshot window.
    invalidate_double_click_candidate();
    if (is_text_editing()) {
        // Enter inserts a line; Ctrl+Z/Y is the text item's own undo; all keys go to the item.
        QGraphicsView::keyPressEvent(event);
        update_edit_handles();
        return;
    }
    event->ignore();
}

void CanvasView::inputMethodEvent(QInputMethodEvent *event) {
    if (is_closed_) return;
    // IME composition between clicks invalidates the confirming double click.
    invalidate_double_click_candidate();
    QGraphicsView::inputMethodEvent(event);
    update_edit_handles();
}

QGraphicsTextItem *CanvasView::get_active_text_item() const {
    auto *focus_item = canvas_scene ? dynamic_cast<QGraphicsTextItem *>(canvas_scene->focusItem()) : nullptr;
    if (focus_item && focus_item->hasFocus() && (focus_item->textInteractionFlags() & Qt::TextEditorInteraction)) return focus_item;
    return nullptr;
}

QGraphicsItem *CanvasView::text_switch_target(const QPointF &scene_pos, QGraphicsItem *editing_item, Qt::KeyboardModifiers modifiers) {
    // Same rule as handle_press via can_select_item; a hit on another kind of item only ends editing.
    auto *controller = smart_edit_controller.get();
    if (!controller) return nullptr;
    for (auto *item : canvas_scene->items(scene_pos)) {
        if (item == editing_item) continue;
        if (!controller->can_select_item(item, modifiers)) continue;
        return dynamic_cast<TextItem *>(item) ? item : nullptr;
    }
    return nullptr;
}

void CanvasView::apply_size_change_to_selection(double scale) {
    auto *controller = smart_edit_controller.get();
    if (!controller || scale <= 0) return;
    bool handled_text = false;
    auto *active_text = get_active_text_item();
    if (active_text) {
        scale_text_item(active_text, scale);
        handled_text = true;
    }
    if (auto *selected = controller->selected_item()) {
        if (handled_text && selected == active_text) return;
        if (scale_item_size(selected, scale)) {
            auto *editor = controller->layer_editor.get();
            if (editor && editor->is_editing() && controller->selected_item() == selected) editor->start_edit(selected);
            canvas_scene->update();
        }
    }
}

bool CanvasView::scale_item_size(QGraphicsItem *item, double scale) {
    if (auto *drawing_item = drawing_mixin(item)) return drawing_item->scale_stroke_width(scale);
    if (auto *text = dynamic_cast<QGraphicsTextItem *>(item)) {
        scale_text_item(text, scale);
        return true;
    }
    return false;
}

void CanvasView::scale_text_item(QGraphicsTextItem *item, double scale) {
    set_text_item_point_size(item, std::max(6.0, get_text_point_size(item) * scale));
}

double CanvasView::get_text_point_size(QGraphicsTextItem *item) {
    const auto font = item->font();
    double point_size = font.pointSizeF();
    if (point_size <= 0) point_size = font.pointSize() ? font.pointSize() : 12;
    return point_size;
}

void CanvasView::set_text_item_point_size(QGraphicsTextItem *item, double point_size) {
    if (!item) return;
    auto font = item->font();
    font.setPointSizeF(std::max(6.0, point_size));
    item->setFont(font);
    item->update();
}

void CanvasView::apply_text_point_size(QGraphicsTextItem *active_item, QGraphicsTextItem *selected_item, double point_size) {
    bool applied = false;
    if (active_item) {
        set_text_item_point_size(active_item, point_size);
        applied = true;
    }
    if (selected_item && selected_item != active_item) {
        set_text_item_point_size(selected_item, point_size);
        applied = true;
    }
    if (applied) canvas_scene->update();
}

void CanvasView::apply_opacity_change_to_selection(double opacity) {
    auto *controller = smart_edit_controller.get();
    if (!controller) return;
    opacity = std::max(0.0, std::min(1.0, opacity));
    bool updated = false;
    auto *active_text = get_active_text_item();
    if (active_text && update_item_visual_opacity(active_text, opacity)) updated = true;
    if (auto *selected = controller->selected_item(); selected && selected != active_text && update_item_visual_opacity(selected, opacity)) updated = true;
    if (updated) canvas_scene->update();
}

void CanvasView::apply_line_style_change_to_selection(const QString &style) {
    auto *controller = smart_edit_controller.get();
    if (!controller) return;
    auto *selected = controller->selected_item();
    // Only pen, rect and ellipse have line styles; ownership, not class, decides.
    const auto owner = selected ? controller->get_item_tool_id(selected) : std::nullopt;
    if (!owner || (*owner != "pen" && *owner != "rect" && *owner != "ellipse")) return;
    auto *shape = dynamic_cast<QAbstractGraphicsShapeItem *>(selected);
    if (!shape) return;
    QPen pen = shape->pen();
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    apply_line_style(pen, style);
    if (auto *stroke = dynamic_cast<StrokeItem *>(selected)) stroke->setPen(pen);
    else if (auto *rect = dynamic_cast<RectItem *>(selected)) rect->setPen(pen);
    else if (auto *ellipse = dynamic_cast<EllipseItem *>(selected)) ellipse->setPen(pen);
    selected->update();
    canvas_scene->update();
}

bool CanvasView::update_item_visual_opacity(QGraphicsItem *item, double opacity) {
    opacity = std::max(0.0, std::min(1.0, opacity));
    if (auto *drawing_item = drawing_mixin(item)) return drawing_item->set_visual_opacity(opacity);
    item->setOpacity(opacity);
    item->update();
    return true;
}

void CanvasView::update_magnifier_overlay(const QPointF &scene_pos) {
    if (auto *overlay = get_magnifier_overlay()) overlay->update_cursor(scene_pos);
}

void CanvasView::clear_magnifier_overlay() {
    if (auto *overlay = get_magnifier_overlay()) overlay->clear_cursor();
}

MagnifierOverlay *CanvasView::get_magnifier_overlay() const {
    auto *host = canvas_host_of(const_cast<CanvasView *>(this));
    return host ? host->canvas_magnifier_overlay() : nullptr;
}

void CanvasView::enter_text_edit_mode(TextItem *item) {
    connect_text_geometry_updates(item);
    item->setTextInteractionFlags(Qt::TextEditorInteraction);
    item->setFocus(Qt::MouseFocusReason);
    // Caret at the end rather than selecting everything.
    auto cursor = item->textCursor();
    cursor.movePosition(QTextCursor::End);
    item->setTextCursor(cursor);
    smart_edit_controller->select_item(item, false);
}

void CanvasView::connect_text_geometry_updates(QGraphicsTextItem *item) {
    // Content changes move the bounds, so the handles must follow.
    auto *document = item->document();
    if (!document || text_geometry_connections_.count(item)) return;
    text_geometry_connections_[item] = connect(document, &QTextDocument::contentsChanged, this, [this, item] {
        if (!item->scene()) return;
        update_edit_handles();
    });
}

void CanvasView::disconnect_text_geometry_updates(QGraphicsTextItem *item) {
    const auto found = text_geometry_connections_.find(item);
    if (found == text_geometry_connections_.end()) return;
    QObject::disconnect(found->second);
    text_geometry_connections_.erase(found);
}

void CanvasView::end_text_edit(QGraphicsTextItem *text_item) {
    log_debug(T("结束文本编辑"), "CanvasView");
    text_item->clearFocus();
    finalize_text_edit_state(text_item);
}

void CanvasView::finalize_text_edit_state(QGraphicsTextItem *text_item) {
    if (text_item) disconnect_text_geometry_updates(text_item);
    if (auto *controller = smart_edit_controller.get(); controller && controller->selected_item() == text_item) controller->clear_selection(true);
    text_drag->reset();
    pending_text_edit->clear();
}

void CanvasView::export_and_close() {
    ExportService exporter(canvas_scene);
    const auto selection_rect = canvas_scene->selection_model->rect();
    log_debug(T("准备导出选区: {selection_rect}", {{"selection_rect", py_repr(selection_rect)}}), "CanvasView");
    const auto result = exporter.export_image(selection_rect);
    if (!result.isNull()) {
        log_info(T("导出成功，图像大小: {width}x{height}", {{"width", result.width()}, {"height", result.height()}}), "CanvasView");
        deliver_image_async(result);
        log_info(T("已完成复制到剪贴板"), "CanvasView");
        window()->close();
    } else {
        log_error(T("导出失败！"), "CanvasView");
    }
}
}
