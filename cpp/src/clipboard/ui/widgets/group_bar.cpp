#include "clipboard/ui/widgets/group_bar.h"
#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/menus/action_menu.h"
#include "clipboard/ui/theme/theme_styles.h"
#include "clipboard/ui/windows/clipboard_window.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/fluent_lite/theme.h"
#include <QAction>
#include <QCursor>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace jietuba::clipboard {
namespace {
// Sizes at 100%; pixels follow the toolbar and panel scale.
// BUTTON is the length along the bar, which the controller's overflow capacity counts on; BAR is the thickness across it.
constexpr int BUTTON = 34;
constexpr int BAR = 42;
constexpr int INDICATOR_THICKNESS = 4;
constexpr int INDICATOR_INSET = 8;

std::optional<qint64> button_group_id(const QPushButton *button) {
    const QVariant value = button->property("group_id");
    if (!value.isValid()) return std::nullopt;
    return value.toLongLong();
}
}

void GroupButton::set_indicator(std::optional<Qt::Edge> edge, const QColor &color) {
    edge_ = edge;
    color_ = color;
    update();
}

void GroupButton::paintEvent(QPaintEvent *event) {
    QPushButton::paintEvent(event);
    if (!edge_ || !isChecked()) return;
    const int thickness = scaled(INDICATOR_THICKNESS), inset = scaled(INDICATOR_INSET);
    QRect line;
    switch (*edge_) {
    case Qt::LeftEdge: line = QRect(0, inset, thickness, height() - 2 * inset); break;
    case Qt::RightEdge: line = QRect(width() - thickness, inset, thickness, height() - 2 * inset); break;
    default: line = QRect(inset, height() - thickness, width() - 2 * inset, thickness); break;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color_);
    painter.drawRoundedRect(line, thickness / 2.0, thickness / 2.0);
}

GroupBar::GroupBar(ClipboardController *controller, ClipboardManager *manager, const Theme &theme, const QString &position, QWidget *parent)
    : QWidget(parent), position(position), controller_(controller), manager_(manager), current_theme_(theme) {
    setFocusPolicy(Qt::NoFocus);
    // The window paints the bar's background and its line from the theme's style sheet.
    setObjectName(BAR_OBJECT_NAME);
    setAttribute(Qt::WA_StyledBackground, true);
    // Shared buttons are created once and survive layout rebuilds.
    close_btn = create_close_btn();
    clipboard_btn = create_clipboard_btn();
    add_group_btn = create_add_group_btn();
    bar_widget = this;
    group_buttons_widget = new QWidget(this);
    group_buttons_widget->setFocusPolicy(Qt::NoFocus);
    set_own_style(group_buttons_widget, "background: transparent;");
    apply_button_look();
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
    build_bar();
    QTimer::singleShot(0, this, [this] { refresh_buttons(); });
}

void GroupBar::set_theme(const Theme &theme) {
    current_theme_ = theme;
    apply_button_look();
}

void GroupBar::apply_button_look() {
    const auto &colors = current_theme_.colors;
    const bool top = position == "top";
    // The buttons fill the bar across its thickness, less the hairline on its inner edge.
    const int across = scaled(BAR) - 1;
    const QSize cell = top ? QSize(scaled(BUTTON), across) : QSize(across, scaled(BUTTON));
    const std::optional<Qt::Edge> edge = top ? Qt::BottomEdge : position == "left" ? Qt::LeftEdge : Qt::RightEdge;
    const QColor accent(colors.accent_primary);
    const QString emoji_style = sidebar_btn_style();
    QList<GroupButton *> emoji_buttons{clipboard_btn};
    emoji_buttons.append(group_buttons);
    for (auto *btn : emoji_buttons) {
        btn->setFixedSize(cell);
        btn->setStyleSheet(emoji_style);
        btn->set_indicator(edge, accent);
    }
    const QString icon_style = icon_btn_style();
    const QSize icon_size(scaled(14), scaled(14));
    for (auto *btn : {close_btn, add_group_btn}) {
        btn->setFixedSize(cell);
        btn->setStyleSheet(icon_style);
        btn->setIconSize(icon_size);
    }
    close_btn->setIcon(tinted_icon(FluentIcon::Cancel, colors.text_secondary));
    // Accent colored and larger than close: the plus is how most people find out groups exist.
    add_group_btn->setIconSize(QSize(scaled(18), scaled(18)));
    add_group_btn->setIcon(tinted_icon(ResourceManager::get_icon_path("add.svg"), colors.accent_primary));
    if (separator_) separator_->setStyleSheet(QString("background: %1;").arg(ThemeStyleGenerator(current_theme_).divider_color()));
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
    apply_button_look();
    group_buttons_layout->addSpacing(scaled(8));

    if (!hidden_groups.isEmpty()) {
        auto *overflow_btn = new QPushButton(QString::fromUtf8("···"));
        overflow_btn->setFixedSize(scaled(BUTTON), scaled(28));
        overflow_btn->setCursor(Qt::PointingHandCursor);
        overflow_btn->setFocusPolicy(Qt::NoFocus);
        overflow_btn->setToolTip(tr_text("More groups ({n})").replace("{n}", QString::number(hidden_groups.size())));
        overflow_btn->setStyleSheet(overflow_btn_style());
        connect(overflow_btn, &QPushButton::clicked, this, [this, hidden = hidden_groups] { show_overflow_group_menu(hidden); });
        group_buttons_layout->addWidget(overflow_btn);
    }
    group_buttons_layout->addWidget(add_group_btn);
}

void GroupBar::switch_to_group(std::optional<qint64> group_id) {
    controller_->switch_to_group(group_id);
    clipboard_btn->setChecked(!group_id);
    for (auto *btn : group_buttons) btn->setChecked(button_group_id(btn) == group_id);
}

// ================================================================
// Buttons
// ================================================================

// The close and add buttons carry SVG icons: the × and + characters come from whichever font Windows falls back to.
QPushButton *GroupBar::create_close_btn() {
    auto *btn = new QPushButton(this);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tr_text("Close"));
    btn->setFocusPolicy(Qt::NoFocus);
    connect(btn, &QPushButton::clicked, this, &GroupBar::close_requested);
    return btn;
}

GroupButton *GroupBar::create_clipboard_btn() {
    auto *btn = new GroupButton(QString::fromUtf8("📋"), this);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tr_text("Clipboard History"));
    btn->setCheckable(true);
    btn->setChecked(true);
    btn->setFocusPolicy(Qt::NoFocus);
    connect(btn, &QPushButton::clicked, this, [this] { emit group_switched(std::nullopt); });
    return btn;
}

QPushButton *GroupBar::create_add_group_btn() {
    auto *btn = new QPushButton(this);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setToolTip(tr_text("New Group"));
    connect(btn, &QPushButton::clicked, this, &GroupBar::manage_groups_requested);
    return btn;
}

GroupButton *GroupBar::make_group_btn(const Group &group) {
    auto *btn = new GroupButton(group.icon && !group.icon->isEmpty() ? *group.icon : QString::fromUtf8("📁"));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(group.name);
    btn->setCheckable(true);
    btn->setChecked(controller_->current_group_id == group.id);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setProperty("group_id", group.id);
    const qint64 group_id = group.id;
    connect(btn, &QPushButton::clicked, this, [this, group_id] { emit group_switched(group_id); });
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
        bar_layout->setContentsMargins(scaled(2), 0, scaled(2), 0);
    } else {
        setMinimumWidth(bar);
        setMaximumWidth(bar);
        setMinimumHeight(0);
        setMaximumHeight(16777215);
        bar_layout->setContentsMargins(0, scaled(2), 0, scaled(2));
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
    separator_ = sep;
    if (is_top) {
        bar_layout->addWidget(clipboard_btn);
        sep->setFixedWidth(1);
        bar_layout->addWidget(sep);
        bar_layout->addWidget(group_buttons_widget);
        group_buttons_layout->addWidget(add_group_btn);
        bar_layout->addStretch();
        bar_layout->addWidget(close_btn);
    } else {
        bar_layout->addWidget(close_btn);
        sep->setFixedHeight(1);
        bar_layout->addWidget(sep);
        bar_layout->addWidget(clipboard_btn);
        bar_layout->addWidget(group_buttons_widget);
        group_buttons_layout->addWidget(add_group_btn);
        bar_layout->addStretch();
    }

    apply_button_look();
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

QString GroupBar::icon_btn_style() const {
    const auto &theme = current_theme_.colors;
    return QString(R"(
            QPushButton {
                background: transparent;
                border: none;
                border-radius: %1px;
                padding: 0px;
            }
            QPushButton:hover {
                background: %2;
            }
        )")
        .arg(scaled(6))
        .arg(theme.bg_hover);
}

QString GroupBar::sidebar_btn_style() const {
    const auto &theme = current_theme_.colors;
    return QString(R"(
            QPushButton {
                background: transparent;
                border: none;
                font-size: %1px;
                border-radius: %2px;
                padding: 0px;
            }
            QPushButton:hover {
                background: %3;
            }
            QToolTip {
                background: %4;
                color: %5;
                border: 1px solid %6;
                border-radius: %7px;
                padding: %7px %8px;
                font-size: %9px;
            }
        )")
        .arg(scaled(18))
        .arg(scaled(6))
        .arg(theme.bg_hover, theme.bg_primary, theme.text_primary, theme.border_primary)
        .arg(scaled(4))
        .arg(scaled(8))
        .arg(scaled(12));
}

QString GroupBar::overflow_btn_style() const {
    const auto &theme = current_theme_.colors;
    return QString(R"(
            QPushButton {
                background: transparent;
                color: %1;
                border: none;
                font-size: %2px;
                font-weight: 600;
                padding: 0px;
                letter-spacing: 1px;
            }
            QPushButton:hover {
                color: %3;
            }
        )")
        .arg(theme.text_tertiary)
        .arg(scaled(24))
        .arg(theme.text_primary);
}

QString GroupBar::menu_style() const { return ThemeStyleGenerator(current_theme_).generate_menu_style(); }

// ================================================================
// Keyboard switching
// ================================================================

QList<QPushButton *> GroupBar::switch_buttons() const {
    QList<QPushButton *> buttons{clipboard_btn};
    for (auto *btn : group_buttons) buttons.append(btn);
    return buttons;
}

int GroupBar::current_button_index() const {
    // A group in the overflow menu has no button; switching then starts from the history.
    for (qsizetype i = 0; controller_->current_group_id && i < group_buttons.size(); ++i)
        if (button_group_id(group_buttons[i]) == controller_->current_group_id) return static_cast<int>(i) + 1;
    return 0;
}

void GroupBar::switch_adjacent_group(int delta) {
    const auto buttons = switch_buttons();
    const int count = static_cast<int>(buttons.size());
    const int index = current_button_index();
    const int next = ((index + delta) % count + count) % count;
    if (next != index) buttons[next]->click();
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
