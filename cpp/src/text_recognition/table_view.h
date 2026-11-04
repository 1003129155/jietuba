#pragma once
#include "text_recognition/table.h"
#include "text_recognition/result_zoom.h"
#include <QTableWidget>
#include <QUndoStack>

namespace jietuba {
// Writes table to the clipboard as HTML, which spreadsheets and documents paste as a table
// with its merged cells, and as tab-separated text for everything else.
void copy_table(const text_table::Table &table);

// The recognized table in the result window: merged cells as recognized, cells edited in
// place; rows and columns inserted and deleted, cells merged and split from the context menu.
// Every change goes through the undo stack, so Ctrl+Z also brings back a table re-rendered by
// a layout option.
//
// As in a spreadsheet: lettered column and numbered row headers select whole columns and
// rows, a column's edge drags its width and a double-click fits it to its text, and
// Ctrl+wheel zooms the text and grid together.
class RecognizedTableView : public QTableWidget {
    Q_OBJECT
public:
    static constexpr int MIN_ZOOM = ResultZoom::MIN_ZOOM;
    static constexpr int MAX_ZOOM = ResultZoom::MAX_ZOOM;
    static constexpr int ZOOM_STEP = ResultZoom::ZOOM_STEP;
    explicit RecognizedTableView(QWidget *parent = nullptr);
    // undoable: one Ctrl+Z brings the previous table back; a fresh result is not undoable.
    void set_table(const text_table::Table &table, bool undoable);
    const text_table::Table &table() const { return table_; }
    // The selected cells' bounding rectangle, merged cells cut to it; the current cell
    // without a selection.
    text_table::Table selected_table() const;
    // Width of all columns as fitted to their text, for fitting the window to the table.
    int content_width() const;
    // Where the selected cells' text sits in the image; edited cells have none. Only an actual
    // selection counts: the current cell alone is where the keyboard is, not a choice.
    QVector<QRectF> selected_boxes() const;
    void copy_selection();
    void cut_selection();
    // Clipboard text from the selection's top-left, the table growing to hold it; one value
    // over several selected cells fills each of them.
    void paste();
    void insert_row(bool below);
    void insert_column(bool right);
    // The selected rows or columns, merged cells counted whole; the last one stays.
    void delete_selected_rows();
    void delete_selected_columns();
    void merge_selection();
    void split_selection();
    void clear_selection_text();
    QUndoStack *undo_stack() { return &undo_; }
    void apply_theme();
    void set_headers_visible(bool visible);
    bool headers_visible() const;
    // Percent of the normal size; columns sized by hand keep their share.
    int zoom_percent() const;
    void set_zoom_percent(int percent);
    // Average, count and sum of the selected cells as a spreadsheet's status bar shows them;
    // empty unless two or more selected cells have text.
    QString selection_summary() const;
    // Width of the column's widest single cell on one line, so a double-click on the
    // column's edge fits it without wrapping.
    int sizeHintForColumn(int column) const override;
signals:
    void table_changed();
    void zoom_changed();
    // From the context menu, for remembering.
    void headers_visibility_chosen(bool visible);
protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    friend class TableChange;
    struct Bounds {
        int row0 = -1, column0 = -1, row1 = -1, column1 = -1;
        bool valid() const { return row0 >= 0; }
    };
    // The selection's bounding rectangle grown over the merged cells it meets; the current
    // cell without a selection.
    Bounds selection_bounds() const;
    void show_menu(const QPoint &global_pos);
    // px at the dialog scale and the zoom.
    int zoomed(int px) const;
    // Shows table, rebuilding the grid only when its shape or merged cells change.
    void show_table(const text_table::Table &table);
    void fit_columns();
    // Widens the fitted columns to fill the view, so the grid ends at the card's edge.
    void spread_columns();
    void on_item_changed(QTableWidgetItem *item);
    void on_column_resized(int column, int size);
    void push(const text_table::Table &table);

    text_table::Table table_;
    QUndoStack undo_;
    QVector<int> fitted_widths_;
    // Widths dragged or fitted by hand at 100% zoom; 0 for columns fitted automatically.
    QVector<double> manual_widths_;
    ResultZoom *zoom_;
    QColor grid_color_;
    bool showing_ = false;
    bool spreading_ = false;
};
}
