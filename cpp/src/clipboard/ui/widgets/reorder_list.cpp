#include "clipboard/ui/widgets/reorder_list.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/theme.h"
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>

namespace jietuba::clipboard {
namespace {
// Dragging this close to an edge scrolls, faster nearer the edge.
constexpr int AUTO_SCROLL_MARGIN = 56;
constexpr int AUTO_SCROLL_MAX_STEP = 28;
constexpr int AUTO_SCROLL_INTERVAL_MS = 16;
}

ReorderListWidget::ReorderListWidget(QWidget *parent) : QListWidget(parent) {
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    setDropIndicatorShown(false);
    // Qt's own auto scroll starts only within 16px of the edge and slowly; long lists took several drags, so a timer of its own does it.
    setAutoScroll(false);
    setMouseTracking(true);
    scroll_timer_.setInterval(AUTO_SCROLL_INTERVAL_MS);
    connect(&scroll_timer_, &QTimer::timeout, this, [this] { auto_scroll_tick(); });
}

void ReorderListWidget::accept_items_from(QListWidget *source, std::function<bool(qint64, qint64)> can_drop) {
    foreign_source_ = source;
    foreign_filter_ = std::move(can_drop);
}

void ReorderListWidget::set_awaiting_foreign_drop(bool awaiting, const QString &hint) {
    awaiting_foreign_drop_ = awaiting;
    drop_hint_ = hint;
    if (!awaiting) {
        drop_target_row_.reset();
        foreign_drag_inside_ = false;
    }
    viewport()->update();
}

bool ReorderListWidget::is_row_droppable(int row) const {
    if (!awaiting_foreign_drop_ || !foreign_source_) return true;
    const QListWidgetItem *dragged = foreign_source_->currentItem();
    const QListWidgetItem *target = item(row);
    if (!dragged || !target) return false;
    return !foreign_filter_ || foreign_filter_(dragged->data(ID_ROLE).toLongLong(), target->data(ID_ROLE).toLongLong());
}

std::optional<qint64> ReorderListWidget::item_id(int row) const {
    const QListWidgetItem *row_item = item(row);
    if (!row_item) return std::nullopt;
    return row_item->data(ID_ROLE).toLongLong();
}

int ReorderListWidget::row_of_id(std::optional<qint64> id) const {
    if (!id) return -1;
    for (int row = 0; row < count(); ++row)
        if (item(row)->data(ID_ROLE).toLongLong() == *id) return row;
    return -1;
}

bool ReorderListWidget::move_row(int source, int target) {
    const int total = count();
    if (!(0 <= source && source < total) || !(0 <= target && target <= total)) return false;
    if (target == source || target == source + 1) return false;
    const int insert_at = target > source ? target - 1 : target;
    // takeItem first moves the current row to a neighbour, which would look like a user choice; this change is not reported.
    blockSignals(true);
    QListWidgetItem *moved = takeItem(source);
    insertItem(insert_at, moved);
    setCurrentRow(insert_at);
    blockSignals(false);
    scrollToItem(moved);
    emit item_moved(moved->data(ID_ROLE).toLongLong(), insert_at > 0 ? item_id(insert_at - 1) : std::nullopt,
                    insert_at + 1 < count() ? item_id(insert_at + 1) : std::nullopt);
    return true;
}

void ReorderListWidget::keyPressEvent(QKeyEvent *event) {
    if (reorder_enabled_ && event->modifiers() == Qt::AltModifier && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        const int row = currentRow();
        if (row >= 0) {
            if (event->key() == Qt::Key_Up) move_row(row, row - 1);
            else move_row(row, row + 2);
        }
        event->accept();
        return;
    }
    QListWidget::keyPressEvent(event);
}

void ReorderListWidget::startDrag(Qt::DropActions) {
    QListWidgetItem *current = currentItem();
    if (!current) return;
    const QRect rect = visualItemRect(current);
    auto *drag = new QDrag(this);
    drag->setMimeData(model()->mimeData(selectedIndexes()));
    drag->setPixmap(drag_pixmap(rect));
    drag->setHotSpot(viewport()->mapFromGlobal(QCursor::pos()) - rect.topLeft());
    // dropEvent moves the row itself; exec's result no longer has Qt delete the source row.
    emit drag_started();
    drag->exec(Qt::MoveAction);
    end_drag_feedback();
    emit drag_finished();
}

QPixmap ReorderListWidget::drag_pixmap(const QRect &rect) {
    const QPixmap source = viewport()->grab(rect);
    QPixmap pixmap(source.size());
    pixmap.setDevicePixelRatio(source.devicePixelRatio());
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setOpacity(0.85);
    painter.drawPixmap(0, 0, source);
    painter.end();
    return pixmap;
}

bool ReorderListWidget::is_foreign_drag(const QDropEvent *event) const { return foreign_source_ && event->source() == foreign_source_; }

std::optional<int> ReorderListWidget::foreign_target_row(const QDropEvent *event) const {
    const QListWidgetItem *target = itemAt(event->position().toPoint());
    const QListWidgetItem *dragged = foreign_source_ ? foreign_source_->currentItem() : nullptr;
    if (!target || !dragged) return std::nullopt;
    if (foreign_filter_ && !foreign_filter_(dragged->data(ID_ROLE).toLongLong(), target->data(ID_ROLE).toLongLong())) return std::nullopt;
    return row(target);
}

void ReorderListWidget::dragEnterEvent(QDragEnterEvent *event) {
    if (is_foreign_drag(event)) {
        foreign_drag_inside_ = true;
        viewport()->update();
    }
    if ((event->source() == this && reorder_enabled_) || is_foreign_drag(event)) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    event->ignore();
}

void ReorderListWidget::dragMoveEvent(QDragMoveEvent *event) {
    const QPoint pos = event->position().toPoint();
    if (is_foreign_drag(event)) {
        drop_target_row_ = foreign_target_row(event);
        update_auto_scroll(pos.y());
        viewport()->update();
        if (!drop_target_row_) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    if (event->source() != this || !reorder_enabled_) {
        event->ignore();
        return;
    }
    drop_row_ = drop_row_at(pos);
    update_auto_scroll(pos.y());
    event->setDropAction(Qt::MoveAction);
    event->accept();
    viewport()->update();
}

void ReorderListWidget::dragLeaveEvent(QDragLeaveEvent *event) {
    end_drag_feedback();
    event->accept();
}

void ReorderListWidget::dropEvent(QDropEvent *event) {
    if (is_foreign_drag(event)) {
        const auto target_row = foreign_target_row(event);
        const QListWidgetItem *dragged = foreign_source_->currentItem();
        end_drag_feedback();
        if (!target_row || !dragged) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        emit item_dropped_on(dragged->data(ID_ROLE).toLongLong(), item(*target_row)->data(ID_ROLE).toLongLong());
        return;
    }
    if (event->source() != this || !reorder_enabled_) {
        event->ignore();
        return;
    }
    const int target = drop_row_at(event->position().toPoint());
    const int source = currentRow();
    end_drag_feedback();
    event->setDropAction(Qt::MoveAction);
    event->accept();
    move_row(source, target);
}

int ReorderListWidget::drop_row_at(const QPoint &pos) const {
    // The upper half of a row inserts before it, the lower half after it.
    const QListWidgetItem *target = itemAt(pos);
    if (!target) {
        if (count() && pos.y() < visualItemRect(item(0)).top()) return 0;
        return count();
    }
    const int target_row = row(target);
    const QRect rect = visualItemRect(target);
    return pos.y() < rect.center().y() ? target_row : target_row + 1;
}

void ReorderListWidget::update_auto_scroll(int y) {
    const int margin = dialog_scaled(AUTO_SCROLL_MARGIN);
    const int height = viewport()->height();
    if (y < margin) {
        const double depth = static_cast<double>(margin - std::max(y, 0)) / margin;
        scroll_step_ = -std::max<int>(1, static_cast<int>(python_round(depth * dialog_scaled(AUTO_SCROLL_MAX_STEP))));
    } else if (y > height - margin) {
        const double depth = static_cast<double>(y - (height - margin)) / margin;
        scroll_step_ = std::max<int>(1, static_cast<int>(python_round(std::min(depth, 1.0) * dialog_scaled(AUTO_SCROLL_MAX_STEP))));
    } else {
        scroll_step_ = 0;
    }
    if (scroll_step_ && !scroll_timer_.isActive()) scroll_timer_.start();
    else if (!scroll_step_) scroll_timer_.stop();
}

void ReorderListWidget::auto_scroll_tick() {
    QScrollBar *bar = verticalScrollBar();
    const int before = bar->value();
    bar->setValue(before + scroll_step_);
    if (bar->value() == before) {
        scroll_timer_.stop();
        return;
    }
    // The view scrolled without the mouse moving; the insert position follows the new content.
    drop_row_ = drop_row_at(viewport()->mapFromGlobal(QCursor::pos()));
    viewport()->update();
}

void ReorderListWidget::end_drag_feedback() {
    scroll_timer_.stop();
    scroll_step_ = 0;
    drop_row_.reset();
    drop_target_row_.reset();
    foreign_drag_inside_ = false;
    viewport()->update();
}

void ReorderListWidget::paintEvent(QPaintEvent *event) {
    QListWidget::paintEvent(event);
    if (awaiting_foreign_drop_) {
        paint_drop_frame();
        if (!foreign_drag_inside_) paint_drop_overlay();
    }
    if (drop_target_row_) {
        paint_drop_target();
        return;
    }
    if (!drop_row_ || count() == 0) return;
    const int y = *drop_row_ < count() ? visualItemRect(item(*drop_row_)).top() : visualItemRect(item(count() - 1)).bottom() + 1;
    const int inset = dialog_scaled(8);
    const int radius = dialog_scaled(3);
    const QColor color(get_ui_theme()->tokens().accent);
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(inset, y - dialog_scaled(1), viewport()->width() - inset * 2, dialog_scaled(3), radius, radius);
    painter.drawEllipse(QPoint(inset, y), radius + 1, radius + 1);
}

void ReorderListWidget::paint_drop_target() {
    const QRect rect = visualItemRect(item(*drop_target_row_)).adjusted(dialog_scaled(4), dialog_scaled(2), -dialog_scaled(4), -dialog_scaled(2));
    const auto &tokens = get_ui_theme()->tokens();
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fill(tokens.accent);
    fill.setAlphaF(0.12f);
    painter.setBrush(fill);
    painter.setPen(QPen(QColor(tokens.accent), dialog_scaled_f(1.5)));
    const int radius = dialog_scaled(8);
    painter.drawRoundedRect(rect, radius, radius);
}

void ReorderListWidget::paint_drop_frame() {
    const auto &tokens = get_ui_theme()->tokens();
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(tokens.accent), 1);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const int radius = dialog_scaled(10);
    painter.drawRoundedRect(viewport()->rect().adjusted(1, 1, -1, -1), radius, radius);
}

void ReorderListWidget::paint_drop_overlay() {
    // A faint wash with the hint in a small centred pill, so the groups below stay readable.
    const auto &tokens = get_ui_theme()->tokens();
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF rect(viewport()->rect().adjusted(2, 2, -2, -2));
    QColor fill = css_color(tokens.accent_soft);
    fill.setAlphaF(0.4f);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    const int radius = dialog_scaled(10);
    painter.drawRoundedRect(rect, radius, radius);
    if (!drop_hint_.isEmpty()) {
        QFont font(this->font());
        font.setPixelSize(dialog_scaled(12));
        font.setWeight(QFont::DemiBold);
        const QFontMetrics metrics(font);
        const int pad_x = dialog_scaled(14), pad_y = dialog_scaled(7);
        const QString text = metrics.elidedText(drop_hint_, Qt::ElideRight, static_cast<int>(rect.width()) - pad_x * 4);
        QRectF pill(0, 0, metrics.horizontalAdvance(text) + pad_x * 2, metrics.height() + pad_y * 2);
        pill.moveCenter(rect.center());
        painter.setFont(font);
        painter.setBrush(css_color(tokens.popup_background));
        painter.setPen(QPen(css_color(tokens.border), 1));
        painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
        painter.setPen(css_color(tokens.accent_text));
        painter.drawText(pill, Qt::AlignCenter, text);
    }
}
}
