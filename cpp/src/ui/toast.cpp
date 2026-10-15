#include "ui/toast.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/theme.h"
#include <QApplication>
#include <QCursor>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba {
Toast::Toast(const QString &text) : QWidget(nullptr) {
    setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, true);

    const auto &t = ui_tokens();
    label = new QLabel(this);
    label->setTextFormat(Qt::PlainText);
    label->setStyleSheet(QString("QLabel { background: %1; color: %2; border: 1px solid %3; border-radius: %4px; padding: %5px %6px; font: %7px %8; }")
                             .arg(t.popup_background, t.text, t.window_border)
                             .arg(dialog_scaled(8))
                             .arg(dialog_scaled(6))
                             .arg(dialog_scaled(12))
                             .arg(dialog_scaled(13))
                             .arg(FONT_FAMILY));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(label);

    anchor_ = QCursor::pos();
    set_text(text);
    show();
}

void Toast::set_text(const QString &text) {
    label->ensurePolished();
    static const QRegularExpression whitespace(R"(\s+)", QRegularExpression::UseUnicodePropertiesOption);
    const auto line = text.split(whitespace, Qt::SkipEmptyParts).join(' ');
    label->setText(label->fontMetrics().elidedText(line, Qt::ElideRight, dialog_scaled(MAX_TEXT_WIDTH)));
    adjustSize();
    place();
}

void Toast::finish(const QString &text) {
    set_text(text);
    finish();
}

void Toast::finish() {
    if (finished_) return;
    finished_ = true;
    QTimer::singleShot(HOLD_MS, this, &Toast::fade_out);
}

void Toast::fade_out() {
    auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(FADE_MS);
    fade->setStartValue(1.0);
    fade->setEndValue(0.0);
    connect(fade, &QPropertyAnimation::finished, this, &QWidget::close);
    fade->start();
}

void Toast::place() {
    auto *screen = QApplication::screenAt(anchor_);
    if (!screen) screen = QApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    const int offset = dialog_scaled(16);
    const int x = std::min(anchor_.x() + offset, area.right() - width());
    int y = anchor_.y() + offset;
    if (y + height() > area.bottom()) y = anchor_.y() - offset - height();
    move(std::max(x, area.left()), std::max(y, area.top()));
}

Toast *show_toast(const QString &text) {
    auto *toast = new Toast(text);
    toast->finish();
    return toast;
}
}
