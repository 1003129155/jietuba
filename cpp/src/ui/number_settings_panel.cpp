#include "ui/number_settings_panel.h"
#include "canvas/items/drawing_items.h"
#include "canvas/items/text_item.h"
#include "core/constants.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/arrow_settings_panel.h"
#include "ui/text_settings_panel.h"
#include <QAbstractItemView>
#include <QEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

namespace jietuba {
QPixmap render_number_style_preview(const QString &style, const QColor &color, int side, int number) {
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    const double radius = side / 2.0 - 2.0;
    NumberItem item(number, QPointF(0, 0), radius, color, style);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(side / 2.0, side / 2.0);
    const QStyleOptionGraphicsItem option;
    item.paint(&painter, &option, nullptr);
    painter.end();
    return pixmap;
}

namespace {
// Thin strokes in the panel's ink, like its chevrons and opacity glyph.
struct Glyph {
    QPixmap pixmap;
    double s;
    explicit Glyph(int side, double ratio) : pixmap(std::max(1, qRound(side * ratio)), std::max(1, qRound(side * ratio))), s(side) {
        pixmap.fill(Qt::transparent);
        pixmap.setDevicePixelRatio(ratio);
    }
    // Stroke width scales with the icon: 1.4 at the base side of 18.
    QPen pen() const { return QPen(CONTROL_INK, s * 1.4 / 18, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); }
};

void stroke_letter_a(QPainter &painter, const QRectF &box) {
    const QPointF apex(box.center().x(), box.top());
    painter.drawPolyline(QPolygonF({QPointF(box.left(), box.bottom()), apex, QPointF(box.right(), box.bottom())}));
    const double bar = box.top() + box.height() * .64;
    const double inset = box.width() * .5 * .36;
    painter.drawLine(QPointF(box.left() + inset, bar), QPointF(box.right() - inset, bar));
}

void stroke_letter_t(QPainter &painter, const QRectF &box) {
    painter.drawLine(box.topLeft(), box.topRight());
    painter.drawLine(QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.bottom()));
}

// The section mark: a T.
QPixmap render_label_section_icon(int side, double ratio) {
    Glyph glyph(side, ratio);
    QPainter painter(&glyph.pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(glyph.pen());
    const double s = glyph.s;
    stroke_letter_t(painter, QRectF(s * .24, s * .2, s * .52, s * .62));
    return glyph.pixmap;
}

// An A with a small ring: the outline.
QPixmap render_label_outline_icon(int side, double ratio) {
    Glyph glyph(side, ratio);
    QPainter painter(&glyph.pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(glyph.pen());
    painter.setBrush(Qt::NoBrush);
    const double s = glyph.s;
    stroke_letter_a(painter, QRectF(s * .12, s * .22, s * .56, s * .6));
    painter.drawEllipse(QPointF(s * .8, s * .26), s * .09, s * .09);
    return glyph.pixmap;
}

// A dot with a line dropping from it and running under the text: the guide line.
QPixmap render_label_guide_icon(int side, double ratio) {
    Glyph glyph(side, ratio);
    QPainter painter(&glyph.pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(glyph.pen());
    painter.setBrush(Qt::NoBrush);
    const double s = glyph.s;
    painter.drawEllipse(QPointF(s * .24, s * .24), s * .09, s * .09);
    painter.drawPolyline(QPolygonF({QPointF(s * .24, s * .38), QPointF(s * .24, s * .8), QPointF(s * .84, s * .8)}));
    return glyph.pixmap;
}

// An A whose height grows with the level.
QPixmap render_label_size_icon(int side, double ratio, int level) {
    Glyph glyph(side, ratio);
    QPainter painter(&glyph.pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(glyph.pen());
    const double s = glyph.s;
    const int last = static_cast<int>(NumberItem::LABEL_SIZE_LEVELS.size()) - 1;
    const double height = s * (.34 + .44 * level / last), width = height * .84;
    stroke_letter_a(painter, QRectF((s - width) / 2, s * .84 - height, width, height));
    return glyph.pixmap;
}
}

NumberStylePopup::NumberStylePopup(QWidget *parent) : HoverPopup(parent) {
    // The strip centres on the preview, so the pointer usually brushes the middle
    // buttons first: hover is barely tinted and selected gets a clear theme frame
    // (not a solid fill, which would hide the dark glyph).
    const auto accent = accent_color();
    set_extra_stylesheet(QString("QToolButton { border: 1px solid transparent; border-radius: %1px; background: transparent; }"
                                 "QToolButton:hover { background-color: rgba(15, 23, 42, 0.06); }"
                                 "QToolButton:checked, QToolButton:checked:hover {"
                                 " background-color: rgba(%2, %3, %4, 0.15);"
                                 " border: 2px solid %5; }")
                             .arg(scaled(6)).arg(accent.red()).arg(accent.green()).arg(accent.blue()).arg(accent.name()));
    auto *layout = new QHBoxLayout(this);
    const int pad = scaled(4);
    layout->setContentsMargins(pad, pad, pad, pad);
    layout->setSpacing(scaled(2));
    const std::pair<QString, const char *> styles[] = {
        {NumberItem::STYLE_SOLID, "Solid"}, {NumberItem::STYLE_HOLLOW_BG, "Hollow Circle"},
        {NumberItem::STYLE_HOLLOW_ALL, "Outline Only"}, {NumberItem::STYLE_NO_CIRCLE, "Number Only"},
    };
    for (const auto &[style, tip] : styles) {
        auto *button = new QToolButton(this);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setToolTip(i18n_tr(tip, "ArrowSettingsPanel"));
        button->setFocusPolicy(Qt::NoFocus);
        const QString key = style;
        connect(button, &QToolButton::clicked, this, [this, key] { choose(key); });
        layout->addWidget(button);
        buttons_[style] = button;
    }
    apply_scale();
}

void NumberStylePopup::apply_scale() {
    HoverPopup::apply_scale();
    if (buttons_.isEmpty()) return;
    const int side = scaled(BASE_ITEM_SIDE);
    const int cell = side + scaled(BASE_ITEM_PADDING);
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) {
        it.value()->setFixedSize(cell, cell);
        it.value()->setIconSize(QSize(side, side));
        it.value()->setIcon(render_number_style_preview(it.key(), PREVIEW_INK, side));
    }
    adjustSize();
}

void NumberStylePopup::set_current_style(const QString &requested) {
    const auto style = NumberItem::normalize_style(requested);
    for (auto it = buttons_.begin(); it != buttons_.end(); ++it) it.value()->setChecked(it.key() == style);
}

void NumberStylePopup::choose(const QString &style) {
    set_current_style(style);
    emit style_selected(style);
    hide();
}

NumberSettingsPanel::NumberSettingsPanel(QWidget *parent) : BaseSettingsPanel(parent) { init_ui(); }

void NumberSettingsPanel::build_extra_controls(QHBoxLayout *layout) {
    next_preview = new QLabel;
    next_preview->setAlignment(Qt::AlignCenter);
    // Drawn with the style so it shows what the next number will look like.
    set_own_style(next_preview, "background: transparent;");
    // Preview and chevrons share one pill like the other value controls.
    next_pill_ = new PillFrame;
    next_pill_layout_ = new QHBoxLayout(next_pill_);
    next_pill_layout_->setSpacing(0);
    next_pill_layout_->addWidget(next_preview);
    layout->insertWidget(0, next_pill_);
    next_value_ = 1;
    current_style_ = NumberItem::DEFAULT_STYLE;
    // Hovering the preview opens the style strip.
    next_preview->setAttribute(Qt::WA_Hover, true);
    next_preview->setToolTip(tr_panel("Number Style"));
    next_preview->installEventFilter(this);
    auto *btn_wrap = new QWidget;
    next_btn_layout_ = new QVBoxLayout(btn_wrap);
    next_btn_layout_->setContentsMargins(0, 0, 0, 0);
    next_btn_layout_->setSpacing(0);
    next_up_btn = new QToolButton;
    next_up_btn->setToolTip(tr_panel("Next Number"));
    next_btn_layout_->addWidget(next_up_btn);
    next_down_btn = new QToolButton;
    next_down_btn->setToolTip(tr_panel("Next Number"));
    next_btn_layout_->addWidget(next_down_btn);
    next_pill_layout_->addWidget(btn_wrap);
    connect(next_up_btn, &QToolButton::clicked, this, [this] { change_next_number(1); });
    connect(next_down_btn, &QToolButton::clicked, this, [this] { change_next_number(-1); });
    build_label_section(layout);
}

void NumberSettingsPanel::build_label_section(QHBoxLayout *layout) {
    // After the colours, before the trailing stretch.
    int at = layout->count() - 1;
    const auto add = [&](QWidget *widget) {
        layout->insertWidget(at++, widget);
        label_widgets_.push_back(widget);
        widget->hide();
    };
    add(make_separator());
    label_icon_ = new QLabel;
    set_own_style(label_icon_, "background: transparent;");
    add(label_icon_);
    label_font_combo = new PillComboBox;
    label_font_combo->addItems(get_available_text_fonts());
    label_font_combo->setCurrentText(get_default_text_font_for_language());
    label_font_combo->setToolTip(TextSettingsPanel::tr_text("Font"));
    label_font_combo->setMaxVisibleItems(15);
    add(label_font_combo);
    label_outline_btn = new QPushButton;
    label_outline_btn->setToolTip(TextSettingsPanel::tr_text("Text Outline"));
    label_guide_btn = new QPushButton;
    label_guide_btn->setToolTip(tr_panel("Text Guide Line"));
    for (auto *button : {label_outline_btn, label_guide_btn}) {
        button->setCheckable(true);
        add(button);
    }
    label_size_combo = new PillComboBox;
    for (size_t level = 0; level < NumberItem::LABEL_SIZE_LEVELS.size(); ++level) label_size_combo->addItem(QString());
    label_size_combo->setCurrentIndex(NumberItem::DEFAULT_LABEL_SIZE_LEVEL);
    label_size_combo->setToolTip(TextSettingsPanel::tr_text("Font Size"));
    add(label_size_combo);
    add(make_separator());
    label_arrow_combo = new PillComboBox;
    for (const auto &style : NumberItem::ARROW_STYLES) label_arrow_combo->addItem(arrow_style_icon(style), QString(), style);
    label_arrow_combo->setItemDelegate(new StylePreviewDelegate(label_arrow_combo));
    label_arrow_combo->setToolTip(i18n_tr("Arrow Style", "ArrowSettingsPanel"));
    add(label_arrow_combo);
    connect(label_font_combo, &QComboBox::currentTextChanged, this, &NumberSettingsPanel::label_font_changed);
    connect(label_size_combo, &QComboBox::currentIndexChanged, this, &NumberSettingsPanel::label_size_changed);
    connect(label_outline_btn, &QPushButton::toggled, this, &NumberSettingsPanel::label_outline_changed);
    connect(label_guide_btn, &QPushButton::toggled, this, &NumberSettingsPanel::label_guide_changed);
    connect(label_arrow_combo, &QComboBox::currentIndexChanged, this, [this](int) { emit arrow_style_changed(label_arrow_combo->currentData().toString()); });
}

void NumberSettingsPanel::apply_label_section_scale() {
    if (!label_icon_) return;
    const double ratio = devicePixelRatioF();
    const int icon = scaled(BASE_LABEL_ICON), button = scaled(BASE_LABEL_BTN);
    label_icon_->setFixedSize(icon, button);
    label_icon_->setPixmap(render_label_section_icon(icon, ratio));
    label_font_combo->setFixedWidth(scaled(BASE_FONT_COMBO_WIDTH));
    label_font_combo->view()->setMinimumWidth(scaled(BASE_FONT_VIEW_WIDTH));
    label_outline_btn->setIcon(QIcon(render_label_outline_icon(icon, ratio)));
    label_guide_btn->setIcon(QIcon(render_label_guide_icon(icon, ratio)));
    for (auto *b : {label_outline_btn, label_guide_btn}) {
        b->setFixedSize(button, button);
        b->setIconSize(QSize(icon, icon));
    }
    // A closed combo draws only its current icon, between the left padding and the arrow.
    const int pill_sides = PillComboBox::BASE_PADDING_LEFT + PillComboBox::BASE_ARROW_COLUMN;
    label_size_combo->setIconSize(QSize(icon, icon));
    label_size_combo->setFixedWidth(scaled(pill_sides + BASE_LABEL_ICON));
    for (int level = 0; level < label_size_combo->count(); ++level) label_size_combo->setItemIcon(level, QIcon(render_label_size_icon(icon, ratio, level)));
    label_arrow_combo->setFixedWidth(scaled(pill_sides + BASE_ARROW_PREVIEW_WIDTH));
    label_arrow_combo->setIconSize(preview_size());
    label_arrow_combo->view()->setMinimumWidth(preview_row_size().width());
    for (int index = 0; index < label_arrow_combo->count(); ++index) label_arrow_combo->setItemIcon(index, arrow_style_icon(label_arrow_combo->itemData(index).toString(), ratio));
}

void NumberSettingsPanel::set_label_section_visible(bool visible) {
    if (visible == label_section_visible_) return;
    label_section_visible_ = visible;
    for (auto *widget : label_widgets_) widget->setVisible(visible);
    adjustSize();
}

void NumberSettingsPanel::set_label_state(const QString &font_family, int size_level, bool outline, bool guide, const QString &arrow_style) {
    const QSignalBlocker font_block(label_font_combo), size_block(label_size_combo), outline_block(label_outline_btn), guide_block(label_guide_btn),
        arrow_block(label_arrow_combo);
    if (const int index = label_arrow_combo->findData(arrow_style); index >= 0) label_arrow_combo->setCurrentIndex(index);
    if (label_font_combo->findText(font_family) < 0) label_font_combo->addItem(font_family);
    label_font_combo->setCurrentText(font_family);
    label_size_combo->setCurrentIndex(std::clamp(size_level, 0, label_size_combo->count() - 1));
    label_outline_btn->setChecked(outline);
    label_guide_btn->setChecked(guide);
}

void NumberSettingsPanel::apply_scale_sizes() {
    BaseSettingsPanel::apply_scale_sizes();
    const int preview = scaled(BASE_NEXT_PREVIEW);
    next_preview->setFixedSize(preview, preview);
    next_pill_->apply_scale();
    next_pill_layout_->setContentsMargins(scaled(4), 0, scaled(3), 0);
    const int half = scaled(PillFrame::BASE_HEIGHT) / 2;
    style_step_button(next_up_btn, "up");
    style_step_button(next_down_btn, "down");
    for (auto *button : {next_up_btn, next_down_btn}) button->setFixedSize(scaled(BASE_NEXT_BTN_WIDTH), half);
    if (style_popup_) style_popup_->apply_scale();
    apply_label_section_scale();
    // The preview is drawn at the label's pixel size, so it is redrawn after scaling.
    update_next_preview();
}

void NumberSettingsPanel::set_next_number(int value) {
    if (!next_preview) return;
    value = std::max(1, value);
    if (next_value_ == value) return;
    next_value_ = value;
    update_next_preview(value);
}

void NumberSettingsPanel::update_next_preview(std::optional<int> value) {
    if (!next_preview) return;
    next_preview->setPixmap(render_number_style_preview(current_style_, NumberStylePopup::PREVIEW_INK, next_preview->width(), value.value_or(next_value_)));
}

void NumberSettingsPanel::change_next_number(int delta) {
    const int value = std::max(1, next_value_ + delta);
    next_value_ = value;
    update_next_preview(value);
    emit next_number_changed(value);
}

bool NumberSettingsPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == next_preview) {
        if (event->type() == QEvent::Enter) show_style_popup();
        else if (event->type() == QEvent::Leave && style_popup_) style_popup_->close_soon();
    }
    return BaseSettingsPanel::eventFilter(watched, event);
}

NumberStylePopup *NumberSettingsPanel::ensure_style_popup() {
    if (!style_popup_) {
        style_popup_ = new NumberStylePopup(this);
        connect(style_popup_, &NumberStylePopup::style_selected, this, [this](const QString &style) {
            set_style(style);
            emit style_changed(style);
        });
    }
    return style_popup_;
}

void NumberSettingsPanel::show_style_popup() {
    auto *popup = ensure_style_popup();
    popup->set_current_style(current_style_);
    popup->show_beside(this, next_preview);
}

void NumberSettingsPanel::set_style(const QString &style) {
    current_style_ = NumberItem::normalize_style(style);
    if (style_popup_) style_popup_->set_current_style(current_style_);
    update_next_preview();
}
}
