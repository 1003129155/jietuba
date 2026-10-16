#include "tools/mosaic.h"
#include "canvas/items/background_item.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/scene.h"
#include "canvas/smart_edit_controller.h"
#include "canvas/view.h"
#include "core/i18n.h"
#include "settings/tool_settings.h"
#include <cmath>
#include <cstdlib>

namespace jietuba {
QVariant MosaicTool::read_setting(const ToolContext &ctx, const QString &key) {
    // Each default is written once here; the cursor preview reads the same way.
    static const QVariantMap defaults = {{"block_size", DEFAULT_BLOCK_SIZE}, {"draw_mode", MODE_FREEHAND}, {"style", STYLE_PIXELATE}};
    const auto fallback = defaults.value(key);
    if (!ctx.settings_manager) return fallback;
    return ctx.settings_manager->get_setting("mosaic", key, fallback);
}

int MosaicTool::clamp_block_size(const QVariant &block_size) {
    // int(): floats truncate, strings must hold an integer.
    bool ok = false;
    long long value = 0;
    if (block_size.metaType().id() == QMetaType::Double || block_size.metaType().id() == QMetaType::Float) {
        const double number = block_size.toDouble();
        ok = std::isfinite(number);
        value = static_cast<long long>(std::trunc(number));
    } else if (block_size.isValid() && !block_size.isNull()) {
        value = block_size.toString().trimmed().toLongLong(&ok);
    }
    if (!ok) return DEFAULT_BLOCK_SIZE;
    int best = BLOCK_SIZE_LEVELS.front();
    for (const int level : BLOCK_SIZE_LEVELS) {
        const auto distance = std::llabs(level - value), best_distance = std::llabs(best - value);
        if (distance < best_distance || (distance == best_distance && level > best)) best = level;
    }
    return best;
}

int MosaicTool::get_block_size(const ToolContext &ctx) { return clamp_block_size(read_setting(ctx, "block_size")); }
QString MosaicTool::get_draw_mode(const ToolContext &ctx) { return read_setting(ctx, "draw_mode").toString(); }
QString MosaicTool::get_style(const ToolContext &ctx) { return read_setting(ctx, "style").toString(); }

MosaicItem *MosaicTool::selected_mosaic(CanvasView *view) {
    auto *controller = view ? view->smart_edit_controller.get() : nullptr;
    return controller ? dynamic_cast<MosaicItem *>(controller->selected_item()) : nullptr;
}

bool MosaicTool::apply_style_change(const QString &style, CanvasView *view) {
    auto *item = selected_mosaic(view);
    if (!item) return false;
    const bool smooth = style == STYLE_BLUR;
    if (smooth == item->smooth()) return false;
    item->set_smooth(smooth);
    return true;
}

bool MosaicTool::apply_block_size_change(const QVariant &requested, CanvasView *view) {
    auto *item = selected_mosaic(view);
    if (!item) return false;
    const int block_size = clamp_block_size(requested);
    if (block_size == item->block_size()) return false;
    auto *canvas = dynamic_cast<CanvasScene *>(item->scene());
    if (!canvas || !canvas->background) return false;
    const auto reduced = canvas->background->reduced_image(block_size);
    if (reduced.isNull()) return false;
    item->set_block_size(block_size, reduced);
    return true;
}

void MosaicTool::reset() {
    drawing = false;
    current_item = nullptr;
    path.reset();
    start_pos.reset();
    last_point.reset();
}

void MosaicTool::remove_live_item(ToolContext &ctx) {
    auto *item = current_item;
    if (!item || item->scene() != ctx.scene) return;
    ctx.scene->removeItem(item);
    delete item;
}

void MosaicTool::on_activate(ToolContext &ctx) {
    Tool::on_activate(ctx);
    // Computed once and cached on the background: the cost lands on the toolbar
    // click instead of the first press of a stroke.
    ctx.scene->background->reduced_image(get_block_size(ctx));
}

bool MosaicTool::on_press(const QPointF &pos, Qt::MouseButton button, ToolContext &ctx) {
    if (button != Qt::LeftButton) return false;
    remove_live_item(ctx);
    reset();
    draw_mode = get_draw_mode(ctx);
    const int block_size = get_block_size(ctx);
    const auto reduced = ctx.scene->background->reduced_image(block_size);
    if (reduced.isNull()) return false;
    const bool fill_mode = draw_mode == MODE_RECT;
    QPainterPath new_path;
    if (fill_mode) new_path.addRect(QRectF(pos, pos));
    else new_path = QPainterPath(pos);
    // The reduced image is the background's cached copy; QImage is implicitly shared.
    auto *item = new MosaicItem(new_path, ctx.stroke_width, block_size, reduced, ctx.scene->scene_rect, fill_mode, get_style(ctx) == STYLE_BLUR);
    ctx.scene->addItem(item);
    path = new_path;
    start_pos = pos;
    last_point = pos;
    current_item = item;
    drawing = true;
    return true;
}

void MosaicTool::on_move(const QPointF &pos, ToolContext &) {
    if (!drawing || !current_item || !path) return;
    if (draw_mode == MODE_RECT) {
        QPainterPath rect_path;
        rect_path.addRect(QRectF(*start_pos, pos).normalized());
        path = rect_path;
    } else {
        // Only freehand paths keep growing, so only they are spacing-filtered.
        if (!should_append_point(last_point, pos)) return;
        path->lineTo(pos);
        last_point = pos;
    }
    current_item->set_path(*path);
}

void MosaicTool::on_release(const QPointF &pos, ToolContext &ctx) {
    if (!drawing) {
        reset();
        return;
    }
    if (draw_mode == MODE_RECT) {
        const auto rect = QRectF(*start_pos, pos).normalized();
        if (rect.width() < MIN_SIZE || rect.height() < MIN_SIZE) {
            remove_live_item(ctx);
            reset();
            return;
        }
    }
    MosaicItem *item_to_select = nullptr;
    if (path && current_item) current_item->set_path(*path);
    if (current_item) {
        // The item is already in the scene, so redo() is a no-op and pushing is the single commit point.
        ctx.undo_stack->push_command(new AddItemCommand(ctx.scene, ItemReference(current_item), i18n_tr("Add Mosaic", "UndoCommands")));
        // Boxes are selected right away like highlighter rectangles; freehand strokes are not.
        if (draw_mode == MODE_RECT) item_to_select = current_item;
    }
    reset();
    if (item_to_select) emit ctx.scene->item_auto_select_requested(item_to_select);
}

void MosaicTool::on_deactivate(ToolContext &ctx) {
    remove_live_item(ctx);
    reset();
    Tool::on_deactivate(ctx);
}
}
