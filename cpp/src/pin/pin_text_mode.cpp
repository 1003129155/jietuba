#include "pin/pin_text_mode.h"
#include "core/i18n.h"
#include "pin/ocr_text_layout.h"
#include "pin/pin_border_overlay.h"
#include "pin/pin_canvas_view.h"
#include "pin/pin_hover.h"
#include "pin/pin_shortcut.h"
#include "pin/pin_window.h"
#include "settings/tool_settings.h"
#include "text_recognition/layout.h"
#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace jietuba {
namespace {
QString pin_tr(const char *text) { return i18n_tr(text, "PinWindow"); }

const QColor PAGE_COLOR(255, 255, 255);
const QColor TEXT_COLOR(31, 31, 31);
constexpr int MIN_PIXEL_SIZE = 12, MAX_PIXEL_SIZE = 72;
// Long enough to read a sentence, unlike the one-second zoom percentage.
constexpr int HINT_MS = 2000;
// Either engine's line boxes run about a tenth taller than the font's pixel size.
constexpr double FONT_TO_LINE = 0.9;

QFont text_font(int pixel_size) {
    QFont font("Microsoft YaHei UI");
    font.setPixelSize(pixel_size);
    return font;
}

int panel_margin(int pixel_size) { return std::max(6, pixel_size / 2); }
}

QString pin_layout_text(const OcrTextLayout &layout, ToolSettingsManager *settings) {
    OcrResult result;
    for (const auto &line : layout.lines()) {
        const auto r = line.reading_bounds;
        result.data.append(OcrLine{{r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()}, line.text});
    }
    if (!settings) settings = get_tool_settings_manager();
    return text_layout::render(group_ocr_lines(result), settings->get_ocr_layout_mode(), settings->get_ocr_remove_spaces(), settings->get_ocr_punctuation()).trimmed();
}

int pin_text_pixel_size(const OcrTextLayout &layout, double view_scale) {
    std::vector<double> thickness;
    for (const auto &line : layout.lines()) thickness.push_back(line.frame.thickness);
    if (thickness.empty()) return MIN_PIXEL_SIZE;
    const auto middle = thickness.begin() + thickness.size() / 2;
    std::nth_element(thickness.begin(), middle, thickness.end());
    return std::clamp(static_cast<int>(std::lround(*middle * FONT_TO_LINE * view_scale)), MIN_PIXEL_SIZE, MAX_PIXEL_SIZE);
}

PinTextEdit::PinTextEdit(PinWindow *pin, QWidget *parent) : QTextEdit(parent), pin_(pin) {
    setAcceptRichText(false);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    // A pin smaller than the editor's usual minimum still lays it out within the pin.
    setMinimumSize(1, 1);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The scroll bar would otherwise take the panel's move cursor.
    setCursor(Qt::ArrowCursor);
    auto palette = this->palette();
    palette.setColor(QPalette::Base, PAGE_COLOR);
    palette.setColor(QPalette::Text, TEXT_COLOR);
    // Read-only the editor never has focus, which would grey out a mouse selection.
    for (const auto role : {QPalette::Highlight, QPalette::HighlightedText})
        palette.setColor(QPalette::Inactive, role, palette.color(QPalette::Active, role));
    setPalette(palette);
    // The cursor tells text from the blank that drags.
    viewport()->setMouseTracking(true);
}

void PinTextEdit::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        if (const int delta = event->angleDelta().y()) {
            auto font = this->font();
            const int old_size = font.pixelSize();
            font.setPixelSize(std::clamp(old_size + (delta > 0 ? 1 : -1), MIN_PIXEL_SIZE, MAX_PIXEL_SIZE));
            if (font.pixelSize() != old_size) setFont(font);
        }
    } else {
        QTextEdit::wheelEvent(event);
    }
    // At the ends of the text the wheel would reach the pin and zoom it.
    event->accept();
}

void PinTextEdit::contextMenuEvent(QContextMenuEvent *event) {
    pin_->show_context_menu(event->globalPos());
    event->accept();
}

void PinTextEdit::keyPressEvent(QKeyEvent *event) {
    // Esc reaches here when no pin is under the pointer to take it as a shortcut.
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        pin_->text_mode_controller()->stop_editing();
        event->accept();
        return;
    }
    // A text mode binding with Ctrl or Alt is never typed, so it switches back from here too.
    if ((event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && PinShortcutController::instance()->matches(event, "inapp_pin_text_mode")) {
        pin_->leave_text_mode();
        event->accept();
        return;
    }
    QTextEdit::keyPressEvent(event);
}

bool PinTextEdit::on_text(const QPoint &viewport_pos) const {
    const QPointF document_pos = QPointF(viewport_pos) + QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
    return document()->documentLayout()->hitTest(document_pos, Qt::ExactHit) >= 0;
}

void PinTextEdit::mousePressEvent(QMouseEvent *event) {
    if (isReadOnly() && event->button() == Qt::LeftButton && !on_text(event->position().toPoint())) {
        dragging_pin_ = true;
        pin_->start_window_drag(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    QTextEdit::mousePressEvent(event);
}

void PinTextEdit::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_pin_) {
        pin_->update_window_drag(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    if (event->buttons() == Qt::NoButton) viewport()->setCursor(!isReadOnly() || on_text(event->position().toPoint()) ? Qt::IBeamCursor : Qt::SizeAllCursor);
    QTextEdit::mouseMoveEvent(event);
}

void PinTextEdit::mouseReleaseEvent(QMouseEvent *event) {
    if (dragging_pin_ && event->button() == Qt::LeftButton) {
        dragging_pin_ = false;
        pin_->end_window_drag();
        event->accept();
        return;
    }
    QTextEdit::mouseReleaseEvent(event);
}

void PinTextEdit::mouseDoubleClickEvent(QMouseEvent *event) {
    // Off the text it is a second press of a drag, not a word to select.
    if (isReadOnly() && !on_text(event->position().toPoint())) {
        event->accept();
        return;
    }
    QTextEdit::mouseDoubleClickEvent(event);
}

void PinTextEdit::focusOutEvent(QFocusEvent *event) {
    QTextEdit::focusOutEvent(event);
    // Another window or widget took the keyboard; the pin's own menu and input popups do not end editing.
    if (event->reason() == Qt::ActiveWindowFocusReason || event->reason() == Qt::MouseFocusReason) pin_->text_mode_controller()->stop_editing();
}

PinTextPanel::PinTextPanel(PinWindow *pin) : QWidget(pin), pin_(pin) {
    setCursor(Qt::SizeAllCursor);
    setMouseTracking(true);
}

void PinTextPanel::paintEvent(QPaintEvent *) { QPainter(this).fillRect(rect(), PAGE_COLOR); }

void PinTextPanel::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) pin_->start_window_drag(event->globalPosition().toPoint());
    event->accept();
}

void PinTextPanel::mouseMoveEvent(QMouseEvent *event) {
    pin_->hover_controls->set_pin_hovered(true);
    if (pin_->is_dragging()) pin_->update_window_drag(event->globalPosition().toPoint());
    event->accept();
}

void PinTextPanel::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) pin_->end_window_drag();
    else if (event->button() == Qt::RightButton) pin_->show_context_menu(event->globalPosition().toPoint());
    event->accept();
}

void PinTextMode::enter() {
    if (active_ || waiting_ || win_->is_closed()) return;
    const auto *selection = win_->text_selection();
    if (editor_ || (selection && selection->has_text())) {
        show_text();
        return;
    }
    waiting_ = true;
    const bool started = win_->ocr_manager()->recognize_then([this](bool success, const QString &) {
        // Left (or switched to thumbnail or click-through) while recognizing.
        if (!waiting_) return;
        waiting_ = false;
        if (success) show_text();
        else win_->show_hint_label(pin_tr("No text was recognized"));
    });
    if (!started) {
        waiting_ = false;
        win_->show_hint_label(pin_tr("OCR recognition could not be started"));
        return;
    }
    win_->show_hint_label(pin_tr("Recognizing..."));
}

void PinTextMode::show_text() {
    if (active_ || win_->is_closed() || win_->thumbnail_mode() || win_->click_through()) return;
    if (!editor_) {
        const auto *selection = win_->text_selection();
        if (!selection || !selection->has_text()) return;
        const double view_scale = win_->view ? std::sqrt(std::abs(win_->view->transform().determinant())) : 1.0;
        build(pin_layout_text(selection->layout(), win_->config_manager), pin_text_pixel_size(selection->layout(), view_scale > 0 ? view_scale : 1.0));
    }
    win_->stop_drawing();
    active_ = true;
    prev_geometry_ = win_->geometry();
    fit_height();
    entry_pos_ = win_->pos();
    sync_geometry();
    panel_->show();
    panel_->raise();
    if (win_->border_overlay) win_->border_overlay->raise();
    win_->raise_control_buttons();
    win_->hover_controls->sync();
    win_->activateWindow();
    // Keys stay with the pin until editing starts.
    if (win_->view) win_->view->setFocus(Qt::OtherFocusReason);
}

void PinTextMode::start_editing() {
    if (!active_ || !editor_) return;
    if (!editing_) {
        editing_ = true;
        set_read_only(false);
        if (!editor_->textCursor().hasSelection()) editor_->moveCursor(QTextCursor::End);
        win_->show_hint_label(pin_tr("Press Esc to stop editing"), HINT_MS);
    }
    win_->activateWindow();
    editor_->setFocus(Qt::OtherFocusReason);
}

void PinTextMode::stop_editing() {
    if (!editing_) return;
    editing_ = false;
    // Also when another window took the keyboard: reactivating the pin gives its last focus widget
    // the keys whatever its focus policy, and a read-only editor would swallow them.
    const bool had_focus = editor_ && win_->focusWidget() == editor_;
    set_read_only(true);
    if (had_focus && win_->view) win_->view->setFocus(Qt::OtherFocusReason);
}

void PinTextMode::set_read_only(bool read_only) {
    editor_->setReadOnly(read_only);
    editor_->setTextInteractionFlags(read_only ? Qt::TextSelectableByMouse : Qt::TextEditorInteraction);
    // Read-only it never holds focus, so keys reach the pin's shortcuts.
    editor_->setFocusPolicy(read_only ? Qt::NoFocus : Qt::StrongFocus);
    editor_->viewport()->setCursor(Qt::IBeamCursor);
}

void PinTextMode::build(const QString &text, int pixel_size) {
    panel_ = new PinTextPanel(win_);
    panel_->hide();
    editor_ = new PinTextEdit(win_, panel_);
    editor_->setFont(text_font(pixel_size));
    const int margin = panel_margin(pixel_size);
    auto *box = new QVBoxLayout(panel_);
    box->setContentsMargins(margin, margin, margin, margin);
    box->addWidget(editor_);
    editor_->setPlainText(text);
    set_read_only(true);
    // The window resizes from the page's edges.
    panel_->installEventFilter(win_);
}

void PinTextMode::fit_height() {
    // Measured on a copy: the editor lays its own document out at its widget width, which a
    // hidden editor does not have yet.
    const auto margins = panel_->layout()->contentsMargins();
    const std::unique_ptr<QTextDocument> probe(editor_->document()->clone());
    // Grown to fit, the text needs no scroll bar, so it has the whole width; capped by the screen it scrolls anyway.
    probe->setTextWidth(std::max(1, win_->width() - margins.left() - margins.right()));
    const int needed = static_cast<int>(std::ceil(probe->size().height())) + margins.top() + margins.bottom();
    if (needed <= win_->height()) return;
    int height = needed;
    if (const auto *screen = win_->screen()) height = std::min(needed, std::max(win_->height(), screen->availableGeometry().bottom() + 1 - win_->y()));
    if (height != win_->height()) win_->resize(win_->width(), height);
}

void PinTextMode::exit() {
    waiting_ = false;
    if (!active_) return;
    stop_editing();
    active_ = false;
    panel_->hide();
    if (prev_geometry_) {
        // Back to its size, wherever it was dragged meanwhile.
        win_->setGeometry(QRect(prev_geometry_->topLeft() + (win_->pos() - entry_pos_), prev_geometry_->size()));
        prev_geometry_.reset();
    }
    win_->hover_controls->sync();
    if (win_->view) win_->view->setFocus(Qt::OtherFocusReason);
}

void PinTextMode::sync_geometry() {
    if (panel_) panel_->setGeometry(win_->rect());
}

QString PinTextMode::text() const { return editor_ ? editor_->toPlainText() : QString(); }

QString PinTextMode::text_to_copy() const {
    if (!editor_) return {};
    const auto cursor = editor_->textCursor();
    return cursor.hasSelection() ? cursor.selection().toPlainText() : editor_->toPlainText();
}


}
