#include "ui/text_settings_panel.h"
#include "canvas/items/text_item.h"
#include "core/constants.h"
#include "core/ui_scale.h"
#include "settings/tool_settings.h"
#include "ui/color_picker_button.h"
#include <QAbstractItemView>
#include <QEvent>
#include <QFrame>
#include <QPainter>
#include <QStyleOptionGraphicsItem>
#include <QStyleOptionSlider>
#include <QTextDocument>

namespace jietuba {
namespace {
// The background, outline and shadow popups share one preset set, so white sits in the same place everywhere.
const std::pair<const char *, const char *> EFFECT_PRESET_COLORS[] = {{"#FFFFFF", "White"}, {"#000000", "Black"}, {"#FFFF00", "Yellow"}};
// Matching TextItem::OUTLINE_WIDTH_LEVELS; no numbers, since levels are font-size ratios.
const char *OUTLINE_WIDTH_TIPS[] = {"Outline: Thin", "Outline: Medium", "Outline: Thick", "Outline: Extra Thick"};
// The icons show the effect and its strength, not its colour, so they use neutral ink.
const QColor EFFECT_ICON_INK("#444444");
constexpr int WIDGET_SIZE_MAX = 16777215;
}

QPixmap render_text_effect_icon(int side, double ratio, std::optional<double> outline_width, bool shadow) {
    const double effective_ratio = std::max(1.0, ratio);
    const int pixels = std::max(1, static_cast<int>(std::nearbyint(side * effective_ratio)));
    QPixmap pixmap(pixels, pixels);
    pixmap.fill(Qt::transparent);
    QFont font("Arial", 48);
    font.setBold(true);
    const QColor ink = outline_width ? QColor("white") : EFFECT_ICON_INK;
    TextItem item("A", QPointF(0, 0), font, ink);
    item.setTextInteractionFlags(Qt::NoTextInteraction);
    item.document()->setDocumentMargin(0);
    if (outline_width) item.set_outline(true, EFFECT_ICON_INK, *outline_width);
    item.set_shadow(shadow);
    const auto rect = item.boundingRect();
    const double scale = pixels / std::max(rect.width(), rect.height());
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(pixels / 2.0, pixels / 2.0);
    painter.scale(scale, scale);
    painter.translate(-rect.center());
    const QStyleOptionGraphicsItem option;
    item.paint(&painter, &option, nullptr);
    painter.end();
    pixmap.setDevicePixelRatio(effective_ratio);
    return pixmap;
}

void FourStepSlider::paintEvent(QPaintEvent *event) {
    QSlider::paintEvent(event);
    QStyleOptionSlider option;
    initStyleOption(&option);
    const auto groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
    const auto handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
    const int span = groove.width() - handle.width();
    if (span <= 0) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#9da5ad"));
    for (int v = minimum(); v <= maximum(); ++v) {
        if (v == value()) continue;
        const int offset = QStyle::sliderPositionFromValue(minimum(), maximum(), v, span, option.upsideDown);
        painter.drawEllipse(QPointF(groove.x() + offset + handle.width() / 2.0, groove.center().y()), 1.75, 1.75);
    }
}

QString TextSettingsPanel::popup_controls_qss() {
    // Four outline steps as a ticked discrete slider: more compact than four buttons,
    // and not pretending to be continuous. Scaled sizes; reapplied after scale changes.
    const auto accent = accent_color().name();
    const QString groove = "rgba(15, 23, 42, 0.12)";
    return QString(R"(
        QSlider {
            background-color: transparent;
        }
        QSlider#outlineWidthSlider {
            min-height: %1px;
        }
        QSlider::groove:horizontal {
            height: %2px;
            margin: 0 %3px;
            background: %4;
            border-radius: %5px;
        }
        QSlider::sub-page:horizontal {
            margin: 0 %3px;
            background: %6;
            border-radius: %5px;
        }
        QSlider::add-page:horizontal {
            margin: 0 %3px;
            background: %4;
            border-radius: %5px;
        }
        QSlider::handle:horizontal {
            width: %7px;
            height: %7px;
            margin: %8px %9px;
            background: white;
            border: 1px solid rgba(0, 0, 0, 0.25);
            border-radius: %10px;
        }
        QSlider::handle:horizontal:hover {
            border-color: %6;
        }
        QFrame#popupDivider {
            background: rgba(%11, %12, %13, %14);
            border: none;
            min-height: 1px;
            max-height: 1px;
        }
        QLabel {
            color: %15;
            font-family: %16;
            font-size: %17px;
        }
    )").arg(scaled(24)).arg(scaled(4)).arg(scaled(6)).arg(groove).arg(scaled(2)).arg(accent).arg(scaled(12)).arg(-scaled(5)).arg(-scaled(6)).arg(scaled(7))
        .arg(DIVIDER_COLOR.red()).arg(DIVIDER_COLOR.green()).arg(DIVIDER_COLOR.blue()).arg(DIVIDER_COLOR.alpha()).arg(CONTROL_INK.name(), CSS_FONT_FAMILY).arg(scaled(11));
}

TextSettingsPanel::TextSettingsPanel(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    cached_fonts_ = get_available_text_fonts();
    current_font = QFont(get_default_text_font_for_language(), 16);
    outline_color = QColor(TextItem::DEFAULT_OUTLINE_COLOR);
    outline_width = TextItem::DEFAULT_OUTLINE_WIDTH;
    shadow_color = QColor(TextItem::DEFAULT_SHADOW_COLOR);
    init_ui();
    connect_signals();
}

void TextSettingsPanel::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

QString TextSettingsPanel::build_stylesheet() const {
    return build_settings_panel_stylesheet(true, "2px 8px 2px 4px", 112, 140) + QString(R"(
            QCheckBox {
                spacing: %1px;
                background-color: transparent;
                border: none;
                font-family: %2;
                font-size: %3px;
                color: #333;
            }
        )").arg(scaled(5)).arg(CSS_FONT_FAMILY).arg(scaled(12));
}

void TextSettingsPanel::apply_scale() {
    // Font, colour and toggles stay; only sizes change.
    setStyleSheet(build_stylesheet());
    const int mh = scaled(BASE_MARGIN_H), mv = scaled(BASE_MARGIN_V);
    layout()->setContentsMargins(mh, mv, mh, mv);
    layout()->setSpacing(scaled(BASE_SPACING));
    font_combo->view()->setMinimumWidth(scaled(BASE_FONT_VIEW_WIDTH));
    size_spin->setFixedWidth(scaled(BASE_SIZE_SPIN_WIDTH));
    const int btn_sz = scaled(BASE_BTN);
    for (const auto &[button, decoration] : font_style_buttons_) {
        button->setFixedSize(btn_sz, btn_sz);
        button->setStyleSheet(QString(R"(
                QPushButton {
                    %1
                    font-size: %2px;
                    font-family: Arial, sans-serif;
                }
            )").arg(decoration).arg(scaled(14)));
    }
    const int icon_sz = scaled(BASE_EFFECT_ICON);
    const double ratio = devicePixelRatioF();
    outline_btn->setIcon(QIcon(render_text_effect_icon(icon_sz, ratio, TextItem::DEFAULT_OUTLINE_WIDTH)));
    shadow_btn->setIcon(QIcon(render_text_effect_icon(icon_sz, ratio, std::nullopt, true)));
    for (auto *button : {background_btn, outline_btn, shadow_btn}) {
        button->setFixedSize(btn_sz, btn_sz);
        button->setIconSize(QSize(icon_sz, icon_sz));
    }
    update_background_btn_style();
    color_row_->apply_scale();
    apply_popup_scale();
    adjustSize();
    update();
}

void TextSettingsPanel::apply_popup_scale() {
    const auto qss = popup_controls_qss();
    for (auto *popup_layout : popup_layouts_) {
        popup_layout->setContentsMargins(scaled(10), scaled(8), scaled(10), scaled(8));
        popup_layout->setSpacing(scaled(6));
    }
    for (auto *row : popup_color_rows_) row->apply_scale();
    for (auto *label : opacity_labels_) label->setFixedWidth(scaled(BASE_OPACITY_LABEL_WIDTH));
    const int icon_side = scaled(BASE_EFFECT_ICON);
    const double ratio = devicePixelRatioF();
    const double levels[] = {TextItem::OUTLINE_WIDTH_LEVELS.front(), TextItem::OUTLINE_WIDTH_LEVELS.back()};
    for (int i = 0; i < outline_width_previews_.size() && i < 2; ++i) {
        outline_width_previews_[i]->setFixedSize(icon_side, icon_side);
        outline_width_previews_[i]->setPixmap(render_text_effect_icon(icon_side, ratio, levels[i]));
    }
    outline_row_->setSpacing(scaled(5));
    outline_width_slider->setFixedWidth(scaled(BASE_OUTLINE_SLIDER_WIDTH));
    for (auto *popup : effect_popups_) {
        popup->set_extra_stylesheet(qss);
        // Release the previous cap first, or the old width is measured again.
        popup->setMaximumWidth(WIDGET_SIZE_MAX);
        popup->adjustSize();
        fit_effect_popup_width(popup);
    }
}

void TextSettingsPanel::init_ui() {
    auto *layout = new QHBoxLayout(this);
    // A short whitelist instead of scanning the system font database.
    font_combo = new PillComboBox;
    font_combo->addItems(cached_fonts_);
    const auto default_font_family = get_default_text_font_for_language();
    if (cached_fonts_.contains(default_font_family)) font_combo->setCurrentText(default_font_family);
    else if (!cached_fonts_.isEmpty()) font_combo->setCurrentIndex(0);
    font_combo->setToolTip(tr_text("Font"));
    font_combo->setMaxVisibleItems(15);
    font_combo->view()->setMinimumWidth(scaled(BASE_FONT_VIEW_WIDTH));
    layout->addWidget(font_combo);
    size_spin = new StepperWidget(16, 8, 144);
    size_spin->setToolTip(tr_text("Font Size"));
    layout->addWidget(size_spin);
    // Weight, italic and underline come from CSS, independent of font variants.
    bold_btn = new QPushButton("B");
    bold_btn->setCheckable(true);
    bold_btn->setToolTip(tr_text("Bold"));
    italic_btn = new QPushButton("I");
    italic_btn->setCheckable(true);
    italic_btn->setToolTip(tr_text("Italic"));
    underline_btn = new QPushButton("U");
    underline_btn->setCheckable(true);
    underline_btn->setToolTip(tr_text("Underline"));
    font_style_buttons_ = {{bold_btn, "font-weight: bold;"}, {italic_btn, "font-style: italic;"}, {underline_btn, "text-decoration: underline;"}};
    layout->addWidget(bold_btn);
    layout->addWidget(italic_btn);
    layout->addWidget(underline_btn);
    // Background, outline and shadow: toggle plus a hover popup once enabled.
    background_btn = new QPushButton("BG");
    outline_btn = new QPushButton;
    shadow_btn = new QPushButton;
    const std::pair<QPushButton *, QString> toggles[] = {{background_btn, tr_text("Text Background")}, {outline_btn, tr_text("Text Outline")}, {shadow_btn, tr_text("Text Shadow")}};
    for (const auto &[button, tip] : toggles) {
        button->setCheckable(true);
        button->setToolTip(tip);
        layout->addWidget(button);
    }
    layout->addWidget(make_separator());
    color_btn = new ColorPickerButton(current_color, true, 28, true, true);
    color_btn->setToolTip(tr_text("Custom Color"));
    color_row_ = new ColorRow(color_btn, PRESET_COLORS, [this](const QString &hex) {
        const QColor color(hex);
        current_color = color;
        color_btn->set_color(color);
        emit color_changed(color);
    });
    layout->addWidget(color_row_);
    layout->addStretch();
    init_effect_popups();
    apply_scale();
}

void TextSettingsPanel::init_effect_popups() {
    // Each popup: colours on the first row, its own amount on the second.
    auto [background_popup, background_custom] = build_effect_popup(background_btn, tr_text("Custom Background Color"), [this](const QColor &color) {
        background_color = color;
        background_color_btn->set_color(background_color);
        update_background_btn_style();
        emit_background_changed();
    });
    background_color_btn = background_custom;
    std::tie(background_opacity_slider, background_opacity_label) = add_opacity_row(background_popup, tr_text("Background Opacity"), [this](int value) {
        background_opacity = value;
        refresh_opacity_label(background_opacity_label, value);
        update_background_btn_style();
        emit_background_changed();
    });
    set_background_settings(background_enabled, background_color, background_opacity);
    fit_effect_popup_width(background_popup);

    auto [outline_popup, outline_custom] = build_effect_popup(outline_btn, tr_text("Custom Outline Color"), [this](const QColor &color) {
        outline_color = color;
        outline_color_btn->set_color(outline_color);
        emit_outline_changed();
    });
    outline_color_btn = outline_custom;
    outline_width_slider = add_outline_width_slider(outline_popup);
    set_outline_settings(outline_enabled, outline_color, outline_width);
    fit_effect_popup_width(outline_popup);

    auto [shadow_popup, shadow_custom] = build_effect_popup(shadow_btn, tr_text("Custom Shadow Color"), [this](const QColor &color) {
        // A new colour keeps the opacity; the slider owns it.
        QColor picked(color);
        picked.setAlpha(shadow_color.alpha());
        shadow_color = picked;
        shadow_color_btn->set_color(picked);
        emit_shadow_changed();
    });
    shadow_color_btn = shadow_custom;
    std::tie(shadow_opacity_slider, shadow_opacity_label) = add_opacity_row(shadow_popup, tr_text("Shadow Opacity"), [this](int value) {
        shadow_color.setAlpha(value);
        refresh_opacity_label(shadow_opacity_label, value);
        emit_shadow_changed();
    });
    set_shadow_settings(shadow_enabled, shadow_color);
    fit_effect_popup_width(shadow_popup);
}

void TextSettingsPanel::fit_effect_popup_width(HoverPopup *popup) { popup->setMaximumWidth(popup->sizeHint().width()); }

std::pair<HoverPopup *, ColorPickerButton *> TextSettingsPanel::build_effect_popup(QPushButton *toggle, const QString &custom_tip, std::function<void(const QColor &)> on_color) {
    auto *popup = new HoverPopup(this);
    popup->set_extra_stylesheet(popup_controls_qss());
    auto *popup_layout = new QVBoxLayout(popup);
    // Colour and opacity are set separately, so the custom colour has no alpha channel.
    auto *custom = new ColorPickerButton(QColor("white"), false, 28, true, true);
    custom->setToolTip(custom_tip);
    connect(custom, &ColorPickerButton::color_changed, this, [on_color](const QColor &color) { on_color(color); });
    QStringList colors;
    for (const auto &entry : EFFECT_PRESET_COLORS) colors.append(entry.first);
    auto *row = new ColorRow(custom, colors, [on_color](const QString &hex) { on_color(QColor(hex)); });
    for (int i = 0; i < row->swatches.size(); ++i) row->swatches[i].first->setToolTip(tr_text(EFFECT_PRESET_COLORS[i].second));
    popup_color_rows_.append(row);
    popup_layouts_.append(popup_layout);
    auto *colors_layout = new QHBoxLayout;
    colors_layout->addStretch();
    colors_layout->addWidget(row);
    colors_layout->addStretch();
    popup_layout->addLayout(colors_layout);
    auto *divider = new QFrame;
    divider->setObjectName("popupDivider");
    divider->setFrameShape(QFrame::HLine);
    divider->setFixedHeight(1);
    popup_layout->addWidget(divider);
    toggle->installEventFilter(this);
    connect(toggle, &QPushButton::toggled, this, [this, toggle](bool checked) { sync_effect_popup(toggle, checked); });
    effect_popups_[toggle] = popup;
    popup->hide();
    return {popup, custom};
}

std::pair<QSlider *, QLabel *> TextSettingsPanel::add_opacity_row(HoverPopup *popup, const QString &tip, std::function<void(int)> on_change) {
    auto *slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, 255);
    slider->setToolTip(tip);
    auto *label = new QLabel;
    opacity_labels_.append(label);
    auto *row = new QHBoxLayout;
    row->addWidget(slider);
    row->addWidget(label);
    static_cast<QVBoxLayout *>(popup->layout())->addLayout(row);
    connect(slider, &QSlider::valueChanged, this, [on_change](int value) { on_change(value); });
    return {slider, label};
}

FourStepSlider *TextSettingsPanel::add_outline_width_slider(HoverPopup *popup) {
    // End icons show the thin-to-thick direction.
    const int icon_side = scaled(BASE_EFFECT_ICON);
    const double ratio = devicePixelRatioF();
    outline_row_ = new QHBoxLayout;
    const std::pair<double, const char *> ends[] = {{TextItem::OUTLINE_WIDTH_LEVELS.front(), OUTLINE_WIDTH_TIPS[0]}, {TextItem::OUTLINE_WIDTH_LEVELS.back(), OUTLINE_WIDTH_TIPS[3]}};
    for (const auto &[level, tip] : ends) {
        auto *preview = new QLabel;
        preview->setFixedSize(icon_side, icon_side);
        preview->setAlignment(Qt::AlignCenter);
        preview->setPixmap(render_text_effect_icon(icon_side, ratio, level));
        preview->setToolTip(tr_text(tip));
        outline_width_previews_.append(preview);
    }
    auto *slider = new FourStepSlider(Qt::Horizontal);
    slider->setObjectName("outlineWidthSlider");
    slider->setRange(0, static_cast<int>(TextItem::OUTLINE_WIDTH_LEVELS.size()) - 1);
    slider->setSingleStep(1);
    slider->setPageStep(1);
    slider->setTickInterval(1);
    slider->setTickPosition(QSlider::TicksBelow);
    slider->setTracking(false);
    slider->setFocusPolicy(Qt::NoFocus);
    outline_row_->addWidget(outline_width_previews_[0]);
    outline_row_->addWidget(slider);
    outline_row_->addWidget(outline_width_previews_[1]);
    static_cast<QVBoxLayout *>(popup->layout())->addLayout(outline_row_);
    connect(slider, &QSlider::valueChanged, this, &TextSettingsPanel::on_outline_width_changed);
    return slider;
}

void TextSettingsPanel::connect_signals() {
    connect(font_combo, &QComboBox::currentTextChanged, this, [this](const QString &) { on_font_changed(); });
    connect(size_spin, &StepperWidget::valueChanged, this, [this](int) { on_font_changed(); });
    for (auto *button : {bold_btn, italic_btn, underline_btn}) connect(button, &QPushButton::toggled, this, [this](bool) { on_font_changed(); });
    connect(color_btn, &ColorPickerButton::color_changed, this, [this](const QColor &color) {
        current_color = color;
        emit color_changed(color);
    });
    connect(background_btn, &QPushButton::toggled, this, [this](bool checked) {
        background_enabled = checked;
        update_background_btn_style();
        emit_background_changed();
    });
    connect(outline_btn, &QPushButton::toggled, this, [this](bool checked) {
        outline_enabled = checked;
        emit_outline_changed();
    });
    connect(shadow_btn, &QPushButton::toggled, this, [this](bool checked) {
        shadow_enabled = checked;
        emit_shadow_changed();
    });
}

void TextSettingsPanel::sync_effect_popup(QPushButton *toggle, bool checked) {
    // Just enabled with the pointer still on it: open the popup in place; disabled: close it.
    auto *popup = effect_popups_[toggle];
    if (!checked) popup->hide();
    else if (toggle->underMouse()) popup->show_beside(this, toggle);
}

bool TextSettingsPanel::eventFilter(QObject *watched, QEvent *event) {
    if (auto *toggle = qobject_cast<QPushButton *>(watched); toggle && effect_popups_.contains(toggle)) {
        auto *popup = effect_popups_[toggle];
        if (event->type() == QEvent::Enter && toggle->isChecked()) popup->show_beside(this, toggle);
        else if (event->type() == QEvent::Leave) popup->close_soon();
    }
    return QWidget::eventFilter(watched, event);
}

void TextSettingsPanel::refresh_opacity_label(QLabel *label, int alpha) {
    const int percent = std::max(0, std::min(100, static_cast<int>(python_round(alpha / 255.0 * 100))));
    label->setText(QString("%1%").arg(percent));
}

void TextSettingsPanel::update_background_btn_style() {
    // While enabled, the button's fill is the background colour itself.
    const int radius = scaled(8);
    if (background_btn->isChecked()) {
        const QColor color(background_color);
        const int alpha = std::max(0, std::min(255, background_opacity));
        const int luminance = color.red() * 3 + color.green() * 6 + color.blue();
        const QString text_color = luminance >= 1280 ? "#000000" : "#ffffff";
        background_btn->setStyleSheet(QString(R"(
                QPushButton {
                    background-color: rgba(%1, %2, %3, %4);
                    color: %5;
                    border: 1px solid rgba(15, 23, 42, 0.2);
                    border-radius: %6px;
                    font-size: %7px;
                }
            )").arg(color.red()).arg(color.green()).arg(color.blue()).arg(alpha).arg(text_color).arg(radius).arg(scaled(12)));
    } else {
        background_btn->setStyleSheet(QString(R"(
                QPushButton {
                    background-color: transparent;
                    border: none;
                    border-radius: %1px;
                    font-size: %2px;
                }
                QPushButton:hover {
                    background-color: rgba(15, 23, 42, 0.06);
                }
            )").arg(radius).arg(scaled(12)));
    }
}

void TextSettingsPanel::emit_background_changed() {
    QColor color(background_color);
    color.setAlpha(background_opacity);
    emit background_changed(background_enabled, color, background_opacity);
}

void TextSettingsPanel::set_background_settings(bool enabled, const QColor &color, int opacity) {
    background_enabled = enabled;
    background_color = color;
    background_opacity = opacity;
    background_btn->blockSignals(true);
    background_btn->setChecked(background_enabled);
    background_btn->blockSignals(false);
    update_background_btn_style();
    background_color_btn->set_color(background_color);
    background_opacity_slider->blockSignals(true);
    background_opacity_slider->setValue(background_opacity);
    background_opacity_slider->blockSignals(false);
    refresh_opacity_label(background_opacity_label, background_opacity);
}

void TextSettingsPanel::on_outline_width_changed(int index) {
    const double level = TextItem::OUTLINE_WIDTH_LEVELS[static_cast<size_t>(index)];
    update_outline_width_slider_tooltip(index);
    // Returning to the current level is no change: no signal, no settings write.
    if (level == outline_width) return;
    outline_width = level;
    emit_outline_changed();
}

void TextSettingsPanel::update_outline_width_slider_tooltip(std::optional<int> index) {
    outline_width_slider->setToolTip(tr_text(OUTLINE_WIDTH_TIPS[index.value_or(outline_width_slider->value())]));
}

void TextSettingsPanel::emit_outline_changed() { emit outline_changed(outline_enabled, outline_color, outline_width); }

void TextSettingsPanel::set_outline_settings(bool enabled, const QColor &color, const QVariant &width) {
    outline_enabled = enabled;
    outline_color = color;
    // TextItem owns the snapping rule, so the highlighted level matches the drawn width.
    outline_width = TextItem::normalize_outline_width(width);
    outline_btn->blockSignals(true);
    outline_btn->setChecked(outline_enabled);
    outline_btn->blockSignals(false);
    outline_color_btn->set_color(outline_color);
    const auto &levels = TextItem::OUTLINE_WIDTH_LEVELS;
    const int index = static_cast<int>(std::find(levels.begin(), levels.end(), outline_width) - levels.begin());
    outline_width_slider->blockSignals(true);
    outline_width_slider->setValue(index);
    outline_width_slider->blockSignals(false);
    update_outline_width_slider_tooltip(index);
}

void TextSettingsPanel::emit_shadow_changed() { emit shadow_changed(shadow_enabled, shadow_color); }

void TextSettingsPanel::set_shadow_settings(bool enabled, const QColor &color) {
    shadow_enabled = enabled;
    shadow_color = color;
    shadow_btn->blockSignals(true);
    shadow_btn->setChecked(shadow_enabled);
    shadow_btn->blockSignals(false);
    shadow_color_btn->set_color(shadow_color);
    shadow_opacity_slider->blockSignals(true);
    shadow_opacity_slider->setValue(shadow_color.alpha());
    shadow_opacity_slider->blockSignals(false);
    refresh_opacity_label(shadow_opacity_label, shadow_color.alpha());
}

void TextSettingsPanel::on_font_changed() {
    QFont font(font_combo->currentText());
    font.setPointSize(std::max(1, size_spin->value()));
    font.setBold(bold_btn->isChecked());
    font.setItalic(italic_btn->isChecked());
    font.setUnderline(underline_btn->isChecked());
    current_font = font;
    emit font_changed(font);
}

void TextSettingsPanel::set_state_from_item(QGraphicsTextItem *item) {
    if (!item) return;
    // Blocked so the updates do not loop back.
    blockSignals(true);
    const auto font = item->font();
    font_combo->setCurrentText(font.family());
    int point_size = font.pointSize();
    if (point_size <= 0) point_size = font.pointSizeF() > 0 ? static_cast<int>(python_round(font.pointSizeF())) : 16;
    size_spin->setValue(std::max(1, point_size));
    bold_btn->setChecked(font.bold());
    italic_btn->setChecked(font.italic());
    underline_btn->setChecked(font.underline());
    if (auto *text = dynamic_cast<TextItem *>(item)) {
        // The item keeps colour and opacity in one QColor; the panel shows them apart.
        QColor color(text->background_color);
        const int opacity = color.alpha();
        color.setAlpha(255);
        set_background_settings(text->has_background, color, opacity);
        const auto [outline_on, outline_c, outline_w] = text->outline_state();
        set_outline_settings(outline_on, outline_c, outline_w);
        const auto [shadow_on, shadow_c] = text->shadow_state();
        set_shadow_settings(shadow_on, shadow_c);
    }
    current_color = item->defaultTextColor();
    color_btn->set_color(current_color);
    blockSignals(false);
}

void TextSettingsPanel::retranslate() {
    font_combo->setToolTip(tr_text("Font"));
    size_spin->setToolTip(tr_text("Font Size"));
    bold_btn->setToolTip(tr_text("Bold"));
    italic_btn->setToolTip(tr_text("Italic"));
    underline_btn->setToolTip(tr_text("Underline"));
    background_btn->setToolTip(tr_text("Text Background"));
    outline_btn->setToolTip(tr_text("Text Outline"));
    shadow_btn->setToolTip(tr_text("Text Shadow"));
    color_btn->setToolTip(tr_text("Custom Color"));
    background_color_btn->setToolTip(tr_text("Custom Background Color"));
    background_opacity_slider->setToolTip(tr_text("Background Opacity"));
    outline_color_btn->setToolTip(tr_text("Custom Outline Color"));
    update_outline_width_slider_tooltip();
    outline_width_previews_[0]->setToolTip(tr_text(OUTLINE_WIDTH_TIPS[0]));
    outline_width_previews_[1]->setToolTip(tr_text(OUTLINE_WIDTH_TIPS[3]));
    shadow_color_btn->setToolTip(tr_text("Custom Shadow Color"));
    shadow_opacity_slider->setToolTip(tr_text("Shadow Opacity"));
}

void TextSettingsPanel::load_from_config() {
    auto *manager = get_tool_settings_manager();
    auto *text_settings = manager->get_tool_settings("text");
    if (!text_settings) return;
    QFont font(normalize_text_font_family(text_settings->get("font_family", "").toString()));
    font.setPointSize(text_settings->get("font_size", 16).toInt());
    font.setBold(text_settings->get("font_bold", false).toBool());
    font.setItalic(text_settings->get("font_italic", false).toBool());
    font.setUnderline(text_settings->get("font_underline", false).toBool());
    const QColor color(text_settings->get("color", "#FF0000").toString());
    font_combo->setCurrentText(font.family());
    int point_size = font.pointSize();
    if (point_size <= 0) point_size = font.pointSizeF() > 0 ? static_cast<int>(python_round(font.pointSizeF())) : 16;
    size_spin->setValue(std::max(1, point_size));
    bold_btn->setChecked(font.bold());
    italic_btn->setChecked(font.italic());
    underline_btn->setChecked(font.underline());
    current_color = color;
    color_btn->set_color(color);
    set_background_settings(text_settings->get("background_enabled", false).toBool(), QColor(text_settings->get("background_color", "#FFFFFF").toString()),
                            text_settings->get("background_opacity", 255).toInt());
    set_outline_settings(text_settings->get("outline_enabled").toBool(), QColor(text_settings->get("outline_color").toString()), text_settings->get("outline_width"));
    set_shadow_settings(text_settings->get("shadow_enabled").toBool(), QColor(text_settings->get("shadow_color").toString()));
}

void TextSettingsPanel::save_font_to_config(const QFont &font) {
    get_tool_settings_manager()->update_settings("text", {{"font_family", font.family()}, {"font_size", font.pointSize()}, {"font_bold", font.bold()},
                                                          {"font_italic", font.italic()}, {"font_underline", font.underline()}});
}

void TextSettingsPanel::save_background_to_config(bool enabled, const QColor &color, int opacity) {
    get_tool_settings_manager()->update_settings("text", {{"background_enabled", enabled}, {"background_color", color.name()}, {"background_opacity", opacity}});
}

void TextSettingsPanel::save_outline_to_config(bool enabled, const QColor &color, double width) {
    get_tool_settings_manager()->update_settings("text", {{"outline_enabled", enabled}, {"outline_color", color.name()}, {"outline_width", TextItem::normalize_outline_width(width)}});
}

void TextSettingsPanel::save_shadow_to_config(bool enabled, const QColor &color) {
    get_tool_settings_manager()->update_settings("text", {{"shadow_enabled", enabled}, {"shadow_color", color.name(QColor::HexArgb)}});
}
}
