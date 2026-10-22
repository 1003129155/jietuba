#include "ui/selection_info/rounded_corners.h"
#include "canvas/items/selection_item.h"
#include "canvas/selection_model.h"
#include "core/export.h"
#include "core/logger.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/mask_overlay.h"
#include <QEvent>
#include <QPainter>
#include <QPainterPath>

namespace jietuba {
namespace {
const QColor POPUP_BG(30, 30, 30, 220);
}

RoundedSliderPopup::RoundedSliderPopup(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    init_ui();
    hide();
    // Connection ends with this widget.
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &RoundedSliderPopup::apply_scale);
    hide_timer_ = new QTimer(this);
    hide_timer_->setSingleShot(true);
    hide_timer_->setInterval(200);
    connect(hide_timer_, &QTimer::timeout, this, &RoundedSliderPopup::do_hide);
}

void RoundedSliderPopup::init_ui() {
    row_layout_ = new QHBoxLayout(this);
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(0, 100);
    slider_->setValue(16);
    slider_->setFocusPolicy(Qt::NoFocus);
    row_layout_->addWidget(slider_);
    label_ = new QLabel("16");
    label_->setAlignment(Qt::AlignCenter);
    row_layout_->addWidget(label_);
    connect(slider_, &QSlider::valueChanged, this, [this](int value) {
        label_->setText(QString::number(value));
        emit radius_changed(value);
    });
    apply_scale();
}

void RoundedSliderPopup::apply_scale() {
    setFixedSize(scaled(BASE_WIDTH), scaled(BASE_HEIGHT));
    row_layout_->setContentsMargins(scaled(10), 0, scaled(10), 0);
    row_layout_->setSpacing(scaled(6));
    slider_->setFixedWidth(scaled(BASE_SLIDER_WIDTH));
    const auto thx = get_theme().theme_color_hex();
    slider_->setStyleSheet(QString(R"(
            QSlider::groove:horizontal {
                height: %1px; background: #555; border-radius: %2px;
            }
            QSlider::handle:horizontal {
                width: %3px; height: %3px; margin: %4px 0;
                background: %5; border-radius: %6px;
            }
            QSlider::sub-page:horizontal {
                background: %5; border-radius: %2px;
            }
        )").arg(scaled(4)).arg(scaled(2)).arg(scaled(12)).arg(-scaled(4)).arg(thx).arg(scaled(6)));
    label_->setFixedWidth(scaled(BASE_LABEL_WIDTH));
    set_own_style(label_, QString("color: #D0D0D0; font-size: %1px; background: transparent;").arg(scaled(12)));
    update();
    if (isVisible() && anchor_) show_near(anchor_);
}

void RoundedSliderPopup::do_hide() {
    // Not while the pointer is still on the popup.
    if (underMouse()) return;
    hide();
}

void RoundedSliderPopup::show_near(QWidget *anchor) {
    anchor_ = anchor;
    auto *parent = parentWidget();
    const int pw = parent->width();
    // Centred horizontally on the anchor.
    const auto local_ref = parent->mapFromGlobal(anchor->mapToGlobal(QPoint(0, 0)));
    int x = local_ref.x() - python_floordiv(width() - anchor->width(), 2);
    if (x + width() > pw) x = pw - width();
    x = std::max(0, x);
    // Above first.
    const int gap = scaled(4);
    const auto above_local = parent->mapFromGlobal(anchor->mapToGlobal(QPoint(0, -height() - gap)));
    int y;
    if (above_local.y() >= 0) y = above_local.y();
    else y = parent->mapFromGlobal(anchor->mapToGlobal(QPoint(0, anchor->height() + gap))).y();
    move(x, y);
    show();
    raise();
}

void RoundedSliderPopup::enterEvent(QEnterEvent *event) {
    cancel_hide();
    QWidget::enterEvent(event);
}

void RoundedSliderPopup::leaveEvent(QEvent *event) {
    schedule_hide();
    QWidget::leaveEvent(event);
}

void RoundedSliderPopup::paintEvent(QPaintEvent *event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(POPUP_BG));
    const int radius = scaled(BASE_RADIUS);
    p.drawRoundedRect(rect(), radius, radius);
    p.end();
    QWidget::paintEvent(event);
}

RoundedCornersLogic::RoundedCornersLogic(QWidget *parent_widget, QPushButton *btn_rounded, SelectionItem *selection_item, MaskOverlayWidget *mask_overlay,
                                         ExportService *export_service, SelectionModel *selection_model, ToolSettingsManager *config_manager)
    : QObject(parent_widget), btn_(btn_rounded), item_(selection_item), mask_(mask_overlay), export_(export_service), model_(selection_model), config_(config_manager) {
    // Last state from settings.
    if (config_) {
        enabled_ = config_->get_screenshot_rounded_enabled();
        radius_ = config_->get_screenshot_rounded_radius();
    }
    popup_ = new RoundedSliderPopup(parent_widget);
    popup_->set_value(radius_);
    // Initial checked state without emitting.
    btn_->blockSignals(true);
    btn_->setChecked(enabled_);
    btn_->blockSignals(false);
    connect(popup_, &RoundedSliderPopup::radius_changed, this, &RoundedCornersLogic::on_radius_changed);
    // The popup may go first; later button events must not reach it.
    connect(popup_, &QObject::destroyed, this, [this] {
        if (btn_) btn_->removeEventFilter(this);
    });
    btn_->setMouseTracking(true);
    btn_->installEventFilter(this);
    install_render_hook();
    install_mask_hook();
    install_export_hook();
}

void RoundedCornersLogic::set_enabled(bool enabled) {
    enabled_ = enabled;
    log_debug(T("圆角截图: {state}  r={radius}", {{"state", QString(enabled ? "ON" : "OFF")}, {"radius", radius_}}), "RoundedCorners");
    if (config_) config_->set_screenshot_rounded_enabled(enabled);
    if (enabled) {
        // The pointer is already on the button, so no Enter will follow: open now.
        popup_->cancel_hide();
        popup_->show_near(btn_);
    } else {
        popup_->hide();
    }
    item_->request_repaint();
    mask_->update();
}

bool RoundedCornersLogic::eventFilter(QObject *watched, QEvent *event) {
    if (watched == btn_ && popup_) {
        if (event->type() == QEvent::Enter) {
            if (enabled_) {
                popup_->cancel_hide();
                popup_->show_near(btn_);
            }
        } else if (event->type() == QEvent::Leave) {
            if (enabled_) popup_->schedule_hide();
        }
    }
    return false;
}

void RoundedCornersLogic::on_radius_changed(int value) {
    radius_ = value;
    log_debug(T("圆角半径: {value}", {{"value", value}}), "RoundedCorners");
    if (config_) config_->set_screenshot_rounded_radius(value);
    item_->request_repaint();
    mask_->update();
}

void RoundedCornersLogic::install_render_hook() {
    item_->render_override = [this](QPainter *painter) {
        if (model_->is_empty()) return true;
        if (!(enabled_ && radius_ > 0)) return false;
        const auto rect = model_->rect();
        const int r = radius_;
        painter->setPen(QPen(get_theme().theme_color(), item_->border_width(), Qt::SolidLine));
        painter->setBrush(Qt::NoBrush);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->drawRoundedRect(rect, r, r);
        // Corner handles cannot follow the arc, so they are left out.
        if (item_->handles_visible()) item_->draw_handles(painter, rect, true);
        return true;
    };
    render_installed_ = true;
}

void RoundedCornersLogic::paint_mask_corners() {
    if (!enabled_ || radius_ <= 0) return;
    if (model_->is_empty()) return;
    const auto local_sel = mask_->scene_to_local(model_->rect());
    const int r = radius_;
    if (r <= 0 || local_sel.width() < r * 2 || local_sel.height() < r * 2) return;
    QPainter painter(mask_);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto color = mask_->mask_color();
    const int sx = local_sel.x(), sy = local_sel.y(), sw = local_sel.width(), sh = local_sel.height();
    struct Corner {
        int x, y, start_angle;
    };
    const Corner corners[] = {{sx, sy, 90}, {sx + sw - r, sy, 0}, {sx, sy + sh - r, 180}, {sx + sw - r, sy + sh - r, 270}};
    for (const auto &corner : corners) {
        const QRectF corner_rect(corner.x, corner.y, r, r);
        QRectF arc_rect;
        if (corner.start_angle == 90) arc_rect = QRectF(corner.x, corner.y, r * 2, r * 2);
        else if (corner.start_angle == 0) arc_rect = QRectF(corner.x - r, corner.y, r * 2, r * 2);
        else if (corner.start_angle == 180) arc_rect = QRectF(corner.x, corner.y - r, r * 2, r * 2);
        else arc_rect = QRectF(corner.x - r, corner.y - r, r * 2, r * 2);
        QPainterPath path;
        path.addRect(corner_rect);
        QPainterPath arc_path;
        arc_path.moveTo(arc_rect.center());
        arc_path.arcTo(arc_rect, corner.start_angle, 90);
        arc_path.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(path - arc_path);
    }
    painter.end();
}

void RoundedCornersLogic::install_mask_hook() {
    mask_hook_ = mask_->paint_hooks.add([this](QPaintEvent *) { paint_mask_corners(); });
}

void RoundedCornersLogic::install_export_hook() {
    export_hook_ = export_->export_hooks.add([this](const QImage &img, const QRectF &) {
        if (!enabled_ || radius_ <= 0) return img;
        if (img.isNull()) return img;
        const int w = img.width(), h = img.height();
        const int r = std::min({radius_, w / 2, h / 2});
        if (r <= 0) return img;
        QImage result(w, h, QImage::Format_ARGB32_Premultiplied);
        result.fill(0);
        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, w, h), r, r);
        painter.setClipPath(clip);
        painter.drawImage(0, 0, img);
        painter.end();
        log_debug(T("圆角裁剪完成: r={r}", {{"r", r}}), "RoundedCorners");
        return result;
    });
}

void RoundedCornersLogic::uninstall() {
    if (render_installed_) {
        item_->render_override = nullptr;
        render_installed_ = false;
    }
    if (mask_) mask_->paint_hooks.remove(mask_hook_);
    export_->export_hooks.remove(export_hook_);
    if (popup_) {
        popup_->hide();
        popup_->deleteLater();
    }
}
}
