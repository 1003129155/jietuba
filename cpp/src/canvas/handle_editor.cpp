#include "canvas/handle_editor.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/mosaic_item.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include "canvas/undo.h"
#include "core/logger.h"
#include "core/resource_manager.h"
#include "core/theme.h"
#include "tools/number.h"
#include <QFile>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QLineF>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QSvgRenderer>
#include <QUndoStack>
#include <QtMath>
#include <array>
#include <cmath>

namespace jietuba {
QString handle_type_name(HandleType type) {
    static const std::array<const char *, 19> names = {{"corner_tl", "corner_tr", "corner_br", "corner_bl", "edge_t", "edge_r", "edge_b", "edge_l",
        "rotate", "arrow_start", "arrow_end", "arrow_control", "corner_radius", "number_increment", "number_decrement", "number_delete", "item_delete", "text_scale",
        "arrow_bend"}};
    return QString::fromLatin1(names.at(static_cast<size_t>(type)));
}

QRectF EditHandle::rect_at(const QPointF &center) const {
    const auto half = size / 2.;
    return {center.x() - half, center.y() - half, static_cast<qreal>(size), static_cast<qreal>(size)};
}

QRectF EditHandle::hit_rect_at(const QPointF &center) const {
    const auto side = size + hit_area_padding * 2;
    const auto half = side / 2.;
    return {center.x() - half, center.y() - half, static_cast<qreal>(side), static_cast<qreal>(side)};
}

namespace {
bool is_number_item(QGraphicsItem *layer) { return dynamic_cast<NumberItem *>(layer) != nullptr; }
bool is_arrow_item(QGraphicsItem *layer) { return dynamic_cast<ArrowItem *>(layer) != nullptr; }
bool is_arrow_handle(HandleType type) {
    return type == HandleType::ARROW_START || type == HandleType::ARROW_END || type == HandleType::ARROW_CONTROL || type == HandleType::ARROW_BEND;
}
// Ids of the two bend handles, in curve order.
constexpr int ARROW_BEND_FIRST_ID = 103;
bool is_freehand_mosaic_item(QGraphicsItem *layer) {
    auto *mosaic = dynamic_cast<MosaicItem *>(layer);
    return mosaic && !mosaic->fill_mode();
}
// Items with get_corner_radius()/set_corner_radius(): RectItem and its subclasses.
RectItem *corner_radius_item(QGraphicsItem *layer) { return dynamic_cast<RectItem *>(layer); }

std::vector<std::pair<QString, QPointF>> corner_points(const QRectF &rect) {
    const auto center = rect.center();
    return {
        {"tl", rect.topLeft()}, {"tr", rect.topRight()}, {"br", rect.bottomRight()}, {"bl", rect.bottomLeft()},
        {"t", {center.x(), rect.top()}}, {"r", {rect.right(), center.y()}}, {"b", {center.x(), rect.bottom()}}, {"l", {rect.left(), center.y()}},
    };
}

QPointF anchor(const std::vector<std::pair<QString, QPointF>> &anchors, const char *key) {
    for (const auto &[name, point] : anchors)
        if (name == QLatin1String(key)) return point;
    return {};
}

std::optional<QCursor> &rotate_cursor_cache() {
    static std::optional<QCursor> cursor;
    return cursor;
}
}

LayerEditor::LayerEditor() { get_rotate_cursor(); }

QCursor LayerEditor::get_rotate_cursor() {
    auto &cached = rotate_cursor_cache();
    if (cached) return *cached;
    const auto svg_path = ResourceManager::get_resource_path(QString::fromUtf8("svg/旋转.svg"));
    if (!QFile::exists(svg_path)) {
        cached = QCursor(Qt::OpenHandCursor);
        log_warning(T("旋转光标SVG未找到: {svg_path}，使用默认光标", {{"svg_path", svg_path}}), "LayerEditor");
        return *cached;
    }
    QSvgRenderer renderer(svg_path);
    if (!renderer.isValid()) {
        cached = QCursor(Qt::OpenHandCursor);
        log_warning(T("旋转光标SVG无效，使用默认光标"), "LayerEditor");
        return *cached;
    }
    // 23x23, matching ROTATE_HANDLE_SIZE; hotspot in the centre.
    constexpr int size = 23;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    renderer.render(&painter);
    painter.end();
    cached = QCursor(pixmap, size / 2, size / 2);
    log_debug(T("旋转光标已加载: {svg_path}", {{"svg_path", svg_path}}), "LayerEditor");
    return *cached;
}

bool LayerEditor::start_edit(QGraphicsItem *layer) {
    if (!layer) {
        stop_edit();
        return false;
    }
    active_layer_.reset(layer);
    number_item_mode_ = is_number_item(layer);
    handles = generate_handles(layer);
    if (handles.empty() && !number_item_mode_) {
        stop_edit();
        return false;
    }
    return true;
}

void LayerEditor::stop_edit() {
    active_layer_.reset();
    handles.clear();
    number_item_mode_ = false;
    set_hovered_handle(std::nullopt);
    dragging_handle.reset();
    drag_start_pos.reset();
    initial_layer_state.reset();
    base_scene_rect_.reset();
    base_local_rect_.reset();
    base_pos_.reset();
    base_transform_.reset();
    base_rotation_.reset();
    rotation_origin_local_.reset();
    arrow_base_start_local_.reset();
    arrow_base_end_local_.reset();
    arrow_base_start_scene_.reset();
    arrow_base_end_scene_.reset();
    arrow_base_control_local_.reset();
    arrow_base_control_scene_.reset();
    arrow_base_control_modified_ = false;
    arrow_base_bend_points_.reset();
    arrow_base_bend_scene_.reset();
    base_corner_radius_.reset();
    base_font_point_size_.reset();
}

void LayerEditor::refresh_handles() {
    if (!is_editing()) return;
    handles = generate_handles(active_layer());
}

std::vector<EditHandle> LayerEditor::generate_handles(QGraphicsItem *layer) {
    // Items that define their own handles (text) take precedence.
    if (auto *text = dynamic_cast<TextItem *>(layer); text && !number_item_mode_) return text->get_edit_handles();
    if (is_arrow_item(layer)) {
        auto arrow_handles = generate_arrow_handles(layer);
        if (!arrow_handles.empty()) return arrow_handles;
    }
    const auto rect = get_scene_rect(layer);
    if (rect && rect->isValid()) {
        if (number_item_mode_) return generate_number_handles(*rect);
        return generate_rect_handles(*rect, layer);
    }
    return {};
}

std::vector<EditHandle> LayerEditor::generate_number_handles(const QRectF &rect) const {
    // + at the top-left, - below it, X at the top-right.
    const int size = FUNCTIONAL_HANDLE_SIZE;
    // The gap is interface size, so it lives in the screen offset.
    const QPointF step(0, size + NUMBER_BUTTON_GAP);
    const QCursor cursor(Qt::PointingHandCursor);
    return {
        {200, HandleType::NUMBER_INCREMENT, rect.topLeft(), cursor, size, 2, {}},
        {201, HandleType::NUMBER_DECREMENT, rect.topLeft(), cursor, size, 2, step},
        {202, HandleType::NUMBER_DELETE, rect.topRight(), cursor, size, 2, {}},
    };
}

std::vector<EditHandle> LayerEditor::generate_arrow_handles(QGraphicsItem *layer) const {
    auto *arrow = dynamic_cast<ArrowItem *>(layer);
    const auto start = arrow->mapToScene(arrow->start_pos);
    const auto end = arrow->mapToScene(arrow->end_pos);
    const int size = HANDLE_SIZE + 2;
    const auto control = arrow->mapToScene(arrow->get_control_point());
    constexpr int hit_padding = 12;
    std::vector<EditHandle> result{
        {100, HandleType::ARROW_START, start, QCursor(Qt::CrossCursor), size, hit_padding, {}},
        {101, HandleType::ARROW_END, end, QCursor(Qt::CrossCursor), size, hit_padding, {}},
        {102, HandleType::ARROW_CONTROL, control, QCursor(Qt::SizeAllCursor), size, hit_padding, {}},
    };
    if (!arrow->is_curved()) return result;
    const auto bends = arrow->bend_points();
    const std::array<QPointF, 2> bend_scene{{arrow->mapToScene(bends[0]), arrow->mapToScene(bends[1])}};
    // Bend handles wait until five handles fit apart on screen; stored ones always show, so they stay reachable.
    if (!arrow->has_bend_points()) {
        const auto screen = to_screen();
        const std::array<QPointF, 5> chain{{start, bend_scene[0], control, bend_scene[1], end}};
        for (size_t i = 1; i < chain.size(); ++i)
            if (QLineF(screen.map(chain[i - 1]), screen.map(chain[i])).length() < ARROW_BEND_SPACING) return result;
    }
    for (int i = 0; i < 2; ++i)
        result.push_back({ARROW_BEND_FIRST_ID + i, HandleType::ARROW_BEND, bend_scene[static_cast<size_t>(i)], QCursor(Qt::SizeAllCursor), HANDLE_SIZE, hit_padding, {}});
    return result;
}

std::optional<QRectF> LayerEditor::get_scene_rect(QGraphicsItem *layer) const {
    if (!layer) return std::nullopt;
    if (auto *number = dynamic_cast<NumberItem *>(layer)) return number->sceneVisualRect();
    return layer->sceneBoundingRect();
}

std::optional<QRectF> LayerEditor::local_bounds(QGraphicsItem *layer) const {
    const auto rect = get_local_rect(layer);
    if (rect && rect->isValid()) return rect;
    const auto bounds = layer->boundingRect();
    if (bounds.isValid()) return bounds;
    return std::nullopt;
}

std::optional<std::vector<std::pair<QString, QPointF>>> LayerEditor::scene_anchors(QGraphicsItem *layer) const {
    const auto local = local_bounds(layer);
    if (!local) return std::nullopt;
    auto points = corner_points(*local);
    for (auto &entry : points) entry.second = layer->mapToScene(entry.second);
    return points;
}

std::vector<EditHandle> LayerEditor::generate_rect_handles(const QRectF &rect, QGraphicsItem *layer) {
    const int hs = HANDLE_SIZE;
    std::vector<EditHandle> result;
    auto anchors = layer ? scene_anchors(layer) : std::nullopt;
    if (!anchors) anchors = corner_points(rect);
    // The top-left corner is the rotate handle.
    result.push_back({0, HandleType::ROTATE, anchor(*anchors, "tl"), get_rotate_cursor(), FUNCTIONAL_HANDLE_SIZE, 8, {}});
    result.push_back({1, HandleType::CORNER_TR, anchor(*anchors, "tr"), QCursor(Qt::SizeBDiagCursor), hs, 8, {}});
    result.push_back({2, HandleType::CORNER_BR, anchor(*anchors, "br"), QCursor(Qt::SizeFDiagCursor), hs, 8, {}});
    result.push_back({3, HandleType::CORNER_BL, anchor(*anchors, "bl"), QCursor(Qt::SizeBDiagCursor), hs, 8, {}});
    result.push_back({4, HandleType::EDGE_T, anchor(*anchors, "t"), QCursor(Qt::SizeVerCursor), hs, 8, {}});
    result.push_back({5, HandleType::EDGE_R, anchor(*anchors, "r"), QCursor(Qt::SizeHorCursor), hs, 8, {}});
    result.push_back({6, HandleType::EDGE_B, anchor(*anchors, "b"), QCursor(Qt::SizeVerCursor), hs, 8, {}});
    result.push_back({7, HandleType::EDGE_L, anchor(*anchors, "l"), QCursor(Qt::SizeHorCursor), hs, 8, {}});
    // Corner-radius handles follow the active layer, not the anchor source.
    if (auto *radius_layer = active_layer(); radius_layer && corner_radius_item(radius_layer))
        append_corner_radius_handles(result, radius_layer);
    return result;
}

void LayerEditor::append_corner_radius_handles(std::vector<EditHandle> &result, QGraphicsItem *layer) const {
    auto *rect_item = corner_radius_item(layer);
    const double r = rect_item->get_corner_radius();
    const auto local_rect = get_local_rect(layer);
    if (!local_rect || !local_rect->isValid()) return;
    const double max_r = std::min(local_rect->width(), local_rect->height()) / 2.0;
    if (max_r <= 0) return;
    // Even at r = 0 the handle sits MIN_OFFSET screen pixels inside the corner, so it can be found.
    double r_visual = std::max(r, RADIUS_HANDLE_MIN_OFFSET * scene_units_per_pixel());
    r_visual = std::min(r_visual, max_r);
    const QCursor cursor(Qt::SizeAllCursor);
    const int rhs = RADIUS_HANDLE_SIZE;
    auto to_scene = [&](double x, double y) { return layer->mapToScene(QPointF(x, y)); };
    result.push_back({10, HandleType::CORNER_RADIUS, to_scene(local_rect->left() + r_visual, local_rect->top() + r_visual), cursor, rhs, 8, {}});
    result.push_back({11, HandleType::CORNER_RADIUS, to_scene(local_rect->right() - r_visual, local_rect->top() + r_visual), cursor, rhs, 8, {}});
    result.push_back({12, HandleType::CORNER_RADIUS, to_scene(local_rect->right() - r_visual, local_rect->bottom() - r_visual), cursor, rhs, 8, {}});
    result.push_back({13, HandleType::CORNER_RADIUS, to_scene(local_rect->left() + r_visual, local_rect->bottom() - r_visual), cursor, rhs, 8, {}});
}

QTransform LayerEditor::to_screen() const { return screen_transform ? screen_transform() : QTransform(); }

double LayerEditor::scene_units_per_pixel() const {
    // Square root of the determinant: m11 may be 0 or negative under rotation or flips.
    const double scale = std::sqrt(std::abs(to_screen().determinant()));
    return scale > 1e-9 ? 1.0 / scale : 1.0;
}

QPointF LayerEditor::screen_center(const EditHandle &handle) const { return to_screen().map(handle.position) + handle.offset; }

QRectF LayerEditor::screen_rect(const EditHandle &handle) const { return handle.rect_at(screen_center(handle)); }

std::optional<EditHandle> LayerEditor::hit_test(const QPointF &pos) const {
    const auto screen_pos = to_screen().map(pos);
    const EditHandle *nearest = nullptr;
    qreal nearest_distance = 0;
    for (const auto &handle : handles) {
        const auto center = screen_center(handle);
        if (!handle.hit_rect_at(center).contains(screen_pos)) continue;
        // Arrow handles crowd on short or tightly bent arrows; the closest one wins, not the first listed.
        if (!is_arrow_handle(handle.handle_type)) return handle;
        const auto distance = QLineF(center, screen_pos).length();
        if (!nearest || distance < nearest_distance) {
            nearest = &handle;
            nearest_distance = distance;
        }
    }
    if (nearest) return *nearest;
    return std::nullopt;
}

void LayerEditor::set_hovered_handle(const std::optional<EditHandle> &handle) {
    const bool same = handle.has_value() == hovered_handle_.has_value() && (!handle || handle->id == hovered_handle_->id);
    hovered_handle_ = handle;
    if (!same) request_repaint();
}

void LayerEditor::set_is_moving_item(bool moving) {
    if (moving != is_moving_item_) {
        is_moving_item_ = moving;
        request_repaint();
    }
}

bool LayerEditor::update_hover(const QPointF &pos) {
    const auto previous = hovered_handle_;
    set_hovered_handle(hit_test(pos));
    return previous.has_value() != hovered_handle_.has_value() || (previous && previous->id != hovered_handle_->id);
}

QCursor LayerEditor::get_cursor(const QPointF &pos) const {
    if (const auto handle = hit_test(pos)) return handle->cursor;
    return QCursor(Qt::ArrowCursor);
}

bool LayerEditor::is_number_adjust_handle(const std::optional<EditHandle> &handle) {
    return handle && (handle->handle_type == HandleType::NUMBER_INCREMENT || handle->handle_type == HandleType::NUMBER_DECREMENT);
}

bool LayerEditor::is_delete_handle(const std::optional<EditHandle> &handle) {
    return handle && (handle->handle_type == HandleType::NUMBER_DELETE || handle->handle_type == HandleType::ITEM_DELETE);
}

bool LayerEditor::adjust_number_with_handle(const EditHandle &handle, QUndoStack *undo_stack) {
    if (!is_editing() || !number_item_mode_) return false;
    if (!is_number_adjust_handle(handle)) return false;
    auto *number_item = dynamic_cast<NumberItem *>(active_layer());
    auto *scene = number_item ? number_item->scene() : nullptr;
    if (!scene) return false;
    const int delta = handle.handle_type == HandleType::NUMBER_INCREMENT ? 1 : -1;
    const auto old_state = copy_layer_state(number_item);
    const int old_number = number_item->number;
    const int new_number = std::max(1, old_number + delta);
    if (new_number == old_number) return true;
    if (!old_state) return false;
    const int next_before = NumberTool::get_next_number(scene);
    const int next_after = NumberTool::get_next_after_number_edit(scene, number_item, old_number, new_number, next_before);
    auto new_state = *old_state;
    new_state["number"] = new_number;
    auto *command = new NumberEditCommand(ItemReference(number_item), *old_state, new_state, next_before, next_after);
    if (undo_stack) {
        undo_stack->push(command);
    } else {
        command->redo();
        delete command;
    }
    return true;
}

void LayerEditor::start_drag(const EditHandle &handle, const QPointF &pos) {
    if (!is_editing()) return;
    auto *layer = active_layer();
    dragging_handle = handle;
    drag_start_pos = pos;
    initial_layer_state = copy_layer_state(layer);
    base_scene_rect_ = get_scene_rect(layer);
    base_pos_ = layer->pos();
    base_transform_ = layer->transform();
    base_local_rect_ = get_local_rect(layer);
    base_font_point_size_.reset();
    if (auto *text = dynamic_cast<TextItem *>(layer)) base_font_point_size_ = text->font_point_size();
    base_rotation_ = layer->rotation();
    rotation_origin_local_.reset();
    if (handle.handle_type == HandleType::ROTATE && base_scene_rect_) {
        rotation_origin_local_ = layer->mapFromScene(base_scene_rect_->center());
        layer->setTransformOriginPoint(*rotation_origin_local_);
    }
    if (is_arrow_item(layer)) capture_arrow_base_geometry(layer);
    base_corner_radius_.reset();
    if (auto *rect_item = corner_radius_item(layer)) base_corner_radius_ = rect_item->get_corner_radius();
}

void LayerEditor::drag_to(const QPointF &pos, bool keep_ratio) {
    if (!dragging_handle || !drag_start_pos || !is_editing()) return;
    const auto delta_scene = pos - *drag_start_pos;
    restore_base_state(active_layer());
    apply_handle_drag(active_layer(), *dragging_handle, delta_scene, keep_ratio);
    handles = generate_handles(active_layer());
}

std::pair<std::optional<QVariantMap>, std::optional<QVariantMap>> LayerEditor::end_drag(QUndoStack *undo_stack) {
    if (!is_editing()) return {std::nullopt, std::nullopt};
    const auto old_state = initial_layer_state;
    const auto new_state = copy_layer_state(active_layer());
    dragging_handle.reset();
    drag_start_pos.reset();
    initial_layer_state.reset();
    base_scene_rect_.reset();
    base_local_rect_.reset();
    base_pos_.reset();
    base_transform_.reset();
    base_rotation_.reset();
    rotation_origin_local_.reset();
    base_corner_radius_.reset();
    base_font_point_size_.reset();
    if (undo_stack && old_state && new_state && active_layer() && *old_state != *new_state) {
        if (auto *tracked = dynamic_cast<TrackedItem *>(active_layer()))
            undo_stack->push(new EditItemCommand(ItemReference(tracked), *old_state, *new_state));
    }
    return {old_state, new_state};
}

void LayerEditor::apply_handle_drag(QGraphicsItem *layer, const EditHandle &handle, const QPointF &delta_scene, bool keep_ratio) {
    if (handle.handle_type == HandleType::ROTATE) { apply_rotation(layer, delta_scene); return; }
    if (handle.handle_type == HandleType::TEXT_SCALE) { apply_text_scale_drag(layer, delta_scene); return; }
    if (handle.handle_type == HandleType::CORNER_RADIUS) { apply_corner_radius_drag(layer, handle, delta_scene); return; }
    // Paths and freehand mosaic have no rectangle; corners scale the transform.
    if (dynamic_cast<StrokeItem *>(layer) || is_freehand_mosaic_item(layer)) {
        apply_stroke_item_drag(layer, handle, delta_scene, keep_ratio);
        return;
    }
    if (is_arrow_item(layer) && is_arrow_handle(handle.handle_type)) {
        apply_arrow_item_drag(layer, handle, delta_scene);
        return;
    }
    if (get_local_rect(layer) && base_local_rect_) {
        // Scene delta mapped into local coordinates.
        const auto p0 = layer->mapFromScene(*drag_start_pos);
        const auto p1 = layer->mapFromScene(*drag_start_pos + delta_scene);
        const QPointF delta_local(p1.x() - p0.x(), p1.y() - p0.y());
        QRectF new_rect(*base_local_rect_);
        apply_rect_delta(new_rect, handle.handle_type, delta_local);
        set_local_rect(layer, new_rect.normalized());
    }
}

void LayerEditor::apply_rect_delta(QRectF &rect, HandleType type, const QPointF &delta) {
    switch (type) {
    case HandleType::CORNER_TL: rect.setTopLeft(rect.topLeft() + delta); break;
    case HandleType::CORNER_TR: rect.setTopRight(rect.topRight() + delta); break;
    case HandleType::CORNER_BR: rect.setBottomRight(rect.bottomRight() + delta); break;
    case HandleType::CORNER_BL: rect.setBottomLeft(rect.bottomLeft() + delta); break;
    case HandleType::EDGE_T: rect.setTop(rect.top() + delta.y()); break;
    case HandleType::EDGE_R: rect.setRight(rect.right() + delta.x()); break;
    case HandleType::EDGE_B: rect.setBottom(rect.bottom() + delta.y()); break;
    case HandleType::EDGE_L: rect.setLeft(rect.left() + delta.x()); break;
    default: break;
    }
}

void LayerEditor::apply_text_scale_drag(QGraphicsItem *layer, const QPointF &delta_scene) {
    // The projection of the drag onto the original diagonal gives the scale,
    // so both axes drive it without a dead direction.
    const auto &base_rect = base_scene_rect_;
    if (!base_font_point_size_ || !base_rect || !base_rect->isValid()) return;
    auto *text = dynamic_cast<TextItem *>(layer);
    if (!text) return;
    const auto diag = base_rect->bottomRight() - base_rect->topLeft();
    const double denom = diag.x() * diag.x() + diag.y() * diag.y();
    if (denom <= 0) return;
    const auto moved = diag + delta_scene;
    double factor = (moved.x() * diag.x() + moved.y() * diag.y()) / denom;
    if (factor <= 0) factor = 0.01;
    text->set_font_point_size(*base_font_point_size_ * factor);
}

void LayerEditor::apply_corner_radius_drag(QGraphicsItem *layer, const EditHandle &handle, const QPointF &delta_scene) {
    // Projects the pointer onto the handle's diagonal track and solves r
    // directly, so there is no accumulated error and no dead direction.
    auto *rect_item = corner_radius_item(layer);
    if (!rect_item) return;
    const auto local_rect = get_local_rect(layer);
    if (!local_rect || !local_rect->isValid()) return;
    const double max_r = std::min(local_rect->width(), local_rect->height()) / 2.0;
    if (max_r <= 0 || !drag_start_pos) return;
    const auto local_pos = layer->mapFromScene(*drag_start_pos + delta_scene);
    const double mx = local_pos.x(), my = local_pos.y();
    const double left = local_rect->left(), top = local_rect->top(), right = local_rect->right(), bottom = local_rect->bottom();
    double r_new = 0;
    switch (handle.id) {
    case 10: r_new = (mx - left + my - top) / 2.0; break;
    case 11: r_new = (right - mx + my - top) / 2.0; break;
    case 12: r_new = (right - mx + bottom - my) / 2.0; break;
    case 13: r_new = (mx - left + bottom - my) / 2.0; break;
    default: return;
    }
    rect_item->set_corner_radius(std::max(0.0, std::min(max_r, r_new)));
}

void LayerEditor::apply_rotation(QGraphicsItem *layer, const QPointF &delta_scene) {
    if (!base_scene_rect_ || !drag_start_pos) return;
    const auto center = base_scene_rect_->center();
    const QPointF start_vec(drag_start_pos->x() - center.x(), drag_start_pos->y() - center.y());
    const auto current_pos = *drag_start_pos + delta_scene;
    const QPointF end_vec(current_pos.x() - center.x(), current_pos.y() - center.y());
    if (std::abs(start_vec.x()) <= 1e-4 && std::abs(start_vec.y()) <= 1e-4) return;
    const double angle_start = qRadiansToDegrees(std::atan2(start_vec.y(), start_vec.x()));
    const double angle_end = qRadiansToDegrees(std::atan2(end_vec.y(), end_vec.x()));
    const double delta_angle = angle_end - angle_start;
    const double base_rotation = base_rotation_ ? *base_rotation_ : layer->rotation();
    layer->setRotation(base_rotation + delta_angle);
    if (rotation_origin_local_) layer->setTransformOriginPoint(*rotation_origin_local_);
    layer->update();
}

void LayerEditor::apply_stroke_item_drag(QGraphicsItem *layer, const EditHandle &handle, const QPointF &delta_scene, bool keep_ratio) {
    if (!base_scene_rect_ || !base_scene_rect_->isValid()) return;
    // The item may already carry an earlier scale or rotation; the new scale is
    // composed with that base transform rather than replacing it.
    const QTransform base_transform = base_transform_ ? *base_transform_ : QTransform();
    QRectF new_scene(*base_scene_rect_);
    apply_rect_delta(new_scene, handle.handle_type, delta_scene);
    new_scene = new_scene.normalized();
    const double w0 = base_scene_rect_->width(), h0 = base_scene_rect_->height();
    const double w1 = new_scene.width(), h1 = new_scene.height();
    if (w0 <= 0 || h0 <= 0 || w1 <= 0 || h1 <= 0) return;
    double sx = w1 / w0, sy = h1 / h0;
    if (keep_ratio) sx = sy = std::min(sx, sy);
    const auto c0_local = layer->mapFromScene(base_scene_rect_->center());
    const auto c1_local = layer->mapFromScene(new_scene.center());
    QTransform t;
    t.translate(c1_local.x(), c1_local.y());
    t.scale(sx, sy);
    t.translate(-c0_local.x(), -c0_local.y());
    layer->setTransform(t * base_transform);
    layer->update();
}

void LayerEditor::capture_arrow_base_geometry(QGraphicsItem *layer) {
    auto *arrow = dynamic_cast<ArrowItem *>(layer);
    arrow_base_start_local_ = arrow->start_pos;
    arrow_base_start_scene_ = arrow->mapToScene(arrow->start_pos);
    arrow_base_end_local_ = arrow->end_pos;
    arrow_base_end_scene_ = arrow->mapToScene(arrow->end_pos);
    arrow_base_control_local_ = arrow->get_control_point();
    arrow_base_control_scene_ = arrow->mapToScene(*arrow_base_control_local_);
    arrow_base_control_modified_ = arrow->_control_modified;
    arrow_base_bend_points_ = arrow->_bend_points;
    const auto bends = arrow->bend_points();
    arrow_base_bend_scene_ = std::array<QPointF, 2>{{arrow->mapToScene(bends[0]), arrow->mapToScene(bends[1])}};
}

void LayerEditor::apply_arrow_item_drag(QGraphicsItem *layer, const EditHandle &handle, const QPointF &delta_scene) {
    auto *arrow = dynamic_cast<ArrowItem *>(layer);
    const auto type = handle.handle_type;
    const auto start_local = arrow_base_start_local_.value_or(arrow->start_pos);
    const auto end_local = arrow_base_end_local_.value_or(arrow->end_pos);
    auto scene_sum = [&](const std::optional<QPointF> &base) -> std::optional<QPointF> {
        if (!base) return std::nullopt;
        return QPointF(base->x() + delta_scene.x(), base->y() + delta_scene.y());
    };
    if (type == HandleType::ARROW_START) {
        const auto target = scene_sum(arrow_base_start_scene_);
        const auto new_start = target ? arrow->mapFromScene(*target) : start_local + delta_scene;
        arrow->set_positions(new_start, end_local);
    } else if (type == HandleType::ARROW_END) {
        const auto target = scene_sum(arrow_base_end_scene_);
        const auto new_end = target ? arrow->mapFromScene(*target) : end_local + delta_scene;
        arrow->set_positions(start_local, new_end);
    } else if (type == HandleType::ARROW_CONTROL) {
        const auto control_local = arrow_base_control_local_.value_or(arrow->get_control_point());
        const auto target = scene_sum(arrow_base_control_scene_);
        arrow->set_control_point(target ? arrow->mapFromScene(*target) : control_local + delta_scene);
    } else if (type == HandleType::ARROW_BEND && arrow_base_bend_scene_) {
        const auto index = handle.id - ARROW_BEND_FIRST_ID;
        if (index == 0 || index == 1) arrow->set_bend_point(index, arrow->mapFromScene((*arrow_base_bend_scene_)[static_cast<size_t>(index)] + delta_scene));
    }
    arrow->update();
}

QRectF LayerEditor::screen_bounds() {
    if (!is_editing()) return {};
    refresh_handles();
    QRectF bounds;
    for (const auto &handle : handles) bounds = bounds.united(screen_rect(handle));
    return bounds;
}

void LayerEditor::render(QPainter *painter) {
    if (!is_editing()) return;
    // The item may have changed size between drags.
    refresh_handles();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // NumberItem draws its own frame; only handles are drawn here.
    std::vector<const EditHandle *> functional, normal, control, radius;
    for (const auto &handle : handles) {
        switch (handle.handle_type) {
        case HandleType::ROTATE: case HandleType::NUMBER_INCREMENT: case HandleType::NUMBER_DECREMENT:
        case HandleType::NUMBER_DELETE: case HandleType::ITEM_DELETE:
            functional.push_back(&handle); break;
        case HandleType::ARROW_CONTROL: case HandleType::ARROW_BEND: control.push_back(&handle); break;
        case HandleType::CORNER_RADIUS: radius.push_back(&handle); break;
        default: normal.push_back(&handle); break;
        }
    }
    for (const auto *h : normal) {
        if (is_hovered(*h)) {
            painter->setPen(QPen(HOVER_COLOR, HANDLE_BORDER_WIDTH));
            painter->setBrush(QBrush(HOVER_FILL));
        } else {
            painter->setPen(QPen(HANDLE_COLOR, HANDLE_BORDER_WIDTH));
            painter->setBrush(QBrush(HANDLE_FILL));
        }
        painter->drawRect(screen_rect(*h));
    }
    // Curve control points are round to tell them apart from end points.
    for (const auto *h : control) {
        if (is_hovered(*h)) {
            painter->setPen(QPen(HOVER_COLOR, HANDLE_BORDER_WIDTH));
            painter->setBrush(QBrush(HOVER_FILL));
        } else {
            painter->setPen(QPen(HANDLE_COLOR, HANDLE_BORDER_WIDTH));
            painter->setBrush(QBrush(QColor(200, 230, 255)));
        }
        const double r = h->size / 2.0;
        painter->drawEllipse(screen_center(*h), r, r);
    }
    // Corner-radius handles are orange diamonds.
    for (const auto *h : radius) {
        const auto center = screen_center(*h);
        const double half = h->size / 2.0;
        const QPolygonF diamond({QPointF(center.x(), center.y() - half), QPointF(center.x() + half, center.y()),
                                 QPointF(center.x(), center.y() + half), QPointF(center.x() - half, center.y())});
        if (is_hovered(*h)) {
            painter->setPen(QPen(RADIUS_HANDLE_HOVER_COLOR, HANDLE_BORDER_WIDTH));
            painter->setBrush(QBrush(QColor(255, 180, 80)));
        } else {
            painter->setPen(QPen(RADIUS_HANDLE_COLOR, HANDLE_BORDER_WIDTH));
            painter->setBrush(QBrush(QColor(255, 255, 255)));
        }
        painter->drawPolygon(diamond);
    }
    // Functional handles last, above everything else.
    for (const auto *h : functional) render_functional_handle(painter, *h);
    painter->restore();
}

void LayerEditor::render_functional_handle(QPainter *painter, const EditHandle &handle) const {
    // Handles that run an action share one look: a square in the theme colour with a centred glyph.
    QColor fill = get_theme().theme_color();
    if (is_hovered(handle)) fill = fill.lighter(118);
    const auto ink = contrast_ink(fill);
    const auto rect = screen_rect(handle);
    painter->save();
    painter->setPen(QPen(QColor(255, 255, 255), 1.4));
    painter->setBrush(QBrush(fill));
    painter->drawRect(rect);
    QPen pen(ink, std::max(1.4, handle.size * 0.11));
    pen.setCapStyle(Qt::RoundCap);
    pen.setCosmetic(false);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    draw_handle_glyph(painter, handle, rect.center(), ink);
    painter->restore();
}

void LayerEditor::draw_handle_glyph(QPainter *painter, const EditHandle &handle, const QPointF &center, const QColor &ink) const {
    const double reach = handle.size * FUNCTIONAL_GLYPH_RATIO;
    const auto kind = handle.handle_type;
    if (kind == HandleType::NUMBER_DELETE || kind == HandleType::ITEM_DELETE) {
        painter->drawLine(QPointF(center.x() - reach, center.y() - reach), QPointF(center.x() + reach, center.y() + reach));
        painter->drawLine(QPointF(center.x() + reach, center.y() - reach), QPointF(center.x() - reach, center.y() + reach));
        return;
    }
    if (kind == HandleType::NUMBER_INCREMENT || kind == HandleType::NUMBER_DECREMENT) {
        painter->drawLine(QPointF(center.x() - reach, center.y()), QPointF(center.x() + reach, center.y()));
        if (kind == HandleType::NUMBER_INCREMENT) painter->drawLine(QPointF(center.x(), center.y() - reach), QPointF(center.x(), center.y() + reach));
        return;
    }
    if (kind == HandleType::ROTATE) {
        // Open arc plus a solid triangle; a line arrowhead blurs at this size.
        const QRectF box(center.x() - reach, center.y() - reach, reach * 2, reach * 2);
        const double start_deg = 105.0, span_deg = 300.0;
        painter->drawArc(box, static_cast<int>(start_deg * 16), static_cast<int>(span_deg * 16));
        // The head sits at the arc end along its tangent.
        const double end_rad = qDegreesToRadians(start_deg + span_deg);
        const QPointF tip_anchor(center.x() + reach * std::cos(end_rad), center.y() - reach * std::sin(end_rad));
        const QPointF direction(-std::sin(end_rad), -std::cos(end_rad));
        const QPointF normal(-direction.y(), direction.x());
        const double head = reach * 0.95;
        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(ink));
        const QPointF points[] = {
            tip_anchor + direction * (head * 0.5),
            tip_anchor - direction * (head * 0.4) + normal * (head * 0.45),
            tip_anchor - direction * (head * 0.4) - normal * (head * 0.45),
        };
        painter->drawPolygon(points, 3);
    }
}

std::optional<QVariantMap> LayerEditor::capture_state(QGraphicsItem *layer) const {
    auto *target = layer ? layer : active_layer();
    if (!target) return std::nullopt;
    return copy_layer_state(target);
}

std::optional<QVariantMap> LayerEditor::copy_layer_state(QGraphicsItem *layer) const {
    if (!layer) return std::nullopt;
    QVariantMap state;
    if (const auto rect = get_local_rect(layer)) state["rect"] = *rect;
    state["pos"] = layer->pos();
    if (auto *text = dynamic_cast<TextItem *>(layer)) state["font_point_size"] = text->font_point_size();
    state["transform"] = layer->transform();
    state["rotation"] = layer->rotation();
    state["transformOriginPoint"] = layer->transformOriginPoint();
    auto *arrow = dynamic_cast<ArrowItem *>(layer);
    if (arrow) {
        state["start"] = arrow->start_pos;
        state["end"] = arrow->end_pos;
        state["control"] = arrow->control_pos();
        state["control_modified"] = arrow->_control_modified;
        QVariantList bends;
        if (arrow->_bend_points)
            for (const auto &point : *arrow->_bend_points) bends << point;
        state["bend_points"] = bends;
    } else {
        state["control"] = QVariant();
    }
    if (auto *rect_item = corner_radius_item(layer)) state["corner_radius"] = rect_item->get_corner_radius();
    if (auto *number = dynamic_cast<NumberItem *>(layer)) state["number"] = std::max(1, number->number);
    return state;
}

void LayerEditor::restore_base_state(QGraphicsItem *layer) {
    if (!layer) return;
    if (base_transform_) layer->setTransform(*base_transform_);
    if (base_pos_) layer->setPos(*base_pos_);
    if (base_local_rect_) set_local_rect(layer, *base_local_rect_);
    if (base_font_point_size_)
        if (auto *text = dynamic_cast<TextItem *>(layer)) text->set_font_point_size(*base_font_point_size_);
    if (auto *arrow = dynamic_cast<ArrowItem *>(layer); arrow && arrow_base_start_local_ && arrow_base_end_local_) {
        arrow->set_positions(*arrow_base_start_local_, *arrow_base_end_local_);
        arrow->_control_modified = arrow_base_control_modified_;
        if (arrow_base_control_local_) arrow->_control_pos = *arrow_base_control_local_;
        arrow->_bend_points = arrow_base_bend_points_;
    }
    if (base_corner_radius_)
        if (auto *rect_item = corner_radius_item(layer)) rect_item->set_corner_radius(*base_corner_radius_);
}

std::optional<QRectF> LayerEditor::get_local_rect(QGraphicsItem *layer) {
    if (auto *rect = dynamic_cast<QGraphicsRectItem *>(layer)) return rect->rect();
    if (auto *ellipse = dynamic_cast<QGraphicsEllipseItem *>(layer)) return ellipse->rect();
    if (auto *mosaic = dynamic_cast<MosaicItem *>(layer)) return mosaic->rect();
    return std::nullopt;
}

void LayerEditor::set_local_rect(QGraphicsItem *layer, const QRectF &rect) {
    if (auto *rect_item = dynamic_cast<RectItem *>(layer)) rect_item->setRect(rect);
    else if (auto *ellipse = dynamic_cast<EllipseItem *>(layer)) ellipse->setRect(rect);
    else if (auto *mosaic = dynamic_cast<MosaicItem *>(layer)) mosaic->setRect(rect);
    else return;
    layer->update();
}
}
