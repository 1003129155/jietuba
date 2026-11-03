#include "text_recognition/table_view.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/fluent_lite/theme.h"
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QUndoCommand>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "TextRecognitionWindow"); }

bool same_shape(const text_table::Table &a, const text_table::Table &b) {
    if (a.rows != b.rows || a.columns != b.columns || a.cells.size() != b.cells.size()) return false;
    for (int k = 0; k < a.cells.size(); ++k) {
        const auto &x = a.cells[k], &y = b.cells[k];
        if (x.row != y.row || x.column != y.column || x.row_span != y.row_span || x.column_span != y.column_span) return false;
    }
    return true;
}
}

void copy_table(const text_table::Table &table) {
    auto *mime = new QMimeData;
    mime->setHtml("<html><body>" + text_table::table_html(table) + "</body></html>");
    mime->setText(text_table::table_tsv(table));
    QApplication::clipboard()->setMimeData(mime);
}

// One change of the whole table; tables are small, so a snapshot each way is enough.
class TableChange : public QUndoCommand {
public:
    TableChange(RecognizedTableView *view, text_table::Table before, text_table::Table after)
        : view_(view), before_(std::move(before)), after_(std::move(after)) {}
    void undo() override { view_->show_table(before_); }
    void redo() override { view_->show_table(after_); }
private:
    RecognizedTableView *view_;
    text_table::Table before_, after_;
};

RecognizedTableView::RecognizedTableView(QWidget *parent) : QTableWidget(parent) {
    setFrameShape(QFrame::NoFrame);
    // Drawn in paintEvent: the view's own grid also draws a line after the last column.
    setShowGrid(false);
    horizontalHeader()->hide();
    verticalHeader()->hide();
    setWordWrap(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(this, &QTableWidget::itemChanged, this, &RecognizedTableView::on_item_changed);
    apply_theme();
}

void RecognizedTableView::apply_theme() {
    const auto &t = ui_tokens(this);
    grid_color_ = css_color(t.border);
    setStyleSheet(QString("QTableWidget { background: transparent; color: %1; font: %3px %4; border: none; outline: 0; "
                          "selection-background-color: %5; selection-color: %1; }"
                          "QTableWidget::item { padding: 0 %6px; border: none; }"
                          "QTableWidget::item:selected { background: %5; color: %1; }"
                          "QTableWidget QLineEdit { background: %7; color: %1; border: 1px solid %8; padding: 0 %9px; "
                          "selection-background-color: %8; selection-color: %10; }")
                      .arg(t.text)
                      .arg(dialog_scaled(14))
                      .arg(FONT_FAMILY, t.accent_soft)
                      .arg(dialog_scaled(8))
                      .arg(t.input_background, t.accent)
                      .arg(dialog_scaled(6))
                      .arg(t.selected_text) +
                  scrollbar_qss(this));
    if (!table_.cells.isEmpty()) fit_columns();
}

void RecognizedTableView::set_table(const text_table::Table &table, bool undoable) {
    if (undoable) {
        push(table);
        return;
    }
    undo_.clear();
    show_table(table);
}

void RecognizedTableView::push(const text_table::Table &table) {
    if (table == table_) return;
    undo_.push(new TableChange(this, table_, table));
}

void RecognizedTableView::show_table(const text_table::Table &table) {
    showing_ = true;
    const bool rebuild = !same_shape(table, table_) || rowCount() != table.rows || columnCount() != table.columns;
    table_ = table;
    if (rebuild) {
        clearSpans();
        clearContents();
        setRowCount(table.rows);
        setColumnCount(table.columns);
        for (const auto &cell : table.cells) {
            auto *item = new QTableWidgetItem(cell.text);
            const bool merged = cell.row_span > 1 || cell.column_span > 1;
            item->setTextAlignment(merged ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
            setItem(cell.row, cell.column, item);
            if (merged) setSpan(cell.row, cell.column, cell.row_span, cell.column_span);
        }
        fit_columns();
    } else {
        for (const auto &cell : table.cells)
            if (auto *it = item(cell.row, cell.column); it && it->text() != cell.text) it->setText(cell.text);
    }
    showing_ = false;
    emit table_changed();
}

void RecognizedTableView::fit_columns() {
    ensurePolished();
    const QFontMetrics metrics(font());
    const int padding = dialog_scaled(28);
    const int widest = dialog_scaled(320);
    QVector<int> widths(table_.columns, dialog_scaled(48));
    for (const auto &cell : table_.cells)
        if (cell.column_span == 1) widths[cell.column] = std::max(widths[cell.column], std::min(widest, metrics.horizontalAdvance(cell.text) + padding));
    // A merged cell wider than its columns widens them evenly.
    for (const auto &cell : table_.cells) {
        if (cell.column_span == 1) continue;
        int have = 0;
        for (int c = cell.column; c < cell.column + cell.column_span; ++c) have += widths[c];
        const int need = std::min(widest * cell.column_span, metrics.horizontalAdvance(cell.text) + padding);
        if (need > have)
            for (int c = cell.column; c < cell.column + cell.column_span; ++c) widths[c] += (need - have + cell.column_span - 1) / cell.column_span;
    }
    fitted_widths_ = widths;
    spread_columns();
}

void RecognizedTableView::spread_columns() {
    if (spreading_ || fitted_widths_.size() != columnCount()) return;
    spreading_ = true;
    const int fitted = content_width();
    const int extra = std::max(0, viewport()->width() - fitted);
    int given = 0;
    for (int c = 0; c < columnCount(); ++c) {
        // In proportion to the fitted widths; the last column takes the rounding remainder.
        const int add = c + 1 == columnCount() ? extra - given : extra * fitted_widths_[c] / fitted;
        given += add;
        setColumnWidth(c, fitted_widths_[c] + add);
    }
    // Long text wraps within the widest column; rows grow to fit it.
    resizeRowsToContents();
    const int row_height = dialog_scaled(30);
    for (int r = 0; r < rowCount(); ++r) setRowHeight(r, std::max(row_height, rowHeight(r)));
    spreading_ = false;
}

int RecognizedTableView::content_width() const {
    int width = 0;
    for (int w : fitted_widths_) width += w;
    return width;
}

void RecognizedTableView::resizeEvent(QResizeEvent *event) {
    QTableWidget::resizeEvent(event);
    spread_columns();
}

void RecognizedTableView::paintEvent(QPaintEvent *event) {
    QTableWidget::paintEvent(event);
    // A line between columns and one under each row; the card's border is the outer edge, so
    // a row line on the view's bottom edge is left out. Vertical lines stop above the row line
    // so crossings are not drawn twice.
    QPainter painter(viewport());
    painter.setPen(grid_color_);
    const int bottom = viewport()->height() - 1;
    for (const auto &cell : table_.cells) {
        const QRect rect = visualRect(model()->index(cell.row, cell.column));
        if (!rect.intersects(event->rect())) continue;
        if (cell.column + cell.column_span < table_.columns) painter.drawLine(rect.right(), rect.top(), rect.right(), rect.bottom() - 1);
        if (rect.bottom() != bottom) painter.drawLine(rect.left(), rect.bottom(), rect.right(), rect.bottom());
    }
}

void RecognizedTableView::on_item_changed(QTableWidgetItem *item) {
    if (showing_ || !item) return;
    const auto edited = text_table::with_text(table_, item->row(), item->column(), item->text());
    // After the editor closes: the change re-shows items, which must not happen inside their own signal.
    QMetaObject::invokeMethod(this, [this, edited] { push(edited); }, Qt::QueuedConnection);
}

text_table::Table RecognizedTableView::selected_table() const {
    const auto ranges = selectedRanges();
    if (ranges.isEmpty()) {
        if (currentRow() < 0 || currentColumn() < 0) return {};
        return text_table::part(table_, currentRow(), currentColumn(), currentRow(), currentColumn());
    }
    int r0 = ranges.front().topRow(), r1 = ranges.front().bottomRow(), c0 = ranges.front().leftColumn(), c1 = ranges.front().rightColumn();
    for (const auto &range : ranges) {
        r0 = std::min(r0, range.topRow());
        r1 = std::max(r1, range.bottomRow());
        c0 = std::min(c0, range.leftColumn());
        c1 = std::max(c1, range.rightColumn());
    }
    return text_table::part(table_, r0, c0, r1, c1);
}

void RecognizedTableView::copy_selection() {
    const auto part = selected_table();
    if (!part.cells.isEmpty()) copy_table(part);
}

void RecognizedTableView::delete_current_row() {
    if (table_.rows > 1 && currentRow() >= 0) push(text_table::without_row(table_, currentRow()));
}

void RecognizedTableView::delete_current_column() {
    if (table_.columns > 1 && currentColumn() >= 0) push(text_table::without_column(table_, currentColumn()));
}

void RecognizedTableView::clear_selection_text() {
    auto table = table_;
    for (const auto *selected : selectedItems()) table = text_table::with_text(table, selected->row(), selected->column(), QString());
    push(table);
}

void RecognizedTableView::contextMenuEvent(QContextMenuEvent *event) {
    if (const auto index = indexAt(viewport()->mapFromGlobal(event->globalPos())); index.isValid() && !selectionModel()->isSelected(index))
        setCurrentCell(index.row(), index.column());
    QMenu menu(this);
    auto *copy = menu.addAction(tr_text("Copy selected cells"), this, &RecognizedTableView::copy_selection);
    copy->setShortcut(QKeySequence::Copy);
    menu.addSeparator();
    menu.addAction(tr_text("Delete row"), this, &RecognizedTableView::delete_current_row)->setEnabled(table_.rows > 1);
    menu.addAction(tr_text("Delete column"), this, &RecognizedTableView::delete_current_column)->setEnabled(table_.columns > 1);
    menu.addSeparator();
    auto *undo = menu.addAction(tr_text("Undo"), &undo_, &QUndoStack::undo);
    undo->setShortcut(QKeySequence::Undo);
    undo->setEnabled(undo_.canUndo());
    style_popup_menu(&menu, ui_tokens(this));
    menu.exec(event->globalPos());
}

void RecognizedTableView::keyPressEvent(QKeyEvent *event) {
    if (state() != QAbstractItemView::EditingState) {
        if (event->matches(QKeySequence::Copy)) {
            copy_selection();
            return;
        }
        if (event->matches(QKeySequence::Undo)) {
            undo_.undo();
            return;
        }
        if (event->matches(QKeySequence::Redo)) {
            undo_.redo();
            return;
        }
        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
            clear_selection_text();
            return;
        }
    }
    QTableWidget::keyPressEvent(event);
}
}
