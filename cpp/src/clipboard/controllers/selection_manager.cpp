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
    connect(list_widget_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { on_item_clicked(item); });
    // Keeps QListWidget's own current item in step.
    connect(list_widget_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current) { on_current_item_changed(current); });
}

std::optional<qint64> SelectionManager::row_id(int row) const {
    const QListWidgetItem *item = list_widget_->item(row);
    if (!item) return std::nullopt;
    return item->data(Qt::UserRole).toLongLong();
}

void SelectionManager::reset() {
    selected_index_ = -1;
    hovered_index_ = -1;
    keyboard_navigation_started_ = false;
    clear_marks();
    list_widget_->clearSelection();
    list_widget_->setCurrentItem(nullptr);
    PreviewPopup::instance()->hide_preview();
    log_debug(T("🔄 SelectionManager 已重置"), "Clipboard");
}

// ── Multiple selection ──────────────────────────────────

QList<qint64> SelectionManager::marked_ids(bool selection_order) const {
    QHash<qint64, int> rows;
    for (int row = 0; row < list_widget_->count(); ++row) rows.insert(list_widget_->item(row)->data(Qt::UserRole).toLongLong(), row);
    QList<qint64> marked;
    for (qint64 item_id : marked_)
        if (rows.contains(item_id)) marked.append(item_id);
    if (!selection_order) std::stable_sort(marked.begin(), marked.end(), [&rows](qint64 a, qint64 b) { return rows.value(a) < rows.value(b); });
    return marked;
}

void SelectionManager::toggle_mark(qint64 item_id) {
    if (marked_.contains(item_id)) marked_.removeOne(item_id);
    else marked_.append(item_id);
    mark_anchor_ = item_id;
    set_current_id(item_id);
    emit marks_changed(static_cast<int>(marked_.size()));
}

void SelectionManager::mark_range_to(qint64 item_id) {
    // Without an anchor, from the row keyboard navigation rests on; without that, only this row.
    const qint64 anchor = range_anchor().value_or(item_id);
    marked_ = ids_between(anchor, item_id);
    mark_anchor_ = anchor;
    set_current_id(item_id);
    emit marks_changed(static_cast<int>(marked_.size()));
}

void SelectionManager::unmark(qint64 item_id) {
    if (marked_.contains(item_id)) {
        marked_.removeOne(item_id);
        emit marks_changed(static_cast<int>(marked_.size()));
    }
    if (mark_anchor_ == item_id) mark_anchor_.reset();
}

bool SelectionManager::clear_marks() {
    const bool had_marks = !marked_.isEmpty();
    marked_.clear();
    mark_anchor_.reset();
    if (had_marks) emit marks_changed(0);
    return had_marks;
}

int SelectionManager::row_of(qint64 item_id) const {
    for (int row = 0; row < list_widget_->count(); ++row)
        if (list_widget_->item(row)->data(Qt::UserRole).toLongLong() == item_id) return row;
    return -1;
}

std::optional<qint64> SelectionManager::range_anchor() const {
    if (mark_anchor_ && row_of(*mark_anchor_) >= 0) return mark_anchor_;
    return get_current_item_id();
}

QList<qint64> SelectionManager::ids_between(qint64 first_id, qint64 last_id) const {
    const int first = row_of(first_id), last = row_of(last_id);
    if (first < 0 || last < 0) return last >= 0 ? QList<qint64>{last_id} : QList<qint64>{};
    const int step = last >= first ? 1 : -1;
    QList<qint64> ids;
    for (int row = first; row != last + step; row += step) ids.append(list_widget_->item(row)->data(Qt::UserRole).toLongLong());
    return ids;
}

void SelectionManager::set_current_id(qint64 item_id) {
    // Keyboard navigation continues from this row.
    const int row = row_of(item_id);
    if (row >= 0) {
        keyboard_navigation_started_ = true;
        list_widget_->setCurrentRow(row);
    }
}

void SelectionManager::extend_marks(int delta) {
    // Shift+arrows: the current row moves one step and the selection spans anchor to current row.
    if (list_widget_->count() == 0) return;
    if (selected_index_ < 0) {
        move_selection(delta);
        const auto current = get_current_item_id();
        mark_anchor_ = current;
        marked_ = current ? QList<qint64>{*current} : QList<qint64>{};
    } else {
        const auto anchor = range_anchor();
        const int row = std::max(0, std::min(list_widget_->count() - 1, selected_index_ + delta));
        select_index(row);
        mark_anchor_ = anchor;
        const auto current = get_current_item_id();
        if (!current) marked_.clear();
        else if (!anchor) marked_ = row_of(*current) >= 0 ? QList<qint64>{*current} : QList<qint64>{};
        else marked_ = ids_between(*anchor, *current);
    }
    emit marks_changed(static_cast<int>(marked_.size()));
}

void SelectionManager::toggle_current_mark() {
    if (const auto item_id = get_current_item_id()) toggle_mark(*item_id);
}

void SelectionManager::shift_selection_after_insert(int row) {
    // Qt sends no currentItemChanged while currentItem stays, though its row number changed.
    if (selected_index_ >= row) selected_index_ += 1;
}

void SelectionManager::shift_selection_after_move(int from_row, int to_row) {
    if (selected_index_ == from_row) selected_index_ = to_row;
    else if (to_row <= selected_index_ && selected_index_ < from_row) selected_index_ += 1;
    else if (from_row < selected_index_ && selected_index_ <= to_row) selected_index_ -= 1;
}

void SelectionManager::shift_selection_after_remove(int row) {
    // The hover index is the row under the mouse; without scrolling the mouse is over the same row number, so it does not follow the item.
    if (selected_index_ == row) clear_selection();
    else if (selected_index_ > row) selected_index_ -= 1;
}

void SelectionManager::clear_selection(bool reset_keyboard_state) {
    selected_index_ = -1;
    hovered_index_ = -1;
    if (reset_keyboard_state) keyboard_navigation_started_ = false;
    list_widget_->clearSelection();
    list_widget_->setCurrentItem(nullptr);
    PreviewPopup::instance()->hide_preview();
}

bool SelectionManager::select_item_id(std::optional<qint64> item_id) {
    if (!item_id) return false;
    for (int i = 0; i < list_widget_->count(); ++i) {
        const QListWidgetItem *item = list_widget_->item(i);
        if (item && item->data(Qt::UserRole).toLongLong() == *item_id) {
            keyboard_navigation_started_ = true;
            select_index(i);
            return true;
        }
    }
    return false;
}

void SelectionManager::set_search_input(QWidget *search_input) {
    search_input_ = search_input;
    search_input->installEventFilter(this);
}

bool SelectionManager::eventFilter(QObject *obj, QEvent *event) {
    // Search field: arrows go to list navigation, other keys pass.
    if (search_input_ && obj == search_input_ && event->type() == QEvent::KeyPress) {
        auto *key_event = static_cast<QKeyEvent *>(event);
        const int key = key_event->key();
        const QString &pos = group_bar_position;
        if ((key == Qt::Key_Up || key == Qt::Key_Down) && key_event->modifiers() == Qt::ShiftModifier) {
            extend_marks(key == Qt::Key_Up ? -1 : 1);
            list_widget_->setFocus();
            return true;
        }
        if (key == Qt::Key_Up) {
            move_selection(-1);
            list_widget_->setFocus();
            return true;
        }
        if (key == Qt::Key_Down) {
            move_selection(1);
            list_widget_->setFocus();
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            activate_current();
            return true;
        }
        if (key == Qt::Key_Right && pos == "top") {
            emit request_group_switch(1);
            return true;
        }
        if (key == Qt::Key_Left && pos == "top") {
            emit request_group_switch(-1);
            return true;
        }
        return false;
    }

    if (obj == list_widget_ && event->type() == QEvent::KeyPress) {
        auto *key_event = static_cast<QKeyEvent *>(event);
        const int key = key_event->key();
        const auto modifiers = key_event->modifiers();
        const QString &pos = group_bar_position;
        if ((key == Qt::Key_Up || key == Qt::Key_Down) && modifiers == Qt::ShiftModifier) {
            extend_marks(key == Qt::Key_Up ? -1 : 1);
            return true;
        }
        if (key == Qt::Key_Space && modifiers == Qt::NoModifier) {
            toggle_current_mark();
            return true;
        }
        if (key == Qt::Key_Up) {
            move_selection(-1);
            return true;
        }
        if (key == Qt::Key_Down) {
            move_selection(1);
            return true;
        }
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            activate_current();
            return true;
        }
        if (key == Qt::Key_Right) {
            if (pos == "top") {
                emit request_group_switch(1);
                return true;
            }
            if (pos == "right") {
                emit request_sidebar_focus();
                return true;
            }
        } else if (key == Qt::Key_Left) {
            if (pos == "top") {
                emit request_group_switch(-1);
                return true;
            }
            if (pos == "left") {
                emit request_sidebar_focus();
                return true;
            }
        } else if (key == Qt::Key_Escape) {
            // Clear the selection first; otherwise the window closes.
            if (selected_index_ >= 0) {
                reset();
                return true;
            }
            return false;
        }
    }
    return QObject::eventFilter(obj, event);
}

void SelectionManager::move_selection(int delta) {
    const int count = list_widget_->count();
    if (count == 0) return;
    // The current row moved, so Shift selection starts from the new place.
    mark_anchor_.reset();

    int new_index = 0;
    if (!keyboard_navigation_started_) {
        keyboard_navigation_started_ = true;
        // The first arrow key starts from the hovered row, else from the current one.
        if (hovered_index_ >= 0) {
            new_index = hovered_index_ + delta;
        } else {
            const int current_row = list_widget_->currentRow();
            if (current_row >= 0 && !list_widget_->selectedItems().isEmpty()) new_index = current_row + delta;
            else if (delta > 0) new_index = 0;
            else new_index = count - 1;
        }
    } else {
        // Navigation started: move normally, without wrapping.
        new_index = selected_index_ + delta;
        if (new_index < 0) new_index = 0;
        else if (new_index >= count) new_index = count - 1;
    }
    if (new_index < 0) new_index = 0;
    else if (new_index >= count) new_index = count - 1;
    select_index(new_index);
}

void SelectionManager::select_index(int index) {
    if (index < 0 || index >= list_widget_->count()) return;
    selected_index_ = index;
    QListWidgetItem *item = list_widget_->item(index);
    if (!item) return;
    list_widget_->setCurrentItem(item);
    list_widget_->scrollToItem(item);
    show_preview_for_item(item);
    emit selection_changed(item->data(Qt::UserRole).toLongLong());
}

void SelectionManager::on_item_clicked(QListWidgetItem *item) {
    if (!item) return;
    const qint64 item_id = item->data(Qt::UserRole).toLongLong();
    if (!item_id) return;
    // Activates (pastes) directly.
    emit item_activated(item_id);
}

void SelectionManager::on_current_item_changed(QListWidgetItem *current) {
    if (!current) return;
    const int index = list_widget_->row(current);
    if (index != selected_index_) selected_index_ = index;
}

void SelectionManager::show_preview_for_item(QListWidgetItem *item) {
    const qint64 item_id = item->data(Qt::UserRole).toLongLong();
    if (!item_id) return;
    const auto clip_item = get_item_data_(item_id);
    if (!clip_item) return;
    // The preview prefers the left side so it does not cover the window; shown at once for keyboard selection.
    const QRect item_rect = list_widget_->visualItemRect(item);
    if (item_rect.isNull()) return;
    const QPoint global_pos = list_widget_->mapToGlobal(item_rect.topLeft());
    std::optional<QRect> window_rect;
    if (QWidget *window = list_widget_->window()) window_rect = window->frameGeometry();
    PreviewPopup::instance()->show_preview(clip_item, global_pos, 0, "left", window_rect);
}

void SelectionManager::activate_current() {
    if (selected_index_ < 0) return;
    QListWidgetItem *item = list_widget_->item(selected_index_);
    if (!item) return;
    const qint64 item_id = item->data(Qt::UserRole).toLongLong();
    if (!item_id) return;
    log_debug(T("✅ 激活项: {item_id}", {{"item_id", item_id}}), "Clipboard");
    emit item_activated(item_id);
}

std::optional<qint64> SelectionManager::get_current_item_id() const {
    if (selected_index_ < 0) return std::nullopt;
    return row_id(selected_index_);
}

void SelectionManager::select_first() {
    if (list_widget_->count() > 0) {
        keyboard_navigation_started_ = true;
        select_index(0);
    }
}

void SelectionManager::select_last() {
    const int count = list_widget_->count();
    if (count > 0) {
        keyboard_navigation_started_ = true;
        select_index(count - 1);
    }
}
}
