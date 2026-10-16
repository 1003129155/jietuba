#pragma once
#include "clipboard/core/models.h"
#include <QListWidget>
#include <QObject>
#include <functional>
#include <optional>

namespace jietuba::clipboard {
// Keyboard and mouse selection in the clipboard list: arrow-key navigation, the
// highlighted row and its preview, and the multi-selection (Ctrl/Shift+click,
// Shift+arrows and Space), kept apart from the keyboard's current row.
class SelectionManager : public QObject {
    Q_OBJECT
public:
    SelectionManager(QListWidget *list_widget, std::function<std::optional<ClipboardItem>(qint64)> get_item_data);

    // "top" / "left" / "right"; set by the window when the layout changes.
    QString group_bar_position = "right";
    // The keyboard's current row; -1 when none.
    int selected_index() const { return selected_index_; }

    // Back to no selection (on opening the window).
    void reset();
    // Marked ids in selection order for pasting, in list order for display.
    QList<qint64> marked_ids(bool selection_order = false) const;
    bool has_marks() const { return !marked_.isEmpty(); }
    // Marks or unmarks the item; it becomes the anchor of range selection.
    void toggle_mark(qint64 item_id);
    // Replaces the marks with the range from the anchor (else the current row) to this item.
    void mark_range_to(qint64 item_id);
    void unmark(qint64 item_id);
    // Whether anything was marked before.
    bool clear_marks();
    // Keep the selected row index in step with rows the window inserts, moves or removes.
    void shift_selection_after_insert(int row);
    void shift_selection_after_move(int from_row, int to_row);
    void shift_selection_after_remove(int row);
    void clear_selection(bool reset_keyboard_state = false);
    bool select_item_id(std::optional<qint64> item_id);
    // Arrow keys in the search box move through the list.
    void set_search_input(QWidget *search_input);
    void move_selection(int delta);
    void set_hovered_index(int index) { hovered_index_ = index; }
    void clear_hovered_index() { hovered_index_ = -1; }
    std::optional<qint64> get_current_item_id() const;
    bool has_selection() const { return selected_index_ >= 0; }
    void select_first();
    void select_last();

signals:
    void selection_changed(std::optional<qint64> item_id);
    // Enter or click on an item.
    void item_activated(qint64 item_id);
    // Into the side group bar (left/right layouts).
    void request_sidebar_focus();
    // Top layout: left/right switches groups by delta.
    void request_group_switch(int delta);
    void marks_changed(int count);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    int row_of(qint64 item_id) const;
    std::optional<qint64> range_anchor() const;
    QList<qint64> ids_between(qint64 first_id, qint64 last_id) const;
    void set_current_id(qint64 item_id);
    void extend_marks(int delta);
    void toggle_current_mark();
    void select_index(int index);
    void on_item_clicked(QListWidgetItem *item);
    void on_current_item_changed(QListWidgetItem *current);
    void show_preview_for_item(QListWidgetItem *item);
    void activate_current();
    std::optional<qint64> row_id(int row) const;

    QListWidget *list_widget_;
    std::function<std::optional<ClipboardItem>(qint64)> get_item_data_;
    int selected_index_ = -1;
    int hovered_index_ = -1;
    // Keyboard navigation starts with the first arrow key after the window opens.
    bool keyboard_navigation_started_ = false;
    QList<qint64> marked_;
    std::optional<qint64> mark_anchor_;
    QWidget *search_input_ = nullptr;
};
}
