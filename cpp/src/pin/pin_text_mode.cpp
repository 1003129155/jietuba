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
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace jietuba {
namespace {
QString pin_tr(const char *text) { return i18n_tr(text, "PinWindow"); }

const QColor PAGE_COLOR(255, 255, 255);
const QColor TEXT_COLOR(31, 31, 31);
constexpr int MIN_PIXEL_SIZE = 12, MAX_PIXEL_SIZE = 72;
// Recognized line boxes hug the glyphs, so the font is a little smaller than the box.
constexpr double FONT_TO_LINE = 0.8;
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
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // The scroll bar would otherwise take the panel's move cursor.
    setCursor(Qt::ArrowCursor);
    auto palette = this->palette();
    palette.setColor(QPalette::Base, PAGE_COLOR);
    palette.setColor(QPalette::Text, TEXT_COLOR);
    setPalette(palette);
}

void PinTextEdit::wheelEvent(QWheelEvent *event) {
    if (event->modifiers() & Qt::ControlModifier) {
        if (const int delta = event->angleDelta().y()) {
            auto font = this->font();
            font.setPixelSize(std::clamp(font.pixelSize() + (delta > 0 ? 1 : -1), MIN_PIXEL_SIZE, MAX_PIXEL_SIZE));
            setFont(font);
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
    // A text mode binding with Ctrl or Alt is never typed, so it switches back from here too.
    const bool leave_key = (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && PinShortcutController::instance()->matches(event, "inapp_pin_text_mode");
    if (leave_key || (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier)) {
        pin_->leave_text_mode();
        event->accept();
        return;
    }
    QTextEdit::keyPressEvent(event);
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
        build(selection->layout());
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
    editor_->setFocus(Qt::OtherFocusReason);
}

void PinTextMode::build(const OcrTextLayout &layout) {
    const double view_scale = win_->view ? std::sqrt(std::abs(win_->view->transform().determinant())) : 1.0;
    const int pixel_size = pin_text_pixel_size(layout, view_scale > 0 ? view_scale : 1.0);
    panel_ = new PinTextPanel(win_);
    panel_->hide();
    editor_ = new PinTextEdit(win_, panel_);
    QFont font("Microsoft YaHei UI");
    font.setPixelSize(pixel_size);
    editor_->setFont(font);
    const int margin = std::max(6, pixel_size / 2);
    auto *box = new QVBoxLayout(panel_);
    box->setContentsMargins(margin, margin, margin, margin);
    box->addWidget(editor_);
    editor_->setPlainText(pin_layout_text(layout, win_->config_manager));
}

void PinTextMode::fit_height() {
    // Measured on a copy: the editor lays its own document out at its widget width, which a
    // hidden editor does not have yet.
    const auto margins = panel_->layout()->contentsMargins();
    QTextDocument probe;
    probe.setDefaultFont(editor_->font());
    probe.setDocumentMargin(editor_->document()->documentMargin());
    probe.setPlainText(editor_->toPlainText());
    probe.setTextWidth(std::max(1, win_->width() - margins.left() - margins.right() - editor_->verticalScrollBar()->sizeHint().width()));
    const int needed = static_cast<int>(std::ceil(probe.size().height())) + margins.top() + margins.bottom();
    if (needed <= win_->height()) return;
    int height = needed;
    if (const auto *screen = win_->screen()) height = std::min(needed, std::max(win_->height(), screen->availableGeometry().bottom() + 1 - win_->y()));
    win_->resize(win_->width(), height);
}

void PinTextMode::exit() {
    waiting_ = false;
    if (!active_) return;
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
