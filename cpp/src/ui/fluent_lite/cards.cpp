#include "ui/fluent_lite/cards.h"
#include "core/qt_utils.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/switch.h"
#include <QPainter>
#include <QPolygonF>

namespace jietuba {
namespace {
constexpr int CARD_LEFT = 16;
constexpr int CARD_RIGHT = 16;
constexpr int CARD_GAP = 20;
constexpr int ICON_SIZE = 20;
constexpr int CARD_RADIUS = 4;
constexpr int CARD_MIN_HEIGHT = 68;
constexpr int ROW_MIN_HEIGHT = 52;
constexpr int ARROW_SIZE = 32;

// The expander's arrow: a chevron that points up while the rows are shown.
class ArrowButton : public QAbstractButton {
public:
    explicit ArrowButton(QWidget *parent) : QAbstractButton(parent) {
        setCheckable(true);
        setChecked(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setFixedSize(dialog_scaled(ARROW_SIZE), dialog_scaled(ARROW_SIZE));
        connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
    }
protected:
    void paintEvent(QPaintEvent *) override {
        const auto &t = get_ui_theme()->tokens();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || hasFocus()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(css_color(t.item_hover));
            painter.drawRoundedRect(QRectF(rect()), dialog_scaled(CARD_RADIUS), dialog_scaled(CARD_RADIUS));
        }
        painter.setPen(QPen(css_color(t.fg_muted), std::max(1.0, dialog_scaled_f(1.4)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const double half = dialog_scaled_f(4), drop = dialog_scaled_f(2) * (isChecked() ? -1 : 1);
        const QPointF center = QRectF(rect()).center();
        painter.drawPolyline(QPolygonF{QPointF(center.x() - half, center.y() - drop), QPointF(center.x(), center.y() + drop),
                                       QPointF(center.x() + half, center.y() - drop)});
    }
    void enterEvent(QEnterEvent *event) override {
        QAbstractButton::enterEvent(event);
        update();
    }
    void leaveEvent(QEvent *event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }
};

QString separator_style() { return QString("background: %1; border: none;").arg(get_ui_theme()->tokens().divider); }
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

int card_right_margin() { return dialog_scaled(CARD_RIGHT); }
int card_text_left() { return dialog_scaled(CARD_LEFT + ICON_SIZE + CARD_GAP); }
int expander_gutter() { return dialog_scaled(ARROW_SIZE + CARD_GAP); }
int card_width_besides_title() { return card_text_left() + dialog_scaled(CARD_GAP + SettingCard::CONTROL_COLUMN_MAX) + card_right_margin() + expander_gutter(); }

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
        fg = muted_ ? t.fg_muted : t.fg;
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
    setMinimumHeight(dialog_scaled(CARD_MIN_HEIGHT));
    hBoxLayout = new QHBoxLayout(this);
    hBoxLayout->setContentsMargins(dialog_scaled(CARD_LEFT), dialog_scaled(12), card_right_margin(), dialog_scaled(12));
    hBoxLayout->setSpacing(dialog_scaled(CARD_GAP));
    iconBadge = new IconBadge(icon, QString(), this, ICON_SIZE, ICON_SIZE, false);
    hBoxLayout->addWidget(iconBadge);
    text_layout_ = new QVBoxLayout;
    text_layout_->setContentsMargins(0, 0, 0, 0);
    text_layout_->setSpacing(dialog_scaled(1));
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
    if (embedded_) {
        apply_style_sheet(this, "QFrame#FluentLiteSettingCard { background: transparent; border: none; }");
        return;
    }
    const auto &t = ui_tokens(this);
    apply_style_sheet(this, QString("QFrame#FluentLiteSettingCard { background: %1; border: 1px solid %2; border-radius: %3px; %4 }")
                                .arg(t.card, t.card_border)
                                .arg(dialog_scaled(CARD_RADIUS))
                                .arg(joined_below_ ? "border-bottom-left-radius: 0; border-bottom-right-radius: 0;" : ""));
}

void SettingCard::setEmbedded(bool embedded) {
    embedded_ = embedded;
    auto margins = hBoxLayout->contentsMargins();
    margins.setRight(card_right_margin() + (embedded ? expander_gutter() : 0));
    hBoxLayout->setContentsMargins(margins);
    _apply_theme();
}

void SettingCard::setJoinedBelow(bool joined) {
    if (joined_below_ == joined) return;
    joined_below_ = joined;
    _apply_theme();
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

void SettingCard::setTone(const QString &tone) { own_tone_ = tone; }
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
    setMinimumHeight(dialog_scaled(ROW_MIN_HEIGHT));
}

CardSurface::CardSurface(QWidget *parent) : QFrame(parent) {
    setFrameShape(QFrame::NoFrame);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { update(); });
}

void CardSurface::setChrome(bool chrome) {
    chrome_ = chrome;
    update();
}

void CardSurface::paintEvent(QPaintEvent *event) {
    if (chrome_) {
        const auto &t = get_ui_theme()->tokens();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(css_color(t.card_border));
        painter.setBrush(css_color(t.card));
        const int radius = dialog_scaled(CARD_RADIUS);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }
    QFrame::paintEvent(event);
}

SettingExpander::SettingExpander(SettingCard *header, QWidget *parent) : QWidget(parent), header_(header) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    header_->setParent(this);
    layout->addWidget(header_);
    arrow_ = new ArrowButton(header_);
    header_->hBoxLayout->addWidget(arrow_, 0, Qt::AlignVCenter);
    body_ = new FoldBox(this);
    body_->setObjectName("FluentLiteExpanderBody");
    body_layout_ = new QVBoxLayout(body_->content());
    body_layout_->setContentsMargins(0, 0, 0, 0);
    body_layout_->setSpacing(0);
    layout->addWidget(body_);
    connect(body_, &FoldBox::resized, this, [this] {
        header_->setJoinedBelow(!body_->isHidden());
        emit resized();
    });
    arrow_->hide();
    connect(arrow_, &QAbstractButton::clicked, this, [this] { setExpanded(arrow_->isChecked()); });
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void SettingExpander::_apply_theme() {
    const auto &t = ui_tokens(this);
    apply_style_sheet(body_, QString("QFrame#FluentLiteExpanderBody { background: %1; border: 1px solid %2; border-top: none;"
                                     " border-bottom-left-radius: %3px; border-bottom-right-radius: %3px; }")
                                 .arg(t.card_body, t.card_border)
                                 .arg(dialog_scaled(CARD_RADIUS)));
    body_->setBackdrop([this] { return painted_over(css_color(ui_tokens(this).card_body), layer_backdrop(this)); }, dialog_scaled(CARD_RADIUS) - 1);
    for (auto *separator : separators_)
        if (separator) set_own_style(separator, separator_style());
}

void SettingExpander::addItem(SettingCard *item) {
    if (items_.isEmpty()) {
        separators_.append(nullptr);
    } else {
        auto *separator = new QFrame(body_->content());
        separator->setFixedHeight(dialog_scaled(1));
        set_own_style(separator, separator_style());
        body_layout_->addWidget(separator);
        separators_.append(separator);
    }
    item->setParent(body_->content());
    item->setEmbedded(true);
    body_layout_->addWidget(item);
    items_.append(item);
    refreshItems();
}

void SettingExpander::refreshItems(bool animated) {
    bool any = false;
    for (int i = 0; i < items_.size(); ++i) {
        const bool visible = !items_[i]->isHidden();
        // A line only between two shown rows.
        if (auto *separator = separators_[i]) separator->setVisible(visible && any);
        any = any || visible;
    }
    body_->setOpen(any && expanded_, animated);
    arrow_->setVisible(any);
    header_->setJoinedBelow(!body_->isHidden());
    emit resized();
}

void SettingExpander::setExpanded(bool expanded, bool animated) {
    expanded_ = expanded;
    arrow_->setChecked(expanded);
    refreshItems(animated);
}

SettingCardGroup::SettingCardGroup(const QString &title, QWidget *parent, const QString &tone) : QWidget(parent), tone_(tone) {
    setObjectName("FluentLiteSettingCardGroup");
    vBoxLayout = new QVBoxLayout(this);
    vBoxLayout->setContentsMargins(0, 0, 0, 0);
    vBoxLayout->setSpacing(dialog_scaled(8));
    titleLabel = new QLabel(title, this);
    vBoxLayout->addWidget(titleLabel);
    card_container_ = new QWidget(this);
    card_container_->setObjectName("FluentLiteGroupBody");
    cardLayout = new ExpandLayout(card_container_);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(dialog_scaled(4));
    vBoxLayout->addWidget(card_container_);
    _apply_theme();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void SettingCardGroup::_apply_theme() {
    const auto &t = ui_tokens(this);
    // The top padding adds to the page's spacing between groups.
    set_own_style(titleLabel, QString("color: %1; font: 600 %2px %3; padding: %4px 0 0 %5px; background: transparent;")
                                  .arg(t.fg).arg(dialog_scaled(14)).arg(FONT_FAMILY).arg(dialog_scaled(16)).arg(dialog_scaled(1)));
}

void SettingCardGroup::addSettingCard(QWidget *card) {
    // A switch row folds into the card before it, unless that is a switch row itself.
    if (auto *sub = qobject_cast<SubSwitchSettingCard *>(card)) {
        auto *header = qobject_cast<SettingCard *>(last_top_);
        if (header && !qobject_cast<SubSwitchSettingCard *>(header)) {
            auto *expander = expanders_.value(header);
            if (!expander) {
                const int index = cardLayout->indexOf(header);
                cardLayout->removeWidget(header);
                expander = new SettingExpander(header, card_container_);
                cardLayout->insertWidget(index, expander);
                expanders_.insert(header, expander);
                connect(expander, &SettingExpander::resized, this, [this] { refreshHeight(); });
                expander->setVisible(!header->isHidden());
            }
            expander->addItem(sub);
            hosts_.insert(sub, expander);
            cards_.append(card);
            adjustSize();
            return;
        }
    }
    card->setParent(card_container_);
    if (auto *surface = qobject_cast<CardSurface *>(card)) surface->setChrome(true);
    cardLayout->addWidget(card);
    cards_.append(card);
    last_top_ = card;
    adjustSize();
}

void SettingCardGroup::addSettingCards(const QList<QWidget *> &cards) {
    for (auto *card : cards) addSettingCard(card);
}

void SettingCardGroup::setCardVisible(QWidget *card, bool visible) {
    card->setVisible(visible);
    if (auto *expander = expanders_.value(card)) expander->setVisible(visible);
    if (auto *host = hosts_.value(card)) host->refreshItems();
    refreshHeight();
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

int SettingCardGroup::header_height() const {
    const auto margins = vBoxLayout->contentsMargins();
    return margins.top() + margins.bottom() + titleLabel->sizeHint().height() + vBoxLayout->spacing();
}

void SettingCardGroup::refreshHeight() { adjustSize(); }

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
    // A derived group's own height, not adjustSize's: a group pinned to its content would be resized to its
    // size hint until the next layout pass, and a window restored from the taskbar paints before that pass.
    refreshHeight();
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
