#include "ui/fluent_lite/fluent_dialog.h"
#include "core/qt_utils.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/cards.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

namespace jietuba {
namespace {
// The caption row of the settings window: the logo lines up with the content below it.
class DialogTitleBar : public FluentTitleBar {
public:
    using FluentTitleBar::FluentTitleBar;
protected:
    void apply_layout_metrics() override {
        hBoxLayout->setContentsMargins(widget_scaled(this, 14), 0, 0, 0);
        titleLabel->setIndent(widget_scaled(this, 8));
    }
};

// QDialogButtonBox's Windows order.
int role_rank(QDialogButtonBox::ButtonRole role) {
    switch (role) {
    case QDialogButtonBox::ResetRole: return 0;
    case QDialogButtonBox::YesRole: return 1;
    case QDialogButtonBox::AcceptRole: return 2;
    case QDialogButtonBox::DestructiveRole: return 4;
    case QDialogButtonBox::NoRole: return 5;
    case QDialogButtonBox::ActionRole: return 6;
    case QDialogButtonBox::RejectRole: return 7;
    case QDialogButtonBox::ApplyRole: return 8;
    case QDialogButtonBox::HelpRole: return 9;
    default: return 10;
    }
}
}

void paint_dialog_frame(QWidget *dialog, int radius, const QRect &band, const QString &band_color) {
    const auto &t = ui_tokens(dialog);
    QPainter painter(dialog);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(dialog->rect(), Qt::transparent);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const auto frame = QRectF(dialog->rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath window;
    window.addRoundedRect(frame, radius, radius);
    painter.setPen(Qt::NoPen);
    painter.setBrush(css_color(t.page_background));
    painter.drawPath(window);
    if (band.isValid()) {
        painter.setClipPath(window);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillRect(QRect(0, band.top(), dialog->width(), band.height()), css_color(band_color));
        painter.setPen(css_color(t.layer_border));
        if (band.top() > 0) painter.drawLine(0, band.top(), dialog->width(), band.top());
        if (band.bottom() < dialog->height() - 1) painter.drawLine(0, band.bottom(), dialog->width(), band.bottom());
        painter.setClipping(false);
        painter.setRenderHint(QPainter::Antialiasing);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(css_color(t.window_border), 1.0));
    painter.drawPath(window);
}

QWidget *dialog_hint(QWidget *parent, const QString &text) {
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(dialog_scaled(4), 0, 0, 0);
    layout->setSpacing(dialog_scaled(8));
    layout->addWidget(new IconBadge(FluentIcon::Info, {}, row, 16, 14), 0, Qt::AlignTop);
    auto *label = new CaptionLabel(text, row);
    label->setWordWrap(true);
    layout->addWidget(label, 1);
    return row;
}

FluentDialog::FluentDialog(QWidget *parent) : FrostedFramelessDialog(parent) {
    scale_dialog_font(*this);
    setWindowIcon(ResourceManager::get_icon_by_name(QString::fromUtf8("托盘.svg")));
    auto *title_bar = new DialogTitleBar(this);
    setTitleBar(title_bar);
    title_bar->minBtn->hide();
    title_bar->maxBtn->hide();
    title_bar->setDoubleClickEnabled(false);
    configure_dialog_control(*title_bar);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, title_bar->height(), 0, 0);
    root->setSpacing(0);
    body_ = new QWidget(this);
    body_->setObjectName("fluent_dialog_body");
    body_layout_ = new QVBoxLayout(body_);
    body_layout_->setContentsMargins(dialog_scaled(24), dialog_scaled(16), dialog_scaled(24), dialog_scaled(16));
    body_layout_->setSpacing(dialog_scaled(4));
    root->addWidget(body_, 1);
    footer_ = new QWidget(this);
    footer_->setObjectName("fluent_dialog_footer");
    footer_layout_ = new QHBoxLayout(footer_);
    footer_layout_->setContentsMargins(dialog_scaled(24), dialog_scaled(18), dialog_scaled(24), dialog_scaled(18));
    footer_layout_->setSpacing(dialog_scaled(8));
    root->addWidget(footer_);
}

void FluentDialog::setFooterButtons(const QList<QPushButton *> &left, const QList<QPushButton *> &right) {
    for (auto *button : left) footer_layout_->addWidget(button);
    footer_layout_->addStretch(1);
    for (auto *button : right) footer_layout_->addWidget(button);
    closing_buttons_ = right;
}

void FluentDialog::finish_controls() {
    configure_dialog_controls(*this);
    if (titleBar()) layout()->setContentsMargins(0, titleBar()->height(), 0, 0);
    int width = dialog_scaled(96);
    for (auto *button : closing_buttons_) width = std::max(width, button->sizeHint().width());
    for (auto *button : closing_buttons_) button->setMinimumWidth(width);
}

void FluentDialog::setStaysOnTop() {
    // Changing window flags recreates the native window, which loses the frameless styles.
    setWindowFlag(Qt::WindowStaysOnTopHint, true);
    helper_.install();
}

int FluentDialog::height_showing(QScrollArea *scroll, int width) const {
    // Before the first show, layouts keep the size hints their children had when first asked, such as
    // a panel before its rows were added; each child that changes has to say so.
    auto *content = scroll->widget();
    for (auto *panel : content->findChildren<QWidget *>())
        if (panel->layout()) panel->updateGeometry();
    const auto refresh = [this, scroll] {
        for (QWidget *widget = scroll; widget && widget != this; widget = widget->parentWidget()) widget->updateGeometry();
        layout()->invalidate();
    };
    const auto margins = body_layout_->contentsMargins();
    // Wrapped text is measured at the width the content gets.
    const int inner = width - margins.left() - margins.right();
    const int content_height = content->hasHeightForWidth() ? content->heightForWidth(inner) : content->sizeHint().height();
    // Pinned to its content for one measurement, the scroll area counts like any other row.
    const int minimum = scroll->minimumHeight(), maximum = scroll->maximumHeight();
    scroll->setFixedHeight(content_height);
    refresh();
    const int height = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width) : layout()->totalSizeHint().height();
    scroll->setMinimumHeight(minimum);
    scroll->setMaximumHeight(maximum);
    refresh();
    return height;
}

void FluentDialog::paintEvent(QPaintEvent *) { paint_dialog_frame(this, frosted_corner_radius_, body_->geometry(), ui_tokens(this).layer); }

DialogPanel::DialogPanel(QWidget *parent) : QFrame(parent) {
    // A property rather than the object name, which callers may set for their own use.
    setProperty("fluentDialogCard", true);
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void DialogPanel::_apply_theme() {
    const auto &t = ui_tokens(this);
    apply_style_sheet(this, QString("QFrame[fluentDialogCard=\"true\"] { background: %1; border: 1px solid %2; border-radius: %3px; }")
                                .arg(t.card, t.card_border)
                                .arg(widget_scaled(this, 4)));
}

DialogCard::DialogCard(const QString &title, QWidget *parent) : DialogPanel(parent) {
    setMinimumHeight(dialog_scaled(68));
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(dialog_scaled(16), dialog_scaled(12), dialog_scaled(16), dialog_scaled(12));
    row->setSpacing(dialog_scaled(20));
    textLayout = new QVBoxLayout;
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(dialog_scaled(1));
    titleLabel = new BodyLabel(title, this);
    textLayout->addWidget(titleLabel);
    row->addLayout(textLayout, 1);
    controlLayout = new QHBoxLayout;
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(dialog_scaled(8));
    row->addLayout(controlLayout);
}

CaptionLabel *DialogCard::addCaption(const QString &text) {
    auto *caption = new CaptionLabel(text, this);
    caption->setWordWrap(true);
    textLayout->addWidget(caption);
    return caption;
}

void DialogCard::setCaptionLabel(QLabel *label) {
    label->setParent(this);
    textLayout->addWidget(label);
}

void DialogCard::addControl(QWidget *control, int stretch) { controlLayout->addWidget(control, stretch, Qt::AlignVCenter); }

DialogButtonRow::DialogButtonRow(QWidget *parent) : QWidget(parent) {
    setObjectName("dialog_button_row");
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(dialog_scaled(24), dialog_scaled(24), dialog_scaled(24), dialog_scaled(24));
    layout_->setSpacing(dialog_scaled(8));
}

QPushButton *DialogButtonRow::addButton(const QString &text, QDialogButtonBox::ButtonRole role) {
    const bool has_primary = std::any_of(buttons_.cbegin(), buttons_.cend(), [](const auto &entry) { return qobject_cast<PrimaryPushButton *>(entry.first); });
    const bool accepting = role == QDialogButtonBox::YesRole || role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::ActionRole;
    QPushButton *button = accepting && !has_primary ? new PrimaryPushButton(text, this) : new PushButton(text, this);
    // Enter takes the button drawn as the one to press.
    button->setDefault(qobject_cast<PrimaryPushButton *>(button));
    configure_dialog_control(*button);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const auto at = std::upper_bound(buttons_.begin(), buttons_.end(), role_rank(role),
                                     [](int rank, const auto &entry) { return rank < role_rank(entry.second); });
    buttons_.insert(at, {button, role});
    relayout();
    return button;
}

QList<QAbstractButton *> DialogButtonRow::buttons() const {
    QList<QAbstractButton *> result;
    for (const auto &[button, role] : buttons_) result.append(button);
    return result;
}

QDialogButtonBox::ButtonRole DialogButtonRow::buttonRole(QAbstractButton *button) const {
    for (const auto &[candidate, role] : buttons_)
        if (candidate == button) return role;
    return QDialogButtonBox::InvalidRole;
}

QSize DialogButtonRow::minimumSizeHint() const { return layout_->minimumSize(); }

void DialogButtonRow::relayout() {
    int width = 0;
    for (const auto &[button, role] : buttons_) {
        layout_->removeWidget(button);
        width = std::max(width, button->sizeHint().width());
    }
    for (const auto &[button, role] : buttons_) {
        button->setMinimumWidth(width);
        layout_->addWidget(button, 1);
    }
}
}
