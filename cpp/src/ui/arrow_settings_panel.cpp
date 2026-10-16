#include "ui/arrow_settings_panel.h"
#include "canvas/items/arrow_item.h"
#include "core/i18n.h"
#include "core/ui_scale.h"
#include "tools/base.h"
#include "ui/color_picker_button.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QHash>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

namespace jietuba {
namespace {
QString arrow_tr(const char *text) { return i18n_tr(text, "ArrowSettingsPanel"); }

// Icons only show the shape; the name lives in the tooltip.
const char *arrow_style_name(const QString &style) {
    static const QHash<QString, const char *> names = {
        {ArrowItem::STYLE_SINGLE, "Solid Arrow"}, {ArrowItem::STYLE_DOUBLE, "Solid Double Arrow"},
        {ArrowItem::STYLE_HOLLOW, "Outlined Arrow"}, {ArrowItem::STYLE_LINE, "Line Arrow"},
        {ArrowItem::STYLE_LINE_DOUBLE, "Line Double Arrow"}, {ArrowItem::STYLE_TRIANGLE, "Thin Arrow"},
        {ArrowItem::STYLE_TRIANGLE_DOUBLE, "Thin Double Arrow"}, {ArrowItem::STYLE_BAR, "Dimension Line"},
        {ArrowItem::STYLE_BAR_ARROW, "Dimension Arrow"},
    };
    return names.value(style, nullptr);
}

const QSize BASE_PREVIEW_SIZE(80, 22);
const QSize BASE_PREVIEW_ROW_SIZE(96, 26);
// Every part of the arrow scales with the stroke width, so this sizes the whole preview.
constexpr int BASE_PREVIEW_STROKE_WIDTH = 3;
// Neutral ink: a style picker, not a colour picker; light colours would vanish on white.
const QColor PREVIEW_INK("#444444");
const QColor PREVIEW_INK_SELECTED = PREVIEW_INK;
}

QSize preview_size() { return QSize(scaled(BASE_PREVIEW_SIZE.width()), scaled(BASE_PREVIEW_SIZE.height())); }
QSize preview_row_size() { return QSize(scaled(BASE_PREVIEW_ROW_SIZE.width()), scaled(BASE_PREVIEW_ROW_SIZE.height())); }

QPixmap render_arrow_style_preview(const QString &style, const QColor &ink, const QSize &size, double ratio) {
    // More pixels plus a DPR keep enlarged previews sharp; QPainter applies the DPR,
    // so everything below is in logical coordinates.
    QPixmap pixmap(static_cast<int>(std::nearbyint(size.width() * ratio)), static_cast<int>(std::nearbyint(size.height() * ratio)));
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    // Margin so the collapsed box does not show the arrow touching its frame.
    const double padding = scaled_f(6.0);
    const double middle = size.height() / 2.0;
    ArrowItem item(QPointF(padding, middle), QPointF(size.width() - padding, middle), QPen(ink, scaled_f(BASE_PREVIEW_STROKE_WIDTH)), style);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QStyleOptionGraphicsItem option;
    item.paint(&painter, &option, nullptr);
    painter.end();
    return pixmap;
}

QIcon arrow_style_icon(const QString &style, double ratio) {
    static QHash<QString, QIcon> cache;
    const auto size = preview_size();
    const auto key = QString("%1|%2|%3|%4").arg(style).arg(ratio).arg(size.width()).arg(size.height());
    if (const auto found = cache.constFind(key); found != cache.constEnd()) return *found;
    QIcon icon;
    icon.addPixmap(render_arrow_style_preview(style, PREVIEW_INK, size, ratio), QIcon::Normal);
    icon.addPixmap(render_arrow_style_preview(style, PREVIEW_INK_SELECTED, size, ratio), QIcon::Selected);
    cache.insert(key, icon);
    return icon;
}

QSize StylePreviewDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const { return preview_row_size(); }

void StylePreviewDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    // Background and states from the style; only the icon is placed here.
    const QIcon icon(opt.icon);
    opt.icon = QIcon();
    opt.text.clear();
    const auto *widget = opt.widget;
    auto *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
    const auto mode = (opt.state & QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal;
    // At the preview's own size, not stretched to the row.
    QRect target(QPoint(0, 0), preview_size().boundedTo(opt.rect.size()));
    target.moveCenter(opt.rect.center());
    icon.paint(painter, target, Qt::AlignCenter, mode, QIcon::Off);
}

ArrowSettingsPanel::ArrowSettingsPanel(QWidget *parent) : QWidget(parent), current_arrow_style(ArrowItem::STYLE_SINGLE) {
    setAttribute(Qt::WA_TranslucentBackground);
    init_ui();
    connect_signals();
}

void ArrowSettingsPanel::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void ArrowSettingsPanel::apply_scale() {
    setStyleSheet(build_settings_panel_stylesheet(true, "1px", BASE_COMBO_WIDTH, BASE_COMBO_WIDTH, true));
    const int mh = scaled(BASE_MARGIN_H), mv = scaled(BASE_MARGIN_V);
    layout()->setContentsMargins(mh, mv, mh, mv);
    layout()->setSpacing(scaled(BASE_SPACING));
    arrow_style_combo->setIconSize(preview_size());
    arrow_style_combo->view()->setMinimumWidth(preview_row_size().width());
    for (int index = 0; index < arrow_style_combo->count(); ++index) {
        const auto style = arrow_style_combo->itemData(index).toString();
        if (!style.isEmpty()) arrow_style_combo->setItemIcon(index, arrow_style_icon(style));
    }
    size_spin->setFixedWidth(scaled(BASE_SIZE_SPIN_WIDTH));
    opacity_spin->setFixedWidth(scaled(BASE_OPACITY_SPIN_WIDTH));
    color_row_->apply_scale();
    adjustSize();
    update();
}

void ArrowSettingsPanel::init_ui() {
    auto *layout = new QHBoxLayout(this);
    arrow_style_combo = new PillComboBox;
    for (const auto &style : ArrowItem::STYLES) {
        arrow_style_combo->addItem(arrow_style_icon(style), "", style);
        if (const auto *name = arrow_style_name(style)) arrow_style_combo->setItemData(arrow_style_combo->count() - 1, arrow_tr(name), Qt::ToolTipRole);
    }
    arrow_style_combo->setToolTip(arrow_tr("Arrow Style"));
    // All at once: styles are recognised by shape, and scrolling makes comparing them hard.
    arrow_style_combo->setMaxVisibleItems(static_cast<int>(ArrowItem::STYLES.size()));
    arrow_style_combo->setItemDelegate(new StylePreviewDelegate(arrow_style_combo));
    layout->addWidget(arrow_style_combo);
    // The arrow tool's real width range, so wheel changes never leave the panel behind.
    size_spin = new StepperWidget(current_size, static_cast<int>(Tool::MIN_WIDTH), static_cast<int>(Tool::MAX_WIDTH), {}, nullptr, "width");
    size_spin->setToolTip(arrow_tr("Line Width"));
    layout->addWidget(size_spin);
    opacity_spin = new StepperWidget(opacity_to_percent(cached_opacity_), 0, 100, "%", nullptr, "opacity");
    opacity_spin->setToolTip(arrow_tr("Opacity (%)"));
    layout->addWidget(opacity_spin);
    layout->addWidget(make_separator());
    color_btn = new ColorPickerButton(current_color, true, 28, true, true);
    color_btn->setToolTip(arrow_tr("Custom Color"));
    color_row_ = new ColorRow(color_btn, PRESET_COLORS, [this](const QString &hex) { on_preset_color_clicked(hex); });
    layout->addWidget(color_row_);
    layout->addStretch();
    apply_scale();
}

void ArrowSettingsPanel::connect_signals() {
    connect(arrow_style_combo, &QComboBox::currentIndexChanged, this, [this](int) {
        auto style = arrow_style_combo->currentData().toString();
        if (style.isEmpty()) style = arrow_style_combo->currentText();
        current_arrow_style = style;
        emit arrow_style_changed(style);
    });
    connect(size_spin, &StepperWidget::valueChanged, this, [this](int value) {
        current_size = value;
        emit size_changed(value);
    });
    connect(opacity_spin, &StepperWidget::valueChanged, this, [this](int value) {
        cached_opacity_ = percent_to_opacity(value);
        emit opacity_changed(cached_opacity_);
    });
    connect(color_btn, &ColorPickerButton::color_changed, this, &ArrowSettingsPanel::on_color_picked);
}

void ArrowSettingsPanel::on_color_picked(const QColor &color) {
    current_color = color;
    const int alpha = color.alpha();
    cached_opacity_ = alpha;
    opacity_spin->blockSignals(true);
    opacity_spin->setValue(opacity_to_percent(alpha));
    opacity_spin->blockSignals(false);
    emit color_changed(color);
}

void ArrowSettingsPanel::on_preset_color_clicked(const QString &color_str) {
    const QColor color(color_str);
    current_color = color;
    color_btn->set_color(color);
    emit color_changed(color);
}

int ArrowSettingsPanel::opacity_to_percent(int opacity) { return std::max(0, std::min(100, static_cast<int>(python_round(opacity / 255.0 * 100)))); }
int ArrowSettingsPanel::percent_to_opacity(int percent) { return std::max(0, std::min(255, static_cast<int>(python_round(percent / 100.0 * 255)))); }

void ArrowSettingsPanel::set_color(const QColor &color) {
    current_color = color;
    color_btn->set_color(color);
}

void ArrowSettingsPanel::set_size(int size) {
    current_size = size;
    size_spin->blockSignals(true);
    size_spin->setValue(current_size);
    size_spin->blockSignals(false);
}

void ArrowSettingsPanel::set_opacity(int opacity) {
    cached_opacity_ = opacity;
    opacity_spin->blockSignals(true);
    opacity_spin->setValue(opacity_to_percent(cached_opacity_));
    opacity_spin->blockSignals(false);
}

void ArrowSettingsPanel::set_arrow_style(const QString &value) {
    if (!ArrowItem::STYLES.contains(value)) return;
    current_arrow_style = value;
    const int idx = arrow_style_combo->findData(value);
    if (idx >= 0) {
        arrow_style_combo->blockSignals(true);
        arrow_style_combo->setCurrentIndex(idx);
        arrow_style_combo->blockSignals(false);
    }
}
}
