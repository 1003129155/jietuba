#include "canvas/undo.h"
#include "core/logger.h"
#include "canvas/items/arrow_item.h"
#include "canvas/items/stroke_item.h"
#include "canvas/items/text_item.h"
#include <QGraphicsTextItem>
#include <QCoreApplication>
#include <QGraphicsView>
#include <algorithm>

namespace jietuba {
namespace {
constexpr auto OFFSET = "_number_tool_offset";
constexpr auto ORDER = "_number_tool_order_counter";
bool numeric(const QVariant &v) {
    const auto type = v.metaType().id();
    return type == QMetaType::Int || type == QMetaType::LongLong || type == QMetaType::Double || type == QMetaType::Bool;
}
bool integer(const QVariant &v) { return v.metaType().id() == QMetaType::Int || v.metaType().id() == QMetaType::LongLong || v.metaType().id() == QMetaType::Bool; }
bool python_truth(const QVariant &v) {
    if (!v.isValid() || v.isNull()) return false;
    if (v.metaType().id() == QMetaType::QString) return !v.toString().isEmpty();
    if (v.metaType().id() == QMetaType::QVariantList) return !v.toList().isEmpty();
    if (v.metaType().id() == QMetaType::QVariantMap) return !v.toMap().isEmpty();
    return numeric(v) ? v.toDouble() != 0 : true;
}
void restore_next(QGraphicsScene *scene, std::optional<int> next) { if (next) NumberSequence::set_next_number_and_refresh(scene, *next); }
}
int NumberSequence::count_numbers(QGraphicsScene *scene) {
    if (!scene) return 0;
    const auto items = scene->items();
    return static_cast<int>(std::count_if(items.begin(), items.end(), [](auto *item) { return dynamic_cast<NumberItem *>(item) != nullptr; }));
}
void CommandUndoStack::undo() {
    if (canUndo()) {
        log_debug(T("执行撤销，当前索引: {index}/{count}", {{"index", index()}, {"count", count()}}), "UndoStack");
        log_debug(T("撤销命令: {undo_text}", {{"undo_text", undoText()}}), "UndoStack");
        QUndoStack::undo();
        log_debug(T("撤销后索引: {index}/{count}", {{"index", index()}, {"count", count()}}), "UndoStack");
    } else {
        log_debug(T("无法撤销，栈为空或已到底部 (索引: {index}/{count})", {{"index", index()}, {"count", count()}}), "UndoStack");
    }
}

void CommandUndoStack::redo() {
    if (canRedo()) {
        log_debug(T("执行重做，当前索引: {index}/{count}", {{"index", index()}, {"count", count()}}), "UndoStack");
        log_debug(T("重做命令: {redo_text}", {{"redo_text", redoText()}}), "UndoStack");
        QUndoStack::redo();
        log_debug(T("重做后索引: {index}/{count}", {{"index", index()}, {"count", count()}}), "UndoStack");
    } else {
        log_debug(T("无法重做，已到顶部 (索引: {index}/{count})", {{"index", index()}, {"count", count()}}), "UndoStack");
    }
}

void CommandUndoStack::print_stack_status() const {
    log_debug(T("撤销栈状态"), "UndoStack");
    log_debug(T("总命令数: {count}", {{"count", count()}}), "UndoStack");
    log_debug(T("当前索引: {index}", {{"index", index()}}), "UndoStack");
    log_debug(T("可撤销: {can_undo}", {{"can_undo", canUndo()}}), "UndoStack");
    log_debug(T("可重做: {can_redo}", {{"can_redo", canRedo()}}), "UndoStack");
    if (canUndo()) log_debug(T("下一个撤销: {undo_text}", {{"undo_text", undoText()}}), "UndoStack");
    if (canRedo()) log_debug(T("下一个重做: {redo_text}", {{"redo_text", redoText()}}), "UndoStack");
}

int NumberSequence::get_next_number(QGraphicsScene *scene) { return scene ? std::max(1, count_numbers(scene) + 1 + scene->property(OFFSET).toInt()) : 1; }
int NumberSequence::set_next_number(QGraphicsScene *scene, int next) {
    if (!scene) return 1;
    const auto count = count_numbers(scene); next = std::max(1, next);
    const auto offset = std::max(1 - (count + 1), next - (count + 1)); scene->setProperty(OFFSET, offset);
    return std::max(1, count + 1 + offset);
}
int NumberSequence::set_next_number_and_refresh(QGraphicsScene *scene, int next, bool force) {
    const auto actual = set_next_number(scene, next);
    try {
        if (scene && !scene->views().isEmpty())
            if (auto *view = dynamic_cast<NumberRefreshContext *>(scene->views().front())) view->refresh_number_toolbar(actual);
        if (force && scene) if (auto *context = dynamic_cast<NumberRefreshContext *>(scene)) context->refresh_number_cursor_if_active(true);
    } catch (...) { /* Original refresh catches dependency failures. */ }
    return actual;
}
int NumberSequence::adjust_next_number(QGraphicsScene *scene, int step) {
    if (!scene || step == 0) return get_next_number(scene);
    const auto count = count_numbers(scene);
    const auto offset = std::max(1 - (count + 1), scene->property(OFFSET).toInt() + step);
    scene->setProperty(OFFSET, offset); return std::max(1, count + 1 + offset);
}
int NumberSequence::assign_number_order(QGraphicsScene *scene, NumberItem *item) {
    if (!item) return 0;
    if (item->number_order && *item->number_order >= 0) return *item->number_order;
    if (!scene) { item->number_order = 0; return 0; }
    auto counter = scene->property(ORDER).toInt(); int max_order = -1;
    for (auto *candidate : scene->items()) if (auto *number = dynamic_cast<NumberItem *>(candidate); number && number->number_order) max_order = std::max(max_order, *number->number_order);
    counter = std::max(counter, max_order + 1); item->number_order = counter; scene->setProperty(ORDER, counter + 1); return counter;
}
int NumberSequence::get_max_number(QGraphicsScene *scene, NumberItem *override_item, int override_number) {
    int max_number = 0;
    if (scene) for (auto *item : scene->items()) if (auto *number = dynamic_cast<NumberItem *>(item)) max_number = std::max(max_number, number == override_item ? override_number : number->number);
    return max_number;
}
int NumberSequence::get_next_after_number_edit(QGraphicsScene *scene, NumberItem *item, int old_number, int new_number, std::optional<int> current) {
    const auto next = std::max(1, current.value_or(get_next_number(scene))); old_number = std::max(1, old_number); new_number = std::max(1, new_number);
    if (new_number >= next) return new_number + 1;
    if (old_number < get_max_number(scene)) return next;
    return std::max(1, get_max_number(scene, item, new_number) + 1);
}
AddItemCommand::AddItemCommand(QGraphicsScene *scene, ItemReference item, const QString &text) : QUndoCommand(text), scene_(scene), item_(std::move(item)) {}
void AddItemCommand::undo() { if (auto *item = item_.get(); scene_ && item && item->scene() == scene_) scene_->removeItem(item); }
void AddItemCommand::redo() { if (auto *item = item_.get(); scene_ && item && item->scene() != scene_) scene_->addItem(item); }
AddNumberCommand::AddNumberCommand(QGraphicsScene *scene, ItemReference item, std::optional<int> before, std::optional<int> after, const QString &text)
    : AddItemCommand(scene, std::move(item), text), next_before_(before.value_or(NumberSequence::get_next_number(scene))) {
    auto *number = item_.as<NumberItem>(); const auto previous = next_before_ ? next_before_ : 1;
    next_after_ = after.value_or(std::max(previous, number ? number->number : previous) + 1);
}
void AddNumberCommand::undo() { AddItemCommand::undo(); restore_next(scene_, next_before_); }
void AddNumberCommand::redo() { AddItemCommand::redo(); restore_next(scene_, next_after_); }
void ClearTextItemCommand::undo() {
    RemoveItemCommand::undo();
    if (auto *text = item_.as<QGraphicsTextItem>()) text->setPlainText(old_text_);
}
void ClearTextItemCommand::redo() {
    // Clear first so a redo after undo does not leave stale text on the detached item.
    if (auto *text = item_.as<QGraphicsTextItem>()) text->setPlainText(QString());
    RemoveItemCommand::redo();
}
void RemoveNumberCommand::undo() { const auto next = NumberSequence::get_next_number(scene_); RemoveItemCommand::undo(); restore_next(scene_, next); }
void RemoveNumberCommand::redo() { const auto next = NumberSequence::get_next_number(scene_); RemoveItemCommand::redo(); restore_next(scene_, next); }
RemoveNumberAndRenumberCommand::RemoveNumberAndRenumberCommand(QGraphicsScene *scene, ItemReference item, const QString &text)
    : RemoveItemCommand(scene, std::move(item), text), next_before_(NumberSequence::get_next_number(scene)) {
    std::vector<NumberItem *> remaining;
    const auto items = scene ? scene->items() : QList<QGraphicsItem *>{};
    for (auto *candidate : items) if (auto *number = dynamic_cast<NumberItem *>(candidate)) {
        old_numbers_.push_back({ItemReference(number), std::max(1, number->number)});
        if (candidate != item_.get()) remaining.push_back(number);
    }
    int start = 1; if (!remaining.empty()) { start = std::max(1, remaining.front()->number); for (auto *number : remaining) start = std::min(start, std::max(1, number->number)); }
    auto order = [&](NumberItem *number) { return number->number_order && *number->number_order >= 0 ? *number->number_order : static_cast<int>(items.size() - 1 - items.indexOf(number)); };
    std::stable_sort(remaining.begin(), remaining.end(), [&](auto *a, auto *b) { return std::make_pair(std::max(1, a->number), order(a)) < std::make_pair(std::max(1, b->number), order(b)); });
    int index = 0; for (auto *number : remaining) new_numbers_.push_back({ItemReference(number), start + index++});
    next_after_ = remaining.empty() ? 1 : start + static_cast<int>(remaining.size());
}
void RemoveNumberAndRenumberCommand::apply_numbers(const std::vector<NumberValue> &values) { for (const auto &value : values) if (auto *number = value.item.as<NumberItem>()) { number->number = std::max(1, value.number); number->update(); } }
void RemoveNumberAndRenumberCommand::undo() { RemoveItemCommand::undo(); apply_numbers(old_numbers_); restore_next(scene_, next_before_); if (scene_) scene_->update(); }
void RemoveNumberAndRenumberCommand::redo() { RemoveItemCommand::redo(); apply_numbers(new_numbers_); restore_next(scene_, next_after_); if (scene_) scene_->update(); }
BatchRemoveCommand::BatchRemoveCommand(QGraphicsScene *scene, std::vector<ItemReference> items, const QString &text, std::optional<int> before)
    : QUndoCommand(text), scene_(scene), items_(std::move(items)), next_before_(before), has_numbers_(std::any_of(items_.begin(), items_.end(), [](const auto &item) { return item.template as<NumberItem>() != nullptr; })) {}
void BatchRemoveCommand::undo() {
    const std::optional<int> next = has_numbers_ ? std::optional<int>(NumberSequence::get_next_number(scene_)) : std::nullopt;
    for (const auto &reference : items_) if (auto *item = reference.get(); scene_ && item && item->scene() != scene_) scene_->addItem(item);
    restore_next(scene_, next);
}
void BatchRemoveCommand::redo() {
    const std::optional<int> next = !has_numbers_ ? std::nullopt : first_redo_ && next_before_ ? next_before_ : std::optional<int>(NumberSequence::get_next_number(scene_));
    for (const auto &reference : items_) if (auto *item = reference.get(); scene_ && item && item->scene() == scene_) scene_->removeItem(item);
    restore_next(scene_, next); first_redo_ = false;
}
NumberStyleCommand::NumberStyleCommand(ItemReference item, QString old_style, QString new_style, const QString &text)
    : QUndoCommand(text.isEmpty() ? QCoreApplication::translate("UndoCommands", "Change Number Style") : text), item_(std::move(item)), old_(std::move(old_style)), next_(std::move(new_style)) {}
void NumberStyleCommand::undo() { if (auto *number = item_.as<NumberItem>()) number->set_style(old_); }
void NumberStyleCommand::redo() { if (auto *number = item_.as<NumberItem>()) number->set_style(next_); }
EditItemCommand::EditItemCommand(ItemReference item, QVariantMap old_state, QVariantMap new_state, const QString &text)
    : QUndoCommand(text), item_(std::move(item)), old_state_(std::move(old_state)), new_state_(std::move(new_state)) {}
void EditItemCommand::apply_state(const QVariantMap &state) {
    auto *item = item_.get(); if (!item) return;
    const auto pos = state.value("pos"), transform = state.value("transform"), rotation = state.value("rotation"), origin = state.value("transformOriginPoint"), opacity = state.value("opacity");
    if (pos.metaType().id() == QMetaType::QPointF) item->setPos(pos.toPointF());
    if (transform.metaType().id() == QMetaType::QTransform) item->setTransform(transform.value<QTransform>());
    if (numeric(rotation)) item->setRotation(rotation.toDouble());
    if (origin.metaType().id() == QMetaType::QPointF) item->setTransformOriginPoint(origin.toPointF());
    if (const auto point_size = state.value("font_point_size"); numeric(point_size)) if (auto *t = dynamic_cast<TextItem *>(item)) t->set_font_point_size(point_size.toDouble());
    if (numeric(opacity)) item->setOpacity(opacity.toDouble());
    if (auto *n = dynamic_cast<NumberItem *>(item)) {
        if (const auto number = state.value("number"); integer(number)) n->number = std::max(1, number.toInt());
        n->apply_extras_state(state);
    }
    const auto rect = state.value("rect");
    if (rect.metaType().id() == QMetaType::QRectF) {
        if (auto *r = dynamic_cast<RectItem *>(item)) r->setRect(rect.toRectF());
        else if (auto *e = dynamic_cast<EllipseItem *>(item)) e->setRect(rect.toRectF());
    }
    if (auto *arrow = dynamic_cast<ArrowItem *>(item)) {
        const auto start = state.value("start"), end = state.value("end"), control = state.value("control"), modified = state.value("control_modified");
        if (start.metaType().id() == QMetaType::QPointF) arrow->start_pos = start.toPointF();
        if (end.metaType().id() == QMetaType::QPointF) arrow->end_pos = end.toPointF();
        arrow->_control_modified = modified.isValid() && !modified.isNull() ? python_truth(modified) : control.metaType().id() == QMetaType::QPointF;
        if (control.metaType().id() == QMetaType::QPointF) arrow->_control_pos = control.toPointF();
        else arrow->_control_pos = QPointF((arrow->start_pos.x() + arrow->end_pos.x()) / 2., (arrow->start_pos.y() + arrow->end_pos.y()) / 2.);
        // States that do not record bend points (style changes) leave them as they are.
        if (state.contains("bend_points")) {
            const auto bends = state.value("bend_points").toList();
            const auto is_point = [](const QVariant &value) { return value.metaType().id() == QMetaType::QPointF; };
            if (bends.size() == 2 && is_point(bends[0]) && is_point(bends[1])) arrow->_bend_points = std::array<QPointF, 2>{{bends[0].toPointF(), bends[1].toPointF()}};
            else arrow->_bend_points.reset();
        }
        const auto style = state.value("arrow_style"); if (style.isValid() && !style.isNull()) arrow->_arrow_style = style.toString();
    }
    const auto pen_state = state.value("pen_state");
    if (pen_state.isValid() && !pen_state.isNull()) {
        const auto fields = pen_state.toMap(); QPen pen;
        const auto color = fields.value("pen_color", QColor(Qt::black));
        pen.setColor(color.value<QColor>()); pen.setWidthF(fields.value("pen_width", 1.).toDouble());
        pen.setStyle(static_cast<Qt::PenStyle>(fields.value("pen_style", static_cast<int>(Qt::SolidLine)).toInt()));
        pen.setCapStyle(static_cast<Qt::PenCapStyle>(fields.value("pen_cap_style", static_cast<int>(Qt::RoundCap)).toInt()));
        pen.setJoinStyle(static_cast<Qt::PenJoinStyle>(fields.value("pen_join_style", static_cast<int>(Qt::RoundJoin)).toInt()));
        const auto dash = fields.value("pen_dash_pattern").toList(); QList<qreal> pattern; for (const auto &value : dash) pattern.append(value.toDouble()); if (!pattern.isEmpty()) pen.setDashPattern(pattern);
        if (auto *s = dynamic_cast<StrokeItem *>(item)) s->setPen(pen);
        else if (auto *r = dynamic_cast<RectItem *>(item)) r->setPen(pen);
        else if (auto *e = dynamic_cast<EllipseItem *>(item)) e->setPen(pen);
        else if (auto *a = dynamic_cast<ArrowItem *>(item)) a->setPen(pen);
    }
    if (auto *arrow = dynamic_cast<ArrowItem *>(item)) {
        try { arrow->update_geometry(); } catch (const std::exception &e) { qWarning("update_geometry: %s", e.what()); }
    }
    const auto corner = state.value("corner_radius"); if (corner.isValid() && !corner.isNull()) if (auto *r = dynamic_cast<RectItem *>(item)) r->set_corner_radius(corner.toDouble());
    item->update();
}
NumberEditCommand::NumberEditCommand(ItemReference item, QVariantMap old_state, QVariantMap new_state, std::optional<int> before, std::optional<int> after, const QString &text, Clock clock)
    : EditItemCommand(std::move(item), std::move(old_state), std::move(new_state), text), scene_(item_.get() ? item_.get()->scene() : nullptr), next_before_(before), next_after_(after), clock_(std::move(clock)) {
    if (!clock_) clock_ = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    last_merge_time_ = clock_();
}
void NumberEditCommand::undo() { EditItemCommand::undo(); restore_next(scene_, next_before_); }
void NumberEditCommand::redo() { EditItemCommand::redo(); restore_next(scene_, next_after_); }
bool NumberEditCommand::mergeWith(const QUndoCommand *command) {
    const auto *other = dynamic_cast<const NumberEditCommand *>(command);
    if (!other || !item_.get() || item_.get() != other->item_.get()) return false;
    const auto now = clock_(); if (now - last_merge_time_ > MERGE_WINDOW_SECONDS) return false;
    new_state_ = other->new_state_; next_after_ = other->next_after_; last_merge_time_ = now; return true;
}
}
