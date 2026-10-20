#include "ui/color_picker_dialog.h"
#include "core/i18n.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/screen_color_picker.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QSignalBlocker>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace jietuba {
namespace {
QString cp_tr(const char *text) { return i18n_tr(text, "ColorPickerDialog"); }

double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

// 8 bits per channel, as the text fields and every caller see the colour.
QColor rgb8(const QColor &color) { return QColor(color.red(), color.green(), color.blue(), color.alpha()); }

// The next dialog opens in the format last chosen during this run.
ColorPickerDialog::Format last_format = ColorPickerDialog::HEX;

// Transparency checkerboard; light in both themes, as image viewers show it.
QBrush checker_brush(int cell) {
    QPixmap tile(cell * 2, cell * 2);
    tile.fill(QColor(255, 255, 255));
    QPainter painter(&tile);
    painter.fillRect(0, 0, cell, cell, QColor(204, 204, 204));
    painter.fillRect(cell, cell, cell, cell, QColor(204, 204, 204));
    return QBrush(tile);
}

// White ring with a soft outline around the colour it marks, readable on any background.
void draw_handle(QPainter &painter, const QPointF &center, double radius, const QColor &fill, const QBrush *under = nullptr) {
    painter.setPen(QPen(QColor(0, 0, 0, 70), 1.0));
    painter.setBrush(Qt::white);
    painter.drawEllipse(center, radius, radius);
    painter.setPen(Qt::NoPen);
    const double inner = radius * 0.6;
    if (under) {
        painter.setBrushOrigin(center);
        painter.setBrush(*under);
        painter.drawEllipse(center, inner, inner);
    }
    painter.setBrush(fill);
    painter.drawEllipse(center, inner, inner);
}

// Pipette pointing down-left: solid bulb and collar, hollow glass tube.
QPixmap eyedropper_pixmap(int size, const QColor &color) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(size / 24.0, size / 24.0);
    painter.translate(12, 12);
    painter.rotate(45);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(0, -7), 4.6, 4.6);
    painter.drawRoundedRect(QRectF(-5.6, -3.8, 11.2, 3.2), 1.4, 1.4);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawRoundedRect(QRectF(-2.3, -0.6, 4.6, 11.6), 2.3, 2.3);
    return pixmap;
}

// Below the anchor, above it when there is no room, kept on the anchor's screen.
void place_below(QWidget *window, QWidget *anchor) {
    const int gap = dialog_scaled(4);
    const QPoint top_left = anchor->mapToGlobal(QPoint(0, 0));
    auto *screen = QApplication::screenAt(anchor->mapToGlobal(anchor->rect().center()));
    if (!screen) screen = QApplication::primaryScreen();
    const QRect area = screen->availableGeometry();
    int x = top_left.x();
    int y = top_left.y() + anchor->height() + gap;
    if (y + window->height() > area.bottom()) y = top_left.y() - window->height() - gap;
    x = std::max(area.left(), std::min(x, area.right() - window->width()));
    y = std::max(area.top(), std::min(y, area.bottom() - window->height()));
    window->move(x, y);
}

// Format choice drawn as text on the left and a chevron on the right.
class FormatCombo : public QComboBox {
public:
    explicit FormatCombo(QWidget *parent) : QComboBox(parent) {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto &t = ui_tokens(this);
        const double radius = dialog_scaled_f(3);
        const QString edge = hasFocus() || view()->isVisible() ? t.accent : underMouse() ? t.border_hover : t.border;
        painter.setPen(QPen(css_color(edge), 1));
        painter.setBrush(css_color(t.input_background));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        painter.setPen(css_color(isEnabled() ? t.text : t.text_disabled));
        painter.drawText(rect().adjusted(dialog_scaled(9), 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, currentText());
        const double half = dialog_scaled_f(3.6), cx = width() - dialog_scaled_f(11.5), cy = height() / 2.0;
        painter.setPen(QPen(css_color(t.text_muted), dialog_scaled_f(1.5), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        const QPointF chevron[] = {{cx - half, cy - half / 2}, {cx, cy + half / 2}, {cx + half, cy - half / 2}};
        painter.drawPolyline(chevron, 3);
    }
};
}

ColorSquare::ColorSquare(QWidget *parent) : QWidget(parent) {
    setCursor(Qt::CrossCursor);
    setFocusPolicy(Qt::ClickFocus);
}

void ColorSquare::set_hue(double hue) {
    hue = clamp01(hue);
    if (hue == hue_) return;
    hue_ = hue;
    update();
}

void ColorSquare::set_sv(double saturation, double value) {
    saturation = clamp01(saturation);
    value = clamp01(value);
    if (saturation == saturation_ && value == value_) return;
    saturation_ = saturation;
    value_ = value;
    update();
}

QRectF ColorSquare::field_rect() const {
    const int inset = dialog_scaled(BASE_INSET);
    return QRectF(rect().adjusted(inset, inset, -inset, -inset));
}

QPointF ColorSquare::handle_center() const {
    const auto field = field_rect();
    return QPointF(field.left() + saturation_ * field.width(), field.top() + (1 - value_) * field.height());
}

void ColorSquare::paintEvent(QPaintEvent *) {
    const QRectF field = field_rect();
    const QSize size = field.size().toSize();
    if (size.isEmpty()) return;
    if (field_cache_.size() != size || cached_hue_ != hue_) {
        // Pure hue, white fading out to the right, black fading in downwards: exactly HSV at each point.
        field_cache_ = QImage(size, QImage::Format_ARGB32_Premultiplied);
        QPainter cache(&field_cache_);
        cache.fillRect(field_cache_.rect(), QColor::fromHsvF(hue_, 1, 1));
        QLinearGradient white(0, 0, size.width(), 0);
        white.setColorAt(0, QColor(255, 255, 255, 255));
        white.setColorAt(1, QColor(255, 255, 255, 0));
        cache.fillRect(field_cache_.rect(), white);
        QLinearGradient black(0, 0, 0, size.height());
        black.setColorAt(0, QColor(0, 0, 0, 0));
        black.setColorAt(1, QColor(0, 0, 0, 255));
        cache.fillRect(field_cache_.rect(), black);
        cached_hue_ = hue_;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.drawImage(field.topLeft(), field_cache_);
    draw_handle(painter, handle_center(), dialog_scaled_f(BASE_HANDLE_RADIUS), QColor::fromHsvF(hue_, saturation_, value_));
}

void ColorSquare::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    pick_at(event->position());
}

void ColorSquare::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_) pick_at(event->position());
}

void ColorSquare::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) dragging_ = false;
}

void ColorSquare::keyPressEvent(QKeyEvent *event) {
    const double step = event->modifiers() & Qt::ShiftModifier ? 0.1 : 0.01;
    switch (event->key()) {
    case Qt::Key_Left: set_and_emit(saturation_ - step, value_); break;
    case Qt::Key_Right: set_and_emit(saturation_ + step, value_); break;
    case Qt::Key_Up: set_and_emit(saturation_, value_ + step); break;
    case Qt::Key_Down: set_and_emit(saturation_, value_ - step); break;
    default: QWidget::keyPressEvent(event);
    }
}

void ColorSquare::pick_at(const QPointF &pos) {
    // Outside the field the handle stays on its edge, the drag still follows the pointer.
    const auto field = field_rect();
    set_and_emit((pos.x() - field.left()) / field.width(), 1 - (pos.y() - field.top()) / field.height());
}

void ColorSquare::set_and_emit(double saturation, double value) {
    saturation = clamp01(saturation);
    value = clamp01(value);
    if (saturation == saturation_ && value == value_) return;
    saturation_ = saturation;
    value_ = value;
    update();
    emit sv_changed(saturation_, value_);
}

ColorSlider::ColorSlider(Kind kind, QWidget *parent) : QWidget(parent), kind_(kind) {
    setFocusPolicy(Qt::ClickFocus);
    setFixedHeight(dialog_scaled(BASE_HEIGHT));
}

void ColorSlider::set_value(double value) {
    value = clamp01(value);
    if (value == value_) return;
    value_ = value;
    update();
}

void ColorSlider::set_color(const QColor &color) {
    if (color == color_) return;
    color_ = color;
    if (kind_ == ALPHA) update();
}

QRectF ColorSlider::track_rect() const {
    const int inset = dialog_scaled(BASE_INSET);
    const double thickness = dialog_scaled(BASE_TRACK);
    return QRectF(inset, (height() - thickness) / 2, width() - 2 * inset, thickness);
}

double ColorSlider::thumb_x() const {
    const auto track = track_rect();
    return track.left() + value_ * track.width();
}

void ColorSlider::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF track = track_rect();
    const double radius = track.height() / 2;
    QLinearGradient gradient(track.left(), 0, track.right(), 0);
    QColor thumb;
    const QBrush checker = checker_brush(dialog_scaled(3));
    painter.setPen(Qt::NoPen);
    if (kind_ == HUE) {
        for (int i = 0; i <= 6; ++i) gradient.setColorAt(i / 6.0, QColor::fromHsvF(i / 6.0, 1, 1));
        thumb = QColor::fromHsvF(value_, 1, 1);
    } else {
        QColor clear(color_), solid(color_);
        clear.setAlpha(0);
        solid.setAlpha(255);
        gradient.setColorAt(0, clear);
        gradient.setColorAt(1, solid);
        thumb = color_;
        thumb.setAlphaF(value_);
        painter.setBrushOrigin(track.topLeft());
        painter.setBrush(checker);
        painter.drawRoundedRect(track, radius, radius);
    }
    painter.setBrush(gradient);
    painter.drawRoundedRect(track, radius, radius);
    draw_handle(painter, QPointF(thumb_x(), height() / 2.0), dialog_scaled_f(BASE_THUMB_RADIUS), thumb, kind_ == ALPHA ? &checker : nullptr);
}

void ColorSlider::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    pick_at(event->position().x());
}

void ColorSlider::mouseMoveEvent(QMouseEvent *event) {
    if (dragging_) pick_at(event->position().x());
}

void ColorSlider::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) dragging_ = false;
}

void ColorSlider::keyPressEvent(QKeyEvent *event) {
    const double step = event->modifiers() & Qt::ShiftModifier ? 0.1 : 0.01;
    switch (event->key()) {
    case Qt::Key_Left: set_and_emit(value_ - step); break;
    case Qt::Key_Right: set_and_emit(value_ + step); break;
    case Qt::Key_Home: set_and_emit(0); break;
    case Qt::Key_End: set_and_emit(1); break;
    default: QWidget::keyPressEvent(event);
    }
}

void ColorSlider::pick_at(double x) {
    const auto track = track_rect();
    set_and_emit((x - track.left()) / track.width());
}

void ColorSlider::set_and_emit(double value) {
    value = clamp01(value);
    if (value == value_) return;
    value_ = value;
    update();
    emit value_changed(value_);
}

ColorPickerDialog::ColorPickerDialog(const QColor &initial, Options options, QWidget *parent)
    : FrostedFramelessDialog(parent), show_alpha_(options.testFlag(ShowAlphaChannel)) {
    scale_dialog_font(*this);
    frosted_corner_radius_ = dialog_scaled(8);
    // exec() would make it application-modal and block the eyedropper window.
    setWindowModality(Qt::WindowModal);
    caption_bar_ = new FluentTitleBar(this);
    configure_dialog_control(*caption_bar_);
    setTitleBar(caption_bar_);
    caption_bar_->maxBtn->hide();
    caption_bar_->minBtn->hide();
    caption_bar_->hBoxLayout->setContentsMargins(dialog_scaled(8), 0, 0, 0);
    caption_bar_->hBoxLayout->setSpacing(dialog_scaled(6));
    setWindowTitle(cp_tr("Select Color"));
    setResizeEnabled(false);

    const int width = dialog_scaled(BASE_WIDTH);
    const int field_inset = dialog_scaled(ColorSquare::BASE_INSET);
    const int margin = dialog_scaled(BASE_MARGIN);
    const int control_height = dialog_scaled(BASE_CONTROL_HEIGHT);
    square = new ColorSquare(this);
    square->setFixedSize(width, dialog_scaled(BASE_FIELD_HEIGHT) + 2 * field_inset);

    eyedropper_button = new QToolButton(this);
    eyedropper_button->setCheckable(true);
    eyedropper_button->setCursor(Qt::PointingHandCursor);
    eyedropper_button->setFixedSize(dialog_scaled(32), dialog_scaled(33));
    eyedropper_button->setToolTip(cp_tr("Pick color from screen") + "\n" + cp_tr("Click to pick; right-click or Esc to cancel"));
    hue_slider = new ColorSlider(ColorSlider::HUE, this);
    hue_slider->setToolTip(cp_tr("Hue"));
    alpha_slider = new ColorSlider(ColorSlider::ALPHA, this);
    alpha_slider->setToolTip(cp_tr("Opacity"));
    alpha_slider->setVisible(show_alpha_);

    QFont control_font = font();
    control_font.setPixelSize(dialog_scaled(BASE_FONT));
    format_combo = new FormatCombo(this);
    format_combo->setFont(control_font);
    format_combo->addItem("Hex", HEX);
    format_combo->addItem("RGB", RGB);
    format_combo->addItem("HSL", HSL);
    format_combo->setCurrentIndex(std::max(0, format_combo->findData(last_format)));
    format_combo->setFixedSize(dialog_scaled(61), control_height);

    value_group = new QFrame(this);
    value_group->setObjectName("valueGroup");
    value_group->setFixedSize(dialog_scaled(133), control_height);
    value_edit = new LineEdit(value_group, false);
    value_divider_ = new QFrame(value_group);
    value_divider_->setFixedWidth(1);
    alpha_edit = new LineEdit(value_group, false);
    alpha_edit->setFixedWidth(dialog_scaled(50));
    alpha_edit->setValidator(new QRegularExpressionValidator(QRegularExpression(R"(\s*\d{0,3}\s*%?\s*)"), alpha_edit));
    alpha_edit->setToolTip(cp_tr("Opacity"));
    value_divider_->setVisible(show_alpha_);
    alpha_edit->setVisible(show_alpha_);
    auto *group_layout = new QHBoxLayout(value_group);
    group_layout->setContentsMargins(1, 1, 1, 1);
    group_layout->setSpacing(0);
    group_layout->addWidget(value_edit, 1);
    group_layout->addWidget(value_divider_);
    group_layout->addWidget(alpha_edit);

    separator_ = new QFrame(this);
    separator_->setFixedHeight(1);
    ok_button = new QPushButton(cp_tr("OK"), this);
    ok_button->setDefault(true);
    cancel_button = new QPushButton(cp_tr("Cancel"), this);
    cancel_button->setAutoDefault(false);
    for (auto *button : {ok_button, cancel_button}) {
        button->setFixedSize(dialog_scaled(BASE_BUTTON_WIDTH), dialog_scaled(BASE_BUTTON_HEIGHT));
        button->setCursor(Qt::PointingHandCursor);
    }

    // Spacing follows the field: tracks beside the eyedropper, format and value on one row,
    // a rule, then the buttons at the right.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, dialog_scaled(BASE_TITLE_HEIGHT + 7) - field_inset, 0, dialog_scaled(9));
    root->setSpacing(0);
    root->addWidget(square);
    root->addSpacing(dialog_scaled(12));
    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(margin, 0, margin, 0);
    tools->setSpacing(dialog_scaled(6));
    tools->addWidget(eyedropper_button, 0, Qt::AlignVCenter);
    auto *tracks = new QVBoxLayout;
    tracks->setContentsMargins(0, 0, 0, 0);
    tracks->setSpacing(0);
    tracks->addWidget(hue_slider);
    tracks->addWidget(alpha_slider);
    tools->addLayout(tracks, 1);
    root->addLayout(tools);
    root->addSpacing(dialog_scaled(2));
    auto *inputs = new QHBoxLayout;
    inputs->setContentsMargins(margin, 0, margin, 0);
    inputs->addWidget(format_combo);
    inputs->addStretch(1);
    inputs->addWidget(value_group);
    root->addLayout(inputs);
    root->addSpacing(dialog_scaled(6));
    auto *rule = new QHBoxLayout;
    rule->setContentsMargins(margin, 0, margin, 0);
    rule->addWidget(separator_);
    root->addLayout(rule);
    root->addSpacing(dialog_scaled(7));
    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(margin, 0, margin, 0);
    buttons->setSpacing(dialog_scaled(5));
    buttons->addStretch(1);
    buttons->addWidget(ok_button);
    buttons->addWidget(cancel_button);
    root->addLayout(buttons);

    connect(square, &ColorSquare::sv_changed, this, [this](double saturation, double value) {
        end_eyedropper_for_edit();
        saturation_ = saturation;
        value_ = value;
        apply_hsv(Origin::SQUARE);
    });
    connect(hue_slider, &ColorSlider::value_changed, this, [this](double hue) {
        end_eyedropper_for_edit();
        hue_ = hue;
        apply_hsv(Origin::HUE);
    });
    connect(alpha_slider, &ColorSlider::value_changed, this, [this](double alpha) {
        end_eyedropper_for_edit();
        QColor color = color_;
        color.setAlpha(static_cast<int>(std::lround(alpha * 255)));
        commit(color, Origin::ALPHA);
    });
    connect(format_combo, &QComboBox::currentIndexChanged, this, [this] {
        last_format = current_format();
        refresh_texts(Origin::OTHER);
    });
    connect(value_edit, &QLineEdit::textEdited, this, [this] { apply_value_text(false); });
    connect(alpha_edit, &QLineEdit::textEdited, this, [this] {
        const auto percent = parse_alpha_text();
        if (!percent) return;
        end_eyedropper_for_edit();
        QColor color = color_;
        color.setAlpha(static_cast<int>(std::lround(*percent * 255 / 100.0)));
        commit(color, Origin::ALPHA_TEXT);
    });
    // Leaving a field shows the colour in its usual spelling again, or undoes unreadable text.
    connect(value_edit, &QLineEdit::editingFinished, this, [this] {
        apply_value_text(true);
        refresh_texts(Origin::OTHER);
    });
    connect(alpha_edit, &QLineEdit::editingFinished, this, [this] { refresh_texts(Origin::OTHER); });
    value_edit->installEventFilter(this);
    alpha_edit->installEventFilter(this);
    connect(eyedropper_button, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) start_eyedropper();
        else stop_eyedropper(true);
    });
    connect(ok_button, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { apply_theme(); });

    apply_theme();
    apply_color(initial.isValid() ? initial : QColor(Qt::white), Origin::OTHER);
    setFixedWidth(width);
    adjustSize();
}

ColorPickerDialog::~ColorPickerDialog() {
    if (picker_) {
        picker_->disconnect(this);
        picker_->close();
    }
}

void ColorPickerDialog::apply_theme() {
    const auto &t = ui_tokens(this);
    // The caption bar restyles itself on theme changes; this runs after it.
    const int title_height = dialog_scaled(BASE_TITLE_HEIGHT);
    caption_bar_->setFixedHeight(title_height);
    caption_bar_->closeBtn->setFixedSize(dialog_scaled(40), title_height);
    const QIcon app_icon = windowIcon();
    caption_bar_->iconLabel->setFixedSize(dialog_scaled(16), dialog_scaled(16));
    caption_bar_->setIcon(app_icon);
    caption_bar_->iconLabel->setVisible(!app_icon.isNull());

    const int radius = dialog_scaled(3);
    const int font_px = dialog_scaled(BASE_FONT);
    const int icon = dialog_scaled(22);
    QIcon glyph;
    glyph.addPixmap(eyedropper_pixmap(icon, css_color(t.text)), QIcon::Normal, QIcon::Off);
    glyph.addPixmap(eyedropper_pixmap(icon, css_color(t.accent_text)), QIcon::Normal, QIcon::On);
    eyedropper_button->setIcon(glyph);
    eyedropper_button->setIconSize(QSize(icon, icon));
    apply_style_sheet(eyedropper_button, QString("QToolButton { background: %1; border: 1px solid %2; border-radius: %3px; }"
                                                 "QToolButton:hover { border-color: %4; }"
                                                 "QToolButton:checked { background: %5; border-color: %6; }")
                                             .arg(t.input_background, t.border)
                                             .arg(radius)
                                             .arg(t.border_hover, t.accent_soft, t.accent));
    apply_style_sheet(format_combo, QString("QComboBox QAbstractItemView { background: %1; color: %2; border: 1px solid %3; outline: none;"
                                            " selection-background-color: %4; selection-color: %2; }")
                                        .arg(t.popup_background, t.text, t.border, t.popup_hover));
    apply_style_sheet(value_group, QString("QFrame#valueGroup { background: %1; border: 1px solid %2; border-radius: %3px; }"
                                           "QFrame#valueGroup[focused=\"true\"] { border-color: %4; }")
                                       .arg(t.input_background, t.border)
                                       .arg(radius)
                                       .arg(t.accent));
    const QString field = QString("QLineEdit { border: none; background: transparent; color: %1; font: %2px %3; padding: 0 %4px;"
                                  " selection-background-color: %5; selection-color: %6; }")
                              .arg(t.text)
                              .arg(font_px)
                              .arg(FONT_FAMILY)
                              .arg(dialog_scaled(6))
                              .arg(t.accent, t.selected_text);
    apply_style_sheet(value_edit, field);
    apply_style_sheet(alpha_edit, field);
    for (auto *line : {value_divider_, separator_}) apply_style_sheet(line, QString("QFrame { background: %1; border: none; }").arg(line == separator_ ? t.separator : t.border));
    const QString button = QString("QPushButton { border-radius: %1px; font: %2px %3; padding: 0; outline: none; }").arg(dialog_scaled(4)).arg(font_px).arg(FONT_FAMILY);
    apply_style_sheet(ok_button, button + QString("QPushButton { background: %1; color: %2; border: none; }"
                                                  "QPushButton:hover { background: %3; } QPushButton:pressed { background: %4; }")
                                              .arg(t.accent_strong, t.selected_text, t.accent_strong_hover, t.accent_strong_pressed));
    apply_style_sheet(cancel_button, button + QString("QPushButton { background: %1; color: %2; border: 1px solid %3; }"
                                                      "QPushButton:hover { border-color: %4; } QPushButton:pressed { background: %5; }")
                                                  .arg(t.input_background, t.text, t.border, t.border_hover, t.surface_subtle));
    update();
    square->update();
    hue_slider->update();
    alpha_slider->update();
    format_combo->update();
}

ColorPickerDialog::Format ColorPickerDialog::current_format() const { return static_cast<Format>(format_combo->currentData().toInt()); }

void ColorPickerDialog::setCurrentColor(const QColor &color) {
    if (color.isValid()) apply_color(color, Origin::OTHER);
}

void ColorPickerDialog::apply_hsv(Origin origin) {
    QColor color = rgb8(QColor::fromHsvF(hue_, saturation_, value_));
    color.setAlpha(color_.isValid() ? color_.alpha() : 255);
    commit(color, origin);
}

void ColorPickerDialog::apply_color(const QColor &color, Origin origin) {
    const QColor hsv = color.toHsv();
    const double value = hsv.valueF();
    // Black has no saturation and greys no hue; the controls keep the last ones, so
    // moving back out of the black edge or the grey axis returns to the same colour.
    if (value > 0) {
        saturation_ = hsv.hsvSaturationF();
        const double hue = hsv.hsvHueF();
        // Red sits at both ends of the hue track; a thumb at the right end stays there.
        if (hue >= 0 && !(hue == 0 && hue_ == 1)) hue_ = hue;
    }
    value_ = value;
    commit(rgb8(color), origin);
}

void ColorPickerDialog::commit(const QColor &input, Origin origin) {
    QColor color = input;
    if (!show_alpha_) color.setAlpha(255);
    // The setters are silent and skip unchanged values, so the control that made the change stays as it is.
    square->set_hue(hue_);
    square->set_sv(saturation_, value_);
    hue_slider->set_value(hue_);
    alpha_slider->set_color(color);
    if (origin != Origin::ALPHA) alpha_slider->set_value(color.alpha() / 255.0);
    const bool changed = color != color_;
    color_ = color;
    refresh_texts(origin);
    if (changed) emit currentColorChanged(color_);
}

void ColorPickerDialog::refresh_texts(Origin origin) {
    if (origin != Origin::VALUE_TEXT) {
        const QString text = format_color(current_format(), color_, hue_);
        if (value_edit->text() != text) value_edit->setText(text);
    }
    if (origin != Origin::ALPHA_TEXT) {
        const QString text = QString("%1%").arg(std::lround(color_.alpha() * 100 / 255.0));
        if (alpha_edit->text() != text) alpha_edit->setText(text);
    }
}

void ColorPickerDialog::apply_value_text(bool finished) {
    const QString text = value_edit->text();
    const auto parsed = parse_color(current_format(), text);
    if (!parsed) return;
    // Three hex digits are also how six begin; while typing they would flash an unrelated colour.
    if (!finished && current_format() == HEX && QString(text).remove('#').trimmed().size() == 3) return;
    QColor color = parsed->color;
    if (!parsed->has_alpha) color.setAlpha(color_.alpha());
    // Unchanged text re-read on leaving would round the controls to 8-bit steps.
    if (color == color_) return;
    end_eyedropper_for_edit();
    apply_color(color, Origin::VALUE_TEXT);
}

std::optional<int> ColorPickerDialog::parse_alpha_text() const {
    QString text = alpha_edit->text().trimmed();
    if (text.endsWith('%')) text.chop(1);
    bool ok = false;
    const int percent = text.trimmed().toInt(&ok);
    if (!ok || percent < 0 || percent > 100) return std::nullopt;
    return percent;
}

std::optional<ColorPickerDialog::ParsedColor> ColorPickerDialog::parse_color(Format format, const QString &input) {
    QString text = input.trimmed();
    if (format == HEX) {
        if (text.startsWith('#')) text.remove(0, 1);
        static const QRegularExpression hex_digits("^[0-9A-Fa-f]+$");
        if (!hex_digits.match(text).hasMatch()) return std::nullopt;
        if (text.size() == 3) text = QString("%1%1%2%2%3%3").arg(text[0]).arg(text[1]).arg(text[2]);
        if (text.size() != 6 && text.size() != 8) return std::nullopt;
        const uint rgb = text.left(6).toUInt(nullptr, 16);
        ParsedColor parsed{QColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF)};
        if (text.size() == 8) {
            parsed.color.setAlpha(text.mid(6).toInt(nullptr, 16));
            parsed.has_alpha = true;
        }
        return parsed;
    }
    // Three numbers, bare or as rgb(...) / hsl(...), separated by commas or spaces; % and deg are allowed.
    static const QRegularExpression wrapper(R"(^(?:rgba?|hsla?)\s*\((.*)\)$)", QRegularExpression::CaseInsensitiveOption);
    if (const auto match = wrapper.match(text); match.hasMatch()) text = match.captured(1);
    static const QRegularExpression separators(R"([\s,]+)");
    const auto parts = text.split(separators, Qt::SkipEmptyParts);
    if (parts.size() != 3) return std::nullopt;
    double values[3];
    for (int i = 0; i < 3; ++i) {
        QString part = parts[i];
        if (part.endsWith('%')) part.chop(1);
        else if (part.endsWith("deg", Qt::CaseInsensitive)) part.chop(3);
        bool ok = false;
        values[i] = part.toDouble(&ok);
        if (!ok || !std::isfinite(values[i]) || values[i] < 0) return std::nullopt;
    }
    if (format == RGB) {
        if (values[0] > 255 || values[1] > 255 || values[2] > 255) return std::nullopt;
        return ParsedColor{QColor(static_cast<int>(std::lround(values[0])), static_cast<int>(std::lround(values[1])), static_cast<int>(std::lround(values[2])))};
    }
    if (values[0] > 360 || values[1] > 100 || values[2] > 100) return std::nullopt;
    return ParsedColor{rgb8(QColor::fromHslF(std::fmod(values[0], 360.0) / 360.0, values[1] / 100.0, values[2] / 100.0))};
}

QString ColorPickerDialog::format_color(Format format, const QColor &color, double hue_fallback) {
    switch (format) {
    case RGB: return QString("%1, %2, %3").arg(color.red()).arg(color.green()).arg(color.blue());
    case HSL: {
        const QColor hsl = color.toHsl();
        int hue = hsl.hslHue();
        if (hue < 0) hue = static_cast<int>(std::lround(clamp01(hue_fallback) * 360)) % 360;
        return QString("%1, %2%, %3%").arg(hue).arg(std::lround(hsl.hslSaturationF() * 100)).arg(std::lround(hsl.lightnessF() * 100));
    }
    case HEX: break;
    }
    return color.name(QColor::HexRgb).toUpper();
}

void ColorPickerDialog::place_near(QWidget *anchor) {
    if (!anchor) return;
    adjustSize();
    place_below(this, anchor);
}

void ColorPickerDialog::end_eyedropper_for_edit() {
    if (picker_) stop_eyedropper(false);
}

void ColorPickerDialog::start_eyedropper() {
    if (picker_) return;
    before_pick_ = color_;
    before_hue_ = hue_;
    before_saturation_ = saturation_;
    before_value_ = value_;
    auto *picker = new ScreenColorPicker;
    picker_ = picker;
    picker->keep_above(this);
    // Screen pixels are opaque; the opacity set in the dialog stays.
    const auto with_alpha = [this](QColor color) {
        color.setAlpha(before_pick_.alpha());
        return color;
    };
    connect(picker, &ScreenColorPicker::hovered, this, [this, with_alpha](const QColor &color) { apply_color(with_alpha(color), Origin::OTHER); });
    connect(picker, &ScreenColorPicker::left, this, [this] { restore_before_pick(); });
    connect(picker, &ScreenColorPicker::picked, this, [this, with_alpha](const QColor &color) {
        apply_color(with_alpha(color), Origin::OTHER);
        stop_eyedropper(false);
    });
    connect(picker, &ScreenColorPicker::cancelled, this, [this] { stop_eyedropper(true); });
    if (!picker->start()) stop_eyedropper(true);
}

void ColorPickerDialog::restore_before_pick() {
    hue_ = before_hue_;
    saturation_ = before_saturation_;
    value_ = before_value_;
    commit(before_pick_, Origin::OTHER);
}

void ColorPickerDialog::stop_eyedropper(bool restore) {
    {
        const QSignalBlocker blocker(eyedropper_button);
        eyedropper_button->setChecked(false);
    }
    if (!picker_) return;
    picker_->disconnect(this);
    picker_->close();
    picker_ = nullptr;
    if (restore) restore_before_pick();
    if (isVisible()) activateWindow();
}

void ColorPickerDialog::paintEvent(QPaintEvent *) {
    // A tinted caption band over the plain body.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const auto &t = ui_tokens(this);
    QPainterPath frame;
    frame.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), frosted_corner_radius_, frosted_corner_radius_);
    painter.fillPath(frame, css_color(t.popup_background));
    painter.save();
    painter.setClipRect(QRect(0, 0, width(), caption_bar_->height()));
    painter.fillPath(frame, css_color(t.window));
    painter.restore();
    painter.setPen(QPen(css_color(t.window_border), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(frame);
}

void ColorPickerDialog::showEvent(QShowEvent *event) {
    FrostedFramelessDialog::showEvent(event);
    // Over the capture and pin windows it is opened from. Only the z-order band changes:
    // the frameless setup has created the native window, which a new window flag would rebuild.
    set_window_topmost(static_cast<quintptr>(winId()), true);
}

void ColorPickerDialog::keyPressEvent(QKeyEvent *event) {
    if (picker_ && event->key() == Qt::Key_Escape) {
        stop_eyedropper(true);
        return;
    }
    FrostedFramelessDialog::keyPressEvent(event);
}

bool ColorPickerDialog::eventFilter(QObject *watched, QEvent *event) {
    if (watched == value_edit || watched == alpha_edit) {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
            // The shared frame takes the focus colour for either field.
            value_group->setProperty("focused", value_edit->hasFocus() || alpha_edit->hasFocus());
            value_group->style()->unpolish(value_group);
            value_group->style()->polish(value_group);
        } else if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                const bool readable = watched == value_edit ? parse_color(current_format(), value_edit->text()).has_value() : parse_alpha_text().has_value();
                // Enter confirms only text that names a colour; otherwise the field shows the colour again.
                if (!readable) {
                    refresh_texts(Origin::OTHER);
                    return true;
                }
            }
        }
    }
    return FrostedFramelessDialog::eventFilter(watched, event);
}

void ColorPickerDialog::done(int result) {
    stop_eyedropper(true);
    if (result == QDialog::Accepted) {
        selected_ = color_;
        emit colorSelected(color_);
    }
    FrostedFramelessDialog::done(result);
}

std::optional<QColor> ColorPickerDialog::getColor(const QColor &initial, QWidget *anchor, const QString &title, Options options,
                                                  const std::function<void(const QColor &)> &on_change) {
    ColorPickerDialog dialog(initial, options);
    if (!title.isEmpty()) dialog.setWindowTitle(title);
    dialog.place_near(anchor);
    bool changed = false;
    if (on_change) {
        connect(&dialog, &ColorPickerDialog::currentColorChanged, &dialog, [&on_change, &changed](const QColor &color) {
            changed = true;
            on_change(color);
        });
    }
    if (exec_dialog(&dialog, anchor) == QDialog::Accepted) return dialog.selectedColor();
    if (changed) on_change(initial);
    return std::nullopt;
}

bool use_system_color_dialog(ToolSettingsManager *config) {
    if (!config) config = get_tool_settings_manager();
    return config->get_app_setting("color_picker_style").toString() == "system";
}

std::optional<QColor> choose_color(const QColor &initial, QWidget *anchor, const QString &title, ColorPickerDialog::Options options,
                                   const std::function<void(const QColor &)> &on_change) {
    if (!use_system_color_dialog()) return ColorPickerDialog::getColor(initial, anchor, title, options, on_change);
    const bool alpha = options.testFlag(ColorPickerDialog::ShowAlphaChannel);
    const auto opaque = [alpha](QColor color) {
        if (!alpha) color.setAlpha(255);
        return color;
    };
    QColorDialog dialog(initial.isValid() ? initial : QColor(Qt::white));
    dialog.setWindowTitle(title.isEmpty() ? cp_tr("Select Color") : title);
    // An ordinary window: unlike the frameless dialog, the flag is safe before it is shown.
    dialog.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    if (alpha) dialog.setOption(QColorDialog::ShowAlphaChannel);
    if (anchor) {
        dialog.adjustSize();
        place_below(&dialog, anchor);
    }
    bool changed = false;
    if (on_change) {
        QObject::connect(&dialog, &QColorDialog::currentColorChanged, &dialog, [&on_change, &changed, opaque](const QColor &color) {
            changed = true;
            on_change(opaque(color));
        });
    }
    if (exec_dialog(&dialog, anchor) == QDialog::Accepted) return opaque(dialog.selectedColor());
    if (changed) on_change(initial);
    return std::nullopt;
}
}
