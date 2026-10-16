#include "clipboard/ui/widgets/group_bar.h"
#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/menus/action_menu.h"
#include "clipboard/ui/theme/theme_styles.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include <QAction>
#include <QCursor>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QMenu>
#include <QPropertyAnimation>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace jietuba::clipboard {
namespace {
// Sizes at 100%; pixels follow the toolbar and panel scale.
constexpr int BUTTON = 34;
constexpr int BAR = 40;
const char *const FOCUS_ANIMATION = "_sidebar_focus_anim";

std::optional<qint64> button_group_id(const QPushButton *button) {
    const QVariant value = button->property("group_id");
    if (!value.isValid()) return std::nullopt;
    return value.toLongLong();
}
}

GroupBar::GroupBar(ClipboardController *controller, ClipboardManager *manager, const Theme &theme, const QString &position, QWidget *parent)
    : QWidget(parent), position(position), controller_(controller), manager_(manager), current_theme_(theme) {
    setFocusPolicy(Qt::NoFocus);
    // Shared buttons are created once and survive layout rebuilds.
    close_btn = create_close_btn();
    clipboard_btn = create_clipboard_btn();
    add_group_btn = create_add_group_btn();
    bar_widget = this;
    group_buttons_widget = new QWidget(this);
    group_buttons_widget->setFocusPolicy(Qt::NoFocus);
    set_own_style(group_buttons_widget, "background: transparent;");
}

// ================================================================
// Public API
// ================================================================

void GroupBar::build(QHBoxLayout *content_layout, QWidget *left_widget, QVBoxLayout *left_layout) {
    content_layout_ = content_layout;
    left_widget_ = left_widget;
    left_layout_ = left_layout;
    build_bar();
    QTimer::singleShot(0, this, [this] { refresh_buttons(); });
}

void GroupBar::set_position(const QString &new_position) {
    if (new_position == position) return;
    position = new_position;
    build_bar();
    QTimer::singleShot(0, this, [this] { refresh_buttons(); });
}

void GroupBar::apply_scale() {
    for (auto *btn : {close_btn, clipboard_btn, add_group_btn}) btn->setFixedSize(scaled(BUTTON), scaled(BUTTON));
    close_btn->setStyleSheet(close_btn_style());
    set_theme(current_theme_);
    build_bar();
    QTimer::singleShot(0, this, [this] { refresh_buttons(); });
}

void GroupBar::set_theme(const Theme &theme) {
    current_theme_ = theme;
    const QString sidebar_style = sidebar_btn_style();
    clipboard_btn->setStyleSheet(sidebar_style);
    for (auto *btn : group_buttons) btn->setStyleSheet(sidebar_style);
    add_group_btn->setStyleSheet(add_group_btn_style());
}

void GroupBar::refresh_buttons() {
    clear_group_buttons_layout();
    clipboard_btn->setChecked(!controller_->current_group_id);

    const bool is_top = position == "top";
    const int bar_size = is_top ? bar_widget->width() : bar_widget->height();
    const auto [visible_groups, hidden_groups] = controller_->get_sidebar_overflow(bar_size, is_top);

    for (const auto &group : visible_groups) {
        auto *btn = make_group_btn(group);
        group_buttons_layout->addWidget(btn);
        group_buttons.append(btn);
    }
    group_buttons_layout->addSpacing(scaled(8));

    if (!hidden_groups.isEmpty()) {
        auto *overflow_btn = new QPushButton(QString::fromUtf8("···"));
        overflow_btn->setFixedSize(scaled(BUTTON), scaled(28));
        overflow_btn->setCursor(Qt::PointingHandCursor);
        overflow_btn->setFocusPolicy(Qt::NoFocus);
        overflow_btn->setToolTip(tr_text("More groups ({n})").replace("{n}", QString::number(hidden_groups.size())));
        overflow_btn->setStyleSheet(QString(R"(
                QPushButton {
                    background: transparent;
                    color: #9CA3AF;
                    border: none;
                    font-size: %1px;
                    font-weight: 600;
                    padding: 0px;
                    letter-spacing: 1px;
                }
                QPushButton:hover {
                    color: #374151;
                }
            )")
                                        .arg(scaled(24)));
        connect(overflow_btn, &QPushButton::clicked, this, [this, hidden = hidden_groups] { show_overflow_group_menu(hidden); });
        group_buttons_layout->addWidget(overflow_btn);
    }
    group_buttons_layout->addWidget(add_group_btn);
}

void GroupBar::enter_sidebar_mode(std::optional<qint64> prev_item_id) {
    const auto buttons = sidebar_buttons();
    if (buttons.isEmpty()) return;
    sidebar_mode_ = true;
    sidebar_prev_item_id_ = prev_item_id;
    emit sidebar_entered();
    sync_sidebar_index();
    set_sidebar_focus_button(buttons[sidebar_index_]);
}

void GroupBar::exit_sidebar_mode() {
    sidebar_mode_ = false;
    set_sidebar_focus_button(nullptr);
    const auto prev = sidebar_prev_item_id_;
    sidebar_prev_item_id_.reset();
    emit sidebar_exited(prev);
}

void GroupBar::move_sidebar_selection(int delta) {
    const auto buttons = sidebar_buttons();
    if (buttons.isEmpty()) return;
    const int count = static_cast<int>(buttons.size());
    sidebar_index_ = ((sidebar_index_ + delta) % count + count) % count;
    auto *target = buttons[sidebar_index_];
    set_sidebar_focus_button(target);
    target->click();
}

void GroupBar::handle_top_group_switch(int delta) {
    sync_sidebar_index();
    const auto buttons = sidebar_buttons();
    if (buttons.isEmpty()) return;
    const int count = static_cast<int>(buttons.size());
    const int new_index = ((sidebar_index_ + delta) % count + count) % count;
    if (new_index == sidebar_index_) return;
    sidebar_index_ = new_index;
    buttons[sidebar_index_]->click();
}

void GroupBar::clear_sidebar_focus() {
    if (!sidebar_mode_) set_sidebar_focus_button(nullptr);
}

void GroupBar::switch_to_group(std::optional<qint64> group_id) {
    controller_->switch_to_group(group_id);
    clipboard_btn->setChecked(!group_id);
    for (auto *btn : group_buttons) btn->setChecked(button_group_id(btn) == group_id);
    sync_sidebar_index();
}

// ================================================================
// Buttons
// ================================================================

QPushButton *GroupBar::create_close_btn() {
    auto *btn = new QPushButton(QString::fromUtf8("×"), this);
    btn->setFixedSize(scaled(BUTTON), scaled(BUTTON));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tr_text("Close"));
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(close_btn_style());
    connect(btn, &QPushButton::clicked, this, &GroupBar::close_requested);
    return btn;
}

QPushButton *GroupBar::create_clipboard_btn() {
    auto *btn = new QPushButton(QString::fromUtf8("📋"), this);
    btn->setFixedSize(scaled(BUTTON), scaled(BUTTON));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tr_text("Clipboard History"));
    btn->setCheckable(true);
    btn->setChecked(true);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(sidebar_btn_style());
    connect(btn, &QPushButton::clicked, this, [this, btn] { on_sidebar_button_clicked(btn, std::nullopt); });
    return btn;
}

QPushButton *GroupBar::create_add_group_btn() {
    auto *btn = new QPushButton("+", this);
    btn->setFixedSize(scaled(BUTTON), scaled(BUTTON));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(add_group_btn_style());
    connect(btn, &QPushButton::clicked, this, &GroupBar::manage_groups_requested);
    return btn;
}

QPushButton *GroupBar::make_group_btn(const Group &group) {
    auto *btn = new QPushButton(group.icon && !group.icon->isEmpty() ? *group.icon : QString::fromUtf8("📁"));
    btn->setFixedSize(scaled(BUTTON), scaled(BUTTON));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(group.name);
    btn->setCheckable(true);
    btn->setChecked(controller_->current_group_id == group.id);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setStyleSheet(sidebar_btn_style());
    btn->setProperty("group_id", group.id);
    const qint64 group_id = group.id;
    connect(btn, &QPushButton::clicked, this, [this, btn, group_id] { on_sidebar_button_clicked(btn, group_id); });
    btn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(btn, &QPushButton::customContextMenuRequested, this, [this, btn, group_id](const QPoint &pos) { show_group_context_menu(btn, group_id, pos); });
    return btn;
}

// ================================================================
// Layout
// ================================================================

void GroupBar::clear_bar_layout() {
    if (!bar_layout) return;
    const QList<QWidget *> preserve_widgets{close_btn, clipboard_btn, add_group_btn, group_buttons_widget};
    while (bar_layout->count()) {
        QLayoutItem *item = bar_layout->takeAt(0);
        QWidget *widget = item->widget();
        if (widget && !preserve_widgets.contains(widget)) widget->deleteLater();
        delete item;
    }
}

void GroupBar::remove_from_host_layouts() {
    content_layout_->removeWidget(this);
    left_layout_->removeWidget(this);
}

void GroupBar::build_bar() {
    const bool is_top = position == "top";
    remove_from_host_layouts();

    const auto direction = is_top ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom;
    if (!bar_layout) {
        bar_layout = new QBoxLayout(direction, this);
    } else {
        clear_bar_layout();
        bar_layout->setDirection(direction);
    }

    const int bar = scaled(BAR);
    if (is_top) {
        setMinimumHeight(bar);
        setMaximumHeight(bar);
        setMinimumWidth(0);
        setMaximumWidth(16777215);
        setStyleSheet("QWidget { background: #FAFAFA; border-bottom: 1px solid #E0E0E0; }");
        bar_layout->setContentsMargins(scaled(2), scaled(2), scaled(2), scaled(2));
    } else {
        setMinimumWidth(bar);
        setMaximumWidth(bar);
        setMinimumHeight(0);
        setMaximumHeight(16777215);
        const QString border_side = position == "left" ? "border-right" : "border-left";
        setStyleSheet(QString("QWidget { background: #FAFAFA; %1: 1px solid #E0E0E0; }").arg(border_side));
        bar_layout->setContentsMargins(scaled(2), scaled(8), scaled(2), scaled(8));
    }
    bar_layout->setSpacing(scaled(4));

    if (!group_buttons_layout) {
        group_buttons_layout = new QBoxLayout(direction, group_buttons_widget);
        group_buttons_layout->setContentsMargins(0, 0, 0, 0);
    } else {
        group_buttons_layout->setDirection(direction);
    }
    group_buttons_layout->setSpacing(scaled(4));

    auto *sep = new QFrame;
    if (is_top) {
        bar_layout->addWidget(clipboard_btn);
        sep->setFixedWidth(1);
        set_own_style(sep, "background: #E0E0E0;");
        bar_layout->addWidget(sep);
        bar_layout->addWidget(group_buttons_widget);
        group_buttons_layout->addWidget(add_group_btn);
        bar_layout->addStretch();
        bar_layout->addWidget(close_btn);
    } else {
        bar_layout->addWidget(close_btn);
        sep->setFixedHeight(1);
        set_own_style(sep, "background: #E0E0E0;");
        bar_layout->addWidget(sep);
        bar_layout->addWidget(clipboard_btn);
        bar_layout->addWidget(group_buttons_widget);
        group_buttons_layout->addWidget(add_group_btn);
        bar_layout->addStretch();
    }

    if (position == "left") {
        content_layout_->insertWidget(0, this);
    } else if (is_top) {
        content_layout_->removeWidget(left_widget_);
        left_layout_->insertWidget(0, this);
        content_layout_->addWidget(left_widget_, 1);
    } else {
        content_layout_->addWidget(this);
    }
}

// ================================================================
// Styles
// ================================================================

QString GroupBar::close_btn_style() const {
    return QString(R"(
            QPushButton {
                background: transparent;
                color: #999999;
                border: none;
                font-size: %1px;
                padding: 0px;
            }
            QPushButton:hover {
                background: #FFEBEE;
                color: #F44336;
                border-radius: %2px;
            }
        )")
        .arg(scaled(20))
        .arg(scaled(4));
}

QString GroupBar::sidebar_btn_style() const {
    const auto &theme = current_theme_.colors;
    return QString(R"(
            QPushButton {
                background: transparent;
                color: %1;
                border: %2px solid transparent;
                font-size: %3px;
                border-radius: %4px;
                padding: 0px;
            }
            QPushButton:hover {
                background: %5;
            }
            QPushButton:checked {
                background: %6;
                color: %7;
                border: %2px solid %8;
            }
            QToolTip {
                background: %9;
                color: %10;
                border: 1px solid %11;
                border-radius: %4px;
                padding: %4px %12px;
                font-size: %13px;
            }
        )")
        .arg(theme.text_secondary)
        .arg(scaled(2))
        .arg(scaled(20))
        .arg(scaled(4))
        .arg(theme.bg_hover, theme.bg_selected, theme.accent_primary, theme.error, theme.bg_primary, theme.text_primary, theme.border_primary)
        .arg(scaled(8))
        .arg(scaled(12));
}

QString GroupBar::add_group_btn_style() const {
    const auto &theme = current_theme_.colors;
    const bool dark = current_theme_.name == "dark";
    const QString bg_color = dark ? "rgba(60, 60, 60, 0.7)" : "rgba(255, 255, 255, 0.55)";
    const QString hover_bg = dark ? "rgba(76, 175, 80, 0.2)" : "#E8F5E9";
    return QString(R"(
            QPushButton {
                background-color: %1;
                color: %2;
                border: %3px dashed %2;
                border-radius: %4px;
                font-size: %5px;
                font-weight: bold;
                padding: 0px;
            }
            QPushButton:hover {
                background: %6;
            }
        )")
        .arg(bg_color, theme.success)
        .arg(scaled(2))
        .arg(scaled(4))
        .arg(scaled(20))
        .arg(hover_bg);
}

QString GroupBar::menu_style() const { return ThemeStyleGenerator(current_theme_).generate_menu_style(); }

// ================================================================
// Side bar navigation state
// ================================================================

QList<QPushButton *> GroupBar::sidebar_buttons() const { return QList<QPushButton *>{clipboard_btn} + group_buttons; }

void GroupBar::on_sidebar_button_clicked(QPushButton *target, std::optional<qint64> group_id) {
    if (!sidebar_mode_) emit sidebar_entered();
    sidebar_mode_ = true;
    const auto buttons = sidebar_buttons();
    if (const auto index = buttons.indexOf(target); index >= 0) sidebar_index_ = static_cast<int>(index);
    set_sidebar_focus_button(target);
    emit group_switched(group_id);
}

void GroupBar::sync_sidebar_index() {
    if (!controller_->current_group_id) {
        sidebar_index_ = 0;
        return;
    }
    for (qsizetype i = 0; i < group_buttons.size(); ++i)
        if (button_group_id(group_buttons[i]) == controller_->current_group_id) {
            sidebar_index_ = static_cast<int>(i) + 1;
            return;
        }
    sidebar_index_ = 0;
}

void GroupBar::set_sidebar_focus_button(QPushButton *target) {
    for (auto *btn : sidebar_buttons()) {
        btn->setProperty("sidebar_focus", btn == target);
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
        auto *anim = btn->findChild<QPropertyAnimation *>(FOCUS_ANIMATION, Qt::FindDirectChildrenOnly);
        if (btn != target && anim) {
            anim->stop();
            if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(btn->graphicsEffect())) effect->setOpacity(1.0);
        }
    }
    if (target && sidebar_mode_ && position != "top") play_sidebar_focus_animation(target);
}

void GroupBar::play_sidebar_focus_animation(QPushButton *target) {
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(target->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(target);
        effect->setOpacity(1.0);
        target->setGraphicsEffect(effect);
    }
    auto *anim = target->findChild<QPropertyAnimation *>(FOCUS_ANIMATION, Qt::FindDirectChildrenOnly);
    if (!anim) {
        anim = new QPropertyAnimation(effect, "opacity", target);
        anim->setObjectName(FOCUS_ANIMATION);
        anim->setDuration(1200);
        anim->setLoopCount(-1);
        anim->setEasingCurve(QEasingCurve::InOutQuad);
        anim->setKeyValueAt(0.0, 1.0);
        anim->setKeyValueAt(0.5, 0.3);
        anim->setKeyValueAt(1.0, 1.0);
    }
    if (anim->state() != QAbstractAnimation::Running) anim->start();
}

// ================================================================
// Overflow menu
// ================================================================

void GroupBar::show_overflow_group_menu(const QList<Group> &hidden_groups) {
    if (hidden_groups.isEmpty()) return;
    QMenu menu(this);
    menu.setStyleSheet(menu_style());
    for (const auto &group : hidden_groups) {
        const QString icon = group.icon && !group.icon->isEmpty() ? *group.icon : QString::fromUtf8("📁");
        auto *action = new QAction(icon + "  " + group.name, &menu);
        action->setCheckable(true);
        action->setChecked(group.id == controller_->current_group_id);
        const qint64 group_id = group.id;
        connect(action, &QAction::triggered, this, [this, group_id] {
            emit group_switched(group_id);
            // The chosen group takes the last visible place, so the buttons are laid out again.
            refresh_buttons();
        });
        menu.addAction(action);
    }
    menu.exec(QCursor::pos());
}

void GroupBar::clear_group_buttons_layout() {
    while (group_buttons_layout->count()) {
        QLayoutItem *item = group_buttons_layout->takeAt(0);
        QWidget *widget = item->widget();
        if (widget && widget != add_group_btn) widget->deleteLater();
        delete item;
    }
    group_buttons.clear();
}

// ================================================================
// Group context menu and management
// ================================================================

void GroupBar::show_group_context_menu(QPushButton *btn, qint64 group_id, const QPoint &pos) {
    const auto actions_data = controller_->build_group_context_menu_data(group_id);
    QMap<QString, ActionHandler> handlers{
        {"edit_group", [this, group_id] { edit_group(group_id); }},
        {"move_group_up", [this, group_id] { move_group_order(group_id, -1); }},
        {"move_group_down", [this, group_id] { move_group_order(group_id, 1); }},
        {"delete_group", [this, group_id] { delete_group(group_id); }},
    };
    ClipboardGroupContextMenu(this, menu_style(), [](const QString &text) { return tr_text(text.toUtf8().constData()); }, handlers).show(btn, pos, actions_data);
}

void GroupBar::delete_group(qint64 group_id) {
    if (controller_->delete_group(group_id, this, [](const char *text) { return tr_text(text); })) refresh_buttons();
}

void GroupBar::move_group_order(qint64 group_id, int direction) {
    if (controller_->move_group_order(group_id, direction)) refresh_buttons();
}

void GroupBar::edit_group(qint64 group_id) {
    controller_->open_manage_dialog_for_group(group_id, [this](ManageDialog *dialog) {
        connect(dialog, &ManageDialog::group_added, this, &GroupBar::refresh_buttons, Qt::UniqueConnection);
        connect(dialog, &ManageDialog::data_changed, this, &GroupBar::on_manage_data_changed, Qt::UniqueConnection);
    });
}

void GroupBar::on_manage_data_changed() {
    // Data changes in the manage window, passed up to the clipboard window.
    if (auto *window = qobject_cast<ClipboardWindow *>(parent())) window->on_manage_data_changed();
}
}
