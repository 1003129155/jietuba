#pragma once
#include "clipboard/core/models.h"
#include <QListWidget>
#include <QObject>
#include <functional>
#include <optional>

namespace jietuba::clipboard {
// Where the user is in the clipboard list. The list is a picker: a click or Enter uses a row at once, so the mouse
// and the keyboard drive one cursor, like a menu. Three things are tracked, each changed only by the operations listed:
//
// - Cursor: the one row being pointed at; Enter, Space and the quick edit key act on it.
//   Moved by moving the mouse onto a row, pressing a row, the arrow keys, and marking a row.
//   Left alone by leaving the list, wheel or keyboard scrolling under a still mouse, and dragging.
// - Marks: the rows picked to be used together (Ctrl/Shift+click, Shift+arrows, Space, the item menu). While there are any,
//   the list is in multiple selection: every row shows a circle, a left click marks and a right click unmarks.
// - Anchor: where a Shift range starts. Set by a plain left press, a keyboard move and marking a row; never by hovering.
//
// Opening the window starts with none of them. Qt's own current item and selection are not used.
class SelectionManager : public QObject {
    Q_OBJECT
public:
    SelectionManager(QListWidget *list_widget, std::function<std::optional<ClipboardItem>(qint64)> get_item_data);

    // "top" / "left" / "right"; set by the window when the layout changes.
    QString group_bar_position = "right";

    // ── Cursor ──
    int cursor_row() const { return cursor_row_; }
    std::optional<qint64> cursor_id() const;
    // The mouse moved onto the row: the cursor follows without scrolling.
    void point_at(int row);
    // The arrow keys: from the cursor, or from the first or last row when there is none. Scrolls to the row and previews it.
    void move_cursor(int delta);
    // Puts the cursor and the anchor on the item as the keyboard would; false when it is not in the list.
    bool move_cursor_to(std::optional<qint64> item_id);
    void clear_cursor();

    // ── Anchor ──
    void set_anchor(qint64 item_id) { anchor_ = item_id; }

    // ── Marks ──
    // Marked ids in selection order for pasting, in list order for display.
    QList<qint64> marked_ids(bool selection_order = false) const;
    bool has_marks() const { return !marked_.isEmpty(); }
    // Marks the item if it is not; the cursor and the anchor go to it.
    void mark(qint64 item_id);
    // Marks or unmarks the item; the cursor and the anchor go to it.
    void toggle_mark(qint64 item_id);
    // Replaces the marks with the range from the anchor (else the cursor) to the item; the cursor goes to it.
    void mark_range_to(qint64 item_id);
    void unmark(qint64 item_id);
    // Whether anything was marked before.
    bool clear_marks();

    // Back to nothing (on opening the window).
    void reset();
    // Keep the cursor row in step with rows the window inserts, moves or removes.
    void row_inserted(int row);
    void row_moved(int from_row, int to_row);
    void row_removed(int row);
    // Arrow keys in the search box move through the list.
    void set_search_input(QWidget *search_input);

signals:
    void cursor_changed(std::optional<qint64> item_id);
    void marks_changed(int count);
    // Enter or click on an item.
    void item_activated(qint64 item_id);
    // Into the side group bar (left/right layouts).
    void request_sidebar_focus();
    // Top layout: left/right switches groups by delta.
    void request_group_switch(int delta);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    int row_of(qint64 item_id) const;
    std::optional<qint64> row_id(int row) const;
    QList<qint64> ids_between(qint64 first_id, qint64 last_id) const;
    // Every cursor change goes through here.
    void set_cursor_row(int row);
    // A keyboard move: the row is clamped, scrolled to and previewed.
    void move_cursor_by_keyboard(int row);
    // Shift+arrows: the cursor moves and the marks span from the anchor to it.
    void extend_marks(int delta);
    void activate_cursor();
    void show_preview_for_row(int row);

    QListWidget *list_widget_;
    std::function<std::optional<ClipboardItem>(qint64)> get_item_data_;
    int cursor_row_ = -1;
    std::optional<qint64> anchor_;
    QList<qint64> marked_;
    QWidget *search_input_ = nullptr;
};
}
