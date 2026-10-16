#pragma once
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace jietuba {
class SimpleCardWidget;

// Rows are plain widgets in a layout rather than list items with built-in drag and
// drop: an internal move removes and reinserts the item, losing the combo boxes and
// check boxes set with setItemWidget. Shared by the toolbar layout and magnifier
// colour format dialogs; each supplies its own row content with a grip.

// The 2x3 dot grip at the start of a row. Only reports press, drag and release; the list moves rows.
class DragGrip : public QWidget {
    Q_OBJECT
public:
    explicit DragGrip(QWidget *parent = nullptr);
signals:
    void pressed();
    void dragged(int global_y);
    void released();
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
};

// A row with a drag grip; subclasses add their own content.
class DraggableRow : public QWidget {
    Q_OBJECT
public:
    explicit DraggableRow(QWidget *parent = nullptr);
    // The dragged row gets an accent backing so the user sees which row moves.
    void set_dragging(bool dragging);
    DragGrip *grip = nullptr;
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    bool dragging_ = false;
};

// A card holding a column of rows that can be dragged into a new order.
class ReorderableRowList : public QScrollArea {
    Q_OBJECT
public:
    explicit ReorderableRowList(QWidget *parent = nullptr);
    DraggableRow *add_row(DraggableRow *row, int index = -1);
    void remove_row(DraggableRow *row);
    // All rows in display order.
    QList<DraggableRow *> rows() const;
    void clear();
    int content_height();
private:
    void place(DraggableRow *row, int index);
    // The dragged row's slot is the number of other rows whose centre is above the pointer.
    void drag_row(DraggableRow *row, int global_y);
    SimpleCardWidget *card_ = nullptr;
    QVBoxLayout *layout_ = nullptr;
};
}
