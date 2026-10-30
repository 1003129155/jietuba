#include "ui/base_settings_panel.h"
#include "core/constants.h"
#include "core/i18n.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/color_picker_button.h"
#include <QApplication>
#include <QEnterEvent>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QScreen>
#include <QWheelEvent>
#include <random>

namespace jietuba {
namespace {
// The panel background is a fixed texture rather than a blur of what lies
// behind: pin toolbars float over the live desktop with nothing to blur.
const QColor PANEL_TOP("#fcfdff");
const QColor PANEL_BOTTOM("#eef2f8");
// (x ratio, y ratio, base radius, RGBA): muted glows imitating blurred colour behind.
struct Glow { double fx, fy, base_radius; int r, g, b, a; };
constexpr Glow PANEL_GLOWS[] = {
    {0.10, 0.0, 230, 120, 170, 255, 46},
    {0.58, 1.2, 260, 190, 160, 255, 34},
    {0.96, 0.0, 200, 255, 190, 160, 36},
};
constexpr double PANEL_NOISE_OPACITY = 0.015;
const QColor PANEL_HIGHLIGHT(255, 255, 255, 235);
const QColor PANEL_INNER_RING(255, 255, 255, 190);
// A dark translucent outer ring: visible on white screenshots, almost invisible on dark ones.
const QColor PANEL_OUTER_RING(15, 23, 42, 30);

// 64x64 black noise with random alpha; a fixed seed keeps the texture stable across runs.
const QImage &panel_noise_tile() {
    static const QImage tile = [] {
        QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
        std::mt19937 generator(7);
        std::uniform_int_distribution<int> distribution(0, 255);
        for (int y = 0; y < 64; ++y) {
            auto *row = reinterpret_cast<quint32 *>(image.scanLine(y));
            for (int x = 0; x < 64; ++x) row[x] = static_cast<quint32>(distribution(generator)) << 24;
        }
        return image;
    }();
    return tile;
}

void draw_panel_background(QPainter *painter, const QRectF &rect, double dpr) {
    const double radius = scaled_f(PANEL_RADIUS);
    const double line = scaled_f(1.0);
    const double half = line / 2;
    QPainterPath path;
    path.addRoundedRect(rect.adjusted(half, half, -half, -half), radius, radius);
    QLinearGradient gradient(rect.topLeft(), rect.bottomLeft());
    gradient.setColorAt(0, PANEL_TOP);
    gradient.setColorAt(1, PANEL_BOTTOM);
    painter->fillPath(path, QBrush(gradient));
    painter->save();
    painter->setClipPath(path);
    for (const auto &glow : PANEL_GLOWS) {
        QRadialGradient radial(rect.left() + glow.fx * rect.width(), rect.top() + glow.fy * rect.height(), scaled_f(glow.base_radius));
        radial.setColorAt(0, QColor(glow.r, glow.g, glow.b, glow.a));
        radial.setColorAt(1, QColor(glow.r, glow.g, glow.b, 0));
        painter->fillRect(rect, QBrush(radial));
    }
    QBrush noise(panel_noise_tile());
    // One grain per physical pixel.
    noise.setTransform(QTransform::fromScale(1 / dpr, 1 / dpr));
    painter->setOpacity(PANEL_NOISE_OPACITY);
    painter->fillRect(rect, noise);
    painter->setOpacity(1.0);
    painter->setPen(QPen(PANEL_HIGHLIGHT, line));
    const double y = rect.top() + line * 1.5;
    painter->drawLine(QPointF(rect.left() + radius * 0.6, y), QPointF(rect.right() - radius * 0.6, y));
    painter->restore();
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(PANEL_INNER_RING, line));
    const double inset = line * 1.5;
    painter->drawRoundedRect(rect.adjusted(inset, inset, -inset, -inset), radius - line, radius - line);
    painter->setPen(QPen(PANEL_OUTER_RING, line));
    painter->drawPath(path);
}

QString rgba(const QColor &color) { return QString("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha()); }

// Chevrons sit on the side next to the other button, so both meet at the pill's middle.
QPixmap chevron_pixmap(int width, int height, bool up) {
    constexpr double ratio = 3.0;
    QPixmap pixmap(static_cast<int>(std::nearbyint(width * ratio)), static_cast<int>(std::nearbyint(height * ratio)));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(CONTROL_SUB, scaled_f(1.4), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const double near_ = scaled_f(2.2), far_ = scaled_f(5.0);
    const double tip = up ? height - far_ : far_;
    const double base = up ? height - near_ : near_;
    const QPointF points[] = {{width * 0.12, base}, {width / 2.0, tip}, {width * 0.88, base}};
    painter.drawPolyline(points, 3);
    painter.end();
    return pixmap;
}

void paint_width_glyph(QPainter *painter, double x, double cy) {
    // Three bars from thin to thick.
    painter->setPen(Qt::NoPen);
    painter->setBrush(CONTROL_SUB);
    const double width = scaled_f(12);
    const double heights[] = {1.2, 2.2, 3.4};
    for (int index = 0; index < 3; ++index) {
        const double top = cy - scaled_f(5.5) + index * scaled_f(4.6);
        painter->drawRoundedRect(QRectF(x, top, width, scaled_f(heights[index])), scaled_f(1), scaled_f(1));
    }
}

void paint_opacity_glyph(QPainter *painter, double x, double cy) {
    // A circle filled on its left half.
    const double radius = scaled_f(5.4);
    const QPointF center(x + scaled_f(6), cy);
    painter->setPen(QPen(CONTROL_SUB, scaled_f(1.4)));
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(center, radius, radius);
    QPainterPath half;
    half.moveTo(center.x(), center.y() - radius);
    half.arcTo(QRectF(center.x() - radius, center.y() - radius, radius * 2, radius * 2), 90, 180);
    half.closeSubpath();
    painter->setBrush(CONTROL_SUB);
    painter->drawPath(half);
}
}

void paint_rounded_panel(QWidget *widget) {
    const int width = widget->width(), height = widget->height();
    if (width <= 0 || height <= 0) return;
    const double dpr = widget->devicePixelRatioF();
    const auto key = QString("%1|%2|%3|%4").arg(width).arg(height).arg(dpr).arg(scale_factor());
    auto cached_key = widget->property("_panel_background_key").toString();
    auto pixmap = widget->property("_panel_background").value<QPixmap>();
    if (cached_key != key || pixmap.isNull()) {
        pixmap = QPixmap(static_cast<int>(std::nearbyint(width * dpr)), static_cast<int>(std::nearbyint(height * dpr)));
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        draw_panel_background(&painter, QRectF(0, 0, width, height), dpr);
        painter.end();
        widget->setProperty("_panel_background_key", key);
        widget->setProperty("_panel_background", pixmap);
    }
    QPainter painter(widget);
    painter.drawPixmap(0, 0, pixmap);
}

QColor accent_color() { return get_theme().theme_color(); }

QString line_style_of_pen(const QPen &pen) {
    QList<double> dash_pattern;
    for (const auto value : pen.dashPattern()) dash_pattern.append(std::nearbyint(value * 10) / 10);
    if (!dash_pattern.isEmpty()) return dash_pattern.size() >= 2 && dash_pattern[0] == 1.0 && dash_pattern[1] == 2.0 ? "dashed_dense" : "dashed";
    if (pen.style() == Qt::DashLine || pen.style() == Qt::CustomDashLine) return "dashed";
    return "solid";
}

void set_step_button_icon(QToolButton *button, const QString &direction) {
    // Drawn here so the system palette cannot recolour it; the icon fills the
    // button height so stacked chevrons meet at the boundary.
    const int width = scaled(8), height = scaled(13);
    button->setArrowType(Qt::NoArrow);
    button->setIcon(QIcon(chevron_pixmap(width, height, direction == "up")));
    button->setIconSize(QSize(width, height));
}

void style_step_button(QToolButton *button, const QString &direction) {
    set_step_button_icon(button, direction);
    apply_style_sheet(button, QString(".QToolButton { background: transparent; border: none; border-radius: %1px; }"
                                      " .QToolButton:hover { background: rgba(15, 23, 42, 0.08); }").arg(scaled(4)));
}

void paint_line_style_preview(QPainter *painter, const QRectF &rect, const QVariant &style) {
    QPen pen(CONTROL_INK, scaled_f(2.0), Qt::SolidLine, Qt::RoundCap);
    const auto value = style.toString();
    if (value == "dashed") {
        pen.setStyle(Qt::CustomDashLine);
        pen.setCapStyle(Qt::FlatCap);
        pen.setDashPattern({3.0, 2.0});
    } else if (value == "dashed_dense") {
        pen.setStyle(Qt::CustomDashLine);
        pen.setCapStyle(Qt::FlatCap);
        pen.setDashPattern({1.0, 1.5});
    }
    painter->setPen(pen);
    const double y = rect.center().y();
    painter->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
}

QString combo_popup_qss() {
    const auto accent = accent_color();
    const auto accent_soft = QString("rgba(%1, %2, %3, 0.3)").arg(accent.red()).arg(accent.green()).arg(accent.blue());
    return QString(R"(
        QComboBox QAbstractItemView {
            border: 1px solid %1;
            background: white;
            selection-background-color: %2;
            selection-color: %3;
            font-family: %4;
            font-size: %5px;
            color: %3;
            outline: none;
            padding: %6px;
        }
        QComboBox QAbstractItemView::item {
            padding: %7px %8px;
            min-height: %9px;
            color: %3;
            background: white;
        }
        QComboBox QAbstractItemView::item:hover {
            background-color: %10;
        }
        QComboBox QAbstractItemView::item:selected {
            background-color: %2;
            color: %3;
        }
    )").arg(rgba(DIVIDER_COLOR), accent_soft, CONTROL_INK.name(), CSS_FONT_FAMILY).arg(scaled(12)).arg(scaled(2)).arg(scaled(3)).arg(scaled(6)).arg(scaled(18))
        .arg(rgba(CONTROL_FILL));
}

QString build_settings_panel_stylesheet(bool combo_enabled, const QString &, int combo_min_width, int combo_max_width, bool) {
    const int font_px = scaled(12);
    const auto accent = accent_color();
    const auto accent_soft = QString("rgba(%1, %2, %3, 0.3)").arg(accent.red()).arg(accent.green()).arg(accent.blue());
    const auto ink = CONTROL_INK.name();
    QString combo_block;
    if (combo_enabled) {
        combo_block = QString(R"(
            QComboBox {
                border: none;
                background: transparent;
                min-height: %1px;
                max-height: %1px;
                min-width: %2px;
                max-width: %3px;
                font-family: %4;
                font-size: %5px;
                color: %6;
            }
            QComboBox::drop-down {
                width: 0px;
                border: none;
            }
            QComboBox::down-arrow {
                image: none;
                width: 0px;
                height: 0px;
            }
        )").arg(scaled(PillComboBox::BASE_HEIGHT)).arg(scaled(combo_min_width)).arg(scaled(combo_max_width)).arg(CSS_FONT_FAMILY).arg(font_px).arg(ink) +
                      combo_popup_qss();
    }
    return QString(R"(
        QWidget {
            background-color: transparent;
            border: none;
        }
        QPushButton {
            background-color: transparent;
            border: none;
            border-radius: %1px;
            padding: %2px;
            font-family: %3;
            font-size: %4px;
            color: %5;
        }
        QPushButton:hover {
            background-color: rgba(15, 23, 42, 0.06);
        }
        QPushButton:pressed {
            background-color: rgba(15, 23, 42, 0.12);
        }
        QPushButton:checked {
            background-color: %6;
        }
        QLabel {
            color: %5;
            background-color: transparent;
            border: none;
            font-family: %3;
            font-size: %4px;
        }
        QFrame#separator {
            background-color: %7;
            margin: %8px 0px;
            border: none;
        }
        %9
    )").arg(scaled(8)).arg(scaled(2)).arg(CSS_FONT_FAMILY).arg(font_px).arg(ink, accent_soft, rgba(DIVIDER_COLOR)).arg(scaled(3)).arg(combo_block);
}

StepperWidget::StepperWidget(int value, int minimum, int maximum, const QString &suffix, QWidget *parent, const QString &glyph)
    : QWidget(parent), min_(minimum), max_(maximum), value_(value), suffix_(suffix), glyph_(glyph == "width" || glyph == "opacity" ? glyph : QString()) {
    setAttribute(Qt::WA_Hover, true);
    label_ = new QLabel(this);
    label_->setAlignment(Qt::AlignCenter);
    up_btn_ = new QToolButton(this);
    down_btn_ = new QToolButton(this);
    for (auto *button : {up_btn_, down_btn_}) {
        button->setAutoRepeat(true);
        button->setAutoRepeatDelay(300);
        button->setAutoRepeatInterval(60);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
    }
    connect(up_btn_, &QToolButton::clicked, this, [this] { step(1); });
    connect(down_btn_, &QToolButton::clicked, this, [this] { step(-1); });
    apply_scale_sizes();
    refresh_label();
}

int StepperWidget::glyph_width() const { return glyph_.isEmpty() ? 0 : scaled(BASE_GLYPH_COLUMN); }

QRect StepperWidget::pill_rect() const {
    const int h = scaled(BASE_HEIGHT);
    const int left = glyph_width();
    return QRect(left, (height() - h) / 2, width() - left, h);
}

void StepperWidget::apply_scale_sizes() {
    QWidget::setFixedHeight(scaled(BASE_HEIGHT));
    set_own_style(label_, QString("background: transparent; color: %1; border: none; font-family: %2; font-size: %3px; font-weight: bold;")
                              .arg(CONTROL_INK.name(), CSS_FONT_FAMILY).arg(scaled(12)));
    style_step_button(up_btn_, "up");
    style_step_button(down_btn_, "down");
    layout_children();
}

void StepperWidget::layout_children() {
    const auto pill = pill_rect();
    const int arrow = scaled(BASE_ARROW_COLUMN);
    const int label_width = std::max(scaled(BASE_LABEL_MIN_WIDTH), pill.width() - arrow);
    label_->setGeometry(pill.left(), pill.top(), label_width, pill.height());
    const int half = pill.height() / 2;
    const int button_left = pill.right() + 1 - arrow;
    const int button_width = arrow - scaled(3);
    up_btn_->setGeometry(button_left, pill.top(), button_width, half);
    down_btn_->setGeometry(button_left, pill.top() + half, button_width, pill.height() - half);
}

void StepperWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    layout_children();
}

void StepperWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF pill(pill_rect());
    painter.setPen(Qt::NoPen);
    painter.setBrush(underMouse() ? CONTROL_FILL_HOVER : CONTROL_FILL);
    const double radius = pill.height() / 2;
    painter.drawRoundedRect(pill, radius, radius);
    if (glyph_ == "width") paint_width_glyph(&painter, 0.0, pill.center().y());
    else if (glyph_ == "opacity") paint_opacity_glyph(&painter, 0.0, pill.center().y());
}

void StepperWidget::enterEvent(QEnterEvent *event) {
    update();
    QWidget::enterEvent(event);
}

void StepperWidget::leaveEvent(QEvent *event) {
    update();
    QWidget::leaveEvent(event);
}

void StepperWidget::refresh_label() { label_->setText(QString("%1%2").arg(value_).arg(suffix_)); }

void StepperWidget::setRange(int minimum, int maximum) {
    min_ = minimum;
    max_ = maximum;
    setValue(value_);
}

void StepperWidget::setValue(int value) {
    value = clamp(value);
    if (value == value_) {
        refresh_label();
        return;
    }
    value_ = value;
    refresh_label();
    if (!signalsBlocked()) emit valueChanged(value_);
}

void StepperWidget::setSuffix(const QString &suffix) {
    suffix_ = suffix;
    refresh_label();
}

void StepperWidget::setToolTip(const QString &text) {
    QWidget::setToolTip(text);
    label_->setToolTip(text);
    up_btn_->setToolTip(text);
    down_btn_->setToolTip(text);
}

void StepperWidget::setFixedWidth(int width) {
    QWidget::setFixedWidth(width);
    apply_scale_sizes();
}

void StepperWidget::wheelEvent(QWheelEvent *event) {
    const int delta = event->angleDelta().y();
    if (delta > 0) step(1);
    else if (delta < 0) step(-1);
    event->accept();
}

PillComboBox::PillComboBox(QWidget *parent, Qt::Alignment text_alignment) : QComboBox(parent), text_alignment_(text_alignment) {
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
}

void PillComboBox::set_preview_painter(std::function<void(QPainter *, const QRectF &, const QVariant &)> painter_fn) {
    preview_painter_ = std::move(painter_fn);
    update();
}

void PillComboBox::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF r(rect());
    const double radius = r.height() / 2;
    painter.setPen(Qt::NoPen);
    painter.setBrush(underMouse() ? CONTROL_FILL_HOVER : CONTROL_FILL);
    painter.drawRoundedRect(r, radius, radius);
    const double arrow = scaled_f(BASE_ARROW_COLUMN);
    const auto content = r.adjusted(scaled_f(BASE_PADDING_LEFT), 0, -arrow, 0);
    const auto icon = currentIndex() >= 0 ? itemIcon(currentIndex()) : QIcon();
    if (preview_painter_) {
        preview_painter_(&painter, content, currentData());
    } else if (!icon.isNull()) {
        icon.paint(&painter, content.toRect(), Qt::AlignCenter);
    } else {
        painter.setPen(CONTROL_INK);
        painter.setFont(font());
        const auto text = fontMetrics().elidedText(currentText(), Qt::ElideRight, static_cast<int>(content.width()));
        painter.drawText(content, static_cast<int>(text_alignment_ | Qt::AlignVCenter), text);
    }
    const double cx = r.right() - scaled_f(15.5), cy = r.center().y();
    const double dx = scaled_f(3.5), dy = scaled_f(1.8);
    painter.setPen(QPen(CONTROL_SUB, scaled_f(1.4), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF points[] = {{cx - dx, cy - dy}, {cx, cy + dy}, {cx + dx, cy - dy}};
    painter.drawPolyline(points, 3);
}

void PillComboBox::enterEvent(QEnterEvent *event) {
    update();
    QComboBox::enterEvent(event);
}

void PillComboBox::leaveEvent(QEvent *event) {
    update();
    QComboBox::leaveEvent(event);
}

PillFrame::PillFrame(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_Hover, true);
    apply_scale();
}

void PillFrame::apply_scale() { setFixedHeight(scaled(BASE_HEIGHT)); }

void PillFrame::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF r(rect());
    painter.setPen(Qt::NoPen);
    painter.setBrush(underMouse() ? CONTROL_FILL_HOVER : CONTROL_FILL);
    painter.drawRoundedRect(r, r.height() / 2, r.height() / 2);
}

void PillFrame::enterEvent(QEnterEvent *event) {
    update();
    QWidget::enterEvent(event);
}

void PillFrame::leaveEvent(QEvent *event) {
    update();
    QWidget::leaveEvent(event);
}

ColorSwatch::ColorSwatch(const QString &hex, QWidget *parent) : QPushButton(parent), color_hex(hex), color_(hex) {
    setFlat(true);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setToolTip(hex);
    apply_scale();
}

void ColorSwatch::apply_scale() {
    const int side = scaled(BASE_SIDE);
    setFixedSize(side, side);
    update();
}

void ColorSwatch::set_selected(bool selected) {
    if (selected != selected_) {
        selected_ = selected;
        update();
    }
}

void ColorSwatch::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto center = QRectF(rect()).center();
    double radius = scaled_f(BASE_RADIUS);
    if (selected_) {
        const double ring = radius + scaled_f(BASE_RING_GAP);
        painter.setPen(QPen(accent_color(), scaled_f(2.0)));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, ring, ring);
    } else if (underMouse()) {
        const double ring = radius + scaled_f(2.5);
        painter.setPen(QPen(QColor(15, 23, 42, 50), scaled_f(1.0)));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(center, ring, ring);
    }
    if (isDown()) radius *= 0.9;
    painter.setPen(QPen(QColor(0, 0, 0, color_.lightness() > 240 ? 56 : 36), 1));
    painter.setBrush(color_);
    painter.drawEllipse(center, radius, radius);
}

ColorRow::ColorRow(ColorPickerButton *picker_button, const QStringList &colors, std::function<void(const QString &)> on_preset, QWidget *parent)
    : QWidget(parent), picker(picker_button) {
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(picker);
    QVector<ColorSwatch *> swatch_widgets;
    for (const auto &color_hex : colors) {
        auto *swatch = new ColorSwatch(color_hex);
        connect(swatch, &QPushButton::clicked, this, [on_preset, color_hex] { on_preset(color_hex); });
        layout->addWidget(swatch);
        swatches.append({swatch, color_hex});
        swatch_widgets.append(swatch);
    }
    picker->link_swatches(swatch_widgets);
    apply_scale();
}

void ColorRow::apply_scale() {
    layout()->setSpacing(scaled(BASE_SPACING));
    for (auto &[swatch, hex] : swatches) swatch->apply_scale();
    const int side = scaled(ColorSwatch::BASE_SIDE);
    picker->setFixedSize(side, side);
}

QFrame *make_separator() {
    auto *line = new QFrame;
    line->setObjectName("separator");
    line->setFrameShape(QFrame::VLine);
    line->setFixedWidth(1);
    return line;
}

QPoint popup_position(QWidget *panel, QWidget *anchor, QWidget *popup) {
    const int gap = scaled(4);
    const QRect panel_rect(panel->mapToGlobal(QPoint(0, 0)), panel->size());
    const auto anchor_top_left = anchor->mapToGlobal(QPoint(0, 0));
    auto *screen = QApplication::screenAt(anchor_top_left);
    if (!screen) screen = QApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    // Panels without a registered toolbar (GIF recording) count as below it.
    bool below_toolbar = true;
    if (auto *toolbar = panel->property("owner_toolbar").value<QWidget *>()) below_toolbar = panel_rect.top() >= toolbar->mapToGlobal(QPoint(0, 0)).y();
    const int outward = below_toolbar ? panel_rect.bottom() + gap : panel_rect.top() - popup->height() - gap;
    const int other = below_toolbar ? panel_rect.top() - popup->height() - gap : panel_rect.bottom() + gap;
    int y = outward;
    if (!(area.top() <= y && y + popup->height() <= area.bottom())) y = other;
    int x = anchor_top_left.x() + anchor->width() / 2 - popup->width() / 2;
    x = std::max(area.left(), std::min(x, area.right() - popup->width()));
    y = std::max(area.top(), std::min(y, area.bottom() - popup->height()));
    return QPoint(x, y);
}

HoverPopup::HoverPopup(QWidget *parent) : QWidget(parent) {
    // The panel's flags: on top and never taking focus, or the panel would close.
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setObjectName("HoverPopup");
    HoverPopup::apply_scale();
    close_timer_.setSingleShot(true);
    close_timer_.setInterval(CLOSE_DELAY_MS);
    connect(&close_timer_, &QTimer::timeout, this, &QWidget::hide);
    if (parent) parent->installEventFilter(this);
}

bool HoverPopup::eventFilter(QObject *watched, QEvent *event) {
    // A separate top-level does not hide with its panel; left behind it would be a white box over the capture.
    if (watched == parentWidget() && event->type() == QEvent::Hide) hide();
    return QWidget::eventFilter(watched, event);
}

void HoverPopup::set_extra_stylesheet(const QString &qss) {
    extra_qss_ = qss;
    apply_scale();
}

void HoverPopup::apply_scale() { setStyleSheet("#HoverPopup { background: transparent; border: none; }" + extra_qss_); }

void HoverPopup::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void HoverPopup::show_beside(QWidget *panel, QWidget *anchor) {
    keep_open();
    adjustSize();
    move(popup_position(panel, anchor, this));
    show();
    raise();
}

void HoverPopup::enterEvent(QEnterEvent *event) {
    keep_open();
    QWidget::enterEvent(event);
}

void HoverPopup::leaveEvent(QEvent *event) {
    close_soon();
    QWidget::leaveEvent(event);
}

BaseSettingsPanel::BaseSettingsPanel(QWidget *parent) : QWidget(parent), current_size(5) {
    setAttribute(Qt::WA_TranslucentBackground);
}

void BaseSettingsPanel::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void BaseSettingsPanel::init_ui() {
    current_size = size_default();
    auto *layout = new QHBoxLayout(this);
    build_size_and_opacity(layout);
    layout->addWidget(make_separator());
    layout->addWidget(build_color_row());
    layout->addStretch();
    connect(size_spin, &StepperWidget::valueChanged, this, &BaseSettingsPanel::on_size_changed);
    connect(opacity_spin, &StepperWidget::valueChanged, this, &BaseSettingsPanel::on_opacity_changed);
    connect(color_picker_btn, &ColorPickerButton::color_changed, this, &BaseSettingsPanel::on_color_picked);
    build_extra_controls(layout);
    apply_scale();
}

void BaseSettingsPanel::build_size_and_opacity(QHBoxLayout *layout) {
    const auto range = size_range();
    size_spin = new StepperWidget(current_size, range.first, range.second, {}, nullptr, size_glyph());
    size_spin->setToolTip(tr_panel(size_tooltip()));
    layout->addWidget(size_spin);
    opacity_spin = new StepperWidget(opacity_to_percent(current_opacity), 0, 100, "%", nullptr, "opacity");
    opacity_spin->setToolTip(tr_panel(opacity_tooltip()));
    layout->addWidget(opacity_spin);
}

ColorRow *BaseSettingsPanel::build_color_row() {
    color_picker_btn = new ColorPickerButton(current_color, true, 28, true, true);
    color_picker_btn->setToolTip(tr_panel("Custom Color"));
    color_row_ = new ColorRow(color_picker_btn, PRESET_COLORS, [this](const QString &hex) { apply_preset_color(hex); });
    return color_row_;
}

void BaseSettingsPanel::apply_scale_sizes() {
    if (auto *panel_layout = layout()) {
        const int mh = scaled(BASE_MARGIN_H), mv = scaled(BASE_MARGIN_V);
        panel_layout->setContentsMargins(mh, mv, mh, mv);
        panel_layout->setSpacing(scaled(BASE_SPACING));
    }
    int size_width = BASE_SIZE_SPIN_WIDTH;
    if (size_glyph().isEmpty()) size_width -= StepperWidget::BASE_GLYPH_COLUMN;
    size_spin->setFixedWidth(scaled(size_width));
    opacity_spin->setFixedWidth(scaled(BASE_OPACITY_SPIN_WIDTH));
    color_row_->apply_scale();
}

void BaseSettingsPanel::apply_scale() {
    // Only display sizes change; values, colours and tool state stay.
    setStyleSheet(build_stylesheet());
    apply_scale_sizes();
    adjustSize();
    update();
}

QString BaseSettingsPanel::tr_panel(const char *text) const { return i18n_tr(text, translation_context().toUtf8().constData()); }

void BaseSettingsPanel::on_size_changed(int value) {
    current_size = value;
    emit size_changed(value);
}

void BaseSettingsPanel::on_opacity_changed(int value) {
    current_opacity = percent_to_opacity(value);
    // Alpha follows, so the colour dialog opens with the right value.
    QColor synced(current_color);
    synced.setAlpha(current_opacity);
    current_color = synced;
    color_picker_btn->set_color(synced);
    emit opacity_changed(current_opacity);
}

void BaseSettingsPanel::on_color_picked(const QColor &color) {
    current_color = color;
    // The picked alpha drives the opacity stepper and ctx.opacity.
    const int alpha = color.alpha();
    if (opacity_spin) {
        current_opacity = alpha;
        opacity_spin->blockSignals(true);
        opacity_spin->setValue(opacity_to_percent(alpha));
        opacity_spin->blockSignals(false);
        emit opacity_changed(current_opacity);
    }
    emit color_changed(color);
}

void BaseSettingsPanel::apply_preset_color(const QString &color_hex) {
    current_color = QColor(color_hex);
    emit color_changed(current_color);
    color_picker_btn->set_color(current_color);
}

int BaseSettingsPanel::opacity_to_percent(int opacity) {
    return std::max(0, std::min(100, static_cast<int>(python_round(opacity / 255.0 * 100))));
}

int BaseSettingsPanel::percent_to_opacity(int percent) {
    return std::max(0, std::min(255, static_cast<int>(python_round(percent / 100.0 * 255))));
}

void BaseSettingsPanel::set_color(const QColor &color) {
    current_color = color;
    color_picker_btn->set_color(color);
}

void BaseSettingsPanel::set_size(int size) {
    // Clamped here too, so the stored size matches what the stepper shows.
    const auto [low, high] = size_range();
    size = std::max(low, std::min(high, size));
    current_size = size;
    if (size_spin) {
        size_spin->blockSignals(true);
        size_spin->setValue(size);
        size_spin->blockSignals(false);
    }
}

void BaseSettingsPanel::set_opacity(int opacity) {
    current_opacity = opacity;
    if (opacity_spin) {
        opacity_spin->blockSignals(true);
        opacity_spin->setValue(opacity_to_percent(opacity));
        opacity_spin->blockSignals(false);
    }
}
}
