#include "ui/mosaic_settings_panel.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "tools/base.h"
#include <QPainter>
#include <QPolygonF>

namespace jietuba {
namespace {
// Shares the other tool panels' translation context.
QString mosaic_tr(const char *text) { return i18n_tr(text, "ArrowSettingsPanel"); }

const QColor SWATCH_DARK("#7a7a7a");
const QColor SWATCH_LIGHT("#e8e8e8");
// Cells per icon, one per MosaicTool::BLOCK_SIZE_LEVELS entry. A relative scale:
// dividing by the real pixel size would leave the strongest level one flat cell,
// where mosaic and blur look identical.
constexpr int SWATCH_GRIDS[] = {8, 5, 3, 2};
// Describes how strong the effect looks rather than pixel counts, and fits both kinds.
const char *BLOCK_SIZE_TIPS[] = {"Strength: Subtle", "Strength: Medium", "Strength: Strong", "Strength: Maximum"};
// Icon buttons like the mode buttons; the label is the tooltip. Mosaic shares the toolbar's mosaic tool icon.
struct KindEntry {
    const QString &value;
    const char *label;
    const char *icon;
};
const KindEntry KINDS[] = {{MosaicTool::STYLE_PIXELATE, "Mosaic", "马赛克.svg"},
                           {MosaicTool::STYLE_BLUR, "Blur", "模糊.svg"},
                           {MosaicTool::STYLE_SMART_ERASE, "Smart Erase", "智能擦除.svg"}};

// Needs a clear diagonal: cells turn it into steps and blur softens it, so both
// kinds stay distinguishable. A checkerboard would blur into uniform grey.
QImage swatch_source(int side) {
    QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
    image.fill(SWATCH_LIGHT);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(SWATCH_DARK);
    painter.drawPolygon(QPolygonF({QPointF(0.0, side), QPointF(side, 0.0), QPointF(side, side)}));
    painter.end();
    return image;
}

QIcon cached_icon(const QString &svg_name) { return ResourceManager::get_icon(ResourceManager::get_icon_path(svg_name)); }
}

QPixmap render_block_size_swatch(int block_size, int side, bool smooth, double ratio) {
    const double effective_ratio = std::max(1.0, ratio);
    const int pixels = std::max(1, static_cast<int>(python_round(side * effective_ratio)));
    const auto source = swatch_source(pixels);
    const auto &levels = MosaicTool::BLOCK_SIZE_LEVELS;
    const auto index = std::find(levels.begin(), levels.end(), MosaicTool::clamp_block_size(block_size)) - levels.begin();
    const int grid = SWATCH_GRIDS[index];
    const auto reduced = source.scaled(grid, grid, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    auto pixmap = QPixmap::fromImage(reduced.scaled(pixels, pixels, Qt::IgnoreAspectRatio, smooth ? Qt::SmoothTransformation : Qt::FastTransformation));
    pixmap.setDevicePixelRatio(effective_ratio);
    return pixmap;
}

MosaicSettingsPanel::MosaicSettingsPanel(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    init_ui();
    connect_signals();
}

void MosaicSettingsPanel::paintEvent(QPaintEvent *) { paint_rounded_panel(this); }

void MosaicSettingsPanel::apply_scale() {
    // Mode, strength and brush size stay; only sizes change.
    setStyleSheet(build_settings_panel_stylesheet());
    const int mh = scaled(BASE_MARGIN_H), mv = scaled(BASE_MARGIN_V);
    layout()->setContentsMargins(mh, mv, mh, mv);
    layout()->setSpacing(scaled(BASE_SPACING));
    const int btn_sz = scaled(BASE_BTN);
    const int icon_sz = scaled(BASE_ICON);
    QList<QPushButton *> icon_buttons{freehand_btn, rect_btn};
    icon_buttons += kind_buttons.values();
    for (auto *button : icon_buttons) {
        button->setFixedSize(btn_sz, btn_sz);
        button->setIconSize(QSize(icon_sz, icon_sz));
    }
    mode_widget->layout()->setSpacing(scaled(2));
    size_spin->setFixedWidth(scaled(BASE_SIZE_SPIN_WIDTH));
    kind_layout_->setSpacing(scaled(2));
    // The swatch fills the button, so selection is an accent outline rather than a fill.
    // Disabled (smart erase) shows no level as chosen.
    const auto level_button_qss = QString("QPushButton { padding: 0px; border: 1px solid transparent; border-radius: %1px; }"
                                          "QPushButton:hover { border-color: rgba(15, 23, 42, 0.25); }"
                                          "QPushButton:checked { border: 2px solid %2; }"
                                          "QPushButton:checked:disabled { border: 1px solid transparent; background-color: transparent; }")
                                      .arg(scaled(6))
                                      .arg(accent_color().name());
    for (auto *button : block_size_buttons) {
        button->setFixedSize(btn_sz, btn_sz);
        button->setIconSize(QSize(icon_sz, icon_sz));
        button->setStyleSheet(level_button_qss);
    }
    block_size_layout_->setSpacing(scaled(2));
    // Swatches are drawn at this pixel size, so they are redrawn after scaling.
    swatch_side_ = icon_sz;
    refresh_block_size_icons();
    adjustSize();
    update();
}

void MosaicSettingsPanel::init_ui() {
    auto *layout = new QHBoxLayout(this);
    mode_widget = new QWidget;
    auto *mode_layout = new QHBoxLayout(mode_widget);
    mode_layout->setContentsMargins(0, 0, 0, 0);
    freehand_btn = new QPushButton;
    freehand_btn->setCheckable(true);
    freehand_btn->setToolTip(mosaic_tr("Freehand Mosaic"));
    freehand_btn->setIcon(cached_icon(QString::fromUtf8("画笔.svg")));
    freehand_btn->setStyleSheet("QPushButton { padding: 0px; }");
    rect_btn = new QPushButton;
    rect_btn->setCheckable(true);
    rect_btn->setToolTip(mosaic_tr("Rect Mosaic"));
    rect_btn->setIcon(cached_icon(QString::fromUtf8("方框.svg")));
    rect_btn->setStyleSheet("QPushButton { padding: 0px; }");
    mode_group = new QButtonGroup(this);
    mode_group->setExclusive(true);
    mode_group->addButton(freehand_btn);
    mode_group->addButton(rect_btn);
    mode_layout->addWidget(freehand_btn);
    mode_layout->addWidget(rect_btn);
    layout->addWidget(mode_widget);

    size_spin = new StepperWidget(current_size, static_cast<int>(Tool::MIN_WIDTH), static_cast<int>(Tool::MAX_WIDTH), {}, nullptr, "width");
    size_spin->setToolTip(mosaic_tr("Brush Size"));
    layout->addWidget(size_spin);
    layout->addWidget(make_separator());

    kind_group = new QButtonGroup(this);
    kind_group->setExclusive(true);
    auto *kind_widget = new QWidget;
    kind_layout_ = new QHBoxLayout(kind_widget);
    kind_layout_->setContentsMargins(0, 0, 0, 0);
    for (const auto &kind : KINDS) {
        auto *button = new QPushButton;
        button->setCheckable(true);
        button->setIcon(cached_icon(QString::fromUtf8(kind.icon)));
        button->setStyleSheet("QPushButton { padding: 0px; }");
        kind_group->addButton(button);
        kind_buttons[kind.value] = button;
        kind_layout_->addWidget(button);
    }
    retranslate_kinds();
    layout->addWidget(kind_widget);
    layout->addWidget(make_separator());

    // Four discrete levels: one click is one change, so downstream recomputation of the
    // whole reduced background (about 28 ms per level at 4K) never runs per drag step.
    block_size_group = new QButtonGroup(this);
    block_size_group->setExclusive(true);
    auto *block_size_widget = new QWidget;
    block_size_layout_ = new QHBoxLayout(block_size_widget);
    block_size_layout_->setContentsMargins(0, 0, 0, 0);
    swatch_side_ = scaled(BASE_ICON);
    for (size_t index = 0; index < MosaicTool::BLOCK_SIZE_LEVELS.size(); ++index) {
        auto *button = new QPushButton;
        button->setCheckable(true);
        button->setToolTip(mosaic_tr(BLOCK_SIZE_TIPS[index]));
        block_size_group->addButton(button);
        block_size_buttons[MosaicTool::BLOCK_SIZE_LEVELS[index]] = button;
        block_size_layout_->addWidget(button);
    }
    layout->addWidget(block_size_widget);

    apply_scale();
    set_draw_mode(current_draw_mode);
    set_style(current_style);
    set_block_size(current_block_size);
}

void MosaicSettingsPanel::connect_signals() {
    connect(mode_group, &QButtonGroup::buttonClicked, this, [this](QAbstractButton *) {
        const auto mode = rect_btn->isChecked() ? MODE_RECT_VALUE : MODE_FREEHAND_VALUE;
        current_draw_mode = mode;
        emit draw_mode_changed(mode);
    });
    connect(size_spin, &StepperWidget::valueChanged, this, [this](int value) {
        current_size = value;
        emit size_changed(value);
    });
    connect(kind_group, &QButtonGroup::buttonClicked, this, [this](QAbstractButton *button) {
        const auto style = kind_buttons.key(static_cast<QPushButton *>(button), STYLE_PIXELATE_VALUE);
        if (style == current_style) return;
        current_style = style;
        refresh_block_size_icons();
        emit style_changed(style);
    });
    connect(block_size_group, &QButtonGroup::buttonClicked, this, &MosaicSettingsPanel::on_block_size_clicked);
}

void MosaicSettingsPanel::on_block_size_clicked(QAbstractButton *button) {
    const int level = block_size_buttons.key(static_cast<QPushButton *>(button));
    // Clicking the current level again is no change; downstream would recompute a full-screen reduction.
    if (level == current_block_size) return;
    current_block_size = level;
    emit block_size_changed(level);
}

void MosaicSettingsPanel::refresh_block_size_icons() {
    const bool smooth = current_style == STYLE_BLUR_VALUE;
    const bool has_strength = current_style != STYLE_SMART_ERASE_VALUE;
    const double ratio = devicePixelRatioF();
    for (auto it = block_size_buttons.begin(); it != block_size_buttons.end(); ++it) {
        it.value()->setIcon(QIcon(render_block_size_swatch(it.key(), swatch_side_, smooth, ratio)));
        it.value()->setEnabled(has_strength);
    }
}

void MosaicSettingsPanel::retranslate_kinds() {
    for (const auto &kind : KINDS) kind_buttons[kind.value]->setToolTip(mosaic_tr(kind.label));
}

void MosaicSettingsPanel::set_size(int size) {
    current_size = size;
    size_spin->blockSignals(true);
    size_spin->setValue(current_size);
    size_spin->blockSignals(false);
}

void MosaicSettingsPanel::set_draw_mode(const QString &requested) {
    const auto mode = (requested == MODE_FREEHAND_VALUE || requested == MODE_RECT_VALUE) ? requested : MODE_FREEHAND_VALUE;
    current_draw_mode = mode;
    freehand_btn->blockSignals(true);
    rect_btn->blockSignals(true);
    freehand_btn->setChecked(mode == MODE_FREEHAND_VALUE);
    rect_btn->setChecked(mode == MODE_RECT_VALUE);
    freehand_btn->blockSignals(false);
    rect_btn->blockSignals(false);
}

void MosaicSettingsPanel::set_style(const QString &requested) {
    const auto style = kind_buttons.contains(requested) ? requested : STYLE_PIXELATE_VALUE;
    current_style = style;
    for (auto it = kind_buttons.begin(); it != kind_buttons.end(); ++it) {
        it.value()->blockSignals(true);
        it.value()->setChecked(it.key() == style);
        it.value()->blockSignals(false);
    }
    refresh_block_size_icons();
}

void MosaicSettingsPanel::set_block_size(const QVariant &value) {
    // MosaicTool owns the snapping rule, so the highlighted level matches what is drawn.
    const int level = MosaicTool::clamp_block_size(value);
    current_block_size = level;
    for (auto it = block_size_buttons.begin(); it != block_size_buttons.end(); ++it) it.value()->setChecked(it.key() == level);
}

void MosaicSettingsPanel::retranslate() {
    freehand_btn->setToolTip(mosaic_tr("Freehand Mosaic"));
    rect_btn->setToolTip(mosaic_tr("Rect Mosaic"));
    size_spin->setToolTip(mosaic_tr("Brush Size"));
    int index = 0;
    for (auto *button : block_size_buttons) button->setToolTip(mosaic_tr(BLOCK_SIZE_TIPS[index++]));
    retranslate_kinds();
    adjustSize();
}
}
