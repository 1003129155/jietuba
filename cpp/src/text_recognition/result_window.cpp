#include "text_recognition/result_window.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "text_recognition/layout.h"
#include "text_recognition/placement.h"
#include "text_recognition/recognizer.h"
#include "text_recognition/table_view.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/fluent_lite.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/submenu_position.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "TextRecognitionWindow"); }

const QString VIEW_TEXT = "text";
const QString VIEW_TABLE = "table";

// The layout menu's choices in menu order. Labels are fetched when the menu opens,
// so a language switch shows in the next menu.
const std::vector<std::pair<QString, const char *>> &layout_labels() {
    static const std::vector<std::pair<QString, const char *>> labels = {
        {text_layout::MODE_AUTO, "Auto"}, {text_layout::MODE_SINGLE_LINE, "Remove line breaks"}, {text_layout::MODE_MULTILINE, "Keep every line"}};
    return labels;
}
const std::vector<std::pair<QString, const char *>> &punctuation_labels() {
    static const std::vector<std::pair<QString, const char *>> labels = {
        {text_layout::PUNCT_KEEP, "As recognized"}, {text_layout::PUNCT_HALFWIDTH, "Half-width"}, {text_layout::PUNCT_FULLWIDTH, "Full-width"}};
    return labels;
}

// Failure reason key to a sentence, fetched each time so a language switch applies.
QString reason_text(const QString &reason) {
    if (reason == recognition_reason::UNAVAILABLE) return tr_text("This build does not include the text recognition engine.");
    if (reason == recognition_reason::NO_TEXT) return tr_text("No text found in the selection.");
    if (reason == recognition_reason::NO_MODEL) return tr_text("No PP-OCR model is installed. Download one in the OCR settings.");
    if (reason == recognition_reason::NO_RUNTIME) return tr_text("ONNX Runtime, which PP-OCR needs, is not installed. Download it in the OCR settings.");
    if (reason == recognition_reason::MODEL_BROKEN)
        return tr_text("The PP-OCR model could not be loaded. Download it again or choose another one in the OCR settings.");
    return tr_text("Text recognition failed.");
}

// The lines laid out by the current settings.
QString render_lines(const QVector<OcrTextLine> &lines) {
    auto *manager = get_tool_settings_manager();
    return text_layout::render(lines, manager->get_ocr_layout_mode(), manager->get_ocr_remove_spaces(), manager->get_ocr_punctuation()).trimmed();
}

int char_count(const QString &text) { return static_cast<int>(text.toUcs4().size()); }

// The cells with the space and punctuation options applied, as the text gets them.
text_table::Table styled_table(const text_table::Table &table) {
    auto *manager = get_tool_settings_manager();
    const bool remove_spaces = manager->get_ocr_remove_spaces();
    const auto punctuation = manager->get_ocr_punctuation();
    return text_table::map_text(table, [&](const QString &text) {
        if (text.isEmpty()) return text;
        return text_layout::render({OcrTextLine{text, 0, 1, 0, 1}}, text_layout::MODE_MULTILINE, remove_spaces, punctuation).trimmed();
    });
}

QString table_size_text(const char *format, const text_table::Table &table) {
    return tr_text(format).replace("%1", QString::number(table.rows)).replace("%2", QString::number(table.columns));
}
}

LayoutMenuButton::LayoutMenuButton(const QString &text, QWidget *parent) : TransparentPushButton(text, parent) {
    // The base constructor styled it with its own sheet.
    _apply_theme();
}

QString LayoutMenuButton::style_sheet() const {
    // Text and arrow each take a side; the right padding leaves room for the arrow.
    // Full text colour, so it reads as a control beside the muted status.
    return TransparentPushButton::style_sheet() +
           QString("QPushButton { padding-right: %1px; color: %2; }").arg(widget_scaled(this, 22)).arg(ui_tokens(this).text);
}

void LayoutMenuButton::paintEvent(QPaintEvent *event) {
    TransparentPushButton::paintEvent(event);
    const double arm = widget_scaled(this, 4);
    const double x = width() - widget_scaled(this, 13);
    const double y = height() / 2.0;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(ui_tokens(this).text_muted), std::max(1.2, widget_scaled(this, 1) * 1.2));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    const QPointF points[] = {QPointF(x - arm, y - arm / 2.0), QPointF(x, y + arm / 2.0), QPointF(x + arm, y - arm / 2.0)};
    painter.drawPolyline(points, 3);
}

TextRecognitionWindow::TextRecognitionWindow(const QImage &image, QWidget *parent, const std::optional<QRect> &anchor)
    : FrostedFramelessDialog(parent), anchor_(anchor) {
    scale_dialog_font(*this);
    setAttribute(Qt::WA_DeleteOnClose, true);
    auto *title_bar = new FluentTitleBar(this);
    configure_dialog_control(*title_bar);
    setTitleBar(title_bar);
    setWindowTitle(tr_text("Text recognition"));

    auto *manager = get_tool_settings_manager();
    pin_button = new TitleBarPinButton(title_bar);
    configure_dialog_control(*pin_button);
    pin_button->setChecked(manager->get_ocr_result_on_top());
    connect(pin_button, &QAbstractButton::clicked, this, &TextRecognitionWindow::on_toggle_pin);
    title_bar->hBoxLayout->insertWidget(title_bar->hBoxLayout->indexOf(title_bar->minBtn), pin_button);

    view_switch = new SegmentedWidget(title_bar);
    // With its track drawn, so it does not read as part of the title.
    view_switch->setFloating(true);
    view_switch->setCompact(true);
    view_switch->addItem(VIEW_TEXT, tr_text("Text"));
    view_switch->addItem(VIEW_TABLE, tr_text("Table"));
    view_switch->setCurrentItem(VIEW_TEXT);
    configure_dialog_control(*view_switch);
    view_switch->hide();
    connect(view_switch, &SegmentedWidget::currentItemChanged, this, [this](const QString &view) { set_view(view, true); });
    const int after_title = title_bar->hBoxLayout->indexOf(title_bar->titleLabel) + 1;
    title_bar->hBoxLayout->insertSpacing(after_title, dialog_scaled(12));
    title_bar->hBoxLayout->insertWidget(after_title + 1, view_switch, 0, Qt::AlignVCenter);

    status_label = new CaptionLabel(tr_text("Recognizing..."), this);
    configure_dialog_control(*status_label);
    text_edit = new TextEdit(this);
    text_edit->setAcceptRichText(false);
    // Not editable while recognizing.
    text_edit->setReadOnly(true);

    // The table sits in a card drawn like the text box; the view itself is frameless.
    table_card_ = new QFrame(this);
    table_card_->setObjectName("tableCard");
    table_view = new RecognizedTableView(table_card_);
    auto *card_layout = new QVBoxLayout(table_card_);
    // The grid runs up to the card's border, which closes its edges.
    card_layout->setContentsMargins(0, 0, 0, 0);
    card_layout->addWidget(table_view);
    connect(table_view, &RecognizedTableView::table_changed, this, &TextRecognitionWindow::update_status);
    pages_ = new QStackedWidget(this);
    pages_->addWidget(text_edit);
    pages_->addWidget(table_card_);

    layout_button = new LayoutMenuButton(tr_text("Layout"), this);
    configure_dialog_control(*layout_button);
    layout_button->setEnabled(false);
    connect(layout_button, &QPushButton::clicked, this, &TextRecognitionWindow::show_layout_menu);

    copy_button = new PrimaryPushButton(tr_text("Copy"), this);
    configure_dialog_control(*copy_button);
    copy_button->setEnabled(false);
    connect(copy_button, &QPushButton::clicked, this, &TextRecognitionWindow::copy);

    // Takes effect at once: this window works as usual, later recognitions skip it;
    // turned off again on the settings' shortcut behaviour page.
    copy_directly_check = new CheckBox(tr_text("Copy directly next time"), this);
    configure_dialog_control(*copy_directly_check);
    copy_directly_check->setChecked(manager->get_ocr_copy_directly_enabled());
    connect(copy_directly_check, &QCheckBox::toggled, this, [](bool checked) { get_tool_settings_manager()->set_ocr_copy_directly_enabled(checked); });

    close_after_copy_check = new CheckBox(tr_text("Close after copying"), this);
    configure_dialog_control(*close_after_copy_check);
    close_after_copy_check->setChecked(manager->get_ocr_close_after_copy());
    connect(close_after_copy_check, &QCheckBox::toggled, this, [](bool checked) { get_tool_settings_manager()->set_ocr_close_after_copy(checked); });

    auto *footer = new QHBoxLayout;
    footer->setSpacing(dialog_scaled(8));
    footer->addWidget(layout_button);
    footer->addWidget(status_label, 1);
    footer->addWidget(copy_directly_check);
    // Wider than a box's gap to its own label, so each box reads with the right text.
    footer->addSpacing(dialog_scaled(8));
    footer->addWidget(close_after_copy_check);
    footer->addWidget(copy_button);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(dialog_scaled(12), title_bar->height() + dialog_scaled(8), dialog_scaled(12), dialog_scaled(12));
    root->setSpacing(dialog_scaled(8));
    root->addWidget(pages_, 1);
    root->addLayout(footer);

    apply_theme();
    update_pin_tooltip();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });
    place();

    // A cancelled recognition never reports, but finished always arrives; the
    // QPointer clears itself once the thread is deleted.
    thread_ = recognize_async(image, this, [this](const QVector<OcrTextLine> &lines, const QString &reason, const text_table::Table &table) {
        on_recognized(lines, reason, table);
    });
}

void TextRecognitionWindow::apply_theme() {
    const auto &t = ui_tokens(this);
    text_edit->setStyleSheet(QString("QTextEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: %4px %5px; "
                                     "font: %6px %7; selection-background-color: %8; selection-color: %9; }")
                                 .arg(t.input_background, t.text, t.border)
                                 .arg(dialog_scaled(8))
                                 .arg(dialog_scaled(10))
                                 .arg(dialog_scaled(14))
                                 .arg(FONT_FAMILY, t.accent, t.selected_text) +
                             scrollbar_qss(this));
    table_card_->setStyleSheet(QString("#tableCard { background: %1; border: 1px solid %2; border-radius: 6px; }").arg(t.input_background, t.border));
    table_view->apply_theme();
}

void TextRecognitionWindow::place() {
    auto *manager = get_tool_settings_manager();
    const auto position = manager->get_ocr_result_position();
    const auto last_center = manager->get_ocr_result_last_center();
    const auto cursor = QCursor::pos();
    std::vector<text_placement::ScreenArea> screens;
    for (auto *screen : QApplication::screens()) screens.push_back({screen->geometry(), screen->availableGeometry()});
    auto *primary_screen = QApplication::primaryScreen();
    const text_placement::ScreenArea primary{primary_screen->geometry(), primary_screen->availableGeometry()};
    const auto screen = text_placement::pick_screen(position, anchor_, cursor, last_center, screens, primary);
    const auto &available = screen.available;
    resize(std::min(dialog_scaled(WIDTH), available.width()), std::min(dialog_scaled(HEIGHT), available.height()));
    move(text_placement::window_position(position, size(), screen, anchor_, cursor, last_center));
}

void TextRecognitionWindow::showEvent(QShowEvent *event) {
    FrostedFramelessDialog::showEvent(event);
    // Only the z-order changes, without WindowStaysOnTopHint (see set_window_topmost),
    // so Qt does not maintain it: apply the current state on show.
    set_window_topmost(static_cast<quintptr>(winId()), pin_button->isChecked());
}

void TextRecognitionWindow::on_toggle_pin(bool checked) {
    get_tool_settings_manager()->set_ocr_result_on_top(checked);
    update_pin_tooltip();
    set_window_topmost(static_cast<quintptr>(winId()), checked);
}

void TextRecognitionWindow::update_pin_tooltip() { pin_button->setToolTip(pin_button->isChecked() ? tr_text("Always on top: ON") : tr_text("Always on top: OFF")); }

void TextRecognitionWindow::show_layout_menu() {
    auto *menu = build_layout_menu();
    // The button sits on the window's bottom edge, so the menu opens upwards.
    menu->exec(layout_button->mapToGlobal(QPoint(0, -menu->sizeHint().height())));
    menu->deleteLater();
}

QMenu *TextRecognitionWindow::build_layout_menu() {
    auto *manager = get_tool_settings_manager();
    auto *menu = new QMenu(this);

    const auto mode = manager->get_ocr_layout_mode();
    for (const auto &[value, label] : layout_labels()) {
        auto *action = menu->addAction(tr_text(label));
        action->setCheckable(true);
        action->setChecked(value == mode);
        // Line breaks do not apply to cells.
        action->setEnabled(!showing_table());
        const auto chosen = value;
        connect(action, &QAction::triggered, this, [this, chosen] { set_layout_mode(chosen); });
    }

    menu->addSeparator();
    auto *remove_spaces = menu->addAction(tr_text("Remove spaces"));
    remove_spaces->setCheckable(true);
    remove_spaces->setChecked(manager->get_ocr_remove_spaces());
    connect(remove_spaces, &QAction::triggered, this, &TextRecognitionWindow::set_remove_spaces);

    auto *copy_on_close = menu->addAction(tr_text("Copy text when closing"));
    copy_on_close->setCheckable(true);
    copy_on_close->setChecked(manager->get_ocr_copy_on_close());
    connect(copy_on_close, &QAction::triggered, this, [](bool checked) { get_tool_settings_manager()->set_ocr_copy_on_close(checked); });

    menu->addSeparator();
    auto *punctuation_menu = menu->addMenu(tr_text("Punctuation"));
    const auto current = manager->get_ocr_punctuation();
    for (const auto &[value, label] : punctuation_labels()) {
        auto *action = punctuation_menu->addAction(tr_text(label));
        action->setCheckable(true);
        action->setChecked(value == current);
        const auto chosen = value;
        connect(action, &QAction::triggered, this, [this, chosen] { set_punctuation(chosen); });
    }

    const auto &tokens = ui_tokens(this);
    style_popup_menu(menu, tokens);
    style_popup_menu(punctuation_menu, tokens);
    avoid_submenu_overlap(menu);
    return menu;
}

void TextRecognitionWindow::set_layout_mode(const QString &mode) {
    get_tool_settings_manager()->set_ocr_layout_mode(mode);
    relayout();
}

void TextRecognitionWindow::set_remove_spaces(bool enabled) {
    get_tool_settings_manager()->set_ocr_remove_spaces(enabled);
    relayout();
}

void TextRecognitionWindow::set_punctuation(const QString &punctuation) {
    get_tool_settings_manager()->set_ocr_punctuation(punctuation);
    relayout();
}

void TextRecognitionWindow::relayout() {
    show_result(render_lines(lines_));
    if (!table_.cells.isEmpty()) table_view->set_table(styled_table(table_), true);
}

void TextRecognitionWindow::show_result(const QString &text) {
    auto cursor = text_edit->textCursor();
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.insertText(text);
    cursor.endEditBlock();
    update_status();
}

bool TextRecognitionWindow::showing_table() const { return pages_->currentWidget() == table_card_; }

void TextRecognitionWindow::update_status() {
    if (showing_table())
        status_label->setText(table_size_text("%1 rows × %2 columns", table_view->table()));
    else
        status_label->setText(tr_text("Recognized %1 characters").replace("%1", QString::number(char_count(text_edit->toPlainText()))));
}

void TextRecognitionWindow::set_view(const QString &view, bool chosen) {
    const bool table = view == VIEW_TABLE && !table_.cells.isEmpty();
    if (chosen) get_tool_settings_manager()->set_ocr_result_view(table ? VIEW_TABLE : VIEW_TEXT);
    {
        const QSignalBlocker blocker(view_switch);
        view_switch->setCurrentItem(table ? VIEW_TABLE : VIEW_TEXT);
    }
    pages_->setCurrentWidget(table ? static_cast<QWidget *>(table_card_) : text_edit);
    if (table) fit_table_width();
    update_status();
    if (table)
        table_view->setFocus();
    else
        text_edit->setFocus();
}

void TextRecognitionWindow::fit_table_width() {
    if (table_fitted_) return;
    table_fitted_ = true;
    const int margins = width() - pages_->width();
    const int needed = table_view->content_width() + table_view->verticalScrollBar()->sizeHint().width() + dialog_scaled(4) + margins;
    const QRect available = screen()->availableGeometry();
    const int target = std::min(needed, static_cast<int>(available.width() * 0.8));
    if (target <= width()) return;
    // Grows to the right, moving left where the screen ends.
    QRect frame = geometry();
    frame.setWidth(target);
    if (frame.right() > available.right()) frame.moveRight(available.right());
    if (frame.left() < available.left()) frame.moveLeft(available.left());
    setGeometry(frame);
}

void TextRecognitionWindow::on_recognized(const QVector<OcrTextLine> &lines, const QString &reason, const text_table::Table &table) {
    if (!reason.isEmpty()) {
        status_label->setText(reason_text(reason));
        return;
    }
    lines_ = lines;
    text_edit->setReadOnly(false);
    show_result(render_lines(lines));
    // The first fill is not an edit, or one Ctrl+Z would clear the result.
    text_edit->document()->clearUndoRedoStacks();
    log_info(T("文字识别完成: {count} 个字符", {{"count", char_count(text_edit->toPlainText())}}), "OCR");
    copy_button->setEnabled(true);
    layout_button->setEnabled(true);
    text_edit->setFocus();
    table_ = table;
    if (!table.cells.isEmpty()) {
        table_view->set_table(styled_table(table), false);
        view_switch->show();
        log_info(T("文字识别发现表格: {rows} 行 {columns} 列", {{"rows", table.rows}, {"columns", table.columns}}), "OCR");
        set_view(get_tool_settings_manager()->get_ocr_result_view(), false);
    }
}

void TextRecognitionWindow::copy() {
    if (showing_table())
        copy_table(table_view->table());
    else
        QApplication::clipboard()->setText(text_edit->toPlainText());
    if (close_after_copy_check->isChecked()) {
        closing_after_copy_ = true;
        close();
        return;
    }
    copy_button->setText(tr_text("Copied"));
    QTimer::singleShot(1500, copy_button, [button = copy_button] { button->setText(tr_text("Copy")); });
}

void TextRecognitionWindow::closeEvent(QCloseEvent *event) {
    if (thread_) {
        // The result is no longer wanted; the thread finishes and is collected on its own.
        thread_->cancel();
        thread_ = nullptr;
    }
    auto *manager = get_tool_settings_manager();
    const auto text = text_edit->toPlainText();
    const bool copy_on_close = manager->get_ocr_copy_on_close() && !closing_after_copy_;
    if (copy_on_close && showing_table())
        copy_table(table_view->table());
    else if (copy_on_close && !text.isEmpty())
        QApplication::clipboard()->setText(text);
    if (manager->get_ocr_result_position() == "last_position") {
        const auto center = geometry().center();
        manager->set_ocr_result_last_center(center.x(), center.y());
    }
    FrostedFramelessDialog::closeEvent(event);
}

RecognitionToast::RecognitionToast(const QImage &image) : Toast(tr_text("Recognizing...")) {
    recognize_async(image, this, [this](const QVector<OcrTextLine> &lines, const QString &reason, const text_table::Table &table) {
        on_recognized(lines, reason, table);
    });
}

void RecognitionToast::on_recognized(const QVector<OcrTextLine> &lines, const QString &reason, const text_table::Table &table) {
    if (!reason.isEmpty()) {
        finish(reason_text(reason));
        return;
    }
    if (!table.cells.isEmpty() && get_tool_settings_manager()->get_ocr_result_view() == VIEW_TABLE) {
        copy_table(styled_table(table));
        log_info(T("文字识别完成并直接复制表格: {rows} 行 {columns} 列", {{"rows", table.rows}, {"columns", table.columns}}), "OCR");
        finish(table_size_text("Copied table (%1 rows × %2 columns)", table));
        return;
    }
    const auto text = render_lines(lines);
    QApplication::clipboard()->setText(text);
    log_info(T("文字识别完成并直接复制: {count} 个字符", {{"count", char_count(text)}}), "OCR");
    finish(tr_text("Copied %1 characters").replace("%1", QString::number(char_count(text))));
}

TextRecognitionWindow *show_text_recognition(const QImage &image, const std::optional<QRect> &anchor) {
    auto *window = new TextRecognitionWindow(image, nullptr, anchor);
    track_modeless_dialog(window);
    window->show();
    window->raise();
    window->activateWindow();
    return window;
}

RecognitionToast *copy_text_recognition(const QImage &image) { return new RecognitionToast(image); }
}
