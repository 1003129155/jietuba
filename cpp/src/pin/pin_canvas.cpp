#include "pin/pin_canvas.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/drawing_items.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/spotlight_item.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include "canvas/items/watermark_item.h"
#include "canvas/scene.h"
#include "canvas/selection_model.h"
#include "canvas/items/selection_item.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/undo.h"
#include "canvas/view.h"
#include "core/logger.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_file_selection.h"
#include "pin/pin_hover.h"
#include "pin/pin_ocr_manager.h"
#include "pin/pin_toolbar.h"
#include "pin/pin_window.h"
#include "tools/controller.h"
#include "tools/cursor_manager.h"
#include "tools/mosaic.h"
#include "tools/number.h"
#include "tools/watermark.h"
#include "ui/paint_settings_panel.h"
#include <QPainter>
#include <QTimer>

namespace jietuba {
namespace {
// rect() of the items that have one.
std::optional<QRectF> item_rect(QGraphicsItem *item) {
    if (auto *rect = dynamic_cast<QGraphicsRectItem *>(item)) return rect->rect();
    if (auto *ellipse = dynamic_cast<QGraphicsEllipseItem *>(item)) return ellipse->rect();
    if (auto *mosaic = dynamic_cast<MosaicItem *>(item)) return mosaic->rect();
    if (auto *watermark = dynamic_cast<WatermarkItem *>(item)) return watermark->rect();
    return std::nullopt;
}

ItemReference reference_of(QGraphicsItem *item) { return ItemReference(dynamic_cast<TrackedItem *>(item)); }
}

PinCanvas::PinCanvas(PinWindow *parent_window, const QSize &base_size, const QImage &background_image)
    : QObject(parent_window), parent_window(parent_window), base_size(base_size) {
    // Mosaic works in pins too: it smears "this scene's background", which here is the
    // pinned image; the scene origin is (0, 0), so its anchor lines up naturally.
    scene = new CanvasScene(background_image, QRectF(0, 0, base_size.width(), base_size.height()), true);
    // The whole image is editable.
    initialize_selection();
    undo_stack = scene->undo_stack;
    tool_controller = scene->tool_controller.get();
}

void PinCanvas::initialize_from_items(const QList<QGraphicsItem *> &drawing_items, const QPoint &selection_offset, std::optional<int> number_next) {
    if (drawing_items.isEmpty()) return;
    // Capture scene coordinates to pin scene coordinates.
    const double offset_x = -selection_offset.x();
    const double offset_y = -selection_offset.y();
    int inherited_count = 0;
    for (auto *item : drawing_items) {
        try {
            auto *cloned_item = clone_graphics_item(item);
            if (!cloned_item) {
                log_warning(T("克隆失败: {item_type} - 返回None", {{"item_type", QString::fromUtf8(typeid(*item).name())}}), "PinCanvas");
                continue;
            }
            apply_static_item_state(item, cloned_item, offset_x, offset_y);
            const auto base_state = capture_item_state(cloned_item);
            // The add command with the drawing-time state first.
            undo_stack->push_command(new AddItemCommand(scene, reference_of(cloned_item)));
            // Then rotation and scale as an edit, when there are any.
            const auto final_state = build_final_state(item, base_state);
            if (base_state != final_state) undo_stack->push_command(new EditItemCommand(reference_of(cloned_item), base_state, final_state));
            // After the moves above, which would drop it.
            if (auto *mosaic = dynamic_cast<MosaicItem *>(cloned_item)) mosaic->adopt_smart_erase(*dynamic_cast<MosaicItem *>(item), QPointF(offset_x, offset_y));
            ++inherited_count;
        } catch (const std::exception &e) {
            log_warning(T("继承项目失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "PinCanvas");
        }
    }
    log_info(T("成功继承 {inherited_count}/{total_count} 个绘制项目", {{"inherited_count", inherited_count}, {"total_count", static_cast<int>(drawing_items.size())}}),
             "PinCanvas");
    if (number_next) sync_number_next(*number_next);
    undo_stack->print_stack_status();
}

void PinCanvas::sync_number_next(int number_next) {
    NumberTool::set_next_number_and_refresh(scene, number_next);
    log_debug(T("同步序号计数器: next={number_next}", {{"number_next", number_next}}), "PinCanvas");
}

QGraphicsItem *PinCanvas::clone_graphics_item(QGraphicsItem *item) {
    if (auto *watermark = dynamic_cast<WatermarkItem *>(item)) {
        // No range to move: in the pin scene it follows that scene's selection (the whole image).
        return new WatermarkItem(watermark->template_text(), watermark->style(), watermark->created_at());
    }
    if (auto *stroke = dynamic_cast<StrokeItem *>(item)) return new StrokeItem(stroke->path(), QPen(stroke->pen()), stroke->is_highlighter);
    if (auto *mosaic = dynamic_cast<MosaicItem *>(item))
        return new MosaicItem(mosaic->path(), mosaic->brush_width(), mosaic->block_size(), mosaic->reduced_image(), mosaic->background_rect(), mosaic->fill_mode(),
                              mosaic->kind());
    // Before RectItem: a hole is a rectangle too and would become a plain one. The
    // darkness belongs to the whole curtain, so it moves to the pin scene's curtain.
    if (auto *spotlight = dynamic_cast<SpotlightItem *>(item)) {
        SpotlightCurtain::of(scene)->setOpacity(spotlight->get_visual_opacity().value_or(1.0));
        return new SpotlightItem(QRectF(spotlight->rect()), spotlight->get_corner_radius());
    }
    if (auto *rect = dynamic_cast<RectItem *>(item)) {
        auto *cloned = new RectItem(QRectF(rect->rect()), QPen(rect->pen()), rect->get_corner_radius());
        cloned->setBrush(rect->brush());
        // Keeps the highlighter-rectangle marker.
        if (rect->is_highlighter_rect) {
            cloned->is_highlighter = true;
            cloned->is_highlighter_rect = true;
        }
        return cloned;
    }
    if (auto *ellipse = dynamic_cast<EllipseItem *>(item)) return new EllipseItem(QRectF(ellipse->rect()), QPen(ellipse->pen()));
    if (auto *arrow = dynamic_cast<ArrowItem *>(item)) {
        const QPen pen(QColor(arrow->color), arrow->base_width);
        const auto arrow_style = arrow->_arrow_style.isEmpty() ? QString("single") : arrow->_arrow_style;
        auto *cloned = new ArrowItem(arrow->start_pos, arrow->end_pos, pen, arrow_style);
        // Only a moved control point carries a curve.
        if (arrow->_control_modified) cloned->set_control_point(arrow->control_pos());
        if (arrow->has_bend_points()) cloned->set_bend_points(arrow->_bend_points);
        return cloned;
    }
    if (auto *text = dynamic_cast<TextItem *>(item)) {
        auto *cloned = new TextItem(text->toPlainText(), text->pos(), QFont(text->font()), QColor(text->defaultTextColor()));
        // The capture's layout width, so the text does not unfold into one line.
        if (text->textWidth() >= 0) cloned->setTextWidth(text->textWidth());
        const auto [outline_on, outline_color, outline_width] = text->outline_state();
        cloned->set_outline(outline_on, outline_color, QVariant(outline_width));
        const auto [shadow_on, shadow_color] = text->shadow_state();
        cloned->set_shadow(shadow_on, shadow_color);
        cloned->has_background = text->has_background;
        cloned->background_color = QColor(text->background_color);
        return cloned;
    }
    if (auto *number = dynamic_cast<NumberItem *>(item)) {
        auto *cloned = new NumberItem(number->number, number->pos(), number->radius, QColor(number->color), number->style);
        cloned->number_order = number->number_order;
        cloned->copy_extras_from(*number);
        return cloned;
    }
    log_warning(T("不支持的item类型: {item_type}", {{"item_type", QString::fromUtf8(typeid(*item).name())}}), "PinCanvas");
    return nullptr;
}

void PinCanvas::apply_static_item_state(QGraphicsItem *source, QGraphicsItem *cloned, double offset_x, double offset_y) {
    const auto src_pos = source->pos();
    cloned->setPos(QPointF(src_pos.x() + offset_x, src_pos.y() + offset_y));
    // The mosaic's background anchor is in scene coordinates and ignores the item's own
    // transform, so it moves with pos() when changing scenes.
    if (auto *mosaic = dynamic_cast<MosaicItem *>(cloned)) mosaic->translate_background_anchor(offset_x, offset_y);
    cloned->setZValue(source->zValue());
    cloned->setOpacity(source->opacity());
}

QVariantMap PinCanvas::capture_item_state(QGraphicsItem *item) {
    QVariantMap state;
    state["pos"] = item->pos();
    state["transform"] = item->transform();
    state["rotation"] = item->rotation();
    state["transformOriginPoint"] = item->transformOriginPoint();
    state["opacity"] = item->opacity();
    if (const auto rect = item_rect(item)) state["rect"] = *rect;
    if (auto *arrow = dynamic_cast<ArrowItem *>(item)) {
        state["start"] = arrow->start_pos;
        state["end"] = arrow->end_pos;
    }
    return state;
}

QVariantMap PinCanvas::build_final_state(QGraphicsItem *source, const QVariantMap &base_state) {
    auto final_state = base_state;
    final_state["transformOriginPoint"] = source->transformOriginPoint();
    final_state["rotation"] = source->rotation();
    final_state["transform"] = source->transform();
    final_state["opacity"] = source->opacity();
    return final_state;
}

void PinCanvas::initialize_selection() {
    const QRectF full_rect(0, 0, base_size.width(), base_size.height());
    scene->selection_model->initialize_confirmed_rect(full_rect);
    scene->selection_item->hide();
    scene->selection_item->setEnabled(false);
}

void PinCanvas::render_to_painter(QPainter *painter, const QRectF &target_rect) {
    painter->save();
    // The scene renders every layer itself.
    scene->render(painter, target_rect, QRectF(0, 0, base_size.width(), base_size.height()));
    painter->restore();
}

bool PinCanvas::activate_tool(const QString &tool_name) {
    if (!tool_controller->get_tool(tool_name)) {
        log_warning(T("钉图不支持工具: {tool_name}", {{"tool_name", tool_name}}), "PinCanvas");
        return false;
    }
    try {
        tool_controller->activate(tool_name);
        const bool editing_mode = tool_name != "cursor";
        is_editing_ = editing_mode;
        parent_window->is_editing = editing_mode;
        is_drawing_ = false;
        notify_editing_changed();
        return true;
    } catch (const std::exception &e) {
        log_error(T("工具激活失败: {e}", {{"e", QString::fromUtf8(e.what())}}), "PinCanvas");
        is_editing_ = false;
        parent_window->is_editing = false;
        notify_editing_changed();
        return false;
    }
}

void PinCanvas::notify_editing_changed() {
    // Drawing over hidden annotations would hide the new ones too.
    if (is_editing_) parent_window->set_annotations_hidden(false);
    if (parent_window->hover_controls) parent_window->hover_controls->on_editing_changed();
    if (auto *files = parent_window->file_selection()) files->refresh();
}

void PinCanvas::deactivate_tool() {
    // During cleanup the scene may already be emptied: just reset the state.
    if (scene && scene->items().isEmpty()) {
        is_editing_ = false;
        is_drawing_ = false;
        parent_window->is_editing = false;
        return;
    }
    if (tool_controller) tool_controller->activate("cursor");
    is_editing_ = false;
    is_drawing_ = false;
    parent_window->is_editing = false;
    notify_editing_changed();
    // The Esc path gets no enterEvent, so the arrow is set here.
    if (auto *view = parent_window->view.data()) {
        view->setCursor(Qt::ArrowCursor);
        view->viewport()->setCursor(Qt::ArrowCursor);
    }
}

void PinCanvas::connect_toolbar(PinToolbar *toolbar, CanvasView *view) {
    connect(toolbar, &Toolbar::tool_changed, this, [this, toolbar, view](const QString &name) { on_tool_changed(name, toolbar, view); });
    connect(toolbar, &Toolbar::undo_clicked, this, [this] { undo_stack->undo(); });
    connect(toolbar, &Toolbar::redo_clicked, this, [this] { undo_stack->redo(); });
    connect(toolbar, &Toolbar::color_changed, this, &PinCanvas::set_color);
    connect(toolbar, &Toolbar::stroke_width_changed, this, [this, view](int width) { on_stroke_width_changed(width, view); });
    connect(toolbar, &Toolbar::opacity_changed, this, [this, view](int opacity) { on_opacity_changed(opacity, view); });
    connect(toolbar, &Toolbar::arrow_style_changed, this, [this, view](const QString &style) { on_arrow_style_changed(style, view); });
    // Mosaic kind and strength are adjustable in pins too, with the capture window's rules.
    connect(toolbar, &Toolbar::mosaic_style_changed, this, [view](const QString &style) { MosaicTool::apply_style_change(style, view); });
    connect(toolbar, &Toolbar::mosaic_block_size_changed, this, [view](int value) { MosaicTool::apply_block_size_change(value, view); });
    connect(toolbar, &Toolbar::line_style_changed, this, [view](const QString &style) {
        if (view) view->apply_line_style_change_to_selection(style);
    });
    // A highlighter mode switch refreshes the cursor at once.
    connect(toolbar, &Toolbar::highlighter_mode_changed, this, [view](const QString &) {
        if (view && view->cursor_manager) view->cursor_manager->set_tool_cursor("highlighter", true);
    });
    connect(toolbar, &Toolbar::number_next_changed, this, [this](int value) { NumberTool::set_next_number_and_refresh(scene, value); });
    connect(toolbar, &Toolbar::number_style_changed, this, [this, view](const QString &style) { NumberTool::apply_style_change(style, view, undo_stack); });
    // Text styles go to the smart edit controller.
    if (view && view->smart_edit_controller) {
        auto *controller = view->smart_edit_controller.get();
        connect(toolbar, &Toolbar::text_font_changed, view, [controller](const QFont &font) { controller->on_text_font_changed(font); });
        connect(toolbar, &Toolbar::text_outline_changed, view, [controller](bool enabled, const QColor &color, double width) { controller->on_text_outline_changed(enabled, color, width); });
        connect(toolbar, &Toolbar::text_shadow_changed, view, [controller](bool enabled, const QColor &color) { controller->on_text_shadow_changed(enabled, color); });
        connect(toolbar, &Toolbar::text_background_changed, view, [controller](bool enabled, const QColor &color, int opacity) { controller->on_text_background_changed(enabled, color, opacity); });
        connect(toolbar, &Toolbar::color_changed, view, [controller](const QColor &color) { controller->on_text_color_changed(color); });
    }
    // Watermark: applying it, its style and its text, as in the capture window.
    connect(toolbar, &Toolbar::watermark_requested, this, [this, toolbar] { on_watermark_requested(toolbar); });
    connect(toolbar, &Toolbar::watermark_style_changed, this, &PinCanvas::on_watermark_style_changed);
    connect(toolbar, &Toolbar::watermark_text_changed, this, [this, toolbar](const QString &template_text) { on_watermark_text_changed(template_text, toolbar); });
    // Saving and copying stay with the window.
    connect(toolbar, &Toolbar::save_clicked, parent_window, &PinWindow::save_image);
    connect(toolbar, &Toolbar::copy_clicked, parent_window, &PinWindow::copy_to_clipboard);
    undo_stack->print_stack_status();
}

void PinCanvas::on_tool_changed(const QString &tool_name, PinToolbar *toolbar, CanvasView *view) {
    if (!tool_name.isEmpty() && tool_name != "cursor") {
        if (!activate_tool(tool_name)) return;
        parent_window->ocr_manager()->set_drawing_mode(true);
        // Sync the toolbar with signals blocked, so the setters do not loop back.
        if (toolbar && tool_controller) {
            const auto &ctx = tool_controller->ctx;
            const bool blocked = toolbar->blockSignals(true);
            toolbar->set_current_color(ctx.color);
            toolbar->set_stroke_width(static_cast<int>(ctx.stroke_width));
            toolbar->set_opacity(static_cast<int>(ctx.opacity * 255));
            // Styles are refilled by show_panel_for_tool; only the next number is
            // scene state rather than a tool default.
            if (tool_name == "number") toolbar->set_number_next_value(NumberTool::get_next_number(scene));
            toolbar->blockSignals(blocked);
        }
        // Focus back to the view.
        QTimer::singleShot(0, view, [view = QPointer<CanvasView>(view)] {
            if (view) view->setFocus();
        });
    } else {
        deactivate_tool();
        parent_window->ocr_manager()->set_drawing_mode(false);
    }
}

void PinCanvas::on_watermark_requested(PinToolbar *toolbar) {
    auto *item = WatermarkTool::add(scene);
    toolbar->show_watermark(item->template_text(), item->created_at());
}

void PinCanvas::on_watermark_style_changed(const QString &key, const QVariant &value) { WatermarkTool::change_style(scene, {{key, value}}); }

void PinCanvas::on_watermark_text_changed(const QString &template_text, PinToolbar *toolbar) {
    if (auto *item = WatermarkTool::change_text(scene, template_text)) toolbar->show_watermark(item->template_text(), item->created_at());
}

void PinCanvas::on_stroke_width_changed(int width, CanvasView *view) {
    const auto *ctx = tool_controller ? &tool_controller->ctx : nullptr;
    const double prev = ctx ? std::max(1.0, ctx->stroke_width) : width;
    set_stroke_width(width);
    const double now = ctx ? std::max(1.0, ctx->stroke_width) : width;
    if (std::abs(now - prev) > 1e-6 && prev > 0 && now > 0 && view) view->apply_size_change_to_selection(now / prev);
    // Like the capture window: the cursor previews the new width.
    if (view && view->cursor_manager) view->cursor_manager->update_tool_cursor_size(width);
}

void PinCanvas::on_opacity_changed(int opacity_int, CanvasView *view) {
    const double opacity = opacity_int / 255.0;
    set_opacity(opacity);
    if (view) view->apply_opacity_change_to_selection(opacity);
}

void PinCanvas::on_arrow_style_changed(const QString &style, CanvasView *view) {
    if (!view || !view->smart_edit_controller) return;
    auto *arrow = dynamic_cast<ArrowItem *>(view->smart_edit_controller->selected_item());
    if (!arrow) return;
    const auto old_state = capture_arrow_state(arrow);
    arrow->set_arrow_style(style);
    const auto new_state = capture_arrow_state(arrow);
    if (undo_stack) undo_stack->push(new EditItemCommand(ItemReference(arrow), old_state, new_state, QString::fromUtf8("修改箭头样式")));
    arrow->update();
}

QVariantMap PinCanvas::capture_arrow_state(QGraphicsItem *item) {
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

void PinCanvas::set_color(const QColor &color) { scene->update_style(color); }

void PinCanvas::set_stroke_width(int width) { scene->update_style(std::nullopt, width); }

void PinCanvas::set_opacity(double opacity) { scene->update_style(std::nullopt, std::nullopt, opacity); }

QImage PinCanvas::export_to_image(const QSize &size, double dpr) {
    QImage image(static_cast<int>(size.width() * dpr), static_cast<int>(size.height() * dpr), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(dpr);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    render_to_painter(&painter, QRectF(0, 0, size.width(), size.height()));
    painter.end();
    return image;
}

void PinCanvas::cleanup() {
    // Leave editing while the scene still exists.
    deactivate_tool();
    // Commands hold items, which hold the scene.
    if (undo_stack) undo_stack->clear();
    // The current tool may hold items.
    if (tool_controller && tool_controller->current_tool) {
        tool_controller->current_tool->on_deactivate(tool_controller->ctx);
        tool_controller->current_tool = nullptr;
    }
    if (scene) {
        scene->deleteLater();
        scene = nullptr;
        undo_stack = nullptr;
        tool_controller = nullptr;
    }
}

void PinCanvas::invalidate_cache() {
    if (scene) scene->invalidate(scene->sceneRect());
}
}
