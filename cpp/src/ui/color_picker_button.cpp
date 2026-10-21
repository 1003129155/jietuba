#include "ui/color_picker_button.h"
#include "core/theme.h"
#include "ui/base_settings_panel.h"
#include "ui/color_picker_dialog.h"
#include <QConicalGradient>
#include <QPainter>
#include <QPointer>

namespace jietuba {
namespace {
// Rainbow hues from red round to red.
constexpr std::pair<double, int> RING_STOPS[] = {{0.0, 0}, {1.0 / 6, 60}, {2.0 / 6, 120}, {3.0 / 6, 180}, {4.0 / 6, 240}, {5.0 / 6, 300}, {1.0, 360}};

QConicalGradient rainbow(const QPointF &center) {
    QConicalGradient gradient(center, 90);
    for (const auto &[stop, hue] : RING_STOPS) gradient.setColorAt(stop, QColor::fromHsv(hue % 360, 255, 255));
    return gradient;
}
}

ColorPickerButton::ColorPickerButton(const QColor &initial_color, bool show_alpha, int size, bool rainbow_ring, bool round_style, QWidget *parent)
    : QPushButton(parent), color_(initial_color), show_alpha_(show_alpha), rainbow_ring_(rainbow_ring || round_style), round_style_(round_style) {
    setFixedSize(size, size);
    if (rainbow_ring_) {
        setFlat(true);
        setAttribute(Qt::WA_Hover, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet("QPushButton { border: none; background: transparent; }");
    } else {
        update_style();
    }
    connect(this, &QPushButton::clicked, this, [this] { pick_color(); });
}

void ColorPickerButton::set_color(const QColor &color) {
    color_ = color;
    refresh();
}

void ColorPickerButton::link_swatches(const QVector<ColorSwatch *> &swatches) {
    swatches_ = swatches;
    sync_swatches();
}

void ColorPickerButton::sync_swatches() {
    if (swatches_.isEmpty()) return;
    const auto rgb = color_.rgb() & 0xFFFFFF;
    bool matched = false;
    for (auto *swatch : swatches_) {
        const bool hit = !matched && (QColor(swatch->color_hex).rgb() & 0xFFFFFF) == rgb;
        swatch->set_selected(hit);
        matched = matched || hit;
    }
    if (custom_active_ == matched) {
        custom_active_ = !matched;
        update();
    }
}

void ColorPickerButton::pick_color() {
    // Pin and GIF panels are separate windows; their host may be closed while
    // the dialog runs, deleting this button.
    QPointer<ColorPickerButton> self(this);
    const auto options = show_alpha_ ? ColorPickerDialog::ShowAlphaChannel : ColorPickerDialog::NoOption;
    const auto picked = choose_color(color_, this, {}, options);
    if (picked && self) {
        color_ = *picked;
        refresh();
        emit color_changed(color_);
    }
}

void ColorPickerButton::refresh() {
    sync_swatches();
    if (rainbow_ring_) update();
    else update_style();
}

void ColorPickerButton::enterEvent(QEnterEvent *event) {
    update();
    QPushButton::enterEvent(event);
}

void ColorPickerButton::leaveEvent(QEvent *event) {
    update();
    QPushButton::leaveEvent(event);
}

void ColorPickerButton::paintEvent(QPaintEvent *event) {
    if (!rainbow_ring_) {
        QPushButton::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (round_style_) {
        paint_round(&painter);
        return;
    }
    const double size = std::min(width(), height());
    const double ring_width = std::max(1.8, size * 0.11);
    const double gap = std::max(1.0, size * 0.05);
    const auto outer_rect = QRectF(rect()).adjusted(ring_width / 2, ring_width / 2, -ring_width / 2, -ring_width / 2);
    const double outer_radius = size * 0.26;
    // The rounded square keeps its shape; only the outline is the rainbow marking "custom colour".
    painter.setPen(QPen(QBrush(rainbow(outer_rect.center())), ring_width));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(outer_rect, outer_radius, outer_radius);
    const double inset = ring_width / 2 + gap;
    auto inner_rect = outer_rect.adjusted(inset, inset, -inset, -inset);
    const double inner_radius = std::max(1.0, outer_radius - inset);
    if (isDown()) {
        const double shrink = inner_rect.width() * 0.06;
        inner_rect = inner_rect.adjusted(shrink, shrink, -shrink, -shrink);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QBrush(color_));
    painter.drawRoundedRect(inner_rect, inner_radius, inner_radius);
    if (underMouse() && !isDown()) {
        painter.setPen(QPen(QColor(0, 0, 0, 70), 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(inner_rect, inner_radius, inner_radius);
    }
}

void ColorPickerButton::paint_round(QPainter *painter) {
    // Same proportions as the round swatches (radius 10 in a 28 hit area); a
    // colour outside the presets gets the selection ring here.
    const double side = std::min(width(), height());
    const auto center = QRectF(rect()).center();
    double radius = side * 10 / 28;
    if (custom_active_) {
        painter->setPen(QPen(get_theme().theme_color(), side * 2 / 28));
        painter->setBrush(Qt::NoBrush);
        const double ring = radius + side * 3.5 / 28;
        painter->drawEllipse(center, ring, ring);
    } else if (underMouse()) {
        painter->setPen(QPen(QColor(15, 23, 42, 50), 1.0));
        painter->setBrush(Qt::NoBrush);
        const double ring = radius + side * 2.5 / 28;
        painter->drawEllipse(center, ring, ring);
    }
    if (isDown()) radius *= 0.9;
    const double ring_width = side * 2.4 / 28;
    painter->setPen(QPen(QBrush(rainbow(center)), ring_width));
    painter->setBrush(Qt::NoBrush);
    const double outer = radius - ring_width / 2;
    painter->drawEllipse(center, outer, outer);
    const double inner = radius - ring_width - side * 1.5 / 28;
    painter->setPen(Qt::NoPen);
    painter->setBrush(QBrush(color_));
    painter->drawEllipse(center, inner, inner);
}

void ColorPickerButton::update_style() {
    const auto color_hex = color_.name();
    const auto border_color = color_.lightness() > 200 ? QString("#888888") : QString("#333333");
    setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                border: 2px solid %2;
                border-radius: 3px;
            }
            QPushButton:hover {
                background-color: %1;
                border: 2px solid #000;
            }
            QPushButton:pressed {
                background-color: %1;
                border: 3px solid #000;
            }
        )").arg(color_hex, border_color));
}
}
