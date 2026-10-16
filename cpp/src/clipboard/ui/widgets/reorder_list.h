#pragma once
#include <QListWidget>
#include <QTimer>
#include <functional>
#include <optional>

namespace jietuba::clipboard {
// Rows keep their item id in this role.
constexpr int ID_ROLE = Qt::UserRole;

// The manage window's sortable group and item lists. A reorder is reported as the moved
// id and its new neighbours, the arguments of move_*_between(before_id, after_id).
// Rows of another list can also be dropped onto a row (an item into a group).
class ReorderListWidget : public QListWidget {
    Q_OBJECT
public:
    explicit ReorderListWidget(QWidget *parent = nullptr);

    // Off while search filtering leaves gaps; drags to another list still work.
    void set_reorder_enabled(bool enabled) { reorder_enabled_ = enabled; }
    bool reorder_enabled() const { return reorder_enabled_; }
    // Lets source's rows be dropped onto a row here when can_drop(dragged_id, target_id).
    void accept_items_from(QListWidget *source, std::function<bool(qint64, qint64)> can_drop);
    // While another list drags: a hint overlay until the drag enters, then a dashed frame,
    // dimmed rows that cannot take it and the highlighted target row.
    void set_awaiting_foreign_drop(bool awaiting, const QString &hint = {});
    // Whether the current foreign drag can land on this row; every row while none is in progress.
    bool is_row_droppable(int row) const;
    std::optional<qint64> item_id(int row) const;
    int row_of_id(std::optional<qint64> item_id) const;
    // Moves row source to position target (counted before removal, may equal count()).
    bool move_row(int source, int target);
    bool move_current_to_top() { return move_row(currentRow(), 0); }
    bool move_current_to_bottom() { return move_row(currentRow(), count()); }

signals:
    void item_moved(qint64 moved_id, std::optional<qint64> before_id, std::optional<qint64> after_id);
    void item_dropped_on(qint64 dragged_id, qint64 target_id);
    void drag_started();
    void drag_finished();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void startDrag(Qt::DropActions supported_actions) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    QPixmap drag_pixmap(const QRect &rect);
    bool is_foreign_drag(const QDropEvent *event) const;
    std::optional<int> foreign_target_row(const QDropEvent *event) const;
    int drop_row_at(const QPoint &pos) const;
    void update_auto_scroll(int y);
    void auto_scroll_tick();
    void end_drag_feedback();
    void paint_drop_target();
    void paint_drop_frame();
    void paint_drop_overlay();

    bool reorder_enabled_ = true;
    std::optional<int> drop_row_;
    std::optional<int> drop_target_row_;
    QListWidget *foreign_source_ = nullptr;
    std::function<bool(qint64, qint64)> foreign_filter_;
    bool awaiting_foreign_drop_ = false;
    bool foreign_drag_inside_ = false;
    QString drop_hint_;
    int scroll_step_ = 0;
    QTimer scroll_timer_;
};
}
