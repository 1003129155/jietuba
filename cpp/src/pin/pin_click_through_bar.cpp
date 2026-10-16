#include "pin/pin_click_through_bar.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "pin/pin_window.h"
#include "ui/base_settings_panel.h"
#include <QApplication>
#include <QMouseEvent>
#include <QScreen>

namespace jietuba {
namespace {
QString bar_tr(const char *text) { return i18n_tr(text, "PinClickThroughBar"); }
}

PinClickThroughMoveHandle::PinClickThroughMoveHandle(QWidget *bar, PinWindow *pin) : QLabel(bar), pin_(pin) {
    setCursor(Qt::SizeAllCursor);
    setAlignment(Qt::AlignCenter);
}

void PinClickThroughMoveHandle::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        pin_->start_window_drag(event->globalPosition().toPoint());
        event->accept();
    }
}

void PinClickThroughMoveHandle::mouseMoveEvent(QMouseEvent *event) {
    pin_->update_window_drag(event->globalPosition().toPoint());
    event->accept();
}

void PinClickThroughMoveHandle::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        pin_->end_window_drag();
        event->accept();
    }
}

PinClickThroughBar::PinClickThroughBar(PinWindow *pin_window)
    : QWidget(pin_window, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint), pin_window(pin_window) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    handle_ = new PinClickThroughMoveHandle(this, pin_window);
    handle_->setToolTip(bar_tr("Drag to move"));
    title_ = new QLabel(bar_tr("Opacity"), this);
    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(static_cast<int>(python_round(MIN_OPACITY * 100)), 100);
    slider_->setFocusPolicy(Qt::NoFocus);
    connect(slider_, &QSlider::valueChanged, this, &PinClickThroughBar::on_slider_changed);
    value_ = new QLabel(this);
    value_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    exit_button_ = new QPushButton(this);
    exit_button_->setIcon(ResourceManager::get_icon(ResourceManager::get_resource_path("svg/fluent_icons/Close.svg")));
    exit_button_->setToolTip(bar_tr("Exit click-through"));
    exit_button_->setFocusPolicy(Qt::NoFocus);
    exit_button_->setCursor(Qt::PointingHandCursor);
    connect(exit_button_, &QPushButton::clicked, this, [this] { this->pin_window->set_click_through(false); });
    layout_ = new QHBoxLayout(this);
    layout_->addWidget(handle_);
    layout_->addWidget(title_);
    layout_->addWidget(slider_);
    layout_->addWidget(value_);
    layout_->addWidget(exit_button_);
    apply_scale();
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &PinClickThroughBar::apply_scale);
}

void PinClickThroughBar::apply_scale() {
    const int height = scaled(BASE_HEIGHT);
    const int icon = scaled(BASE_ICON);
    layout_->setContentsMargins(scaled(4), 0, scaled(4), 0);
    layout_->setSpacing(scaled(6));
    handle_->setFixedSize(height - scaled(6), height - scaled(6));
    handle_->setPixmap(ResourceManager::get_icon(ResourceManager::get_resource_path(QString::fromUtf8("svg/移动窗口.svg"))).pixmap(QSize(icon, icon), devicePixelRatioF()));
    exit_button_->setFixedSize(height - scaled(6), height - scaled(6));
    exit_button_->setIconSize(QSize(scaled(12), scaled(12)));
    slider_->setFixedWidth(scaled(BASE_SLIDER_WIDTH));
    value_->setFixedWidth(scaled(34));
    const auto theme = get_theme().theme_color_hex();
    slider_->setStyleSheet(QString(R"(
            QSlider::groove:horizontal {
                height: %1px; background: #d9dce3; border-radius: %2px;
            }
            QSlider::handle:horizontal {
                width: %3px; height: %3px; margin: %4px 0;
                background: %5; border-radius: %6px;
            }
            QSlider::sub-page:horizontal {
                background: %5; border-radius: %2px;
            }
        )").arg(scaled(4)).arg(scaled(2)).arg(scaled(12)).arg(-scaled(4)).arg(theme).arg(scaled(6)));
    const auto text_style = QString("font-size: %1px; background: transparent;").arg(scaled(12));
    set_own_style(title_, QString("color: %1; %2").arg(CONTROL_INK.name(), text_style));
    set_own_style(value_, QString("color: %1; %2").arg(CONTROL_SUB.name(), text_style));
    exit_button_->setStyleSheet(QString(R"(
            QPushButton { background: transparent; border: none; border-radius: %1px; }
            QPushButton:hover { background: rgba(231, 76, 60, 40); }
            QPushButton:pressed { background: rgba(231, 76, 60, 80); }
        )").arg(scaled(6)));
    setFixedHeight(height);
    adjustSize();
    if (isVisible()) follow_pin();
}

void PinClickThroughBar::set_opacity(double opacity) {
    slider_->blockSignals(true);
    slider_->setValue(static_cast<int>(python_round(opacity * 100)));
    slider_->blockSignals(false);
    value_->setText(QString("%1%").arg(slider_->value()));
}

void PinClickThroughBar::on_slider_changed(int value) {
    value_->setText(QString("%1%").arg(value));
    pin_window->set_click_through_opacity(value / 100.0);
}

void PinClickThroughBar::follow_pin() {
    const auto pin_geometry = pin_window->frameGeometry();
    auto *screen = pin_window->screen();
    if (!screen) screen = QApplication::primaryScreen();
    const auto area = screen->geometry();
    const int gap = scaled(4);
    int y = pin_geometry.top() - height() - gap;
    if (y < area.top()) {
        y = pin_geometry.bottom() + 1 + gap;
        if (y + height() > area.bottom()) y = pin_geometry.top() + gap;
    }
    const int x = std::max(area.left(), std::min(pin_geometry.left(), area.right() - width()));
    move(x, y);
}

void PinClickThroughBar::setVisible(bool visible) {
    if (visible) follow_pin();
    QWidget::setVisible(visible);
    if (visible) raise();
}

void PinClickThroughBar::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }
}
