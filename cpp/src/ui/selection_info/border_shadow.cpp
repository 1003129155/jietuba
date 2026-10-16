#include "ui/selection_info/border_shadow.h"
#include "canvas/selection_model.h"
#include "core/export.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/color_picker_button.h"
#include "ui/mask_overlay.h"
#include "ui/selection_info/rounded_corners.h"
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>

namespace jietuba {
namespace {
QString popup_tr(const char *text) { return i18n_tr(text, "BorderShadowPopup"); }
const QColor POPUP_BG(30, 30, 30, 230);
}

void BorderShadowGroup::mousePressEvent(QMouseEvent *event) {
    if (on_press) on_press(event);
}

BorderShadowPopup::BorderShadowPopup(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setMouseTracking(true);
    init_ui();
    hide_timer_ = new QTimer(this);
    hide_timer_->setSingleShot(true);
    hide_timer_->setInterval(200);
    connect(hide_timer_, &QTimer::timeout, this, &BorderShadowPopup::do_hide);
    hide();
    // Connection ends with this widget.
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, &BorderShadowPopup::apply_scale);
}

void BorderShadowPopup::init_ui() {
    root_layout_ = new QVBoxLayout(this);
    // First row: [colour Shadow] [colour Border], equal widths.
    mode_row_ = new QHBoxLayout;
    mode_row_->setSpacing(0);
    shadow_group_ = new BorderShadowGroup;
    shadow_group_->setCursor(Qt::PointingHandCursor);
    shadow_row_ = new QHBoxLayout(shadow_group_);
    shadow_color_block_ = new ColorPickerButton(QColor(shadow_color_), true, scaled(BASE_COLOR_BLOCK), false);
    shadow_color_block_->setCursor(Qt::PointingHandCursor);
    shadow_color_block_->setFocusPolicy(Qt::NoFocus);
    shadow_color_block_->setToolTip(popup_tr("Select shadow color"));
    shadow_label_ = new QLabel(popup_tr("Shadow"));
    shadow_row_->addWidget(shadow_color_block_);
    shadow_row_->addWidget(shadow_label_);
    shadow_row_->addStretch();
    mode_row_->addWidget(shadow_group_, 1);
    mode_row_->addSpacing(scaled(6));
    border_group_ = new BorderShadowGroup;
    border_group_->setCursor(Qt::PointingHandCursor);
    border_row_ = new QHBoxLayout(border_group_);
    border_color_block_ = new ColorPickerButton(QColor(border_color_), true, scaled(BASE_COLOR_BLOCK), false);
    border_color_block_->setCursor(Qt::PointingHandCursor);
    border_color_block_->setFocusPolicy(Qt::NoFocus);
    border_color_block_->setToolTip(popup_tr("Select border color"));
    border_label_ = new QLabel(popup_tr("Border"));
    border_row_->addWidget(border_color_block_);
    border_row_->addWidget(border_label_);
    border_row_->addStretch();
    mode_row_->addWidget(border_group_, 1);
    root_layout_->addLayout(mode_row_);
    // Second row: size slider and value.
    slider_row_ = new QHBoxLayout;
    size_label_ = new QLabel(popup_tr("Size"));
    slider_row_->addWidget(size_label_);
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(1, 50);
    slider_->setValue(21);
    slider_->setFocusPolicy(Qt::NoFocus);
    slider_row_->addWidget(slider_, 1);
    label_val_ = new QLabel("21");
    label_val_->setAlignment(Qt::AlignCenter);
    slider_row_->addWidget(label_val_);
    root_layout_->addLayout(slider_row_);
    // Third row: keep enabled across captures.
    chk_persist_ = new QCheckBox(popup_tr("Keep enabled for every screenshot"));
    chk_persist_->setFocusPolicy(Qt::NoFocus);
    root_layout_->addWidget(chk_persist_);
    apply_scale();
    shadow_group_->on_press = [this](QMouseEvent *e) {
        if (e->button() == Qt::RightButton) shadow_color_block_->click();
        else select_mode("shadow");
    };
    border_group_->on_press = [this](QMouseEvent *e) {
        if (e->button() == Qt::RightButton) border_color_block_->click();
        else select_mode("border");
    };
    connect(shadow_color_block_, &ColorPickerButton::color_changed, this, [this](const QColor &color) {
        shadow_color_ = color.name();
        emit shadow_color_changed(shadow_color_);
    });
    connect(border_color_block_, &ColorPickerButton::color_changed, this, [this](const QColor &color) {
        border_color_ = color.name();
        emit border_color_changed(border_color_);
    });
    connect(slider_, &QSlider::valueChanged, this, [this](int value) {
        label_val_->setText(QString::number(value));
        emit size_changed(value);
    });
    connect(chk_persist_, &QCheckBox::toggled, this, &BorderShadowPopup::persist_changed);
    refresh_mode_buttons();
}

void BorderShadowPopup::apply_scale() {
    const auto theme_hex = get_theme().theme_color_hex();
    setFixedWidth(scaled(BASE_WIDTH));
    root_layout_->setContentsMargins(scaled(10), scaled(8), scaled(10), scaled(8));
    root_layout_->setSpacing(scaled(6));
    for (auto *row : {shadow_row_, border_row_}) {
        row->setContentsMargins(scaled(6), scaled(4), scaled(6), scaled(4));
        row->setSpacing(scaled(5));
    }
    slider_row_->setSpacing(scaled(6));
    const int block = scaled(BASE_COLOR_BLOCK);
    for (auto *color_block : {shadow_color_block_, border_color_block_}) color_block->setFixedSize(block, block);
    set_own_style(size_label_, QString("color: #AAAAAA; font-size: %1px; background: transparent;").arg(scaled(12)));
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
        )").arg(scaled(4)).arg(scaled(2)).arg(scaled(12)).arg(-scaled(4)).arg(theme_hex).arg(scaled(6)));
    label_val_->setFixedWidth(scaled(BASE_VALUE_LABEL_WIDTH));
    set_own_style(label_val_, QString("color: #D0D0D0; font-size: %1px; background: transparent;").arg(scaled(12)));
    const int indicator = scaled(14);
    chk_persist_->setStyleSheet(QString(R"(
            QCheckBox { color: #AAAAAA; font-size: %1px;
                         background: transparent; spacing: %2px; }
            QCheckBox::indicator { width: %3px; height: %3px;
                                    border: 1px solid #666; border-radius: %4px;
                                    background: transparent; }
            QCheckBox::indicator:checked { background: %5;
                                            border-color: %5; }
        )").arg(scaled(11)).arg(scaled(4)).arg(indicator).arg(scaled(3)).arg(theme_hex));
    refresh_mode_buttons();
    adjustSize();
    update();
    if (isVisible() && anchor_) show_near(anchor_);
}

void BorderShadowPopup::refresh_mode_buttons() {
    const int radius = scaled(4);
    const int font_px = scaled(12);
    const auto active = QString(R"(
            background: rgba(64,224,208,50);
            border: 1px solid rgba(64,224,208,120);
            border-radius: %1px;
        )").arg(radius);
    const auto inactive = QString(R"(
            background: transparent;
            border: none;
            border-radius: %1px;
        )").arg(radius);
    const std::tuple<QWidget *, QLabel *, QString> groups[] = {{shadow_group_, shadow_label_, "shadow"}, {border_group_, border_label_, "border"}};
    for (const auto &[group, label, mode] : groups) {
        if (mode == current_mode_) {
            group->setStyleSheet(active);
            set_own_style(label, QString("color: #FFFFFF; font-size: %1px; background: transparent;").arg(font_px));
        } else {
            group->setStyleSheet(inactive);
            set_own_style(label, QString("color: #999999; font-size: %1px; background: transparent;").arg(font_px));
        }
    }
}

void BorderShadowPopup::set_mode(const QString &mode) {
    current_mode_ = mode;
    refresh_mode_buttons();
}

void BorderShadowPopup::set_shadow_color(const QString &color) {
    shadow_color_ = color;
    shadow_color_block_->set_color(QColor(color));
}

void BorderShadowPopup::set_border_color(const QString &color) {
    border_color_ = color;
    border_color_block_->set_color(QColor(color));
}

void BorderShadowPopup::select_mode(const QString &mode) {
    if (current_mode_ == mode) return;
    current_mode_ = mode;
    refresh_mode_buttons();
    emit mode_changed(mode);
}

void BorderShadowPopup::do_hide() {
    if (underMouse()) return;
    hide();
}

void BorderShadowPopup::show_near(QWidget *anchor) {
    anchor_ = anchor;
    auto *parent = parentWidget();
    const int pw = parent->width();
    const auto local_ref = parent->mapFromGlobal(anchor->mapToGlobal(QPoint(0, 0)));
    int x = local_ref.x() - python_floordiv(width() - anchor->width(), 2);
    if (x + width() > pw) x = pw - width();
    x = std::max(0, x);
    const int gap = scaled(4);
    const auto above_local = parent->mapFromGlobal(anchor->mapToGlobal(QPoint(0, -height() - gap)));
    int y;
    if (above_local.y() >= 0) y = above_local.y();
    else y = parent->mapFromGlobal(anchor->mapToGlobal(QPoint(0, anchor->height() + gap))).y();
    move(x, y);
    show();
    raise();
}

void BorderShadowPopup::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    emit visibility_changed(true);
}

void BorderShadowPopup::hideEvent(QHideEvent *event) {
    QWidget::hideEvent(event);
    emit visibility_changed(false);
}

void BorderShadowPopup::enterEvent(QEnterEvent *event) {
    cancel_hide();
    QWidget::enterEvent(event);
}

void BorderShadowPopup::leaveEvent(QEvent *event) {
    schedule_hide();
    QWidget::leaveEvent(event);
}

void BorderShadowPopup::paintEvent(QPaintEvent *event) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(POPUP_BG));
    const int radius = scaled(BASE_RADIUS);
    p.drawRoundedRect(rect(), radius, radius);
    p.end();
    QWidget::paintEvent(event);
}

BorderShadowLogic::BorderShadowLogic(QWidget *parent_widget, QPushButton *btn_border, SelectionItem *, MaskOverlayWidget *mask_overlay,
                                     ExportService *export_service, SelectionModel *selection_model, ToolSettingsManager *config_manager,
                                     RoundedCornersLogic *rounded_corners_logic)
    : QObject(parent_widget), btn_(btn_border), mask_(mask_overlay), export_(export_service), model_(selection_model), config_(config_manager),
      rounded_logic_(rounded_corners_logic) {
    if (config_) {
        enabled_ = config_->get_app_setting("screenshot_border_enabled", false).toBool();
        mode_ = config_->get_app_setting("screenshot_border_mode", "border").toString();
        size_ = config_->get_app_setting("screenshot_border_size", 21).toInt();
        shadow_color_ = config_->get_app_setting("screenshot_shadow_color", "#0078FF").toString();
        border_color_ = config_->get_app_setting("screenshot_border_color", "#FF0000").toString();
        persist_ = config_->get_app_setting("screenshot_border_persist", false).toBool();
    }
    // Without "keep enabled" every capture starts with the effect off.
    if (!persist_) enabled_ = false;
    popup_ = new BorderShadowPopup(parent_widget);
    popup_->set_mode(mode_);
    popup_->set_size_value(size_);
    popup_->set_shadow_color(shadow_color_);
    popup_->set_border_color(border_color_);
    popup_->set_persist(persist_);
    btn_->blockSignals(true);
    btn_->setChecked(enabled_);
    btn_->blockSignals(false);
    connect(popup_, &BorderShadowPopup::mode_changed, this, &BorderShadowLogic::on_mode_changed);
    connect(popup_, &BorderShadowPopup::size_changed, this, &BorderShadowLogic::on_size_changed);
    connect(popup_, &BorderShadowPopup::shadow_color_changed, this, &BorderShadowLogic::on_shadow_color_changed);
    connect(popup_, &BorderShadowPopup::border_color_changed, this, &BorderShadowLogic::on_border_color_changed);
    connect(popup_, &BorderShadowPopup::persist_changed, this, &BorderShadowLogic::on_persist_changed);
    connect(popup_, &BorderShadowPopup::visibility_changed, this, &BorderShadowLogic::on_popup_visibility_changed);
    // The button outlives sessions on the reused capture window; once the popup
    // is gone its hover events must not reach this filter.
    connect(popup_, &QObject::destroyed, this, [this] {
        if (btn_) btn_->removeEventFilter(this);
    });
    btn_->setMouseTracking(true);
    btn_->installEventFilter(this);
    mask_hook_ = mask_->paint_hooks.add([this](QPaintEvent *) { paint_preview(); });
    export_hook_ = export_->export_hooks.add([this](const QImage &img, const QRectF &) {
        if (!enabled_) return img;
        if (img.isNull()) return img;
        const QColor color(active_color());
        if (mode_ == "border") return apply_border_export(img, size_, color, corner_radius());
        return apply_shadow_export(img, size_, color, corner_radius());
    });
}

int BorderShadowLogic::corner_radius() const {
    if (rounded_logic_ && rounded_logic_->enabled()) return rounded_logic_->radius();
    return 0;
}

QString BorderShadowLogic::active_color() const { return mode_ == "border" ? border_color_ : shadow_color_; }

void BorderShadowLogic::set_enabled(bool enabled) {
    enabled_ = enabled;
    log_debug(T("描边/阴影: {state}  mode={mode} size={size}", {{"state", QString(enabled ? "ON" : "OFF")}, {"mode", mode_}, {"size", size_}}), "BorderShadow");
    if (config_) config_->set_app_setting("screenshot_border_enabled", enabled);
    if (enabled) {
        popup_->cancel_hide();
        popup_->show_near(btn_);
    } else {
        popup_->hide();
        preview_visible_ = false;
        mask_->update();
    }
}

bool BorderShadowLogic::eventFilter(QObject *watched, QEvent *event) {
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

void BorderShadowLogic::on_mode_changed(const QString &mode) {
    mode_ = mode;
    log_debug(T("模式切换: {mode}", {{"mode", mode}}), "BorderShadow");
    if (config_) config_->set_app_setting("screenshot_border_mode", mode);
    if (preview_visible_) mask_->update();
}

void BorderShadowLogic::on_size_changed(int value) {
    size_ = value;
    if (config_) config_->set_app_setting("screenshot_border_size", value);
    if (preview_visible_) mask_->update();
}

void BorderShadowLogic::on_shadow_color_changed(const QString &color_hex) {
    shadow_color_ = color_hex;
    if (config_) config_->set_app_setting("screenshot_shadow_color", color_hex);
    if (preview_visible_ && mode_ == "shadow") mask_->update();
}

void BorderShadowLogic::on_border_color_changed(const QString &color_hex) {
    border_color_ = color_hex;
    if (config_) config_->set_app_setting("screenshot_border_color", color_hex);
    if (preview_visible_ && mode_ == "border") mask_->update();
}

void BorderShadowLogic::on_persist_changed(bool checked) {
    persist_ = checked;
    if (config_) config_->set_app_setting("screenshot_border_persist", checked);
}

void BorderShadowLogic::on_popup_visibility_changed(bool visible) {
    preview_visible_ = visible && enabled_;
    if (mask_) mask_->update();
}

void BorderShadowLogic::paint_preview() {
    // Runs after the mask and the rounded-corner fill.
    if (!preview_visible_) return;
    if (model_->is_empty()) return;
    const auto local_sel = mask_->scene_to_local(model_->rect());
    const QColor color(active_color());
    if (local_sel.width() < 2 || local_sel.height() < 2) return;
    QPainter painter(mask_);
    painter.setRenderHint(QPainter::Antialiasing);
    if (mode_ == "border") draw_border_preview(painter, local_sel, size_, color, corner_radius());
    else draw_shadow_preview(painter, local_sel, size_, color, corner_radius());
    painter.end();
}

void BorderShadowLogic::uninstall() {
    if (mask_) mask_->paint_hooks.remove(mask_hook_);
    export_->export_hooks.remove(export_hook_);
    if (popup_) {
        popup_->hide();
        popup_->deleteLater();
    }
}

void draw_border_preview(QPainter &painter, const QRect &local_sel, int size, const QColor &color, int corner_radius) {
    QPen pen(color, size);
    pen.setJoinStyle(Qt::MiterJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    // Half a stroke outward, so the stroke lies outside the selection.
    const double half = size / 2.0;
    const QRectF r(local_sel.x() - half, local_sel.y() - half, local_sel.width() + size, local_sel.height() + size);
    if (corner_radius > 0) {
        // The outer stroke's radius is larger by half a stroke.
        const double cr = corner_radius + half;
        painter.drawRoundedRect(r, cr, cr);
    } else {
        painter.drawRect(r);
    }
}

void draw_shadow_preview(QPainter &painter, const QRect &local_sel, int size, const QColor &color, int corner_radius) {
    const int layers = std::min(size, 20);
    if (layers <= 0) return;
    const int base_alpha = std::min(color.alpha(), 120);
    for (int i = 0; i < layers; ++i) {
        const double t = static_cast<double>(i + 1) / layers;  // fainter outward
        const int alpha = static_cast<int>(base_alpha * (1.0 - t) * 0.6);
        if (alpha <= 0) continue;
        const QColor c(color.red(), color.green(), color.blue(), alpha);
        const int spread = static_cast<int>(size * t);
        const QRectF r(local_sel.x() - spread, local_sel.y() - spread, local_sel.width() + spread * 2, local_sel.height() + spread * 2);
        painter.setPen(QPen(c, 1.5));
        painter.setBrush(Qt::NoBrush);
        if (corner_radius > 0) {
            const int cr = corner_radius + spread;
            painter.drawRoundedRect(r, cr, cr);
        } else {
            painter.drawRect(r);
        }
    }
}

QImage apply_border_export(const QImage &img, int size, const QColor &color, int corner_radius) {
    const int w = img.width(), h = img.height();
    QImage result(w + size * 2, h + size * 2, QImage::Format_ARGB32_Premultiplied);
    result.fill(0);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, size);
    pen.setJoinStyle(Qt::MiterJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const double half = size / 2.0;
    const QRectF stroke_rect(half, half, w + size, h + size);
    if (corner_radius > 0) {
        const double cr = corner_radius + half;
        painter.drawRoundedRect(stroke_rect, cr, cr);
    } else {
        painter.drawRect(stroke_rect);
    }
    painter.drawImage(size, size, img);
    painter.end();
    log_debug(T("描边导出完成: size={size} corner_r={corner_radius}", {{"size", size}, {"corner_radius", corner_radius}}), "BorderShadow");
    return result;
}

QImage apply_shadow_export(const QImage &img, int size, const QColor &color, int corner_radius) {
    const int w = img.width(), h = img.height();
    const int margin = size;
    QImage result(w + margin * 2, h + margin * 2, QImage::Format_ARGB32_Premultiplied);
    result.fill(0);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    const int layers = std::min(size, 25);
    const int base_alpha = std::min(color.alpha(), 180);
    for (int i = 0; i < layers; ++i) {
        const double t = static_cast<double>(i + 1) / layers;
        const int alpha = static_cast<int>(base_alpha * (1.0 - t) * 0.5);
        if (alpha <= 0) continue;
        const QColor c(color.red(), color.green(), color.blue(), alpha);
        const int spread = static_cast<int>(margin * t);
        const QRectF r(margin - spread, margin - spread, w + spread * 2, h + spread * 2);
        painter.setPen(QPen(c, 1.5));
        painter.setBrush(Qt::NoBrush);
        if (corner_radius > 0) {
            const int cr = corner_radius + spread;
            painter.drawRoundedRect(r, cr, cr);
        } else {
            painter.drawRect(r);
        }
    }
    painter.drawImage(margin, margin, img);
    painter.end();
    log_debug(T("阴影导出完成: size={size} corner_r={corner_radius}", {{"size", size}, {"corner_radius", corner_radius}}), "BorderShadow");
    return result;
}
}
