#include "ui/fluent_lite/cards.h"
#include "core/qt_utils.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/switch.h"
#include <QPainter>

namespace jietuba {
namespace {
constexpr int CARD_LEFT = 16;
constexpr int CARD_GAP = 14;
constexpr int TILE_SIZE = 34;
}

const ToneColors &feature_tone(const QString &tone) {
    static const QHash<QString, ToneColors> tones = {
        {"capture", {"#F3E3E3", "#B0575A", "rgba(227, 154, 156, 0.16)", "#E39A9C"}},
        {"clipboard", {"#E1EEE6", "#4E8A67", "rgba(140, 201, 165, 0.15)", "#8CC9A5"}},
        {"pin", {"#F5EAD9", "#9E6B28", "rgba(224, 176, 112, 0.15)", "#E0B070"}},
        {"translate", {"#E9E4F3", "#7A63A8", "rgba(183, 163, 227, 0.16)", "#B7A3E3"}},
        {"neutral", {"#E3EAF0", "#526D85", "rgba(111, 143, 171, 0.20)", "#AFC3D4"}},
    };
    const auto found = tones.constFind(tone);
    return found != tones.constEnd() ? *found : *tones.constFind("neutral");
}

int card_right_margin() { return dialog_scaled(24); }
int card_text_left() { return dialog_scaled(CARD_LEFT + TILE_SIZE + CARD_GAP); }

IconBadge::IconBadge(const IconSource &icon, const QString &tone, QWidget *parent, int size, int icon_size, bool muted)
    : QWidget(parent), icon_(icon), tone_(tone), icon_size_(icon_size), muted_(muted) {
    setFixedSize(dialog_scaled(size), dialog_scaled(size));
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

void IconBadge::setTone(const QString &tone) {
    tone_ = tone;
    update();
}

void IconBadge::paintEvent(QPaintEvent *) {
    const auto &t = get_ui_theme()->tokens();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QString fg;
    if (tone_.isEmpty()) {
        fg = muted_ ? t.text_muted : t.text;
    } else {
        const auto &colors = feature_tone(tone_);
        painter.setPen(Qt::NoPen);
        painter.setBrush(css_color(t.is_dark() ? colors.dark_bg : colors.light_bg));
        const int radius = dialog_scaled(10);
        painter.drawRoundedRect(QRectF(rect()), radius, radius);
        fg = t.is_dark() ? colors.dark_fg : colors.light_fg;
    }
    if (std::holds_alternative<std::monostate>(icon_)) return;
    const int side = dialog_scaled(icon_size_);
    QRect target(0, 0, side, side);
    target.moveCenter(rect().center());
    const auto icon = std::holds_alternative<QIcon>(icon_) ? std::get<QIcon>(icon_) : tinted_icon(icon_, fg);
    icon.paint(&painter, target);
}

int ExpandLayout::heightForWidth(int width) const {
    const auto margins = contentsMargins();
    const int available_width = std::max(0, width - margins.left() - margins.right());
    int height = margins.top() + margins.bottom();
    int visible = 0;
    for (auto *widget : layout_widgets(const_cast<ExpandLayout *>(this))) {
        if (widget->isHidden()) continue;
        // Word-wrapped descriptions report phantom lines in their width-independent hint.
        const int preferred = widget->hasHeightForWidth() ? widget->heightForWidth(available_width) : widget->sizeHint().height();
        height += std::max(widget->minimumHeight(), preferred);
        ++visible;
    }
    return height + std::max(0, visible - 1) * spacing();
}

SettingCard::SettingCard(const IconSource &icon, const QString &title, const QString &content, QWidget *parent, const QString &tone)
    : QFrame(parent), own_tone_(tone) {
    setObjectName("FluentLiteSettingCard");
    setMinimumHeight(dialog_scaled(62));
    hBoxLayout = new QHBoxLayout(this);
    hBoxLayout->setContentsMargins(dialog_scaled(CARD_LEFT), dialog_scaled(9), card_right_margin(), dialog_scaled(9));
    hBoxLayout->setSpacing(dialog_scaled(CARD_GAP));
    iconBadge = new IconBadge(icon, tone.isEmpty() ? QString("neutral") : tone, this, TILE_SIZE, 19);
    hBoxLayout->addWidget(iconBadge);
    text_layout_ = new QVBoxLayout;
    text_layout_->setContentsMargins(0, 0, 0, 0);
    text_layout_->setSpacing(dialog_scaled(3));
    titleLabel = new BodyLabel(title, this);
    text_layout_->addWidget(titleLabel);
    contentLabel = new CaptionLabel(content, this);
    contentLabel->setWordWrap(true);
    contentLabel->setVisible(!content.isEmpty());
    text_layout_->addWidget(contentLabel);
    hBoxLayout->addLayout(text_layout_, 1);
    controlContainer = new QWidget(this);
    set_own_style(controlContainer, "background: transparent; border: none;");
    controlLayout = new QHBoxLayout(controlContainer);
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(dialog_scaled(8));
    hBoxLayout->addWidget(controlContainer);
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void SettingCard::_apply_theme() {
    const auto &t = ui_tokens(this);
    apply_style_sheet(this, QString(R"(
            QFrame#FluentLiteSettingCard { background: transparent; border: none; border-radius: %1px; }
            QFrame#FluentLiteSettingCard:hover { background: %2; border: none; }
        )").arg(dialog_scaled(12)).arg(t.surface_subtle));
    set_own_style(titleLabel, QString("color: %1; font: 600 %2px %3; background: transparent; border: none;").arg(t.text).arg(dialog_scaled(13)).arg(FONT_FAMILY));
    set_own_style(contentLabel, QString("color: %1; font: %2px %3; background: transparent; border: none;").arg(t.text_muted).arg(dialog_scaled(12)).arg(FONT_FAMILY));
}

void SettingCard::addControl(QWidget *widget, std::optional<Qt::Alignment> align) {
    Qt::Alignment alignment = Qt::AlignVCenter;
    if (align) alignment |= *align;
    controlLayout->addWidget(widget, align ? 0 : 1, alignment);
    setControlColumnWidth(controlColumnHint());
}

int SettingCard::controlColumnHint() const {
    int total = 0, count = 0;
    for (auto *widget : layout_widgets(controlLayout)) {
        if (widget->isHidden()) continue;
        total += std::max(widget->sizeHint().width(), widget->minimumWidth());
        ++count;
    }
    if (!count) return 0;
    return total + controlLayout->spacing() * (count - 1);
}

void SettingCard::setControlColumnWidth(int width) {
    controlContainer->setFixedWidth(std::min(width, dialog_scaled(CONTROL_COLUMN_MAX)));
    // The group unifies widths in showEvent after a first layout pass; relayout now
    // so right-aligned switches do not wait for the next resize.
    hBoxLayout->activate();
    controlLayout->activate();
}

void SettingCard::setTone(const QString &tone) { iconBadge->setTone(tone); }
void SettingCard::setTitle(const QString &title) { titleLabel->setText(title); }

void SettingCard::setContent(const QString &content) {
    contentLabel->setText(content);
    contentLabel->setVisible(!content.isEmpty());
}

QSize SettingCard::minimumSizeHint() const {
    return QSize(dialog_scaled(180), std::max(minimumHeight(), hBoxLayout->minimumSize().height()));
}

SwitchSettingCard::SwitchSettingCard(const IconSource &icon, const QString &title, const QString &content, QWidget *parent, const QString &tone)
    : SettingCard(icon, title, content, parent, tone) {
    switchButton = new SwitchButton(this);
    connect(switchButton, &SwitchButton::checkedChanged, this, &SwitchSettingCard::checkedChanged);
    addControl(switchButton, Qt::AlignRight);
}

void SwitchSettingCard::setChecked(bool checked) { switchButton->setChecked(checked); }
bool SwitchSettingCard::isChecked() const { return switchButton->isChecked(); }

SubSwitchSettingCard::SubSwitchSettingCard(const QString &title, const QString &content, QWidget *parent)
    : SwitchSettingCard(IconSource{}, title, content, parent) {
    iconBadge->hide();
    const auto margins = hBoxLayout->contentsMargins();
    hBoxLayout->setContentsMargins(card_text_left(), margins.top(), margins.right(), margins.bottom());
    setMinimumHeight(dialog_scaled(52));
}

SettingCardGroup::SettingCardGroup(const QString &title, QWidget *parent, const QString &tone) : QWidget(parent), tone_(tone) {
    setObjectName("FluentLiteSettingCardGroup");
    vBoxLayout = new QVBoxLayout(this);
    vBoxLayout->setContentsMargins(0, 0, 0, 0);
    vBoxLayout->setSpacing(dialog_scaled(10));
    titleLabel = new QLabel(title, this);
    vBoxLayout->addWidget(titleLabel);
    card_container_ = new QFrame(this);
    card_container_->setObjectName("FluentLiteGroupBody");
    cardLayout = new ExpandLayout(card_container_);
    cardLayout->setContentsMargins(dialog_scaled(2), dialog_scaled(2), dialog_scaled(2), dialog_scaled(2));
    cardLayout->setSpacing(0);
    vBoxLayout->addWidget(card_container_);
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void SettingCardGroup::_apply_theme() {
    const auto &t = ui_tokens(this);
    set_own_style(titleLabel, QString("color: %1; font: 600 %2px %3; padding: %4px %5px 0 %5px; background: transparent;")
                                  .arg(t.text).arg(dialog_scaled(12)).arg(FONT_FAMILY).arg(dialog_scaled(2)).arg(dialog_scaled(8)));
    apply_style_sheet(card_container_, QString(R"(
            QFrame#FluentLiteGroupBody {
                background: %1;
                border: 1px solid %2;
                border-radius: %3px;
            }
        )").arg(t.surface, t.border).arg(dialog_scaled(15)));
    for (auto *separator : separators_)
        set_own_style(separator, QString("background: %1; border: none; margin-left: %2px;").arg(t.separator).arg(card_text_left()));
}

void SettingCardGroup::addSettingCard(QWidget *card, bool with_separator) {
    if (!cards_.isEmpty() && with_separator) {
        auto *separator = new QFrame(card_container_);
        separator->setFixedHeight(dialog_scaled(1));
        set_own_style(separator, QString("background: %1; border: none; margin-left: %2px;").arg(ui_tokens(this).separator).arg(card_text_left()));
        cardLayout->addWidget(separator);
        separators_.append(separator);
        leading_separators_.insert(card, separator);
    }
    card->setParent(card_container_);
    if (auto *setting = qobject_cast<SettingCard *>(card); setting && !setting->has_own_tone()) setting->setTone(tone_);
    cardLayout->addWidget(card);
    cards_.append(card);
    adjustSize();
}

void SettingCardGroup::addSettingCards(const QList<QWidget *> &cards) {
    for (auto *card : cards) addSettingCard(card);
}

void SettingCardGroup::setCardVisible(QWidget *card, bool visible) {
    card->setVisible(visible);
    if (auto *separator = leading_separators_.value(card)) separator->setVisible(visible);
}

void SettingCardGroup::sync_control_column() {
    QList<SettingCard *> cards;
    for (auto *card : findChildren<SettingCard *>())
        if (card->isVisibleTo(this)) cards.append(card);
    if (cards.isEmpty()) return;
    int width = 0;
    for (auto *card : cards) width = std::max(width, card->controlColumnHint());
    for (auto *card : cards) card->setControlColumnWidth(width);
}

void SettingCardGroup::adjustSize() {
    setMinimumHeight(vBoxLayout->sizeHint().height());
    updateGeometry();
}

QSize SettingCardGroup::minimumSizeHint() const {
    // Long translated text should be compressed by the scroll area, not scrolled horizontally.
    return QSize(0, vBoxLayout->minimumSize().height());
}

void SettingCardGroup::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    sync_control_column();
    adjustSize();
}

SimpleCardWidget::SimpleCardWidget(QWidget *parent) : QFrame(parent) {
    setObjectName("FluentLiteSimpleCard");
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void SimpleCardWidget::_apply_theme() {
    const auto &t = ui_tokens(this);
    apply_style_sheet(this, QString("QFrame#FluentLiteSimpleCard { background: %1; border: 1px solid %2; border-radius: 14px; }").arg(t.surface, t.border));
}
}
