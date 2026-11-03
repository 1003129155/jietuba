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
#include <functional>

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

RecognizedTableView::Bounds RecognizedTableView::selection_bounds() const {
    Bounds b;
    const auto ranges = selectedRanges();
    if (ranges.isEmpty()) {
        if (currentRow() < 0 || currentColumn() < 0) return b;
        b = {currentRow(), currentColumn(), currentRow(), currentColumn()};
    } else {
        b = {ranges.front().topRow(), ranges.front().leftColumn(), ranges.front().bottomRow(), ranges.front().rightColumn()};
        for (const auto &range : ranges) {
            b.row0 = std::min(b.row0, range.topRow());
            b.row1 = std::max(b.row1, range.bottomRow());
            b.column0 = std::min(b.column0, range.leftColumn());
            b.column1 = std::max(b.column1, range.rightColumn());
        }
    }
    // Growing over one merged cell can meet another; repeat until nothing grows.
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto &c : table_.cells) {
            if (c.row > b.row1 || c.row + c.row_span <= b.row0 || c.column > b.column1 || c.column + c.column_span <= b.column0) continue;
            const Bounds next{std::min(b.row0, c.row), std::min(b.column0, c.column), std::max(b.row1, c.row + c.row_span - 1),
                              std::max(b.column1, c.column + c.column_span - 1)};
            if (next.row0 != b.row0 || next.column0 != b.column0 || next.row1 != b.row1 || next.column1 != b.column1) {
                b = next;
                grew = true;
            }
        }
    }
    return b;
}

text_table::Table RecognizedTableView::selected_table() const {
    const auto b = selection_bounds();
    if (!b.valid()) return {};
    return text_table::part(table_, b.row0, b.column0, b.row1, b.column1);
}

void RecognizedTableView::copy_selection() {
    const auto part = selected_table();
    if (!part.cells.isEmpty()) copy_table(part);
}

void RecognizedTableView::cut_selection() {
    copy_selection();
    clear_selection_text();
}

void RecognizedTableView::paste() {
    const auto b = selection_bounds();
    const auto grid = text_table::parse_tsv(QApplication::clipboard()->text());
    if (!b.valid() || grid.isEmpty()) return;
    if (grid.size() == 1 && grid.front().size() == 1 && selectedItems().size() > 1) {
        auto table = table_;
        for (const auto *selected : selectedItems()) table = text_table::with_text(table, selected->row(), selected->column(), grid.front().front());
        push(table);
        return;
    }
    push(text_table::pasted(table_, b.row0, b.column0, grid));
}

void RecognizedTableView::insert_row(bool below) {
    const auto b = selection_bounds();
    if (!b.valid()) return;
    const int row = below ? b.row1 + 1 : b.row0, column = currentColumn();
    push(text_table::with_row_inserted(table_, row));
    setCurrentCell(row, std::max(0, column));
}

void RecognizedTableView::insert_column(bool right) {
    const auto b = selection_bounds();
    if (!b.valid()) return;
    const int column = right ? b.column1 + 1 : b.column0, row = currentRow();
    push(text_table::with_column_inserted(table_, column));
    setCurrentCell(std::max(0, row), column);
}

void RecognizedTableView::delete_selected_rows() {
    const auto b = selection_bounds();
    if (!b.valid() || b.row1 - b.row0 + 1 >= table_.rows) return;
    auto table = table_;
    for (int r = b.row1; r >= b.row0; --r) table = text_table::without_row(table, r);
    push(table);
}

void RecognizedTableView::delete_selected_columns() {
    const auto b = selection_bounds();
    if (!b.valid() || b.column1 - b.column0 + 1 >= table_.columns) return;
    auto table = table_;
    for (int c = b.column1; c >= b.column0; --c) table = text_table::without_column(table, c);
    push(table);
}

void RecognizedTableView::merge_selection() {
    const auto b = selection_bounds();
    if (!b.valid() || !text_table::can_merge(table_, b.row0, b.column0, b.row1, b.column1)) return;
    push(text_table::merged(table_, b.row0, b.column0, b.row1, b.column1));
    setCurrentCell(b.row0, b.column0);
}

void RecognizedTableView::split_selection() {
    const auto b = selection_bounds();
    if (b.valid()) push(text_table::split(table_, b.row0, b.column0, b.row1, b.column1));
}

void RecognizedTableView::clear_selection_text() {
    auto table = table_;
    for (const auto *selected : selectedItems()) table = text_table::with_text(table, selected->row(), selected->column(), QString());
    push(table);
}

void RecognizedTableView::contextMenuEvent(QContextMenuEvent *event) {
    if (const auto index = indexAt(viewport()->mapFromGlobal(event->globalPos())); index.isValid() && !selectionModel()->isSelected(index))
        setCurrentCell(index.row(), index.column());
    const auto b = selection_bounds();
    const bool any = b.valid();
    QMenu menu(this);
    auto add = [&](const char *text, std::function<void()> slot, bool enabled, const QKeySequence &shortcut = {}) {
        auto *action = menu.addAction(tr_text(text), this, std::move(slot));
        action->setEnabled(enabled);
        action->setShortcut(shortcut);
    };
    add("Copy selected cells", [this] { copy_selection(); }, any, QKeySequence::Copy);
    add("Cut", [this] { cut_selection(); }, any, QKeySequence::Cut);
    add("Paste", [this] { paste(); }, any && !QApplication::clipboard()->text().isEmpty(), QKeySequence::Paste);
    menu.addSeparator();
    add("Insert row above", [this] { insert_row(false); }, any);
    add("Insert row below", [this] { insert_row(true); }, any);
    add("Insert column left", [this] { insert_column(false); }, any);
    add("Insert column right", [this] { insert_column(true); }, any);
    add("Delete row", [this] { delete_selected_rows(); }, any && b.row1 - b.row0 + 1 < table_.rows);
    add("Delete column", [this] { delete_selected_columns(); }, any && b.column1 - b.column0 + 1 < table_.columns);
    menu.addSeparator();
    add("Merge cells", [this] { merge_selection(); }, any && text_table::can_merge(table_, b.row0, b.column0, b.row1, b.column1));
    add("Split cells", [this] { split_selection(); }, any && !(text_table::split(table_, b.row0, b.column0, b.row1, b.column1) == table_));
    menu.addSeparator();
    add("Undo", [this] { undo_.undo(); }, undo_.canUndo(), QKeySequence::Undo);
    add("Redo", [this] { undo_.redo(); }, undo_.canRedo(), QKeySequence::Redo);
    style_popup_menu(&menu, ui_tokens(this));
    menu.exec(event->globalPos());
}

void RecognizedTableView::keyPressEvent(QKeyEvent *event) {
    if (state() != QAbstractItemView::EditingState) {
        if (event->matches(QKeySequence::Copy)) {
            copy_selection();
            return;
        }
        if (event->matches(QKeySequence::Cut)) {
            cut_selection();
            return;
        }
        if (event->matches(QKeySequence::Paste)) {
            paste();
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
