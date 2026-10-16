#include "ui/reorderable_rows.h"
#include "core/qt_utils.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/theme.h"
#include <QMouseEvent>
#include <QPainter>

namespace jietuba {
DragGrip::DragGrip(QWidget *parent) : QWidget(parent) {
    setFixedSize(dialog_scaled(16), dialog_scaled(28));
    setCursor(Qt::OpenHandCursor);
}

void DragGrip::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        setCursor(Qt::ClosedHandCursor);
        emit pressed();
    }
}

void DragGrip::mouseMoveEvent(QMouseEvent *event) {
    // The press grabs the mouse implicitly, so moves outside the grip or dialog still arrive.
    if (event->buttons() == Qt::LeftButton) emit dragged(event->globalPosition().toPoint().y());
}

void DragGrip::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        setCursor(Qt::OpenHandCursor);
        emit released();
    }
}

void DragGrip::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(ui_tokens(this).text_muted));
    const QPointF center(rect().center());
    for (int dx : {-3, 3})
        for (int dy : {-6, 0, 6})
            painter.drawEllipse(center + QPointF(dialog_scaled(dx), dialog_scaled(dy)), dialog_scaled_f(1.5), dialog_scaled_f(1.5));
    painter.end();
}

DraggableRow::DraggableRow(QWidget *parent) : QWidget(parent) { grip = new DragGrip(this); }

void DraggableRow::set_dragging(bool dragging) {
    dragging_ = dragging;
    update();
}

void DraggableRow::paintEvent(QPaintEvent *) {
    if (!dragging_) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(ui_tokens(this).accent_soft));
    painter.drawRoundedRect(rect(), dialog_scaled(8), dialog_scaled(8));
    painter.end();
}

ReorderableRowList::ReorderableRowList(QWidget *parent) : QScrollArea(parent) {
    card_ = new SimpleCardWidget;
    layout_ = new QVBoxLayout(card_);
    layout_->setContentsMargins(dialog_scaled(6), dialog_scaled(6), dialog_scaled(6), dialog_scaled(6));
    layout_->setSpacing(0);
    layout_->setAlignment(Qt::AlignTop);
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet("QScrollArea { border: none; background: transparent; }");
    setWidget(card_);
}

DraggableRow *ReorderableRowList::add_row(DraggableRow *row, int index) {
    row->setParent(card_);
    connect(row->grip, &DragGrip::pressed, row, [row] { row->set_dragging(true); });
    connect(row->grip, &DragGrip::dragged, this, [this, row](int global_y) { drag_row(row, global_y); });
    connect(row->grip, &DragGrip::released, row, [row] { row->set_dragging(false); });
    if (index < 0) layout_->addWidget(row);
    else layout_->insertWidget(index, row);
    return row;
}

void ReorderableRowList::remove_row(DraggableRow *row) {
    layout_->removeWidget(row);
    row->setParent(nullptr);
    row->deleteLater();
}

QList<DraggableRow *> ReorderableRowList::rows() const {
    QList<DraggableRow *> result;
    for (auto *widget : layout_widgets(layout_))
        if (auto *row = qobject_cast<DraggableRow *>(widget)) result.append(row);
    return result;
}

void ReorderableRowList::clear() {
    for (auto *row : rows()) remove_row(row);
}

int ReorderableRowList::content_height() {
    layout_->activate();
    return layout_->sizeHint().height() + 2;
}

void ReorderableRowList::place(DraggableRow *row, int index) {
    layout_->removeWidget(row);
    layout_->insertWidget(index, row);
}

void ReorderableRowList::drag_row(DraggableRow *row, int global_y) {
    const int y = card_->mapFromGlobal(QPoint(0, global_y)).y();
    int index = 0;
    for (auto *other : rows())
        if (other != row && other->geometry().center().y() < y) ++index;
    if (layout_->indexOf(row) != index) {
        place(row, index);
        // Geometry in the new order right away, so the next move compares against it.
        layout_->activate();
    }
    // Keeps scrolling when dragged past the visible area.
    ensureWidgetVisible(row, 0, 0);
}
}
