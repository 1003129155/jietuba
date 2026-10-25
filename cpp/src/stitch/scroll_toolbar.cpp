#include "stitch/scroll_toolbar.h"
#include "core/i18n.h"
#include "core/platform_utils.h"
#include "core/resource_manager.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/base_settings_panel.h"
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QVBoxLayout>

namespace jietuba {
namespace {
QString tr_text(const char *text) { return i18n_tr(text, "FloatingToolbar"); }

QString rgba(const QColor &color) { return QString("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha()); }

QIcon icon(const QString &name) { return ResourceManager::get_icon(ResourceManager::get_resource_path("svg/" + name)); }

const QColor DOT_COLOR(0x5F, 0x63, 0x68);
const QColor DOT_COLOR_IDLE(0xB4, 0xB8, 0xBE);
}

DragHandle::DragHandle(QWidget *parent) : QWidget(parent) {
    setCursor(Qt::SizeAllCursor);
    apply_scale();
}

void DragHandle::apply_scale() {
    setFixedWidth(scaled(BASE_WIDTH));
    update();
}

void DragHandle::set_manual_mode(bool manual) {
    if (manual_mode_ != manual) {
        manual_mode_ = manual;
        update();
    }
}

void DragHandle::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && manual_mode_) emit reset_requested();
    QWidget::mouseDoubleClickEvent(event);
}

void DragHandle::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto r = rect();
    const int cx = r.center().x(), cy = r.center().y();
    painter.setPen(Qt::NoPen);
    painter.setBrush(manual_mode_ ? DOT_COLOR : DOT_COLOR_IDLE);
    const int dot_r = scaled(BASE_DOT_RADIUS), gap = scaled(BASE_DOT_GAP);
    for (const int dy : {-gap, 0, gap}) painter.drawEllipse(QPoint(cx, cy + dy), dot_r, dot_r);
    painter.end();
}

FloatingToolbar::FloatingToolbar(QWidget *parent) : QWidget(parent) {
    setup_toolbar_window();
    setup_toolbar_ui();
    // Recomputes sizes when the scale changes; the connection ends with this widget.
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &FloatingToolbar::apply_scale);
}

void FloatingToolbar::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void FloatingToolbar::setup_toolbar_window() {
    setWindowFlags(Qt::WindowStaysOnTopHint | Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
}

void FloatingToolbar::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // The system restores the shadow after hide/show, so it is dropped on every show,
    // and only takes effect once the native window is shown.
    QTimer::singleShot(0, this, &FloatingToolbar::drop_system_shadow);
}

void FloatingToolbar::drop_system_shadow() { set_window_rounded_corners(static_cast<quintptr>(winId()), false); }

void FloatingToolbar::apply_scale() {
    setFixedHeight(scaled(BASE_HEIGHT));
    setMinimumWidth(scaled(BASE_MIN_WIDTH));
    row_layout_->setContentsMargins(0, 0, scaled(6), 0);
    left_handle->apply_scale();
    set_own_style(size_label, QString("color: %1; font-size: %2pt;").arg(CONTROL_INK.name()).arg(scaled(9)));
    size_label->ensurePolished();
    // Sized for the longest value so growing numbers neither widen nor move the toolbar.
    size_label->setFixedWidth(size_label->fontMetrics().horizontalAdvance(QString::fromUtf8("88888 × 88888")) + scaled(8));
    direction_btn->setFixedHeight(scaled(BASE_PILL_HEIGHT));
    direction_btn->setStyleSheet(direction_btn_style());
    direction_btn->setIconSize(QSize(scaled(14), scaled(14)));
    const int btn_w = scaled(BASE_BTN_WIDTH), btn_h = scaled(BASE_HEIGHT);
    const int icon_sz = scaled(BASE_ICON);
    for (auto *button : icon_buttons_) {
        button->setFixedSize(btn_w, btn_h);
        button->setIconSize(QSize(icon_sz, icon_sz));
        button->setStyleSheet(icon_btn_style());
    }
    adjustSize();
    // A new size needs re-docking beside the capture window, unless the user moved it.
    if (!manual_positioned_ && reposition) reposition();
}

void FloatingToolbar::setup_toolbar_ui() {
    auto *container = new QWidget;
    container->setObjectName("toolbar_container");
    container->setStyleSheet("QWidget#toolbar_container { background: transparent; }");

    auto *main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->addWidget(container);

    auto *toolbar_layout = new QHBoxLayout(container);
    // Buttons inset themselves with margins into rounded cells, so no extra spacing.
    toolbar_layout->setSpacing(0);
    row_layout_ = toolbar_layout;

    left_handle = new DragHandle(container);
    left_handle->setToolTip(tr_text("Drag to move"));
    left_handle->installEventFilter(this);
    connect(left_handle, &DragHandle::reset_requested, this, &FloatingToolbar::reset_auto_position);
    toolbar_layout->addWidget(left_handle);

    // Long image size, updated on every stitched frame and crop.
    size_label = new QLabel;
    size_label->setAlignment(Qt::AlignCenter);
    size_label->setToolTip(tr_text("Size of the long screenshot"));
    toolbar_layout->addWidget(size_label, 0, Qt::AlignVCenter);

    direction_btn = new QPushButton;
    direction_btn->setToolTip(tr_text("Switch scroll direction"));
    connect(direction_btn, &QPushButton::clicked, this, &FloatingToolbar::direction_changed);
    toolbar_layout->addWidget(direction_btn, 0, Qt::AlignVCenter);
    update_direction("vertical");

    // Stays pressed while scrolling.
    auto_scroll_btn = add_icon_button(toolbar_layout, QString::fromUtf8("自动滚动.svg"), tr_text("Auto scroll (move the mouse to stop)"),
                                      [this] { emit auto_scroll_clicked(); });
    auto_scroll_btn->setCheckable(true);
    // Crops the long image to the current view (the green frame in the preview).
    crop_btn = add_icon_button(toolbar_layout, QString::fromUtf8("裁剪.svg"), tr_text("Crop"), [this] { show_crop_menu(); });
    save_btn = add_icon_button(toolbar_layout, QString::fromUtf8("下载.svg"), tr_text("Save to file"), [this] { emit save_clicked(); });
    ocr_btn =add_icon_button(toolbar_layout, QString::fromUtf8("文字识别.svg"), tr_text("Recognize text (OCR)"), [this] { emit ocr_clicked(); });

    // The last three buttons match the capture toolbar's order and icons.
    cancel_btn = add_icon_button(toolbar_layout, QString::fromUtf8("结束截图.svg"), tr_text("Cancel long screenshot"), [this] { emit cancel_clicked(); });
    pin_btn = add_icon_button(toolbar_layout, QString::fromUtf8("钉图.svg"), tr_text("Pin to desktop"), [this] { emit pin_clicked(); });
    finish_btn = add_icon_button(toolbar_layout, QString::fromUtf8("确定.svg"), tr_text("Finish and save"), [this] { emit finish_clicked(); });

    apply_scale();
}

QPushButton *FloatingToolbar::add_icon_button(QHBoxLayout *layout, const QString &icon_name, const QString &tooltip, std::function<void()> slot) {
    auto *button = new QPushButton;
    button->setIcon(icon(icon_name));
    button->setToolTip(tooltip);
    connect(button, &QPushButton::clicked, this, [slot = std::move(slot)] { slot(); });
    layout->addWidget(button, 0, Qt::AlignVCenter);
    icon_buttons_.push_back(button);
    return button;
}

QString FloatingToolbar::direction_btn_style() {
    // Font size in pt, not px: pt follows the system DPI, and with Qt's high-DPI scaling
    // off a px size shrinks this button on high-DPI screens. The radius is half the
    // fixed height: a radius above half the height makes Qt ignore it entirely.
    return QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: none;
                padding: 0 %3px;
                margin: 0 %4px;
                font-size: %5pt;
                border-radius: %6px;
                min-width: %7px;
            }
            QPushButton:hover {
                background-color: %8;
            }
        )")
        .arg(rgba(CONTROL_FILL), CONTROL_INK.name())
        .arg(scaled(10))
        .arg(scaled(6))
        .arg(scaled(9))
        .arg(python_floordiv(scaled(BASE_PILL_HEIGHT), 2))
        .arg(scaled(50))
        .arg(rgba(CONTROL_FILL_HOVER));
}

QString FloatingToolbar::icon_btn_style() {
    // The same button cells as the main toolbar.
    const auto tc = get_theme().theme_color();
    return QString(R"(
            QPushButton {
                background-color: transparent;
                border: none;
                border-radius: %1px;
                margin: %2px;
            }
            QPushButton:hover {
                background-color: rgba(0, 0, 0, 0.06);
            }
            QPushButton:pressed {
                background-color: rgba(0, 0, 0, 0.12);
            }
            QPushButton:checked {
                background-color: rgba(%3, %4, %5, 0.3);
            }
        )")
        .arg(scaled(9))
        .arg(scaled(3))
        .arg(tc.red())
        .arg(tc.green())
        .arg(tc.blue());
}

void FloatingToolbar::update_direction(const QString &direction) {
    direction_ = direction;
    if (direction == "horizontal") {
        direction_btn->setIcon(icon(QString::fromUtf8("横向.svg")));
        direction_btn->setText(tr_text("Horizontal"));
    } else {
        direction_btn->setIcon(icon(QString::fromUtf8("竖向.svg")));
        direction_btn->setText(tr_text("Vertical"));
    }
}

void FloatingToolbar::set_result_size(int width, int height) { size_label->setText(QString::fromUtf8("%1 × %2").arg(width).arg(height)); }

void FloatingToolbar::set_auto_scrolling(bool running) { auto_scroll_btn->setChecked(running); }

QMenu *FloatingToolbar::crop_menu() {
    auto *menu = new QMenu(this);
    QString before, after;
    if (direction_ == "horizontal") {
        before = tr_text("Crop left: remove everything left of the current view");
        after = tr_text("Crop right: remove everything right of the current view");
    } else {
        before = tr_text("Crop top: remove everything above the current view");
        after = tr_text("Crop bottom: remove everything below the current view");
    }
    menu->addAction(before, this, [this] { emit crop_requested("top"); });
    menu->addAction(after, this, [this] { emit crop_requested("bottom"); });
    return menu;
}

void FloatingToolbar::show_crop_menu() {
    auto *menu = crop_menu();
    menu->exec(crop_btn->mapToGlobal(QPoint(0, crop_btn->height())));
    menu->deleteLater();
}

void FloatingToolbar::set_manual_positioned(bool manual) {
    manual_positioned_ = manual;
    left_handle->set_manual_mode(manual);
}

void FloatingToolbar::reset_auto_position() {
    set_manual_positioned(false);
    if (reposition) reposition();
}

bool FloatingToolbar::eventFilter(QObject *watched, QEvent *event) {
    if (watched == left_handle) {
        const auto type = event->type();
        // Double clicks are not intercepted; DragHandle handles them itself.
        if (type == QEvent::MouseButtonDblClick) return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (type == QEvent::MouseButtonPress) {
            if (mouse->button() == Qt::LeftButton) {
                dragging_ = true;
                drag_offset_ = mouse->globalPosition().toPoint() - frameGeometry().topLeft();
                left_handle->setCursor(Qt::ClosedHandCursor);
            }
            return true;
        }
        if (type == QEvent::MouseMove) {
            if (mouse->buttons() == Qt::LeftButton && dragging_) move(mouse->globalPosition().toPoint() - drag_offset_);
            return true;
        }
        if (type == QEvent::MouseButtonRelease) {
            if (mouse->button() == Qt::LeftButton) {
                if (dragging_) set_manual_positioned(true);
                dragging_ = false;
                left_handle->setCursor(Qt::SizeAllCursor);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
}
