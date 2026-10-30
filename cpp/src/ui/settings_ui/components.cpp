#include "ui/settings_ui/components.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/settings_ui/dialog.h"
#include "ui/settings_ui/search.h"
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QPainter>
#include <QPushButton>
#include <QStackedWidget>
#include <QWheelEvent>

namespace jietuba {
namespace {
// Hands wheel events to the nearest enclosing scroll area instead of the control.
// Forwarded explicitly: Qt does not propagate synthesised wheel events.
class WheelPassThrough : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() != QEvent::Wheel) return false;
        auto *widget = qobject_cast<QWidget *>(obj);
        QWidget *parent = widget ? widget->parentWidget() : nullptr;
        while (parent && !qobject_cast<QAbstractScrollArea *>(parent)) parent = parent->parentWidget();
        if (parent) QApplication::sendEvent(static_cast<QAbstractScrollArea *>(parent)->viewport(), event);
        // Already forwarded; accepted so Qt does not bubble it up the parents again.
        event->accept();
        return true;
    }
};

class Separator : public QWidget {
public:
    explicit Separator(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(1);
        connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
    }
protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), css_color(get_ui_theme()->tokens().separator)); }
};

QString suffix_of(const QString &extra) { return extra.trimmed().isEmpty() ? QString() : " " + extra.trimmed(); }
}

QScrollArea *page_scroll_area(SettingsDialog *dialog) {
    auto *area = new QScrollArea(dialog ? dialog->content_stack : nullptr);
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    return area;
}

void disable_wheel_on_value_controls(QWidget *root) {
    auto *wheel_filter = new WheelPassThrough(root);
    for (auto *widget : root->findChildren<QWidget *>())
        if (qobject_cast<QComboBox *>(widget) || qobject_cast<QAbstractSpinBox *>(widget)) widget->installEventFilter(wheel_filter);
}

QString theme_color(const QString &light, const QString &dark) { return get_ui_theme()->is_dark() ? dark : light; }
QString theme_surface_color() { return theme_color("#FAFBFC", "#202124"); }
QString theme_border_color() { return theme_color("rgba(255, 255, 255, 0.76)", "rgba(255, 255, 255, 0.08)"); }
QString theme_input_background() { return theme_color("rgba(255, 255, 255, 0.78)", "#2B2D31"); }
QString theme_popup_background() { return theme_color("#FFFFFF", "#2A2C30"); }
QString theme_popup_hover_background() { return theme_color("#EAF2FA", "#36393F"); }

QString theme_text_style(int font_size, bool bold, const QString &extra) {
    const QString weight = bold ? " font-weight: 600;" : "";
    return QString("font-size: %1px; color: %2; background: transparent;%3%4")
        .arg(dialog_scaled(font_size))
        .arg(get_ui_theme()->tokens().text, weight, suffix_of(extra));
}

QString theme_caption_style(int font_size, const QString &extra) {
    return QString("font-size: %1px; color: %2; background: transparent;%3").arg(dialog_scaled(font_size)).arg(get_ui_theme()->tokens().text_muted, suffix_of(extra));
}

void apply_theme_text_style(QWidget *widget, int font_size, bool bold, const QString &extra, bool caption) {
    widget->setProperty("_ui_theme_text_spec", QVariantList{font_size, bold, extra, caption});
    apply_style_sheet(widget, caption ? theme_caption_style(font_size, extra) : theme_text_style(font_size, bold, extra));
}

void refresh_theme_widget_styles(QWidget *root) {
    QList<QWidget *> widgets{root};
    widgets += root->findChildren<QWidget *>();
    for (auto *widget : widgets) {
        const auto spec = widget->property("_ui_theme_text_spec").toList();
        if (spec.size() == 4) apply_theme_text_style(widget, spec[0].toInt(), spec[1].toBool(), spec[2].toString(), spec[3].toBool());
    }
}

QString theme_menu_style() {
    const auto &text = get_ui_theme()->tokens().text;
    return QString(R"(
        QMenu {
            background-color: %1;
            color: %2;
            border: 1px solid %3;
            border-radius: %4px;
            padding: %5px 0;
        }
        QMenu::item {
            padding: %6px %7px;
            font-size: %8px;
            color: %2;
            background: transparent;
        }
        QMenu::item:selected {
            background-color: %9;
        }
    )")
        .arg(theme_popup_background(), text, theme_border_color())
        .arg(dialog_scaled(6))
        .arg(dialog_scaled(4))
        .arg(dialog_scaled(6))
        .arg(dialog_scaled(20))
        .arg(dialog_scaled(14))
        .arg(theme_popup_hover_background());
}

SettingsCardGroup::SettingsCardGroup(const QString &title, QWidget *parent, const QString &tone) : SettingCardGroup(title, parent, tone) {
    // No vertical surplus: the page ends with addStretch(), which should take the extra
    // height. Otherwise the group spreads it between cards as a gap above the group and
    // pushes later cards out of view.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
}

void SettingsCardGroup::refreshHeight() {
    // The column width changes how descriptions wrap, so it comes before the height.
    sync_control_column();
    // Both bounds are fixed: with only a minimum, the card container would take all the
    // surplus height the page gives the group and spread it between cards.
    setFixedHeight(cardLayout->heightForWidth(width()) + dialog_scaled(46));
}

void SettingsCardGroup::showEvent(QShowEvent *event) {
    SettingCardGroup::showEvent(event);
    // Card heights are only accurate once shown.
    refreshHeight();
}

void SettingsCardGroup::resizeEvent(QResizeEvent *event) {
    SettingCardGroup::resizeEvent(event);
    // A new width changes how many lines wrapped descriptions take.
    refreshHeight();
}

void adjust_button_width(QPushButton *button, int min_width, int horizontal_padding) {
    button->ensurePolished();
    const int content_width = button->sizeHint().width() + dialog_scaled(horizontal_padding);
    button->setMinimumWidth(std::max(dialog_scaled(min_width), content_width));
}

WhiteCard::WhiteCard(QWidget *parent) : QFrame(parent) {
    setFrameShape(QFrame::NoFrame);
    setAttribute(Qt::WA_StyledBackground, false);
}

QSize WhiteCard::minimumSizeHint() const { return QSize(0, std::max(minimumHeight(), layout() ? layout()->minimumSize().height() : 0)); }

ToggleSwitch::ToggleSwitch(QWidget *parent) : SwitchButton(parent) {
    setOnText("");
    setOffText("");
    connect(this, &SwitchButton::checkedChanged, this, &ToggleSwitch::toggled);
}

SectionCard::SectionCard(const IconSource &icon, const QString &title, const QString &caption, QWidget *parent) : QFrame(parent) {
    setFrameShape(QFrame::NoFrame);
    bodyLayout = new QVBoxLayout(this);
    bodyLayout->setContentsMargins(dialog_scaled(20), 0, dialog_scaled(20), dialog_scaled(4));
    bodyLayout->setSpacing(0);

    auto *header = new QWidget(this);
    header->setFixedHeight(dialog_scaled(50));
    auto *header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(0, 0, 0, 0);
    header_layout->setSpacing(dialog_scaled(10));
    header_layout->addWidget(new IconBadge(icon, QString(), header, 20, 19, false));
    titleLabel = new QLabel(title, header);
    apply_theme_text_style(titleLabel, 15, true);
    header_layout->addWidget(titleLabel);
    if (!caption.isEmpty()) {
        captionLabel = new QLabel(caption, header);
        apply_theme_text_style(captionLabel, 12, false, {}, true);
        header_layout->addWidget(captionLabel);
    }
    header_layout->addStretch(1);
    bodyLayout->addWidget(header);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

void SectionCard::addRow(QWidget *widget) { add_separated_row(bodyLayout, widget); }

void SectionCard::addWidget(QWidget *widget) { bodyLayout->addWidget(widget); }

void SectionCard::paintEvent(QPaintEvent *) {
    const auto &t = get_ui_theme()->tokens();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(css_color(t.border));
    painter.setBrush(css_color(t.surface));
    const int radius = dialog_scaled(15);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

void add_separated_row(QVBoxLayout *layout, QWidget *widget) {
    layout->addWidget(new Separator(widget->parentWidget()));
    layout->addWidget(widget);
}

IconSource icon_ref(const IconSource &ref) {
    if (const auto *name = std::get_if<QString>(&ref)) return ResourceManager::get_icon_path(*name);
    return ref;
}

QLabel *row_label(QWidget *parent, const QString &text) {
    auto *label = new QLabel(text, parent);
    apply_theme_text_style(label, 14, false, "font-weight: 500;");
    mark_search_row(parent, label);
    return label;
}
}
