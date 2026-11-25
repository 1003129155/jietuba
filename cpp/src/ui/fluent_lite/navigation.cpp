#include "ui/fluent_lite/navigation.h"
#include "core/ui_scale.h"
#include "ui/fluent_lite/cards.h"
#include <QCoreApplication>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QPropertyAnimation>
#include <QTimer>
#include <QVariantAnimation>
#include <algorithm>

namespace jietuba {
namespace {
constexpr int UNFOLD_MS = 200;
constexpr int ARROW_TURN_MS = 150;

// Thin line between navigation groups, inset to match the selection pill.
class NavSeparator : public QWidget {
public:
    explicit NavSeparator(QWidget *parent) : QWidget(parent) { setFixedHeight(dialog_scaled(11)); }
protected:
    void paintEvent(QPaintEvent *) override {
        const int inset = dialog_scaled(13);
        QPainter painter(this);
        painter.fillRect(inset, height() / 2, width() - 2 * inset, 1, css_color(ui_tokens(this).divider));
    }
};

// Geometry at 100%: rows are 36 high with a 4 px side margin (set on the list), the icon 16 and
// the text 14. Sub-items have no icon and start their text where the item text starts.
int row_height() { return dialog_scaled(36); }
int icon_left() { return dialog_scaled(12); }
int text_left() { return icon_left() + dialog_scaled(16) + dialog_scaled(12); }
int text_right_pad() { return dialog_scaled(12); }
// The press target of an item's unfold arrow, at the right end of the row.
int arrow_width() { return dialog_scaled(32); }

QFont row_font() {
    QFont font;
    font.setFamilies(FONT_FAMILIES());
    font.setPixelSize(dialog_scaled(14));
    return font;
}

// Neutral wash for hover and the current row.
QColor row_fill(const UIThemeTokens &tokens, bool pressed) {
    QColor color = css_color(tokens.item_hover);
    if (pressed) color.setAlpha(color.alpha() * 2 / 3);
    return color;
}

// A row of the list: the fill, the current row's bar and the focus outline. A style sheet left
// border would run the full height and bend with the corner radius.
class NavRowButton : public QPushButton {
public:
    explicit NavRowButton(const QString &text, QWidget *parent) : QPushButton(text, parent) {
        setAttribute(Qt::WA_Hover);
        setFont(row_font());
        // Keyboard focus only: a click would leave the outline behind.
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
    }
protected:
    virtual bool current() const = 0;
    void paint_row(QPainter &painter) {
        const auto &tokens = ui_tokens(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal radius = dialog_scaled(4);
        const bool lit = current();
        if (lit || underMouse() || isDown()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(row_fill(tokens, isDown() && !lit));
            painter.drawRoundedRect(QRectF(rect()), radius, radius);
        }
        if (lit) {
            const qreal bar_width = dialog_scaled(3);
            const qreal bar_height = dialog_scaled(16);
            painter.setPen(Qt::NoPen);
            painter.setBrush(css_color(ACCENT));
            painter.drawRoundedRect(QRectF(0, (height() - bar_height) / 2, bar_width, bar_height), bar_width / 2, bar_width / 2);
        }
        if (hasFocus()) {
            painter.setPen(QPen(css_color(ACCENT), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        }
        painter.setFont(row_font());
        painter.setPen(css_color(tokens.fg));
    }
};

// A page of the list. When one of its sub-items is current, that one carries the highlight.
class NavItemButton : public NavRowButton {
public:
    explicit NavItemButton(const QString &text, QWidget *parent) : NavRowButton(text, parent) {
        setFixedHeight(row_height());
        turn_.setDuration(ARROW_TURN_MS);
        turn_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&turn_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            angle_ = value.toReal();
            update();
        });
    }
    std::function<void()> on_arrow;
    void setSubActive(bool active) {
        if (sub_active_ == active) return;
        sub_active_ = active;
        update();
    }
    void setExpandable(bool expandable) {
        if (expandable_ == expandable) return;
        expandable_ = expandable;
        updateGeometry();
        update();
    }
    bool expandable() const { return expandable_; }
    // The arrow points down when folded and up when unfolded; it turns when animated.
    void setExpanded(bool expanded, bool animate) {
        expanded_ = expanded;
        update();
        turn_.stop();
        const qreal end = expanded ? 180 : 0;
        if (animate && isVisible() && angle_ != end) {
            turn_.setStartValue(angle_);
            turn_.setEndValue(end);
            turn_.start();
            return;
        }
        angle_ = end;
        update();
    }
    qreal arrowAngle() const { return angle_; }
    QRect arrow_rect() const { return QRect(width() - arrow_width(), 0, arrow_width(), height()); }
    QSize sizeHint() const override { return QSize(text_left() + QFontMetrics(row_font()).horizontalAdvance(text()) + right_room(), row_height()); }
    QSize minimumSizeHint() const override { return QSize(0, row_height()); }
protected:
    // Folded, the current sub-item is hidden, so the item marks the page itself.
    bool current() const override { return isChecked() && (!sub_active_ || !expanded_); }
    int right_room() const { return expandable_ ? arrow_width() : text_right_pad(); }
    void mousePressEvent(QMouseEvent *event) override {
        if (expandable_ && event->button() == Qt::LeftButton && arrow_rect().contains(event->position().toPoint())) {
            arrow_pressed_ = true;
            event->accept();
            return;
        }
        QPushButton::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (arrow_pressed_) {
            arrow_pressed_ = false;
            event->accept();
            // Only the arrow reacts: the page is not opened.
            if (arrow_rect().contains(event->position().toPoint()) && on_arrow) on_arrow();
            return;
        }
        QPushButton::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        const bool unfolded = angle_ > 90;
        if (expandable_ && on_arrow && ((event->key() == Qt::Key_Right && !unfolded) || (event->key() == Qt::Key_Left && unfolded))) {
            on_arrow();
            return;
        }
        QPushButton::keyPressEvent(event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        paint_row(painter);
        const int icon_size = dialog_scaled(16);
        if (!icon().isNull())
            painter.drawPixmap(QPoint(icon_left(), (height() - icon_size) / 2), icon().pixmap(QSize(icon_size, icon_size), devicePixelRatioF()));
        painter.drawText(QRect(text_left(), 0, std::max(0, width() - text_left() - right_room()), height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, text());
        if (!expandable_) return;
        // A chevron about 12 px wide, turned half way round when unfolded.
        painter.save();
        painter.translate(arrow_rect().center() + QPointF(0.5, 0.5));
        painter.rotate(angle_);
        QPen pen(css_color(ui_tokens(this).fg_muted), dialog_scaled_f(1.5));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const qreal half_width = dialog_scaled_f(5.5), half_height = dialog_scaled_f(2.75);
        painter.drawPolyline(QPolygonF{QPointF(-half_width, -half_height), QPointF(0, half_height), QPointF(half_width, -half_height)});
        painter.restore();
    }
private:
    bool sub_active_ = false;
    bool expandable_ = false;
    bool expanded_ = false;
    bool arrow_pressed_ = false;
    qreal angle_ = 0;
    QVariantAnimation turn_;
};

// A group of a page. The text wraps instead of being cut, so a long translation grows the row.
class NavSubButton : public NavRowButton {
public:
    explicit NavSubButton(const QString &text, QWidget *parent) : NavRowButton(text, parent) {
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }
    void setActive(bool active) {
        if (active_ == active) return;
        active_ = active;
        update();
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override {
        const int room = std::max(1, width - text_left() - text_right_pad());
        const QRect box = QFontMetrics(row_font()).boundingRect(QRect(0, 0, room, 0), Qt::AlignLeft | Qt::TextWordWrap, text());
        return std::max(row_height(), box.height() + 2 * dialog_scaled(8));
    }
    QSize sizeHint() const override { return QSize(text_left() + QFontMetrics(row_font()).horizontalAdvance(text()) + text_right_pad(), row_height()); }
    QSize minimumSizeHint() const override { return QSize(0, row_height()); }
protected:
    bool current() const override { return active_; }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        paint_row(painter);
        painter.drawText(QRect(text_left(), 0, std::max(0, width() - text_left() - text_right_pad()), height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, text());
    }
private:
    bool active_ = false;
};
}

// The sub-items of one page, in a box that folds by its height. The rows keep their place and size
// inside it and are clipped as it shrinks. Folded it is hidden, so it takes no clicks or focus.
class NavSubList : public QWidget {
public:
    explicit NavSubList(QWidget *parent) : QWidget(parent), animation_(new QPropertyAnimation(this, "maximumHeight", this)) {
        setObjectName("FluentLiteNavSubList");
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        inner_ = new QWidget(this);
        rows_ = new QVBoxLayout(inner_);
        rows_->setContentsMargins(0, 0, 0, 0);
        rows_->setSpacing(dialog_scaled(2));
        animation_->setDuration(UNFOLD_MS);
        animation_->setEasingCurve(QEasingCurve::OutCubic);
        // Folded, it goes hidden; unfolded, it takes its natural height back (a wrapped title may grow it later).
        connect(animation_, &QAbstractAnimation::finished, this, [this] {
            setMaximumHeight(unfolding_ ? QWIDGETSIZE_MAX : 0);
            if (!unfolding_) hide();
        });
        setMaximumHeight(0);
        hide();
    }
    std::vector<NavSubButton *> buttons;
    QPropertyAnimation *animation() const { return animation_; }

    void set_titles(const QStringList &titles, const std::function<void(int)> &on_click) {
        for (auto *button : buttons) {
            rows_->removeWidget(button);
            button->hide();
            button->deleteLater();
        }
        buttons.clear();
        for (int i = 0; i < titles.size(); ++i) {
            auto *button = new NavSubButton(titles[i], inner_);
            button->setObjectName("FluentLiteNavSubItem");
            connect(button, &QPushButton::clicked, this, [on_click, i] { on_click(i); });
            rows_->addWidget(button);
            buttons.push_back(button);
            button->show();
        }
        set_titles_done();
    }

    // Height of the rows at the given width.
    int natural_height(int width) const {
        const int wrapped = rows_->hasHeightForWidth() ? rows_->heightForWidth(width) : -1;
        return std::max({wrapped, rows_->sizeHint().height(), rows_->minimumSize().height()});
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return natural_height(width); }
    QSize sizeHint() const override { return QSize(0, natural_height(std::max(1, width()))); }
    QSize minimumSizeHint() const override { return sizeHint(); }
    void set_titles_done() {
        updateGeometry();
        inner_->setGeometry(0, 0, width(), natural_height(std::max(1, width())));
    }

    void unfold(bool unfolded, bool animate, int width) {
        animation_->stop();
        unfolding_ = unfolded;
        if (!animate) {
            setMaximumHeight(unfolded ? QWIDGETSIZE_MAX : 0);
            setVisible(unfolded);
            return;
        }
        const int start = isVisible() ? std::min(height(), maximumHeight()) : 0;
        const int end = unfolded ? natural_height(width) : 0;
        setMaximumHeight(start);
        show();
        animation_->setStartValue(start);
        animation_->setEndValue(end);
        animation_->start();
    }
protected:
    void resizeEvent(QResizeEvent *) override { inner_->setGeometry(0, 0, width(), natural_height(std::max(1, width()))); }
private:
    QWidget *inner_;
    QVBoxLayout *rows_;
    QPropertyAnimation *animation_;
    bool unfolding_ = false;
};

NavigationInterface::NavigationInterface(QWidget *parent) : QWidget(parent), group_(new QButtonGroup(this)) {
    group_->setExclusive(true);
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(dialog_scaled(2));
    // The top list scrolls when the window is shorter than the whole navigation, so the
    // bottom items stay visible. It follows the viewport width: a shown scroll bar takes
    // its width from the items instead of clipping their right edge.
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    set_own_style(scroll_, "background: transparent;");
    auto *top_view = new QWidget;
    top_view->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    set_own_style(top_view, "background: transparent;");
    auto *top_view_layout = new QVBoxLayout(top_view);
    top_view_layout->setContentsMargins(dialog_scaled(4), 0, dialog_scaled(4), 0);
    top_ = new QVBoxLayout;
    top_->setSpacing(dialog_scaled(2));
    top_view_layout->addLayout(top_);
    top_view_layout->addStretch(1);
    scroll_->setWidget(top_view);
    bottom_ = new QVBoxLayout;
    bottom_->setContentsMargins(dialog_scaled(4), 0, dialog_scaled(4), 0);
    bottom_->setSpacing(dialog_scaled(2));
    layout_->addWidget(scroll_, 1);
    layout_->addLayout(bottom_);
    set_own_style(this, "background: transparent;");
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { _apply_theme(); });
}

void NavigationInterface::addSeparator() { top_->addWidget(new NavSeparator(this)); }

QPushButton *NavigationInterface::addItem(const QString &route_key, const IconSource &icon, const QString &text, std::function<void()> on_click,
                                          NavigationItemPosition position, const QString &tooltip) {
    auto *button = new NavItemButton(text, this);
    button->setObjectName("FluentLiteNavItem");
    button->setCheckable(true);
    button->setIconSize(QSize(dialog_scaled(16), dialog_scaled(16)));
    // Labels are visible already; a tooltip only when it adds context.
    if (!tooltip.isEmpty()) button->setToolTip(tooltip);
    button->on_arrow = [this, route_key] { emit arrowClicked(route_key); };
    connect(button, &QPushButton::clicked, this, [this, route_key, button, on_click] {
        // The open page's row folds and unfolds like its arrow; the page is already shown.
        if (current_ == route_key && button->expandable()) {
            emit arrowClicked(route_key);
            return;
        }
        setCurrentItem(route_key);
        if (on_click) on_click();
    });
    group_->addButton(button);
    items_[route_key] = button;
    icons_[route_key] = icon;
    connect(button, &QPushButton::toggled, this, [this, route_key](bool) { apply_icon(route_key); });
    apply_icon(route_key);
    (position == NavigationItemPosition::BOTTOM ? bottom_ : top_)->addWidget(button);
    return button;
}

void NavigationInterface::apply_icon(const QString &route_key) {
    auto *button = items_[route_key];
    const auto &icon = icons_[route_key];
    if (const auto *qicon = std::get_if<QIcon>(&icon)) {
        button->setIcon(*qicon);
        return;
    }
    // In the text colour like the card icons; the current item is marked by its bar.
    button->setIcon(tinted_icon(icon, ui_tokens(button).fg));
}

void NavigationInterface::_apply_theme() {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        apply_icon(it.key());
        it.value()->update();
    }
    for (auto *list : lists_)
        for (auto *button : list->buttons) button->update();
}

void NavigationInterface::setCurrentItem(const QString &route_key) {
    auto *button = items_.value(route_key);
    if (!button) return;
    current_ = route_key;
    button->setChecked(true);
    // Before the first show the viewport has no real size yet, so scrolling then would be wrong.
    if (scroll_->isVisible() && scroll_->isAncestorOf(button)) scroll_->ensureWidgetVisible(button, 0, 0);
}

void NavigationInterface::clearCurrentItem() {
    group_->setExclusive(false);
    for (auto *button : items_) button->setChecked(false);
    group_->setExclusive(true);
    current_.clear();
}

void NavigationInterface::setExpandable(const QString &route_key, bool expandable) {
    if (auto *button = static_cast<NavItemButton *>(items_.value(route_key))) {
        if (top_->indexOf(button) >= 0) button->setExpandable(expandable);
    }
}

bool NavigationInterface::isExpandable(const QString &route_key) const {
    const auto *button = static_cast<const NavItemButton *>(items_.value(route_key));
    return button && button->expandable();
}

void NavigationInterface::setExpanded(const QString &route_key, bool expanded, bool animate) {
    auto *button = static_cast<NavItemButton *>(items_.value(route_key));
    if (!button || top_->indexOf(button) < 0) return;
    expanded_[route_key] = expanded;
    button->setExpanded(expanded, animate);
    if (auto *list = list_of(route_key)) {
        const int width = std::max(1, scroll_->viewport()->width() - 2 * dialog_scaled(4));
        list->unfold(expanded, animate && scroll_->isVisible(), width);
    }
}

void NavigationInterface::setSubItems(const QString &route_key, const QStringList &titles) {
    auto *parent_item = items_.value(route_key);
    if (!parent_item || top_->indexOf(parent_item) < 0) return;
    auto *list = list_of(route_key);
    if (titles.isEmpty()) {
        if (!list) return;
        top_->removeWidget(list);
        list->hide();
        list->deleteLater();
        lists_.remove(route_key);
        if (current_sub_route_ == route_key) {
            current_sub_route_.clear();
            current_sub_ = -1;
            static_cast<NavItemButton *>(parent_item)->setSubActive(false);
        }
        return;
    }
    const bool is_new = !list;
    if (is_new) {
        list = new NavSubList(scroll_->widget());
        connect(list->animation(), &QAbstractAnimation::finished, this, [this] { flush_visible(); });
        top_->insertWidget(top_->indexOf(parent_item) + 1, list);
        lists_[route_key] = list;
    } else if (subItemTitles(route_key) == titles) {
        return;
    }
    const bool had_current = current_sub_route_ == route_key;
    list->set_titles(titles, [this, route_key](int index) { emit subItemClicked(route_key, index); });
    if (had_current) setCurrentSubItem(route_key, std::min<int>(current_sub_, titles.size() - 1));
    // A list made for an item that is already unfolded opens with the animation; a rebuilt one stays as it is.
    if (is_new && isExpanded(route_key)) {
        const int width = std::max(1, scroll_->viewport()->width() - 2 * dialog_scaled(4));
        list->unfold(true, scroll_->isVisible(), width);
    } else if (!is_new) {
        list->setVisible(isExpanded(route_key));
    }
}

int NavigationInterface::subItemCount(const QString &route_key) const {
    const auto *list = list_of(route_key);
    return list ? static_cast<int>(list->buttons.size()) : 0;
}

QPushButton *NavigationInterface::subItem(const QString &route_key, int index) const {
    const auto *list = list_of(route_key);
    return list && index >= 0 && index < static_cast<int>(list->buttons.size()) ? list->buttons[index] : nullptr;
}

QStringList NavigationInterface::subItemTitles(const QString &route_key) const {
    QStringList titles;
    if (const auto *list = list_of(route_key))
        for (const auto *button : list->buttons) titles << button->text();
    return titles;
}

void NavigationInterface::setCurrentSubItem(const QString &route_key, int index) {
    if (index < 0 || index >= subItemCount(route_key)) index = -1;
    if (index < 0) current_sub_route_.clear();
    else current_sub_route_ = route_key;
    current_sub_ = index;
    for (auto it = lists_.begin(); it != lists_.end(); ++it)
        for (int i = 0; i < static_cast<int>(it.value()->buttons.size()); ++i) it.value()->buttons[i]->setActive(it.key() == current_sub_route_ && i == current_sub_);
    for (auto it = items_.begin(); it != items_.end(); ++it) static_cast<NavItemButton *>(it.value())->setSubActive(it.key() == current_sub_route_);
    if (index < 0) return;
    // The first group shows its page's item too.
    request_visible(index == 0 ? static_cast<QWidget *>(items_.value(route_key)) : static_cast<QWidget *>(subItem(route_key, index)));
}

bool NavigationInterface::animating() const {
    for (const auto *list : lists_)
        if (list->animation()->state() == QAbstractAnimation::Running) return true;
    return false;
}

void NavigationInterface::request_visible(QWidget *widget) {
    pending_visible_ = widget;
    // Deferred: a list just built has no geometry yet.
    QTimer::singleShot(0, this, [this] { flush_visible(); });
}

void NavigationInterface::flush_visible() {
    if (animating() || !pending_visible_) return;
    QWidget *target = pending_visible_;
    pending_visible_.clear();
    if (scroll_->isVisible() && target->isVisible() && scroll_->isAncestorOf(target)) scroll_->ensureWidgetVisible(target, 0, dialog_scaled(8));
}
}
