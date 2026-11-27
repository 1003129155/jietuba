#include "gallery_window.h"
#include "clipboard/ui/theme/themes.h"
#include "core/i18n.h"
#include "core/resource_manager.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "translation/translation_dialog.h"
#include "ui/color_picker_dialog.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/fluent_dialog.h"
#include "ui/fluent_lite/fluent_lite.h"
#include "ui/fluent_lite/text_context_menu.h"
#include "ui/key_chip.h"
#include "ui/motion.h"
#include "ui/settings_ui/components.h"
#include "ui/toast.h"
#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVariantAnimation>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>

namespace jietuba::gallery {
namespace {
using Maker = std::function<QWidget *(int column)>;
struct StateRow {
    QString name;
    Maker make;
};
struct ColorRow {
    QString name;
    QStringList values;
};

QString text(const char *chinese) { return QString::fromUtf8(chinese); }

// A colour over a checkerboard, so translucency shows; the tooltip carries the value as written.
class ColorSwatch : public QWidget {
public:
    explicit ColorSwatch(const QString &value, QWidget *parent = nullptr) : QWidget(parent), color_(css_color(value)) {
        setFixedSize(dialog_scaled(56), dialog_scaled(24));
        setToolTip(value);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const int radius = dialog_scaled(4);
        QPainterPath shape;
        shape.addRoundedRect(frame, radius, radius);
        painter.save();
        painter.setClipPath(shape);
        const int tile = dialog_scaled(6);
        for (int y = 0; y < height(); y += tile)
            for (int x = 0; x < width(); x += tile) painter.fillRect(x, y, tile, tile, ((x / tile + y / tile) % 2) ? QColor(204, 204, 204) : QColor(255, 255, 255));
        painter.fillRect(rect(), color_.isValid() ? color_ : QColor(255, 0, 255));
        painter.restore();
        painter.setPen(QColor(128, 128, 128, 120));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(frame, radius, radius);
    }

private:
    QColor color_;
};

// One box drawn at a real size and radius, optionally with the content area inside its padding.
class MetricSample : public QWidget {
public:
    MetricSample(const QSize &size, double radius, const QMargins &padding = {}, QWidget *parent = nullptr)
        : QWidget(parent), radius_(radius), padding_(padding) {
        setFixedSize(size);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        const auto &t = ui_tokens(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(css_color(t.control_border_bottom));
        painter.setBrush(painted_over(css_color(t.control), layer_backdrop(this)));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius_, radius_);
        if (padding_.isNull()) return;
        painter.setPen(Qt::NoPen);
        painter.setBrush(css_color(t.accent_soft));
        painter.drawRect(rect().marginsRemoved(padding_));
    }

private:
    double radius_;
    QMargins padding_;
};

// A dot travelling along a track with the application's easing and the length of one motion step.
class MotionDemo : public QWidget {
public:
    MotionDemo(int milliseconds, QWidget *parent = nullptr) : QWidget(parent), milliseconds_(milliseconds) {
        setFixedSize(dialog_scaled(220), dialog_scaled(24));
        animation_.setEasingCurve(motion::decelerate());
        connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            progress_ = value.toDouble();
            update();
        });
    }
    void play() {
        forward_ = !forward_;
        animation_.stop();
        animation_.setDuration(motion::duration(milliseconds_));
        animation_.setStartValue(forward_ ? 0.0 : 1.0);
        animation_.setEndValue(forward_ ? 1.0 : 0.0);
        animation_.start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        const auto &t = ui_tokens(this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const double dot = dialog_scaled(14);
        const QRectF track(dot / 2, (height() - dialog_scaled(4)) / 2.0, width() - dot, dialog_scaled(4));
        painter.setPen(Qt::NoPen);
        painter.setBrush(css_color(t.control_border_bottom));
        painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);
        painter.setBrush(css_color(t.accent));
        const double x = track.left() + progress_ * track.width();
        painter.drawEllipse(QPointF(x, height() / 2.0), dot / 2, dot / 2);
    }

private:
    int milliseconds_;
    bool forward_ = false;
    double progress_ = 0;
    QVariantAnimation animation_;
};

// The capture selection as drawn from the screenshot theme: mask outside, outline and handles on it.
class SelectionPreview : public QWidget {
public:
    explicit SelectionPreview(QWidget *parent = nullptr) : QWidget(parent) { setFixedSize(dialog_scaled(180), dialog_scaled(90)); }

protected:
    void paintEvent(QPaintEvent *) override {
        auto &theme = get_theme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor(236, 236, 236));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(190, 190, 190));
        for (int line = 0; line < 5; ++line) painter.drawRect(QRect(dialog_scaled(10), dialog_scaled(10 + line * 16), dialog_scaled(120 - line * 12), dialog_scaled(6)));
        const QRect selection = rect().adjusted(dialog_scaled(40), dialog_scaled(20), -dialog_scaled(30), -dialog_scaled(20));
        QPainterPath mask;
        mask.addRect(QRectF(rect()));
        mask.addRect(QRectF(selection));
        painter.fillPath(mask, theme.mask_color());
        painter.setPen(QPen(theme.theme_color(), theme.selection_border_width()));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(selection);
        painter.setPen(QPen(Qt::white, theme.selection_handle_ring_width()));
        painter.setBrush(theme.theme_color());
        const double diameter = theme.selection_handle_diameter();
        for (const auto &corner : {selection.topLeft(), selection.topRight(), selection.bottomLeft(), selection.bottomRight()})
            painter.drawEllipse(QPointF(corner), diameter / 2, diameter / 2);
    }
};

QString css_rgba(const QColor &color) {
    return QString("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alphaF(), 0, 'f', 2);
}

SettingsCardGroup *add_group(QVBoxLayout *layout, QWidget *view, const QString &title) {
    auto *group = new SettingsCardGroup(title, view);
    layout->addWidget(group);
    return group;
}

QGridLayout *add_grid_card(SettingsCardGroup *group) {
    auto *card = new WhiteCard;
    auto *grid = new QGridLayout(card);
    grid->setContentsMargins(dialog_scaled(16), dialog_scaled(12), dialog_scaled(16), dialog_scaled(12));
    grid->setHorizontalSpacing(dialog_scaled(16));
    grid->setVerticalSpacing(dialog_scaled(10));
    group->addSettingCard(card);
    return grid;
}

CaptionLabel *add_note(QVBoxLayout *layout, const QString &note) {
    auto *label = new CaptionLabel(note);
    label->setWordWrap(true);
    layout->addWidget(label);
    return label;
}

// A swatch and its value, side by side.
QWidget *color_cell(const QString &value) {
    auto *cell = new QWidget;
    auto *row = new QHBoxLayout(cell);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(dialog_scaled(8));
    row->addWidget(new ColorSwatch(value));
    auto *caption = new CaptionLabel(value);
    caption->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(caption, 1);
    return cell;
}

// Name and one swatch-and-value cell per header.
void add_color_table(SettingsCardGroup *group, const QStringList &headers, const QList<ColorRow> &rows) {
    auto *grid = add_grid_card(group);
    // The same column widths in every table, so the swatches line up from one group to the next.
    grid->setColumnMinimumWidth(0, dialog_scaled(210));
    for (int column = 0; column < headers.size(); ++column) {
        grid->addWidget(new CaptionLabel(headers[column]), 0, column + 1);
        grid->setColumnStretch(column + 1, 1);
        grid->setColumnMinimumWidth(column + 1, dialog_scaled(250));
    }
    for (int row = 0; row < rows.size(); ++row) {
        grid->addWidget(new BodyLabel(rows[row].name), row + 1, 0);
        for (int column = 0; column < rows[row].values.size(); ++column) grid->addWidget(color_cell(rows[row].values[column]), row + 1, column + 1);
    }
}

// The rows in two blocks side by side, for a table with a single value column.
void add_paired_color_table(SettingsCardGroup *group, const QList<ColorRow> &rows) {
    auto *grid = add_grid_card(group);
    const int half = (rows.size() + 1) / 2;
    for (int index = 0; index < rows.size(); ++index) {
        const int block = index / half, row = index % half;
        grid->addWidget(new BodyLabel(rows[index].name), row, block * 2);
        grid->addWidget(color_cell(rows[index].values.value(0)), row, block * 2 + 1);
    }
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
}

// Name and one control per column; a maker builds the control for its column.
void add_state_table(SettingsCardGroup *group, const QStringList &headers, const QList<StateRow> &rows, Qt::Alignment align = Qt::AlignLeft) {
    auto *grid = add_grid_card(group);
    for (int column = 0; column < headers.size(); ++column) {
        grid->addWidget(new CaptionLabel(headers[column]), 0, column + 1);
        grid->setColumnStretch(column + 1, 1);
    }
    for (int row = 0; row < rows.size(); ++row) {
        grid->addWidget(new BodyLabel(rows[row].name), row + 1, 0);
        for (int column = 0; column < headers.size(); ++column) grid->addWidget(rows[row].make(column), row + 1, column + 1, align | Qt::AlignVCenter);
    }
}


// ---- Page 1: window palette ----

QString token_group(const QString &name) {
    if (name.startsWith("accent")) return text("强调色");
    if (name == "success" || name == "danger" || name == "danger_soft" || name == "status_idle" || name == "selected_text") return text("状态色");
    if (name == "page_background" || name == "layer" || name == "layer_border" || name.startsWith("card") || name == "window_border") return text("窗口与卡片");
    if (name.startsWith("fg")) return text("文字");
    if (name.startsWith("control") || name == "item_hover" || name == "divider" || name == "switch_off_fill") return text("控件与分隔");
    if (name.startsWith("popup")) return text("菜单与提示");
    return text("其他");
}

void build_colors(QVBoxLayout *layout, QWidget *view) {
    add_note(layout, text("数值来自 core/ui_theme 的 LIGHT_TOKENS 与 DARK_TOKENS，每个字段一行；半透明色画在棋盘格上，悬停色块可看原值。"));
    const auto light = ui_token_entries(LIGHT_TOKENS());
    const auto dark = ui_token_entries(DARK_TOKENS());
    const QStringList order = {text("强调色"), text("状态色"), text("窗口与卡片"), text("文字"), text("控件与分隔"), text("菜单与提示"), text("其他")};
    std::map<QString, QList<ColorRow>> groups;
    for (int index = 0; index < light.size(); ++index)
        groups[token_group(light[index].first)].append({light[index].first, {light[index].second, dark.value(index).second}});
    for (const auto &title : order) {
        if (groups[title].isEmpty()) continue;
        add_color_table(add_group(layout, view, title), {text("浅色"), text("深色")}, groups[title]);
    }
}

// ---- Page 2: sizes and motion ----

void add_metric_row(QGridLayout *grid, int row, const QString &name, const QString &value, QWidget *sample) {
    grid->addWidget(new BodyLabel(name), row, 0);
    grid->addWidget(new CaptionLabel(value), row, 1);
    grid->addWidget(sample, row, 2, Qt::AlignLeft | Qt::AlignVCenter);
}

QString scaled_value(int base) { return text("%1 px，随窗口缩放（现在 %2 px）").arg(base).arg(dialog_scaled(base)); }

void build_metrics(QVBoxLayout *layout, QWidget *view) {
    auto *sizes = add_group(layout, view, text("尺寸"));
    auto *grid = add_grid_card(sizes);
    grid->setColumnMinimumWidth(0, dialog_scaled(270));
    grid->setColumnStretch(1, 1);
    int row = 0;
    const int sample_w = dialog_scaled(160);
    add_metric_row(grid, row++, "INPUT_RADIUS", scaled_value(INPUT_RADIUS), new MetricSample({sample_w, dialog_scaled(INPUT_HEIGHT)}, dialog_scaled(INPUT_RADIUS)));
    add_metric_row(grid, row++, "INPUT_CONTENT_HEIGHT", scaled_value(INPUT_CONTENT_HEIGHT),
                   new MetricSample({sample_w, dialog_scaled(INPUT_HEIGHT)}, dialog_scaled(INPUT_RADIUS),
                                    QMargins(dialog_scaled(INPUT_PADDING_H) + 1, dialog_scaled(INPUT_PADDING_V) + 1, dialog_scaled(INPUT_PADDING_H) + 1, dialog_scaled(INPUT_PADDING_V) + 1)));
    add_metric_row(grid, row++, "INPUT_PADDING_V / INPUT_PADDING_H", text("%1 px / %2 px，浅色块是内容区").arg(INPUT_PADDING_V).arg(INPUT_PADDING_H),
                   new MetricSample({sample_w, dialog_scaled(INPUT_HEIGHT)}, dialog_scaled(INPUT_RADIUS),
                                    QMargins(dialog_scaled(INPUT_PADDING_H) + 1, dialog_scaled(INPUT_PADDING_V) + 1, dialog_scaled(INPUT_PADDING_H) + 1, dialog_scaled(INPUT_PADDING_V) + 1)));
    auto *line_edit = new LineEdit;
    line_edit->setText(text("真实输入框"));
    line_edit->setFixedWidth(sample_w);
    add_metric_row(grid, row++, "INPUT_HEIGHT", text("%1 px = 内容 %2 + 上下内边距 %3×2 + 边框 2，现在 %4 px").arg(INPUT_HEIGHT).arg(INPUT_CONTENT_HEIGHT).arg(INPUT_PADDING_V).arg(dialog_scaled(INPUT_HEIGHT)), line_edit);
    add_metric_row(grid, row++, "CARD_RADIUS", scaled_value(CARD_RADIUS), new MetricSample({sample_w, dialog_scaled(40)}, dialog_scaled(CARD_RADIUS)));
    add_metric_row(grid, row++, "CARD_MIN_HEIGHT", scaled_value(CARD_MIN_HEIGHT), new MetricSample({sample_w, dialog_scaled(CARD_MIN_HEIGHT)}, dialog_scaled(CARD_RADIUS)));
    auto *simple = new SimpleCardWidget;
    simple->setFixedSize(sample_w, dialog_scaled(48));
    add_metric_row(grid, row++, "SimpleCardWidget", text("圆角 14 px，写死在样式里，不随缩放"), simple);
    add_metric_row(grid, row++, text("窗口外圆角"), text("8 px，不随缩放：Windows 11 按这个裁窗口"), new MetricSample({sample_w, dialog_scaled(48)}, 8));
    add_metric_row(grid, row++, text("窗口内容层圆角"), scaled_value(8), new MetricSample({sample_w, dialog_scaled(48)}, dialog_scaled(8)));
    add_metric_row(grid, row++, "IconBadge", text("方块 36 px、图标 20 px、圆角 10 px，随窗口缩放"), new IconBadge(FluentIcon::Setting, "neutral", nullptr));
    auto *toggle = new SwitchButton;
    add_metric_row(grid, row++, "SwitchButton", text("40×20 px，随窗口缩放（现在 %1×%2 px）").arg(dialog_scaled(40)).arg(dialog_scaled(20)), toggle);

    auto *steps = add_group(layout, view, text("动效时长（ui/motion.h）"));
    auto *motion_grid = add_grid_card(steps);
    motion_grid->setColumnMinimumWidth(0, dialog_scaled(270));
    motion_grid->setColumnStretch(1, 1);
    const std::pair<const char *, int> durations[] = {{"FAST_MS", motion::FAST_MS}, {"MID_MS", motion::MID_MS}, {"SLOW_MS", motion::SLOW_MS}};
    const char *uses[] = {"颜色变化", "小位移、小弹出层", "较大的区域"};
    int motion_row = 0;
    for (const auto &[name, milliseconds] : durations) {
        const int use_index = motion_row;
        auto *demo = new MotionDemo(milliseconds);
        auto *play = new PushButton(text("播放"));
        QObject::connect(play, &QPushButton::clicked, demo, [demo] { demo->play(); });
        auto *sample = new QWidget;
        auto *sample_row = new QHBoxLayout(sample);
        sample_row->setContentsMargins(0, 0, 0, 0);
        sample_row->setSpacing(dialog_scaled(12));
        sample_row->addWidget(demo);
        sample_row->addWidget(play);
        add_metric_row(motion_grid, motion_row++, name, text("%1 ms，用于%2").arg(milliseconds).arg(text(uses[use_index])), sample);
    }
}

// ---- Pages 3 to 5: controls ----

void build_buttons(QVBoxLayout *layout, QWidget *view) {
    auto *group = add_group(layout, view, text("按钮（ui/fluent_lite/buttons）"));
    const auto enable = [](QWidget *widget, int column) {
        widget->setEnabled(column == 0);
        return widget;
    };
    add_state_table(group, {text("正常"), text("禁用")},
                    {
                        {"PushButton", [=](int column) { return enable(new PushButton(text("普通按钮")), column); }},
                        {"PrimaryPushButton", [=](int column) { return enable(new PrimaryPushButton(text("主要按钮")), column); }},
                        {"TransparentPushButton", [=](int column) { return enable(new TransparentPushButton(text("透明按钮")), column); }},
                        {"HyperlinkButton", [=](int column) { return enable(new HyperlinkButton({}, text("链接按钮")), column); }},
                        {"TransparentToolButton", [=](int column) { return enable(new TransparentToolButton(FluentIcon::Setting), column); }},
                        {text("PushButton 带图标"), [=](int column) { return enable(new PushButton(text("保存"), nullptr, FluentIcon::Save), column); }},
                        {text("PrimaryPushButton 带图标"), [=](int column) { return enable(new PrimaryPushButton(text("发送"), nullptr, FluentIcon::Send), column); }},
                    });
}

void build_inputs(QVBoxLayout *layout, QWidget *view) {
    auto *group = add_group(layout, view, text("输入（ui/fluent_lite/inputs、ui/key_chip）"));
    const auto enable = [](QWidget *widget, int column) {
        widget->setEnabled(column == 0);
        return widget;
    };
    add_state_table(group, {text("正常"), text("禁用")},
                    {
                        {"LineEdit", [=](int column) {
                             auto *edit = new LineEdit;
                             edit->setText(text("示例文字"));
                             return enable(edit, column);
                         }},
                        {text("LineEdit 占位文字"), [=](int column) {
                             auto *edit = new LineEdit;
                             edit->setPlaceholderText(text("请输入…"));
                             return enable(edit, column);
                         }},
                        {"TextEdit", [=](int column) {
                             auto *edit = new TextEdit;
                             edit->setPlainText(text("多行文字\n第二行"));
                             edit->setFixedHeight(dialog_scaled(72));
                             edit->setStyleSheet(text_edit_qss(edit));
                             return enable(edit, column);
                         }},
                        {"ComboBox", [=](int column) {
                             auto *combo = new ComboBox;
                             combo->addItems({text("选项一"), text("选项二"), text("选项三")});
                             return enable(combo, column);
                         }},
                        {"SpinBox", [=](int column) {
                             auto *spin = new SpinBox;
                             spin->setRange(0, 100);
                             spin->setValue(42);
                             return enable(spin, column);
                         }},
                        {"DoubleSpinBox", [=](int column) {
                             auto *spin = new DoubleSpinBox;
                             spin->setRange(0, 10);
                             spin->setValue(2.5);
                             return enable(spin, column);
                         }},
                        {"ValueSlider", [=](int column) {
                             auto *slider = new ValueSlider;
                             slider->setRange(0, 100);
                             slider->setValue(40);
                             return enable(slider, column);
                         }},
                        {"KeyChipLineEdit", [=](int column) {
                             auto *chip = new KeyChipLineEdit;
                             chip->setText("ctrl+shift+a");
                             return enable(chip, column);
                         }},
                    },
                    Qt::AlignLeft);
}

void build_choices(QVBoxLayout *layout, QWidget *view) {
    auto *group = add_group(layout, view, text("选择（ui/fluent_lite）"));
    const QStringList headers = {text("未选"), text("已选"), text("禁用未选"), text("禁用已选")};
    const auto state = [](QAbstractButton *button, int column) {
        button->setCheckable(true);
        button->setChecked(column % 2 == 1);
        button->setEnabled(column < 2);
        return button;
    };
    const auto segmented = [](int column, bool large, bool compact, bool floating) {
        auto *widget = new SegmentedWidget;
        widget->setLarge(large);
        if (compact) widget->setCompact(true);
        widget->setFloating(floating);
        for (const auto &[key, label] : {std::pair{"a", "选项一"}, std::pair{"b", "选项二"}, std::pair{"c", "选项三"}}) widget->addItem(key, QString::fromUtf8(label));
        widget->setCurrentItem(column % 2 == 1 ? "b" : "a");
        widget->setEnabled(column < 2);
        return widget;
    };
    add_state_table(group, headers,
                    {
                        {"CheckBox", [=](int column) { return state(new CheckBox(text("复选框")), column); }},
                        {"LabeledSwitch", [=](int column) { return state(new LabeledSwitch(text("带文字的开关")), column); }},
                        {"SwitchButton", [=](int column) {
                             auto *toggle = new SwitchButton;
                             toggle->setChecked(column % 2 == 1);
                             toggle->setEnabled(column < 2);
                             return toggle;
                         }},
                    });
    // The segmented selectors are wide; they get one column pair each.
    auto *segmented_group = add_group(layout, view, "SegmentedWidget");
    add_state_table(segmented_group, {text("选中第一项"), text("选中第二项")},
                    {
                        {text("默认"), [=](int column) { return segmented(column, false, false, false); }},
                        {text("大号"), [=](int column) { return segmented(column, true, false, false); }},
                        {text("紧凑"), [=](int column) { return segmented(column, false, true, false); }},
                        {text("浮动（不透明底）"), [=](int column) { return segmented(column, false, false, true); }},
                        {text("禁用"), [=](int column) {
                             auto *widget = segmented(column, false, false, false);
                             widget->setEnabled(false);
                             return widget;
                         }},
                    });
}

// ---- Page 6: cards and layout ----

void build_cards(QVBoxLayout *layout, QWidget *view) {
    auto *rows = add_group(layout, view, "SettingCardGroup · SettingCard");
    auto *plain = new SettingCard(FluentIcon::Camera, text("设置卡片"), text("图标、标题、一行说明，右侧是控件列。"));
    auto *combo = new ComboBox;
    combo->addItems({text("选项一"), text("选项二")});
    plain->addControl(combo);
    rows->addSettingCard(plain);
    auto *button_card = new SettingCard(FluentIcon::Folder, text("带按钮的卡片"), text("控件列里放一个按钮。"));
    button_card->addControl(new PrimaryPushButton(text("打开")));
    rows->addSettingCard(button_card);
    auto *switch_card = new SwitchSettingCard(FluentIcon::Pin, "SwitchSettingCard", text("右侧是开关，下面两行折进它。"));
    switch_card->setChecked(true);
    rows->addSettingCard(switch_card);
    for (const char *title : {"子开关一", "子开关二"}) {
        auto *sub = new SubSwitchSettingCard(QString::fromUtf8(title), text("SubSwitchSettingCard 自动折成展开项。"));
        sub->setChecked(true);
        rows->addSettingCard(sub);
    }

    auto *expander_group = add_group(layout, view, "SettingExpander");
    auto *header = new SettingCard(FluentIcon::Language, text("可展开的卡片"), text("点右侧箭头折叠或展开。"));
    auto *expander = new SettingExpander(header);
    for (const char *title : {"展开项一", "展开项二"}) {
        auto *item = new SettingCard(IconSource{}, QString::fromUtf8(title), text("展开后才看得见。"));
        item->addControl(new SwitchButton);
        expander->addItem(item);
    }
    expander->setExpanded(true, false);
    expander_group->addSettingCard(expander);

    auto *others = add_group(layout, view, text("其他卡片与图标"));
    auto *simple_host = new QWidget;
    auto *simple_row = new QHBoxLayout(simple_host);
    simple_row->setContentsMargins(0, 0, 0, 0);
    simple_row->setSpacing(dialog_scaled(12));
    auto *simple = new SimpleCardWidget;
    auto *simple_layout = new QVBoxLayout(simple);
    simple_layout->setContentsMargins(dialog_scaled(16), dialog_scaled(12), dialog_scaled(16), dialog_scaled(12));
    simple_layout->addWidget(new BodyLabel("SimpleCardWidget"));
    simple_layout->addWidget(new CaptionLabel(text("圆角 14 px 的卡片底")));
    simple_row->addWidget(simple, 1);
    auto *badges = new SimpleCardWidget;
    auto *badge_row = new QHBoxLayout(badges);
    badge_row->setContentsMargins(dialog_scaled(16), dialog_scaled(12), dialog_scaled(16), dialog_scaled(12));
    badge_row->setSpacing(dialog_scaled(10));
    badge_row->addWidget(new IconBadge(FluentIcon::Setting, QString()));
    for (const char *tone : {"capture", "clipboard", "pin", "translate", "neutral"}) badge_row->addWidget(new IconBadge(FluentIcon::Camera, tone));
    badge_row->addStretch(1);
    simple_row->addWidget(badges, 1);
    others->addSettingCard(simple_host);
    auto *dialog_card = new DialogCard("DialogCard");
    dialog_card->addCaption(text("对话框里的一行，名字在左，控件在右。"));
    auto *edit = new LineEdit;
    edit->setText(text("示例文字"));
    dialog_card->addControl(edit, 1);
    others->addSettingCard(dialog_card);
}

// ---- Page 7: navigation, menus and popups ----

void build_popups(QVBoxLayout *layout, QWidget *view) {
    auto *navigation_group = add_group(layout, view, "NavigationInterface");
    auto *host = new QWidget;
    host->setFixedHeight(dialog_scaled(300));
    auto *host_row = new QHBoxLayout(host);
    host_row->setContentsMargins(0, 0, 0, 0);
    auto *navigation = new NavigationInterface(host);
    navigation->setFixedWidth(dialog_scaled(220));
    navigation->setExpandWidth(dialog_scaled(220));
    navigation->setMinimumExpandWidth(0);
    navigation->expand();
    navigation->addItem("one", FluentIcon::Camera, text("第一项"), [] {});
    navigation->addItem("two", FluentIcon::Paste, text("第二项"), [] {});
    navigation->addItem("three", FluentIcon::Folder, text("可展开的一项"), [] {});
    navigation->setExpandable("three", true);
    navigation->setSubItems("three", {text("子项一"), text("子项二")});
    navigation->setExpanded("three", true, false);
    navigation->addItem("bottom", FluentIcon::Setting, text("底部的一项"), [] {}, NavigationItemPosition::BOTTOM);
    navigation->setCurrentItem("one");
    host_row->addWidget(navigation);
    host_row->addStretch(1);
    navigation_group->addSettingCard(host);

    auto *popups = add_group(layout, view, text("菜单与弹层"));
    auto *grid = add_grid_card(popups);
    auto *menu_button = new PushButton(text("弹出菜单"));
    QObject::connect(menu_button, &QPushButton::clicked, menu_button, [menu_button] {
        // Parentless: a menu under a styled widget would take that sheet instead of the global QMenu rules.
        auto *menu = new QMenu;
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->addAction(QString::fromUtf8("普通项"));
        auto *checked = menu->addAction(QString::fromUtf8("可勾选项"));
        checked->setCheckable(true);
        checked->setChecked(true);
        menu->addAction(QString::fromUtf8("禁用项"))->setEnabled(false);
        menu->addSeparator();
        auto *sub = menu->addMenu(QString::fromUtf8("子菜单"));
        sub->addAction(QString::fromUtf8("子项一"));
        sub->addAction(QString::fromUtf8("子项二"));
        menu->popup(menu_button->mapToGlobal(QPoint(0, menu_button->height())));
    });
    auto *sample_edit = new LineEdit;
    sample_edit->setText(text("在这里点右键"));
    auto *context_button = new PushButton(text("文本右键菜单"));
    QObject::connect(context_button, &QPushButton::clicked, context_button, [context_button, sample_edit] {
        show_text_context_menu(sample_edit, context_button->mapToGlobal(QPoint(0, context_button->height())));
    });
    auto *toast_button = new PushButton(text("Toast 提示"));
    QObject::connect(toast_button, &QPushButton::clicked, toast_button, [] { show_toast(QString::fromUtf8("这是一条 Toast 提示")); });
    auto *tip_button = new PushButton(text("悬停看提示文字"));
    tip_button->setToolTip(text("这是控件的提示文字"));
    grid->addWidget(new BodyLabel("QMenu"), 0, 0);
    grid->addWidget(menu_button, 0, 1, Qt::AlignLeft);
    grid->addWidget(new BodyLabel(text("文本右键菜单")), 1, 0);
    grid->addWidget(sample_edit, 1, 1);
    grid->addWidget(context_button, 1, 2, Qt::AlignLeft);
    grid->addWidget(new BodyLabel("Toast"), 2, 0);
    grid->addWidget(toast_button, 2, 1, Qt::AlignLeft);
    grid->addWidget(new BodyLabel("QToolTip"), 3, 0);
    grid->addWidget(tip_button, 3, 1, Qt::AlignLeft);
    grid->setColumnStretch(1, 1);
}

// ---- Page 8: dialogs ----

void build_dialogs(GalleryWindow *window, QVBoxLayout *layout, QWidget *view) {
    auto *group = add_group(layout, view, text("对话框"));
    auto *grid = add_grid_card(group);
    auto *fluent = new PushButton(text("打开"));
    QObject::connect(fluent, &QPushButton::clicked, window, [window] {
        std::unique_ptr<QDialog> dialog(window->make_fluent_dialog());
        exec_dialog(dialog.get(), window);
    });
    auto *standard = new PushButton(text("打开"));
    QObject::connect(standard, &QPushButton::clicked, window, [window] {
        std::unique_ptr<QDialog> dialog(window->make_standard_dialog());
        exec_dialog(dialog.get(), window);
    });
    auto *picker = new PushButton(text("打开"));
    QObject::connect(picker, &QPushButton::clicked, window, [window, picker] {
        ColorPickerDialog dialog(css_color(get_ui_theme()->tokens().accent), ColorPickerDialog::ShowAlphaChannel, window);
        dialog.place_near(picker);
        exec_dialog(&dialog, window);
    });
    grid->addWidget(new BodyLabel(text("FluentDialog（带 DialogCard）")), 0, 0);
    grid->addWidget(fluent, 0, 1, Qt::AlignLeft);
    grid->addWidget(new BodyLabel(text("StandardDialog（消息框）")), 1, 0);
    grid->addWidget(standard, 1, 1, Qt::AlignLeft);
    grid->addWidget(new BodyLabel("ColorPickerDialog"), 2, 0);
    grid->addWidget(picker, 2, 1, Qt::AlignLeft);
    grid->setColumnStretch(2, 1);

    // Pictures of the dialogs as they open, so they can be compared without opening them.
    const auto add_preview = [&](const QString &title, QDialog *dialog) {
        const std::unique_ptr<QDialog> owned(dialog);
        auto *preview_group = add_group(layout, view, title);
        auto *host = new QWidget;
        auto *row = new QHBoxLayout(host);
        row->setContentsMargins(0, 0, 0, 0);
        auto *picture = new QLabel;
        picture->setPixmap(dialog->grab());
        row->addWidget(picture);
        row->addStretch(1);
        preview_group->addSettingCard(host);
    };
    add_preview("FluentDialog", window->make_fluent_dialog());
    add_preview("StandardDialog", window->make_standard_dialog());
    add_preview("ColorPickerDialog", new ColorPickerDialog(css_color(get_ui_theme()->tokens().accent), ColorPickerDialog::ShowAlphaChannel, window));
}

// ---- Pages 9 and 10: colours of the windows and tools that keep their own ----

void build_translation(QVBoxLayout *layout, QWidget *view) {
    add_note(layout, text("翻译窗口和翻译小窗保留独立的风格，配色是一套有名字的「翻译风格」：dashboard_light 与 dashboard_dark。"));
    const auto &light = dashboard_light();
    const auto &dark = dashboard_dark();
    const auto row = [&](const char *name, const QString DashboardPalette::*field) { return ColorRow{name, {light.*field, dark.*field}}; };
    add_color_table(add_group(layout, view, text("翻译风格")), {text("浅色"), text("深色")},
                    {row("window", &DashboardPalette::window), row("field", &DashboardPalette::field), row("surface_strong", &DashboardPalette::surface_strong),
                     row("text", &DashboardPalette::text), row("text_2", &DashboardPalette::text_2), row("text_3", &DashboardPalette::text_3),
                     row("accent", &DashboardPalette::accent), row("accent_hover", &DashboardPalette::accent_hover), row("accent_tint", &DashboardPalette::accent_tint),
                     row("fill", &DashboardPalette::fill), row("fill_hover", &DashboardPalette::fill_hover), row("danger", &DashboardPalette::danger),
                     row("green", &DashboardPalette::green)});
}

void build_tools(QVBoxLayout *layout, QWidget *view) {
    add_note(layout, text("这一页的配色独立，不跟窗口色板：截图主题色由用户在设置里选，剪贴板面板有 7 套用户可选的主题。"));
    auto &theme = get_theme();
    auto *capture = add_group(layout, view, text("截图主题色（core/theme）"));
    add_color_table(capture, {text("当前值")},
                    {{text("主题色 theme_color"), {theme.theme_color_hex()}},
                     {text("遮罩色 mask_color"), {css_rgba(theme.mask_color())}},
                     {text("主题色上的文字 contrast_ink"), {contrast_ink(theme.theme_color()).name().toUpper()}}});
    auto *preview_card = add_grid_card(capture);
    preview_card->addWidget(new BodyLabel(text("选区预览")), 0, 0);
    preview_card->addWidget(new SelectionPreview, 0, 1, Qt::AlignLeft);
    preview_card->setColumnStretch(1, 1);

    using clipboard::ThemeColors;
    const std::pair<const char *, QString ThemeColors::*> fields[] = {
        {"bg_primary", &ThemeColors::bg_primary},         {"bg_secondary", &ThemeColors::bg_secondary},   {"bg_tertiary", &ThemeColors::bg_tertiary},
        {"bg_hover", &ThemeColors::bg_hover},             {"bg_selected", &ThemeColors::bg_selected},     {"text_primary", &ThemeColors::text_primary},
        {"text_secondary", &ThemeColors::text_secondary}, {"text_accent", &ThemeColors::text_accent},     {"border_primary", &ThemeColors::border_primary},
        {"border_selected", &ThemeColors::border_selected}, {"accent_primary", &ThemeColors::accent_primary}, {"success", &ThemeColors::success},
        {"error", &ThemeColors::error},                   {"warning", &ThemeColors::warning},
    };
    for (const auto &[name, preset] : clipboard::PRESET_THEMES()) {
        QList<ColorRow> rows;
        for (const auto &[field_name, member] : fields) rows.append({field_name, {preset.colors.*member}});
        add_paired_color_table(add_group(layout, view, text("剪贴板面板：%1（%2）").arg(preset.display_name, name)), rows);
    }
}
}

const QList<GalleryPage> &GalleryWindow::pages() {
    static const QList<GalleryPage> list = {
        {"colors", text("窗口色板")},   {"metrics", text("尺寸与动效")},     {"buttons", text("按钮")},     {"inputs", text("输入")},
        {"choices", text("选择")},      {"cards", text("卡片与布局")},       {"popups", text("导航、菜单与弹层")}, {"dialogs", text("对话框")},
        {"translation", text("翻译风格")}, {"tools", text("工具类配色")},
    };
    return list;
}

QStringList GalleryWindow::language_codes() { return {"zh", "zh_TW", "en", "ja", "ko"}; }

void GalleryWindow::apply_state(const GalleryState &state) {
    get_ui_theme()->set_mode(state.dark ? "dark" : "light", false);
    get_dialog_scale().set_percent(state.scale_percent);
    if (I18nManager::get_current_language() != state.language) I18nManager::load_language(state.language);
}

GalleryWindow::GalleryWindow(const GalleryState &state, QWidget *parent) : FrostedFramelessDialog(parent), state_(state) {
    apply_state(state_);
    setObjectName("GalleryWindow");
    setWindowTitle(text("jietuba 界面样本"));
    setWindowIcon(ResourceManager::get_icon_by_name(QString::fromUtf8("托盘.svg")));
    scale_dialog_font(*this);
    QFont window_font = font();
    window_font.setFamilies(FONT_FAMILIES());
    window_font.setPointSize(dialog_scaled(11));
    setFont(window_font);

    auto *title_bar = new FluentTitleBar(this);
    configure_dialog_control(*title_bar);
    setTitleBar(title_bar);
    title_bar->setFixedHeight(title_bar->buttonLayout->sizeHint().height());
    title_bar->hBoxLayout->setContentsMargins(dialog_scaled(14), 0, 0, 0);
    title_bar->titleLabel->setIndent(dialog_scaled(8));
    if (title_bar->maxBtn) title_bar->maxBtn->hide();
    title_bar->setDoubleClickEnabled(false);

    auto *main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(dialog_scaled(10), title_bar->height() + dialog_scaled(6), 0, 0);
    main_layout->setSpacing(0);
    main_layout->addWidget(build_navigation(this));

    right_area_ = new QWidget(this);
    auto *right_layout = new QVBoxLayout(right_area_);
    right_layout->setContentsMargins(dialog_scaled(24), dialog_scaled(12), dialog_scaled(4), 0);
    right_layout->setSpacing(dialog_scaled(10));
    right_layout->addWidget(build_toolbar(right_area_));
    stack_ = new QStackedWidget(right_area_);
    for (int index = 0; index < page_count(); ++index) stack_->addWidget(new QWidget);
    right_layout->addWidget(stack_, 1);
    main_layout->addWidget(right_area_, 1);

    apply_window_sheet();
    resize(dialog_scaled(1180), dialog_scaled(800));
    setMinimumSize(dialog_scaled(960), dialog_scaled(560));
    state_.page = std::clamp(state_.page, 0, page_count() - 1);
    show_page(state_.page);
    configure_dialog_controls(*this);
}

QWidget *GalleryWindow::build_navigation(QWidget *parent) {
    auto *panel = new QWidget(parent);
    panel->setFixedWidth(dialog_scaled(210));
    auto *panel_layout = new QVBoxLayout(panel);
    panel_layout->setContentsMargins(dialog_scaled(4), 0, dialog_scaled(4), dialog_scaled(8));
    navigation_ = new NavigationInterface(panel);
    navigation_->setExpandWidth(dialog_scaled(198));
    navigation_->setMinimumExpandWidth(0);
    navigation_->expand();
    navigation_->setMinimumWidth(dialog_scaled(198));
    navigation_->setMaximumWidth(dialog_scaled(198));
    const FluentIcon icons[] = {FluentIcon::Palette, FluentIcon::Alignment, FluentIcon::Send, FluentIcon::Edit, FluentIcon::Check,
                                FluentIcon::Document, FluentIcon::Search, FluentIcon::Application, FluentIcon::Language, FluentIcon::Brush};
    for (int index = 0; index < page_count(); ++index) navigation_->addItem(pages()[index].key, icons[index], pages()[index].title, [this, index] { show_page(index); });
    panel_layout->addWidget(navigation_, 1);
    return panel;
}

QWidget *GalleryWindow::build_toolbar(QWidget *parent) {
    auto *bar = new QWidget(parent);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, dialog_scaled(10), 0);
    row->setSpacing(dialog_scaled(10));
    const auto request = [this](const GalleryState &next) { emit state_requested(next); };
    const auto add_selector = [&](const QString &title, const QList<std::pair<QString, QString>> &items, const QString &current,
                                  const std::function<void(const QString &)> &choose) {
        row->addWidget(new CaptionLabel(title));
        auto *selector = new SegmentedWidget(bar);
        selector->setCompact(true);
        for (const auto &[key, label] : items) selector->addItem(key, label, [choose, key] { choose(key); });
        selector->setCurrentItem(current);
        row->addWidget(selector);
        row->addSpacing(dialog_scaled(8));
    };
    add_selector(text("主题"), {{"light", text("浅色")}, {"dark", text("深色")}}, state_.dark ? "dark" : "light", [this, request](const QString &key) {
        auto next = state_;
        next.dark = key == "dark";
        request(next);
    });
    add_selector(text("窗口缩放"), {{"100", "100%"}, {"150", "150%"}, {"200", "200%"}}, QString::number(state_.scale_percent), [this, request](const QString &key) {
        auto next = state_;
        next.scale_percent = key.toInt();
        request(next);
    });
    add_selector(text("语言"), {{"zh", text("简体")}, {"zh_TW", text("繁體")}, {"en", "English"}, {"ja", text("日本語")}, {"ko", text("한국어")}}, state_.language,
                 [this, request](const QString &key) {
                     auto next = state_;
                     next.language = key;
                     request(next);
                 });
    row->addStretch(1);
    return bar;
}

void GalleryWindow::apply_window_sheet() {
    const auto &tokens = get_ui_theme()->tokens();
    const QString sheet = QString("#GalleryWindow { background: transparent; border: none; } QLabel { color: %1; } QStackedWidget { background: transparent; }").arg(tokens.fg) +
                          scrollbar_qss(this) + "QScrollBar::handle:vertical:disabled { background: transparent; }";
    apply_style_sheet(this, sheet);
}

QWidget *GalleryWindow::build_page(int index) {
    auto *scroll = new QScrollArea(stack_);
    scroll->setObjectName("GalleryPage_" + pages()[index].key);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background: transparent; }");
    auto *view = new QWidget;
    set_own_style(view, "background: transparent;");
    auto *layout = new QVBoxLayout(view);
    layout->setContentsMargins(0, 0, dialog_scaled(10), dialog_scaled(24));
    layout->setSpacing(dialog_scaled(14));
    switch (index) {
    case 0: build_colors(layout, view); break;
    case 1: build_metrics(layout, view); break;
    case 2: build_buttons(layout, view); break;
    case 3: build_inputs(layout, view); break;
    case 4: build_choices(layout, view); break;
    case 5: build_cards(layout, view); break;
    case 6: build_popups(layout, view); break;
    case 7: build_dialogs(this, layout, view); break;
    case 8: build_translation(layout, view); break;
    default: build_tools(layout, view); break;
    }
    layout->addStretch(1);
    scroll->setWidget(view);
    return scroll;
}

QScrollArea *GalleryWindow::page_area(int index) {
    if (index < 0 || index >= page_count()) return nullptr;
    auto *current = stack_->widget(index);
    if (auto *built = qobject_cast<QScrollArea *>(current)) return built;
    const bool was_current = stack_->currentIndex() == index;
    auto *page = build_page(index);
    stack_->removeWidget(current);
    current->deleteLater();
    stack_->insertWidget(index, page);
    if (was_current) stack_->setCurrentIndex(index);
    configure_dialog_controls(*page);
    return qobject_cast<QScrollArea *>(page);
}

void GalleryWindow::show_page(int index) {
    if (!page_area(index)) return;
    state_.page = index;
    stack_->setCurrentIndex(index);
    if (navigation_) navigation_->setCurrentItem(pages()[index].key);
}

QDialog *GalleryWindow::make_fluent_dialog() {
    auto *dialog = new FluentDialog(this);
    dialog->setWindowTitle("FluentDialog");
    auto *name_card = new DialogCard(text("名称"), dialog->body());
    name_card->addCaption(text("DialogCard 的说明文字"));
    auto *edit = new LineEdit;
    edit->setText(text("示例文字"));
    name_card->addControl(edit, 1);
    dialog->bodyLayout()->addWidget(name_card);
    auto *switch_card = new DialogCard(text("开关"), dialog->body());
    switch_card->addControl(new SwitchButton);
    dialog->bodyLayout()->addWidget(switch_card);
    dialog->bodyLayout()->addWidget(dialog_hint(dialog->body(), text("对话框卡片下方的提示文字，带信息图标。")));
    dialog->bodyLayout()->addStretch(1);
    auto *reset = new PushButton(text("恢复默认"));
    auto *ok = new PrimaryPushButton(text("确定"));
    auto *cancel = new PushButton(text("取消"));
    connect(ok, &QPushButton::clicked, dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    dialog->setFooterButtons({reset}, {ok, cancel});
    dialog->resize(dialog_scaled(520), dialog_scaled(380));
    dialog->finish_controls();
    return dialog;
}

QDialog *GalleryWindow::make_standard_dialog() {
    auto *dialog = new StandardDialog(this, text("确认操作"), text("这是消息框的正文：标题在上，正文在下，按钮并排铺满底部。"));
    auto *ok = dialog->button_box->addButton(text("确定"), QDialogButtonBox::AcceptRole);
    auto *cancel = dialog->button_box->addButton(text("取消"), QDialogButtonBox::RejectRole);
    connect(ok, &QPushButton::clicked, dialog, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    return dialog;
}

void GalleryWindow::paintEvent(QPaintEvent *) {
    paint_layered_background(this, frosted_corner_radius_, right_area_ ? right_area_->geometry() : QRect(), dialog_scaled(8));
}

void GalleryWindow::resizeEvent(QResizeEvent *event) {
    FrostedFramelessDialog::resizeEvent(event);
    if (titleBar()) titleBar()->resize(width(), titleBar()->height());
}

void init_gallery_runtime(QApplication &application, ToolSettingsManager *config) {
    get_ui_theme()->init(config, &application);
    motion::install_popup_fade(application);
    I18nManager::load_language("zh");
    get_theme().init(config);
    get_ui_scale().init(config);
    get_dialog_scale().init(config);
}
}
