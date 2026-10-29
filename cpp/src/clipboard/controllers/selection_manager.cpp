#include "clipboard/controllers/selection_manager.h"
#include "clipboard/ui/widgets/preview_popup.h"
#include "core/logger.h"
#include <QHash>
#include <QKeyEvent>
#include <algorithm>

namespace jietuba::clipboard {
SelectionManager::SelectionManager(QListWidget *list_widget, std::function<std::optional<ClipboardItem>(qint64)> get_item_data)
    : list_widget_(list_widget), get_item_data_(std::move(get_item_data)) {
    list_widget_->installEventFilter(this);
}

std::optional<qint64> SelectionManager::row_id(int row) const {
    const QListWidgetItem *item = list_widget_->item(row);
    if (!item) return std::nullopt;
    return item->data(Qt::UserRole).toLongLong();
}

int SelectionManager::row_of(qint64 item_id) const {
    for (int row = 0; row < list_widget_->count(); ++row)
        if (list_widget_->item(row)->data(Qt::UserRole).toLongLong() == item_id) return row;
    return -1;
}

void SelectionManager::reset() {
    anchor_.reset();
    clear_marks();
    clear_cursor();
    log_debug(T("🔄 SelectionManager 已重置"), "Clipboard");
}

// ── Cursor ──────────────────────────────────────────────

std::optional<qint64> SelectionManager::cursor_id() const { return cursor_row_ >= 0 ? row_id(cursor_row_) : std::nullopt; }

void SelectionManager::set_cursor_row(int row) {
    if (row < 0 || row >= list_widget_->count()) row = -1;
    if (row == cursor_row_) return;
    cursor_row_ = row;
    emit cursor_changed(cursor_id());
}

void SelectionManager::point_at(int row) { set_cursor_row(row); }

void SelectionManager::clear_cursor() {
    set_cursor_row(-1);
    PreviewPopup::instance()->hide_preview();
}

void SelectionManager::move_cursor(int delta) {
    const int count = list_widget_->count();
    if (count == 0) return;
    const int row = cursor_row_ >= 0 ? cursor_row_ + delta : delta > 0 ? 0 : count - 1;
    move_cursor_by_keyboard(row);
    anchor_ = cursor_id();
}

bool SelectionManager::move_cursor_to(std::optional<qint64> item_id) {
    const int row = item_id ? row_of(*item_id) : -1;
    if (row < 0) return false;
    move_cursor_by_keyboard(row);
    anchor_ = item_id;
    return true;
}

void SelectionManager::move_cursor_by_keyboard(int row) {
    row = std::clamp(row, 0, list_widget_->count() - 1);
    set_cursor_row(row);
    list_widget_->scrollToItem(list_widget_->item(row));
    show_preview_for_row(row);
}

void SelectionManager::show_preview_for_row(int row) {
    QListWidgetItem *item = list_widget_->item(row);
    const auto clip_item = item ? get_item_data_(item->data(Qt::UserRole).toLongLong()) : std::nullopt;
    if (!clip_item) return;
    // The preview prefers the left side so it does not cover the window; shown at once for keyboard moves.
    const QRect item_rect = list_widget_->visualItemRect(item);
    if (item_rect.isNull()) return;
    std::optional<QRect> window_rect;
    if (QWidget *window = list_widget_->window()) window_rect = window->frameGeometry();
    PreviewPopup::instance()->show_preview(clip_item, list_widget_->mapToGlobal(item_rect.topLeft()), 0, "left", window_rect);
}

void SelectionManager::activate_cursor() {
    const auto item_id = cursor_id();
    if (!item_id || !*item_id) return;
    log_debug(T("✅ 激活项: {item_id}", {{"item_id", *item_id}}), "Clipboard");
    emit item_activated(*item_id);
}

void SelectionManager::row_inserted(int row) {
    if (cursor_row_ >= row) cursor_row_ += 1;
}

void SelectionManager::row_moved(int from_row, int to_row) {
    if (cursor_row_ == from_row) cursor_row_ = to_row;
    else if (to_row <= cursor_row_ && cursor_row_ < from_row) cursor_row_ += 1;
    else if (from_row < cursor_row_ && cursor_row_ <= to_row) cursor_row_ -= 1;
}

void SelectionManager::row_removed(int row) {
    if (cursor_row_ == row) clear_cursor();
    else if (cursor_row_ > row) cursor_row_ -= 1;
}

// ── Marks ───────────────────────────────────────────────

QList<qint64> SelectionManager::marked_ids(bool selection_order) const {
    QHash<qint64, int> rows;
    for (int row = 0; row < list_widget_->count(); ++row) rows.insert(list_widget_->item(row)->data(Qt::UserRole).toLongLong(), row);
    QList<qint64> marked;
    for (qint64 item_id : marked_)
        if (rows.contains(item_id)) marked.append(item_id);
    if (!selection_order) std::stable_sort(marked.begin(), marked.end(), [&rows](qint64 a, qint64 b) { return rows.value(a) < rows.value(b); });
    return marked;
}

void SelectionManager::mark(qint64 item_id) {
    if (!marked_.contains(item_id)) {
        toggle_mark(item_id);
        return;
    }
    anchor_ = item_id;
    set_cursor_row(row_of(item_id));
}

void SelectionManager::toggle_mark(qint64 item_id) {
    if (marked_.contains(item_id)) marked_.removeOne(item_id);
    else marked_.append(item_id);
    anchor_ = item_id;
    set_cursor_row(row_of(item_id));
    emit marks_changed(static_cast<int>(marked_.size()));
}

void SelectionManager::mark_range_to(qint64 item_id) {
    const bool anchored = anchor_ && row_of(*anchor_) >= 0;
    const qint64 anchor = anchored ? *anchor_ : cursor_id().value_or(item_id);
    marked_ = ids_between(anchor, item_id);
    anchor_ = anchor;
    set_cursor_row(row_of(item_id));
    emit marks_changed(static_cast<int>(marked_.size()));
}

void SelectionManager::unmark(qint64 item_id) {
    if (marked_.removeOne(item_id)) emit marks_changed(static_cast<int>(marked_.size()));
    if (anchor_ == item_id) anchor_.reset();
}

bool SelectionManager::clear_marks() {
    const bool had_marks = !marked_.isEmpty();
    marked_.clear();
    if (had_marks) emit marks_changed(0);
    return had_marks;
}

QList<qint64> SelectionManager::ids_between(qint64 first_id, qint64 last_id) const {
    const int first = row_of(first_id), last = row_of(last_id);
    if (first < 0 || last < 0) return last >= 0 ? QList<qint64>{last_id} : QList<qint64>{};
    const int step = last >= first ? 1 : -1;
    QList<qint64> ids;
    for (int row = first; row != last + step; row += step) ids.append(list_widget_->item(row)->data(Qt::UserRole).toLongLong());
    return ids;
}

void SelectionManager::extend_marks(int delta) {
    if (list_widget_->count() == 0) return;
    const std::optional<qint64> from = cursor_id();
    move_cursor_by_keyboard(from ? cursor_row_ + delta : delta > 0 ? 0 : list_widget_->count() - 1);
    if (!anchor_ || row_of(*anchor_) < 0) anchor_ = from.value_or(*cursor_id());
    marked_ = ids_between(*anchor_, *cursor_id());
    emit marks_changed(static_cast<int>(marked_.size()));
}

// ── Keys ────────────────────────────────────────────────

void SelectionManager::set_search_input(QWidget *search_input) {
    search_input_ = search_input;
    search_input->installEventFilter(this);
}

bool SelectionManager::eventFilter(QObject *obj, QEvent *event) {
    const bool from_search = search_input_ && obj == search_input_;
    if ((!from_search && obj != list_widget_) || event->type() != QEvent::KeyPress) return QObject::eventFilter(obj, event);
    const auto *key_event = static_cast<QKeyEvent *>(event);
    const int key = key_event->key();
    const auto modifiers = key_event->modifiers();
    const bool vertical = key == Qt::Key_Up || key == Qt::Key_Down;
    const int delta = key == Qt::Key_Up ? -1 : 1;

    if (vertical && modifiers == Qt::ShiftModifier) extend_marks(delta);
    else if (vertical) move_cursor(delta);
    else if (key == Qt::Key_Return || key == Qt::Key_Enter) activate_cursor();
    else if (key == Qt::Key_Space && modifiers == Qt::NoModifier && !from_search) {
        if (const auto item_id = cursor_id()) toggle_mark(*item_id);
    }
    // Up and down always walk the list, so left and right are free for the groups; in the search box they move the text cursor.
    else if ((key == Qt::Key_Left || key == Qt::Key_Right) && !from_search) emit request_group_switch(key == Qt::Key_Right ? 1 : -1);
    else return false;

    // Arrows typed in the search box carry on in the list.
    if (from_search && vertical) list_widget_->setFocus();
    return true;
}
}
