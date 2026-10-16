#include "tools/cursor_manager.h"
#include "canvas/items/drawing_items.h"
#include "canvas/items/selection_item.h"
#include "canvas/scene.h"
#include "canvas/view.h"
#include "settings/tool_settings.h"
#include "tools/controller.h"
#include "tools/mosaic.h"
#include "tools/number.h"
#include <QApplication>
#include <QGraphicsEllipseItem>
#include <QPainter>
#include <QPixmap>
#include <QStyleOptionGraphicsItem>
#include <algorithm>
#include <cmath>

namespace jietuba {
CursorManager::CursorManager(CanvasView *v) : view(v), scene(v->scene()) {}

ToolController *CursorManager::tool_controller() const { return view ? view->tool_controller() : nullptr; }

void CursorManager::set_tool_cursor(const QString &tool_id, bool force) {
    if (!view || !scene) return;
    // Only the cursor tool lets the selection item react to hover and clicks.
    if (auto *canvas = dynamic_cast<CanvasScene *>(scene); canvas && canvas->selection_item) {
        auto *item = canvas->selection_item;
        const bool is_cursor_mode = tool_id == "cursor";
        item->setAcceptHoverEvents(is_cursor_mode);
        item->setAcceptedMouseButtons(is_cursor_mode ? Qt::LeftButton : Qt::NoButton);
        if (!is_cursor_mode) item->unsetCursor();
    }
    // In cursor mode the canvas always shows a cross (arrows are for toolbar widgets).
    if (tool_id == "cursor") {
        current_cursor = QCursor(Qt::CrossCursor);
        apply_cursor(*current_cursor);
        current_tool_id = tool_id;
        return;
    }
    const double brush_size = get_current_brush_size();
    const bool tool_changed = current_tool_id != tool_id;
    // The number preview changes with the next number even at the same size.
    if (tool_id == "number") force = true;
    if (!force && !tool_changed && current_brush_size == brush_size) {
        // Reapplied anyway in case the view reset it.
        if (current_cursor) apply_cursor(*current_cursor);
        return;
    }
    if (const auto cursor = create_tool_cursor_with_size(tool_id, brush_size)) {
        current_cursor = *cursor;
        current_tool_id = tool_id;
        current_brush_size = brush_size;
        apply_cursor(*cursor);
    } else {
        current_cursor = QCursor(Qt::CrossCursor);
        apply_cursor(*current_cursor);
    }
}

void CursorManager::show_brush_indicator(const QPointF &pos, double size) {
    hide_brush_indicator();
    const double radius = size / 2;
    brush_indicator = new QGraphicsEllipseItem(pos.x() - radius, pos.y() - radius, size, size);
    QPen pen(QColor(100, 100, 100, 180));
    pen.setStyle(Qt::DashLine);
    pen.setWidth(1);
    brush_indicator->setPen(pen);
    brush_indicator->setBrush(QBrush(Qt::NoBrush));
    brush_indicator->setZValue(10000);
    scene->addItem(brush_indicator);
    indicator_visible = true;
}

void CursorManager::update_brush_indicator(const QPointF &pos, double size) {
    if (brush_indicator) {
        const double radius = size / 2;
        brush_indicator->setRect(pos.x() - radius, pos.y() - radius, size, size);
    } else {
        show_brush_indicator(pos, size);
    }
}

void CursorManager::hide_brush_indicator() {
    if (!brush_indicator) return;
    if (scene) scene->removeItem(brush_indicator);
    delete brush_indicator;
    brush_indicator = nullptr;
    indicator_visible = false;
}

QCursor CursorManager::create_crosshair_cursor(double brush_size, const QColor &color) const {
    constexpr double line_len = 6, gap = 4;
    const double diameter = std::max(brush_size, 4.0);
    const double radius = diameter / 2;
    // Room for the stroke as well.
    const int total_size = static_cast<int>(diameter + 2 * (gap + line_len) + 8);
    const double center = total_size / 2.0;
    QPixmap pixmap(total_size, total_size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    // Dot in the brush size and colour with a white outline.
    painter.setBrush(QBrush(color));
    QPen stroke_pen(QColor(255, 255, 255, 255));
    stroke_pen.setWidth(1);
    painter.setPen(stroke_pen);
    painter.drawEllipse(static_cast<int>(center - radius), static_cast<int>(center - radius), static_cast<int>(diameter), static_cast<int>(diameter));
    const std::pair<QPointF, QPointF> lines[] = {
        {{center, center - radius - gap}, {center, center - radius - gap - line_len}},
        {{center, center + radius + gap}, {center, center + radius + gap + line_len}},
        {{center - radius - gap, center}, {center - radius - gap - line_len, center}},
        {{center + radius + gap, center}, {center + radius + gap + line_len, center}},
    };
    // Grey backing, then the colour on top.
    QPen bg_pen(QColor(190, 190, 190, 255));
    bg_pen.setWidth(6);
    bg_pen.setCapStyle(Qt::RoundCap);
    painter.setPen(bg_pen);
    for (const auto &[p1, p2] : lines) painter.drawLine(p1, p2);
    QPen fg_pen(color);
    fg_pen.setWidth(4);
    fg_pen.setCapStyle(Qt::RoundCap);
    painter.setPen(fg_pen);
    for (const auto &[p1, p2] : lines) painter.drawLine(p1, p2);
    painter.end();
    return QCursor(pixmap, static_cast<int>(center), static_cast<int>(center));
}

QCursor CursorManager::create_number_cursor(double brush_size) const {
    // The cursor previews what a click will draw, so it paints a real NumberItem.
    auto *controller = tool_controller();
    const ToolContext *ctx = controller ? &controller->ctx : nullptr;
    QColor color = ctx ? ctx->color : QColor(Qt::red);
    color = color_with_opacity(color, ctx ? ctx->opacity : 1.0);
    const int next_number = NumberTool::get_next_number(scene);
    const double radius = NumberTool::get_radius_for_width(brush_size);
    const auto style = ctx ? NumberTool::get_style(*ctx) : NumberItem::normalize_style("solid");
    const int total_size = static_cast<int>(radius * 2 + brush_size * 2 + 10);
    const double center = total_size / 2.0;
    QPixmap pixmap(total_size, total_size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(center, center);
    NumberItem item(next_number, QPointF(0, 0), radius, color, style);
    const QStyleOptionGraphicsItem option;
    item.paint(&painter, &option, nullptr);
    painter.end();
    return QCursor(pixmap, static_cast<int>(center), static_cast<int>(center));
}

std::optional<QCursor> CursorManager::create_tool_cursor_with_size(const QString &tool_id, double brush_size) {
    auto *controller = tool_controller();
    if (tool_id == "pen" || tool_id == "highlighter") {
        if (tool_id == "highlighter" && controller && controller->ctx.settings_manager) {
            if (controller->ctx.settings_manager->get_tool_settings("highlighter")->get("draw_mode").toString() == "rect")
                return QCursor(Qt::CrossCursor);
        }
        QColor color = controller ? controller->ctx.color : QColor(Qt::red);
        // The highlighter draws about three times wider than the pen.
        double real_size = brush_size;
        if (tool_id == "highlighter") {
            real_size = brush_size * 3;
            color.setAlpha(150);
        } else {
            color.setAlpha(255);
        }
        real_size = std::max(4, static_cast<int>(real_size * view_scale_));
        return create_crosshair_cursor(real_size, color);
    }
    if (tool_id == "mosaic") {
        // Read through the tool so the preview and the tool cannot disagree on defaults.
        if (controller && MosaicTool::get_draw_mode(controller->ctx) == MosaicTool::MODE_RECT) return QCursor(Qt::CrossCursor);
        const double real_size = std::max(4, static_cast<int>(brush_size * view_scale_));
        return create_crosshair_cursor(real_size, QColor(110, 110, 110, 220));
    }
    if (tool_id == "number") return create_number_cursor(std::max(4, static_cast<int>(brush_size * view_scale_)));
    // Text clicks start typing, not drawing.
    if (tool_id == "text") return QCursor(Qt::IBeamCursor);
    return QCursor(Qt::CrossCursor);
}

void CursorManager::update_tool_cursor_size(double) {
    if (!current_tool_id.isEmpty() && current_tool_id != "cursor") {
        current_brush_size.reset();
        set_tool_cursor(current_tool_id);
    }
}

void CursorManager::update_tool_cursor_color(const QColor &) {
    if (current_tool_id == "pen" || current_tool_id == "highlighter" || current_tool_id == "number") {
        current_brush_size.reset();
        set_tool_cursor(current_tool_id);
    }
}

void CursorManager::update_tool_cursor_opacity() {
    if (current_tool_id == "number") {
        current_brush_size.reset();
        set_tool_cursor(current_tool_id);
    }
}

double CursorManager::get_current_brush_size() const {
    if (!scene) return 2;
    auto *controller = tool_controller();
    return controller ? controller->ctx.stroke_width : 2;
}

void CursorManager::update_view_scale(double scale) {
    scale = std::max(0.1, scale);
    if (std::abs(scale - view_scale_) < 0.001) return;
    view_scale_ = scale;
    if (!current_tool_id.isEmpty() && current_tool_id != "cursor") {
        current_brush_size.reset();
        set_tool_cursor(current_tool_id);
    }
}

void CursorManager::apply_cursor(const QCursor &cursor) {
    if (!view) return;
    if (!view->can_apply_tool_cursor()) return;
    view->QGraphicsView::setCursor(cursor);
    if (auto *viewport = view->viewport(); viewport && viewport != view) viewport->setCursor(cursor);
    // Keep an active override cursor in step so it does not mask the tool cursor.
    if (QApplication::overrideCursor()) QApplication::changeOverrideCursor(cursor);
}
}
