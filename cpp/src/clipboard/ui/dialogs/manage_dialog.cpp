#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/controllers/context_menu_controller.h"
#include "clipboard/core/manager.h"
#include "clipboard/services/file_payload_service.h"
#include "clipboard/services/group_service.h"
#include "clipboard/services/import_export_service.h"
#include "clipboard/services/manage_dialog_service.h"
#include "clipboard/ui/forms/file_content_form.h"
#include "clipboard/ui/forms/form_widgets.h"
#include "clipboard/ui/forms/group_form.h"
#include "clipboard/ui/forms/group_icon_picker.h"
#include "clipboard/ui/forms/image_content_form.h"
#include "clipboard/ui/forms/import_export_form.h"
#include "clipboard/ui/forms/text_content_form.h"
#include "clipboard/ui/group_kind_style.h"
#include "clipboard/ui/image_item_actions.h"
#include "clipboard/ui/layout_scale.h"
#include "clipboard/ui/widgets/manage_rows.h"
#include "clipboard/ui/widgets/reorder_list.h"
#include "clipboard/ui/windows/pin_window.h"
#include "core/clipboard_utils.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/theme.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/settings_ui/components.h"
#include <QAbstractScrollArea>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDir>
#include <QHBoxLayout>
#include <QListWidgetItem>
#include <QMenu>
#include <QScreen>
#include <QUrl>
#include <algorithm>
#include <chrono>

namespace jietuba::clipboard {
namespace {
// The manage window loads a group's whole content, so drag reordering reaches any position.
constexpr qint64 ITEMS_PER_GROUP = 100000;
// A new image waits for the clipboard watcher to record it: poll interval and longest wait.
constexpr int IMAGE_CAPTURE_POLL_MS = 150;
constexpr qint64 IMAGE_CAPTURE_TIMEOUT_MS = 3000;

QPointer<ManageDialog> manage_window_instance;

qint64 monotonic_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

QString short_time(const QDateTime &moment) {
    if (!moment.isValid()) return {};
    const QDateTime now = QDateTime::currentDateTime();
    if (moment.date() == now.date()) return moment.toString("HH:mm");
    if (moment.date().year() == now.date().year()) return moment.toString("MM-dd");
    return moment.toString("yyyy-MM-dd");
}

QString display_icon(const Group &group) { return get_group_display_icon(group.icon, group.group_type, group.filter_rule); }

QAction *add_menu_action(QMenu *menu, const QString &text, const std::function<void()> &handler) {
    QAction *action = menu->addAction(text);
    QObject::connect(action, &QAction::triggered, menu, [handler] { handler(); });
    return action;
}

std::optional<QString> stripped_or_none(const QString &text) {
    const QString stripped = text.trimmed();
    return stripped.isEmpty() ? std::nullopt : std::optional<QString>(stripped);
}
}

ManageDialog *get_manage_dialog(ClipboardManager *manager) {
    if (!manage_window_instance) manage_window_instance = new ManageDialog(manager ? manager : &ClipboardManager::instance());
    return manage_window_instance;
}

ManageDialog *get_existing_manage_dialog() { return manage_window_instance; }

bool destroy_manage_dialog() {
    QPointer<ManageDialog> dialog = manage_window_instance;
    manage_window_instance = nullptr;
    if (!dialog) return false;
    const bool was_visible = dialog->isVisible();
    // closeEvent only hides; this bypasses close() and destroys it, so the next open rebuilds with new construction-time values.
    dialog->hide();
    dialog->deleteLater();
    return was_visible;
}

ManageDialog::ManageDialog(ClipboardManager *manager, QWidget *parent) : FrostedFramelessDialog(parent), manager(manager) {
    image_wait_timer_.setInterval(IMAGE_CAPTURE_POLL_MS);
    connect(&image_wait_timer_, &QTimer::timeout, this, [this] { poll_captured_image(); });

    setup_titlebar();

    setWindowTitle(tr_text("Clipboard Management"));
    fit_to_screen();
    setWindowFlags(windowFlags() | Qt::Window | Qt::WindowCloseButtonHint | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint);
    // setWindowFlags recreates the native window, dropping the frameless WS_THICKFRAME and shadow, so the edges no longer resize; add them back.
    helper_.install();
    setAttribute(Qt::WA_DeleteOnClose, false);

    setup_ui();
    // Static controls are built once; they join the standalone window scale before the dynamic forms.
    configure_dialog_controls(*this);
    disable_auto_default(this);
    refresh_group_list();
    on_group_selected(selected_group_id);
    apply_ui_theme(get_ui_theme()->tokens());
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this](const UIThemeTokens &tokens) { apply_ui_theme(tokens); });
}

// ------------------------------------------------------------------
// Window shell
// ------------------------------------------------------------------

void ManageDialog::setup_titlebar() {
    auto *title_bar = new FluentTitleBar(this);
    // Scaled before setup_ui: the content margin follows the title bar height, and an unscaled one puts the columns under the title buttons.
    configure_dialog_control(*title_bar);
    setTitleBar(title_bar);
    title_bar->setDoubleClickEnabled(true);
}

void ManageDialog::fit_to_screen() {
    if (isVisible()) return;
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) fit_to_available(screen->availableGeometry().size());
}

void ManageDialog::fit_to_available(const QSize &available) {
    // Decided first: raising the minimum makes Qt enlarge the window at once, which would then read as dragged.
    const bool user_sized = fitted_size_ && size() != *fitted_size_;
    const auto [min_width, min_height] = fit_manage_dialog_min_size(available.width(), available.height());
    const QSize min_size(min_width, min_height);
    setMinimumSize(min_size);
    QSize target;
    if (user_sized) {
        target = size().boundedTo(available).expandedTo(min_size);
    } else {
        const auto [width, height] = fit_manage_dialog_size(available.width(), available.height());
        target = QSize(width, height);
    }
    if (target != size()) resize(target);
    if (!user_sized) fitted_size_ = size();
}

void ManageDialog::show_and_activate() {
    if (isMinimized()) {
        showNormal();
    } else {
        fit_to_screen();
        show();
    }
    raise();
    activateWindow();
}

void ManageDialog::closeEvent(QCloseEvent *event) {
    // Only hidden, not destroyed.
    hide();
    event->ignore();
}

void ManageDialog::hideEvent(QHideEvent *event) {
    FrostedFramelessDialog::hideEvent(event);
    // The close button and Esc both end here; minimizing is a spontaneous hide and keeps the content.
    if (!event->spontaneous() && !needs_reload_) release_memory();
}

void ManageDialog::release_memory() {
    // The hidden window stays resident, so large data goes: the full image preview, the image to add, group contents and the thumbnail cache.
    clear_detail_layout();
    pending_image_.reset();
    item_list->clear();
    item_types_.clear();
    clear_thumbnail_cache();
    needs_reload_ = true;
    request_trim_working_set();
    log_debug(T("管理窗口已隐藏，已释放预览、列表和缩略图缓存"), "Clipboard");
}

void ManageDialog::showEvent(QShowEvent *event) {
    FrostedFramelessDialog::showEvent(event);
    // Opening at a given place from outside reloads in advance, so nothing loads twice here.
    reload_if_released();
}

void ManageDialog::reload_if_released() {
    // Reloaded before showing or switching forms from outside, or the reload would replace the form just chosen.
    if (needs_reload_) {
        refresh_group_list();
        on_group_selected(selected_group_id);
    }
}

// ------------------------------------------------------------------
// Layout
// ------------------------------------------------------------------

void ManageDialog::setup_ui() {
    const int title_height = titleBar() ? titleBar()->height() : dialog_scaled(32);
    auto *main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(dialog_scaled(10), title_height + dialog_scaled(2), dialog_scaled(10), dialog_scaled(10));
    main_layout->setSpacing(dialog_scaled(10));

    sidebar = create_sidebar();
    main_layout->addWidget(sidebar);

    panel = new QWidget;
    panel->setObjectName("ManagePanel");
    auto *panel_layout = new QHBoxLayout(panel);
    panel_layout->setContentsMargins(0, 0, 0, 0);
    panel_layout->setSpacing(0);

    list_column = create_list_column();
    panel_layout->addWidget(list_column);

    column_separator = new QFrame;
    column_separator->setFixedWidth(1);
    panel_layout->addWidget(column_separator);

    detail_column = create_detail_column();
    panel_layout->addWidget(detail_column, 1);
    main_layout->addWidget(panel, 1);
}

QWidget *ManageDialog::create_sidebar() {
    auto *bar = new QWidget;
    bar->setObjectName("ManageSidebar");
    bar->setFixedWidth(dialog_scaled(228));
    auto *layout = new QVBoxLayout(bar);
    layout->setContentsMargins(dialog_scaled(4), dialog_scaled(4), dialog_scaled(4), dialog_scaled(4));
    layout->setSpacing(dialog_scaled(6));

    groups_caption = new QLabel(tr_text("Groups"));
    groups_caption->setContentsMargins(dialog_scaled(12), dialog_scaled(6), dialog_scaled(6), 0);
    apply_theme_text_style(groups_caption, 12, false, "font-weight: 600;", true);
    layout->addWidget(groups_caption);

    // The same full-width dashed button as the middle column's "Add Content", lined up with the group rows.
    auto *add_row = new QHBoxLayout;
    add_row->setContentsMargins(dialog_scaled(4), 0, dialog_scaled(4), 0);
    new_group_btn = new QPushButton("+  " + tr_text("New Group"));
    new_group_btn->setObjectName("ManageAddButton");
    // Checked while a new group is being created, instead of a highlighted group row.
    new_group_btn->setCheckable(true);
    new_group_btn->setCursor(Qt::PointingHandCursor);
    connect(new_group_btn, &QPushButton::clicked, this, [this] { show_new_group_form(); });
    add_row->addWidget(new_group_btn);
    layout->addLayout(add_row);

    group_list = new ReorderListWidget;
    group_list->setObjectName("ManageGroupList");
    group_list->setItemDelegate(new GroupRowDelegate(group_list));
    connect(group_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current) { on_group_row_changed(current); });
    connect(group_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) { show_edit_group_form(item->data(ID_ROLE).toLongLong()); });
    connect(group_list, &ReorderListWidget::item_moved, this, [this](qint64 id, std::optional<qint64> before, std::optional<qint64> after) { on_group_moved(id, before, after); });
    // At drop the item list is still in its own drag loop; the move and refresh are queued until it ends.
    connect(group_list, &ReorderListWidget::item_dropped_on, this, [this](qint64 item_id, qint64 group_id) { move_item_to_group(item_id, group_id); },
            Qt::QueuedConnection);
    group_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(group_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) { show_group_menu(pos); });
    layout->addWidget(group_list, 1);

    import_export_btn = new QPushButton(tr_text("Import / Export"));
    import_export_btn->setObjectName("ManageSidebarAction");
    import_export_btn->setCursor(Qt::PointingHandCursor);
    connect(import_export_btn, &QPushButton::clicked, this, [this] { switch_mode("import_export"); });
    layout->addWidget(import_export_btn);
    return bar;
}

QWidget *ManageDialog::create_list_column() {
    auto *column = new QWidget;
    column->setObjectName("ManageListColumn");
    column->setFixedWidth(dialog_scaled(340));
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(dialog_scaled(16), dialog_scaled(20), dialog_scaled(16), dialog_scaled(12));
    layout->setSpacing(dialog_scaled(10));

    auto *header = new QHBoxLayout;
    header->setContentsMargins(dialog_scaled(6), 0, 0, 0);
    header->setSpacing(dialog_scaled(6));
    auto *title_box = new QVBoxLayout;
    title_box->setSpacing(dialog_scaled(2));
    list_title = new QLabel;
    apply_theme_text_style(list_title, 16, true);
    list_count = new QLabel;
    apply_theme_text_style(list_count, 12, false, {}, true);
    title_box->addWidget(list_title);
    title_box->addWidget(list_count);
    header->addLayout(title_box, 1);
    // QPushButton's icon-text spacing cannot be styled; a space separates them.
    edit_group_btn = new QPushButton(" " + tr_text("Edit"));
    edit_group_btn->setObjectName("ManageTextButton");
    edit_group_btn->setToolTip(tr_text("Edit Group"));
    edit_group_btn->setCursor(Qt::PointingHandCursor);
    connect(edit_group_btn, &QPushButton::clicked, this, [this] {
        if (selected_group_id) show_edit_group_form(*selected_group_id);
    });
    header->addWidget(edit_group_btn, 0, Qt::AlignTop);
    layout->addLayout(header);

    search_input = new LineEdit;
    search_input->setPlaceholderText(tr_text("Search title or content"));
    search_input->setClearButtonEnabled(true);
    search_action = search_input->addAction(QIcon(), QLineEdit::LeadingPosition);
    connect(search_input, &QLineEdit::textChanged, this, [this](const QString &text) { apply_search_filter(text); });
    layout->addWidget(search_input);

    add_item_btn = new QPushButton("+  " + tr_text("Add Content"));
    add_item_btn->setObjectName("ManageAddButton");
    add_item_btn->setCursor(Qt::PointingHandCursor);
    connect(add_item_btn, &QPushButton::clicked, this, [this] { on_add_item_clicked(); });
    layout->addWidget(add_item_btn);

    list_stack = new QStackedWidget;
    item_list = new ReorderListWidget;
    item_list->setObjectName("ManageItemList");
    item_list->setItemDelegate(new ContentRowDelegate(item_list));
    connect(item_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { open_item_if_needed(item->data(ID_ROLE).toLongLong()); });
    connect(item_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current) {
        // Keyboard up and down opens the editor with the selection; clicks go through itemClicked.
        if (current && item_list->hasFocus()) open_item_if_needed(current->data(ID_ROLE).toLongLong());
    });
    connect(item_list, &ReorderListWidget::item_moved, this, [this](qint64 id, std::optional<qint64> before, std::optional<qint64> after) { on_item_moved(id, before, after); });
    connect(item_list, &ReorderListWidget::drag_started, this, [this] { on_content_drag(true); });
    connect(item_list, &ReorderListWidget::drag_finished, this, [this] { on_content_drag(false); });
    item_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(item_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) { show_item_menu(pos); });
    list_stack->addWidget(item_list);
    group_list->accept_items_from(item_list, [this](qint64 item_id, qint64 group_id) { return can_move_item_to_group(item_id, group_id); });

    empty_label = new QLabel;
    empty_label->setAlignment(Qt::AlignCenter);
    empty_label->setWordWrap(true);
    apply_theme_text_style(empty_label, 12, false, QString("padding: %1px;").arg(dialog_scaled(24)), true);
    list_stack->addWidget(empty_label);
    layout->addWidget(list_stack, 1);

    reorder_hint = new QLabel(tr_text("Drag or press Alt+Up/Down to reorder"));
    reorder_hint->setAlignment(Qt::AlignCenter);
    apply_theme_text_style(reorder_hint, 12, false, {}, true);
    layout->addWidget(reorder_hint);
    return column;
}

QWidget *ManageDialog::create_detail_column() {
    auto *column = new QWidget;
    column->setObjectName("ManageDetailColumn");
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(dialog_scaled(36), dialog_scaled(24), dialog_scaled(36), dialog_scaled(20));
    layout->setSpacing(dialog_scaled(6));

    detail_title = new QLabel;
    apply_theme_text_style(detail_title, 18, true);
    layout->addWidget(detail_title);

    detail_subtitle = new QLabel;
    detail_subtitle->setWordWrap(true);
    apply_theme_text_style(detail_subtitle, 12, false, {}, true);
    detail_subtitle->hide();
    layout->addWidget(detail_subtitle);
    layout->addSpacing(dialog_scaled(12));

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");
    detail_content = new QWidget;
    detail_content->setObjectName("ManageDetailContent");
    detail_layout = new QVBoxLayout(detail_content);
    detail_layout->setContentsMargins(0, 0, dialog_scaled(2), 0);
    detail_layout->setSpacing(dialog_scaled(8));
    scroll->setWidget(detail_content);
    layout->addWidget(scroll, 1);

    auto *bottom = new QVBoxLayout;
    bottom->setContentsMargins(0, dialog_scaled(12), 0, 0);
    bottom->setSpacing(dialog_scaled(8));
    // On its own line: beside the buttons the narrowest window has no room for it in every language.
    detail_meta = new QLabel;
    apply_theme_text_style(detail_meta, 12, false, {}, true);
    detail_meta->setWordWrap(true);
    detail_meta->hide();
    bottom->addWidget(detail_meta);
    auto *footer = new QHBoxLayout;
    footer->setSpacing(dialog_scaled(10));
    footer->addStretch(1);

    pin_btn = new QPushButton(tr_text("Pin to Screen"));
    pin_btn->setObjectName("ManagePinButton");
    pin_btn->setCursor(Qt::PointingHandCursor);
    connect(pin_btn, &QPushButton::clicked, this, [this] {
        if (editing_item_id) create_pin_from_clipboard_item(*editing_item_id, manager, this);
    });
    pin_btn->hide();
    footer->addWidget(pin_btn);

    delete_btn = new QPushButton(tr_text("Delete"));
    delete_btn->setObjectName("ManageDeleteButton");
    delete_btn->setCursor(Qt::PointingHandCursor);
    connect(delete_btn, &QPushButton::clicked, this, [this] { on_delete_clicked(); });
    delete_btn->hide();
    footer->addWidget(delete_btn);

    save_btn = new QPushButton(tr_text("Save"));
    save_btn->setObjectName("ManageSaveButton");
    save_btn->setCursor(Qt::PointingHandCursor);
    connect(save_btn, &QPushButton::clicked, this, [this] { on_save_clicked(); });
    footer->addWidget(save_btn);
    bottom->addLayout(footer);
    layout->addLayout(bottom);
    return column;
}

// ------------------------------------------------------------------
// Theme
// ------------------------------------------------------------------

void ManageDialog::refresh_theme_scope() {
    // Controls re-apply their Fluent style once in this window; those without _apply_theme only repaint.
    for (QWidget *widget : findChildren<QWidget *>()) {
        if (widget->metaObject()->indexOfMethod("_apply_theme()") >= 0) {
            QMetaObject::invokeMethod(widget, "_apply_theme", Qt::DirectConnection);
        } else if (auto *area = qobject_cast<QAbstractScrollArea *>(widget)) {
            area->viewport()->update();
        } else {
            widget->update();
        }
    }
}

void ManageDialog::apply_ui_theme(const UIThemeTokens &tokens) {
    const auto s = [](int value) { return QString::number(dialog_scaled(value)); };
    refresh_theme_scope();
    refresh_theme_widget_styles(this);
    setStyleSheet(QString(R"(
            QWidget#ManagePanel {
                background: %1;
                border: 1px solid %2;
                border-radius: %3px;
            }
            QWidget#ManageSidebar, QWidget#ManageListColumn,
            QWidget#ManageDetailColumn, QWidget#ManageDetailContent {
                background: transparent;
            }
            QListWidget {
                background: transparent;
                border: none;
                outline: none;
            }
            QFrame#ManageSectionCard {
                background: %4;
                border: 1px solid %2;
                border-radius: %3px;
            }
            QTextEdit {
                border: 1px solid %5;
                border-radius: %6px;
                padding: %7px %3px;
                font-size: %8px;
                background: %4;
                color: %9;
                selection-background-color: %10;
            }
            QTextEdit:focus {
                border-color: %10;
            }
            QPushButton#ManageTextButton {
                background: transparent;
                border: none;
                border-radius: %6px;
                color: %11;
                font-size: %3px;
                font-weight: 600;
                padding: %12px %7px;
            }
            QPushButton#ManageTextButton:hover {
                background: %13;
            }
            QPushButton#ManageSidebarAction {
                background: transparent;
                border: 1px solid transparent;
                border-radius: %6px;
                color: %14;
                font-size: %3px;
                padding: %6px %3px;
                text-align: left;
            }
            QPushButton#ManageSidebarAction:hover {
                background: %13;
                color: %11;
            }
            QPushButton#ManageAddButton {
                background: transparent;
                border: 1px dashed %15;
                border-radius: %7px;
                color: %11;
                font-size: %3px;
                font-weight: 600;
                min-height: %16px;
            }
            QPushButton#ManageAddButton:hover {
                background: %13;
                border-color: %10;
            }
            QPushButton#ManagePinButton,
            QPushButton#ManageDeleteButton,
            QPushButton#ManageSaveButton {
                background: %4;
                border: 1px solid %5;
                border-radius: %26px;
                color: %9;
                font-size: %8px;
                font-weight: 500;
                min-height: %18px;
                min-width: %23px;
                padding: 0 %19px;
            }
            QPushButton#ManagePinButton:hover,
            QPushButton#ManageDeleteButton:hover {
                background: %13;
                border-color: %15;
            }
            QPushButton#ManagePinButton:pressed {
                background: %13;
                border-color: %10;
            }
            QPushButton#ManageDeleteButton {
                color: %17;
            }
            QPushButton#ManageDeleteButton:pressed {
                background: %20;
                border-color: %17;
            }
            QPushButton#ManageSaveButton {
                background: %21;
                border-color: %21;
                color: #FFFFFF;
                min-width: %22px;
            }
            QPushButton#ManageSaveButton:hover {
                background: %24;
                border-color: %24;
            }
            QPushButton#ManageSaveButton:pressed {
                background: %25;
                border-color: %25;
            }
            QPushButton#ManagePinButton:disabled,
            QPushButton#ManageDeleteButton:disabled,
            QPushButton#ManageSaveButton:disabled {
                background: %28;
                border-color: %5;
                color: %27;
            }
        )")
                      .arg(theme_surface_color(), tokens.separator, s(12), tokens.input_background, tokens.border, s(8), s(10), s(13), tokens.text)
                      .arg(tokens.accent, tokens.accent_text, s(4), tokens.accent_soft, tokens.text_muted, tokens.border_hover, s(34), tokens.danger, s(30))
                      .arg(s(8), tokens.danger_soft, tokens.accent_strong, s(66), s(50), tokens.accent_strong_hover, tokens.accent_strong_pressed)
                      .arg(s(6), tokens.text_disabled, tokens.surface_subtle) +
                  QString("QPushButton#ManageAddButton:checked { background: %1; border: 1px solid %2; }").arg(tokens.accent_soft, tokens.accent) +
                  scrollbar_qss(this));
    set_own_style(column_separator, QString("background: %1; border: none;").arg(tokens.separator));
    search_action->setIcon(tinted_icon(FluentIcon::Search, tokens.text_muted));
    import_export_btn->setIcon(tinted_icon(FluentIcon::Sync, tokens.text_muted));
    edit_group_btn->setIcon(tinted_icon(FluentIcon::Edit, tokens.accent_text));
    apply_dynamic_form_theme();
    update();
}

void ManageDialog::apply_dynamic_form_theme() {
    // Form controls rebuilt when the selection or mode changes.
    refresh_theme_widget_styles(detail_content);
    for (int index = 0; index < emoji_tab_buttons.size(); ++index) emoji_tab_buttons[index]->setStyleSheet(emoji_tab_style(index == emoji_current_idx, this));
}

// ------------------------------------------------------------------
// Group list
// ------------------------------------------------------------------

QString ManageDialog::group_tag(const Group &group) const {
    if (group.group_type == GroupType::FILE) return tr_text("Launch");
    if (group.group_type == GroupType::HIDDEN) return tr_text("Hidden");
    if (group.is_filter()) return tr_text("Filter");
    return {};
}

void ManageDialog::refresh_group_list() {
    const auto groups = manager->get_groups();
    group_types_.clear();
    bool selected_exists = false;
    for (const auto &group : groups) {
        group_types_.insert(group.id, group.group_type);
        selected_exists = selected_exists || (selected_group_id && group.id == *selected_group_id);
    }
    if (!selected_exists) selected_group_id = groups.isEmpty() ? std::nullopt : std::optional<qint64>(groups.front().id);

    group_list->blockSignals(true);
    group_list->clear();
    for (const auto &group : groups) {
        auto *item = new QListWidgetItem(group.name);
        item->setData(ID_ROLE, group.id);
        item->setData(ICON_ROLE, display_icon(group));
        item->setData(TAG_ROLE, group_tag(group));
        item->setData(KIND_ROLE, group_kind(group.group_type));
        group_list->addItem(item);
    }
    const int row = group_list->row_of_id(selected_group_id);
    if (row >= 0 && !creating_group_) group_list->setCurrentRow(row);
    group_list->blockSignals(false);
}

void ManageDialog::enter_creating_group() {
    new_group_btn->setChecked(true);
    if (creating_group_) return;
    creating_group_ = true;
    group_list->blockSignals(true);
    group_list->clearSelection();
    group_list->setCurrentRow(-1);
    group_list->blockSignals(false);
    refresh_content_list();
}

void ManageDialog::leave_creating_group(bool refresh) {
    new_group_btn->setChecked(false);
    if (!creating_group_) return;
    creating_group_ = false;
    const int row = group_list->row_of_id(selected_group_id);
    group_list->blockSignals(true);
    if (row >= 0) group_list->setCurrentRow(row);
    group_list->blockSignals(false);
    if (refresh) refresh_content_list();
}

void ManageDialog::on_group_row_changed(QListWidgetItem *current) {
    if (!current) return;
    on_group_selected(current->data(ID_ROLE).toLongLong());
}

void ManageDialog::on_group_selected(std::optional<qint64> group_id) {
    // Filter groups open their settings; stored-content groups open the new item form.
    selected_group_id = group_id;
    leave_creating_group(false);
    if (!search_input->text().isEmpty()) {
        search_input->blockSignals(true);
        search_input->clear();
        search_input->blockSignals(false);
    }
    refresh_content_list();
    show_new_content_form();
}

std::optional<Group> ManageDialog::get_selected_group() {
    if (!selected_group_id) return std::nullopt;
    for (const auto &group : manager->get_groups())
        if (group.id == *selected_group_id) return group;
    return std::nullopt;
}

void ManageDialog::on_group_moved(qint64 group_id, std::optional<qint64> before_id, std::optional<qint64> after_id) {
    if (manager->move_group_between(group_id, before_id, after_id)) {
        emit group_added();
        emit data_changed();
        return;
    }
    log_error(T("拖拽分组移动失败"), "GroupDrag");
    refresh_group_list();
}

void ManageDialog::show_group_menu(const QPoint &pos) {
    QListWidgetItem *item = group_list->itemAt(pos);
    if (!item) return;
    group_list->setCurrentItem(item);
    const qint64 group_id = item->data(ID_ROLE).toLongLong();
    QMenu menu(this);
    menu.setStyleSheet(theme_menu_style());
    add_menu_action(&menu, tr_text("Edit Group"), [this, group_id] { show_edit_group_form(group_id); });
    menu.addSeparator();
    add_menu_action(&menu, tr_text("Move to Top"), [this] { group_list->move_current_to_top(); });
    add_menu_action(&menu, tr_text("Move to Bottom"), [this] { group_list->move_current_to_bottom(); });
    menu.addSeparator();
    add_menu_action(&menu, tr_text("Delete Group"), [this, group_id] { delete_group(group_id); });
    menu.exec(group_list->viewport()->mapToGlobal(pos));
}

// ------------------------------------------------------------------
// Item list
// ------------------------------------------------------------------

QListWidgetItem *ManageDialog::item_row(const ClipboardItem &item) const {
    auto *row = new QListWidgetItem(item.display_text());
    row->setData(ID_ROLE, item.id);
    row->setData(ICON_ROLE, item.icon());
    row->setData(THUMB_ROLE, item.thumbnail.value_or(QString()));
    row->setData(META_ROLE, short_time(item.created_at));
    row->setData(SEARCH_ROLE, item.content_type == "file" ? extract_first_file_path_from_content(item.content) : item.content);
    return row;
}

void ManageDialog::refresh_content_list() {
    needs_reload_ = false;
    if (creating_group_) {
        show_new_group_placeholder();
        return;
    }
    const auto group = get_selected_group();
    const bool stores_content = group.has_value() && !group->is_filter();
    edit_group_btn->setVisible(group.has_value());
    add_item_btn->setVisible(!group || !group->is_filter());
    search_input->setVisible(!group || !group->is_filter());
    add_item_btn->setEnabled(stores_content);
    search_input->setEnabled(stores_content);

    item_list->blockSignals(true);
    item_list->clear();
    if (!group) {
        list_title->setText(tr_text("No Group"));
        list_count->setText("");
        show_list_empty(tr_text("Create a group on the left first"));
        item_list->blockSignals(false);
        return;
    }
    list_title->setText(display_icon(*group) + "  " + group->name);
    if (group->is_filter()) {
        list_count->setText("");
        item_types_.clear();
        show_list_empty(tr_text("Filter groups store no content; they show the clipboard history that matches the chosen type and source app"));
        item_list->blockSignals(false);
        return;
    }
    const auto items = manager->get_by_group(group->id, 0, ITEMS_PER_GROUP);
    list_count->setText(tr_text("{count} items").replace("{count}", QString::number(items.size())));
    item_types_.clear();
    for (const auto &item : items) {
        item_types_.insert(item.id, {item.content_type, item.sub_kind});
        item_list->addItem(item_row(item));
    }
    const int row = item_list->row_of_id(editing_item_id);
    if (row >= 0) item_list->setCurrentRow(row);
    item_list->blockSignals(false);
    apply_search_filter(search_input->text());
}

void ManageDialog::show_new_group_placeholder() {
    // The new group is not saved yet: nothing to list and nothing can be added.
    edit_group_btn->setVisible(false);
    add_item_btn->setVisible(true);
    search_input->setVisible(true);
    add_item_btn->setEnabled(false);
    search_input->setEnabled(false);
    item_list->blockSignals(true);
    item_list->clear();
    item_list->blockSignals(false);
    item_types_.clear();
    list_title->setText(tr_text("New Group"));
    list_count->setText("");
    show_list_empty(tr_text("Save the new group first, then add content to it"));
}

void ManageDialog::show_list_empty(const QString &text) {
    empty_label->setText(text);
    list_stack->setCurrentWidget(empty_label);
    reorder_hint->hide();
}

void ManageDialog::apply_search_filter(const QString &text) {
    const QString keyword = text.trimmed();
    if (!selected_group_id) return;
    if (item_list->count() == 0) {
        show_list_empty(tr_text("No content in this group yet"));
        return;
    }
    int visible = 0;
    for (int row = 0; row < item_list->count(); ++row) {
        QListWidgetItem *row_item = item_list->item(row);
        const QString haystack = row_item->text() + " " + row_item->data(SEARCH_ROLE).toString();
        const bool hidden = !keyword.isEmpty() && !haystack.contains(keyword, Qt::CaseInsensitive);
        item_list->setRowHidden(row, hidden);
        visible += hidden ? 0 : 1;
    }
    const int total = item_list->count();
    list_count->setText(!keyword.isEmpty()
                            ? tr_text("{visible} of {count} items").replace("{visible}", QString::number(visible)).replace("{count}", QString::number(total))
                            : tr_text("{count} items").replace("{count}", QString::number(total)));
    // A filtered list holds only some rows, so drag reordering would land in the wrong place.
    item_list->set_reorder_enabled(keyword.isEmpty());
    reorder_hint->setVisible(keyword.isEmpty());
    if (visible) list_stack->setCurrentWidget(item_list);
    else show_list_empty(tr_text("No matching content"));
}

void ManageDialog::open_item_if_needed(qint64 item_id) {
    if (item_id != editing_item_id || current_mode != "content") show_edit_content_form(item_id);
}

void ManageDialog::on_item_moved(qint64 item_id, std::optional<qint64> before_id, std::optional<qint64> after_id) {
    if (manager->move_item_between(item_id, before_id, after_id)) {
        emit content_added(selected_group_id.value_or(0));
        emit data_changed();
        return;
    }
    log_error(T("拖拽内容移动失败"), "ItemDrag");
    refresh_content_list();
}

void ManageDialog::show_item_menu(const QPoint &pos) {
    QListWidgetItem *item = item_list->itemAt(pos);
    if (!item) return;
    item_list->setCurrentItem(item);
    const qint64 item_id = item->data(ID_ROLE).toLongLong();
    show_edit_content_form(item_id);
    QMenu menu(this);
    menu.setStyleSheet(theme_menu_style());
    // As in the clipboard window: images, text and files, which pin as the pin hotkey pins copied files.
    const auto clipboard_item = manager->get_item(item_id);
    add_menu_action(&menu, tr_text("Pin to Screen"), [this, item_id] { create_pin_from_clipboard_item(item_id, manager, this); })
        ->setEnabled(clipboard_item && can_pin(*clipboard_item));
    menu.addSeparator();
    add_menu_action(&menu, tr_text("Move to Top"), [this] { item_list->move_current_to_top(); });
    add_menu_action(&menu, tr_text("Move to Bottom"), [this] { item_list->move_current_to_bottom(); });
    QMenu *group_menu = menu.addMenu(tr_text("Move to Group"));
    group_menu->setStyleSheet(theme_menu_style());
    for (const auto &group : manager->get_groups()) {
        if (selected_group_id && group.id == *selected_group_id) continue;
        const qint64 group_id = group.id;
        QAction *action = add_menu_action(group_menu, display_icon(group) + "  " + group.name, [this, item_id, group_id] { move_item_to_group(item_id, group_id); });
        action->setEnabled(can_move_item_to_group(item_id, group.id));
    }
    group_menu->setEnabled(!group_menu->isEmpty());
    menu.addSeparator();
    add_menu_action(&menu, tr_text("Delete"), [this] { on_delete_clicked(); });
    menu.exec(item_list->viewport()->mapToGlobal(pos));
}

void ManageDialog::on_content_drag(bool dragging) { group_list->set_awaiting_foreign_drop(dragging, tr_text("Drop on a group to move it there")); }

bool ManageDialog::can_move_item_to_group(qint64 item_id, std::optional<qint64> group_id) const {
    if (!group_id || group_id == selected_group_id) return false;
    // What each group type takes is decided by the Rust core (move_to_group refuses the rest too); this only greys the target out.
    const auto [content_type, sub_kind] = item_types_.value(item_id, {"text", {}});
    return group_accepts(group_types_.value(*group_id, static_cast<int>(GroupType::NORMAL)), content_type, sub_kind);
}

void ManageDialog::move_item_to_group(qint64 item_id, qint64 group_id) {
    if (!can_move_item_to_group(item_id, group_id)) return;
    if (!manager->move_to_group(item_id, group_id)) {
        show_warning_dialog(this, tr_text("Failed"), tr_text("Failed to move to group"));
        return;
    }
    const bool was_editing = editing_item_id == item_id;
    if (was_editing) editing_item_id.reset();
    emit content_added(selected_group_id.value_or(0));
    emit content_added(group_id);
    emit data_changed();
    refresh_content_list();
    if (was_editing) show_new_content_form();
}

void ManageDialog::on_add_item_clicked() {
    item_list->clearSelection();
    item_list->setCurrentRow(-1);
    show_new_content_form();
}

// ------------------------------------------------------------------
// Entry points from outside
// ------------------------------------------------------------------

void ManageDialog::switch_mode(const QString &mode) {
    reload_if_released();
    if (mode == "group") show_new_group_form();
    else if (mode == "import_export") show_import_export_form();
    else show_new_content_form();
}

void ManageDialog::switch_page(int index) { switch_mode(index == 0 ? "group" : "content"); }

void ManageDialog::select_group(std::optional<qint64> group_id) {
    const int row = group_list->row_of_id(group_id);
    if (row < 0) return;
    if (row == group_list->currentRow()) on_group_selected(group_id);
    else group_list->setCurrentRow(row);
}

void ManageDialog::refresh_after_external_change(std::optional<qint64> deleted_group_id) {
    setUpdatesEnabled(false);
    if (deleted_group_id) {
        if (editing_group_id == deleted_group_id) editing_group_id.reset();
        if (selected_group_id == deleted_group_id) selected_group_id.reset();
    }
    if (editing_item_id && !manager->get_item(*editing_item_id)) editing_item_id.reset();

    refresh_group_list();
    refresh_content_list();
    if (current_mode == "group") {
        if (editing_group_id) show_edit_group_form(*editing_group_id);
        else show_new_group_form();
    } else if (current_mode == "content") {
        if (editing_item_id) show_edit_content_form(*editing_item_id);
        else show_new_content_form();
    }
    setUpdatesEnabled(true);
}

void ManageDialog::open_group_editor(qint64 group_id) {
    setUpdatesEnabled(false);
    refresh_group_list();
    select_group(group_id);
    show_edit_group_form(group_id);
    setUpdatesEnabled(true);
    show_and_activate();
}

void ManageDialog::open_item_editor(qint64 item_id, std::optional<qint64> group_id) {
    setUpdatesEnabled(false);
    refresh_group_list();
    if (group_id) select_group(group_id);
    editing_item_id = item_id;
    refresh_content_list();
    show_edit_content_form(item_id);
    setUpdatesEnabled(true);
    show_and_activate();
}

// ------------------------------------------------------------------
// Right column: switching forms
// ------------------------------------------------------------------

void ManageDialog::clear_detail_layout() {
    detail_form_token += 1;
    const QList<QWidget *> stale{group_name_input, icon_input, icon_preview, radio_normal, radio_file, radio_hidden, radio_filter, filter_kind_box,
                                 filter_kind_combo, filter_app_combo, emoji_scroll, image_preview, content_kind_switch, image_drop_zone, paste_image_btn};
    for (QWidget *widget : stale)
        if (widget) widget->blockSignals(true);
    if (group_type_btn_group) group_type_btn_group->blockSignals(true);
    group_name_input = nullptr;
    icon_input = nullptr;
    icon_preview = nullptr;
    radio_normal = nullptr;
    radio_file = nullptr;
    radio_hidden = nullptr;
    radio_filter = nullptr;
    filter_kind_box = nullptr;
    filter_kind_combo = nullptr;
    filter_app_combo = nullptr;
    group_type_btn_group = nullptr;
    emoji_scroll = nullptr;
    image_preview = nullptr;
    content_kind_switch = nullptr;
    editing_image_size.reset();
    image_drop_zone = nullptr;
    paste_image_btn = nullptr;
    emoji_tab_buttons.clear();
    emoji_group_order.clear();
    emoji_groups.clear();
    detail_meta->clear();
    detail_meta->hide();

    std::function<void(QLayout *)> clear_layout = [&clear_layout](QLayout *layout) {
        while (layout->count()) {
            QLayoutItem *child = layout->takeAt(0);
            if (QWidget *widget = child->widget()) {
                // Qt queues showing controls just added to a layout; without hiding first, that show pops them up as windows once unparented.
                widget->hide();
                widget->setParent(nullptr);
                widget->deleteLater();
            } else if (QLayout *nested = child->layout()) {
                clear_layout(nested);
            }
            delete child;
        }
    };
    clear_layout(detail_layout);
}

void ManageDialog::finish_detail_form_setup() {
    // The detail form is rebuilt per selection and mode, out of reach of the one-time setup; only this part is marked so existing controls are not enlarged twice.
    configure_dialog_controls(*detail_content);
    disable_auto_default(detail_content);
    refresh_theme_scope();
    apply_dynamic_form_theme();
}

void ManageDialog::disable_auto_default(QWidget *root) {
    // Buttons in a QDialog are autoDefault, so Enter in the search or title field would trigger delete or new.
    for (auto *button : root->findChildren<QPushButton *>()) button->setAutoDefault(false);
}

bool ManageDialog::is_current_detail_form_token(std::optional<int> form_token) const { return !form_token || *form_token == detail_form_token; }

void ManageDialog::set_detail_header(const QString &title, const QString &subtitle) {
    detail_title->setText(title);
    detail_subtitle->setText(subtitle);
    detail_subtitle->setVisible(!subtitle.isEmpty());
}

void ManageDialog::set_footer(const std::optional<QString> &save_text, bool deletable) {
    save_btn->setVisible(save_text.has_value());
    save_btn->setEnabled(true);
    save_btn->setToolTip({});
    if (save_text) save_btn->setText(*save_text);
    delete_btn->setVisible(deletable);
    pin_btn->hide();
}

void ManageDialog::show_new_group_form() {
    current_mode = "group";
    clear_detail_layout();
    set_footer(tr_text("Create"), false);
    editing_group_id.reset();
    enter_creating_group();
    build_new_group_form(this);
    update_group_form_header();
    finish_detail_form_setup();
    group_name_input->setFocus();
}

void ManageDialog::show_edit_group_form(qint64 group_id) {
    std::optional<Group> group;
    for (const auto &candidate : manager->get_groups())
        if (candidate.id == group_id) group = candidate;
    if (!group) return;
    leave_creating_group();
    current_mode = "group";
    clear_detail_layout();
    set_footer(tr_text("Save"), true);
    editing_group_id = group_id;
    build_edit_group_form(this, *group);
    update_group_form_header();
    finish_detail_form_setup();
}

void ManageDialog::on_group_type_toggled(bool checked, std::optional<int> form_token) {
    // An icon the user did not change follows the group type.
    if (!is_current_detail_form_token(form_token)) return;
    if (!icon_input) return;
    if (!checked) return;
    icon_input->setText(get_toggled_default_group_icon(icon_input->text(), selected_group_type(), selected_filter_rule()));
    if (filter_kind_box && radio_filter) filter_kind_box->setVisible(radio_filter->isChecked());
    // A filter on neither a type nor an app would be the whole history: it cannot be saved.
    const auto rule = selected_filter_rule();
    const bool empty_filter = rule && !rule->kind && !rule->app;
    save_btn->setEnabled(!empty_filter);
    save_btn->setToolTip(empty_filter ? tr_text("Choose a content type or a source app") : QString());
    refresh_group_icon_kind(this);
    update_group_form_header();
}

void ManageDialog::on_filter_kind_changed(const QString &, std::optional<int> form_token) {
    // The default icon follows the filtered type; a custom icon stays.
    on_group_type_toggled(true, form_token);
}

int ManageDialog::selected_group_type() const {
    if (radio_hidden && radio_hidden->isChecked()) return static_cast<int>(GroupType::HIDDEN);
    if (radio_file && radio_file->isChecked()) return static_cast<int>(GroupType::FILE);
    if (radio_filter && radio_filter->isChecked()) return static_cast<int>(GroupType::FILTER);
    return static_cast<int>(GroupType::NORMAL);
}

std::optional<FilterRule> ManageDialog::selected_filter_rule() const {
    if (selected_group_type() != GroupType::FILTER || !filter_kind_combo || !filter_app_combo) return std::nullopt;
    const auto value = [](const ComboBox *combo) {
        const QString data = combo->currentData().toString();
        return data.isEmpty() ? std::nullopt : std::optional(data);
    };
    return FilterRule{value(filter_kind_combo), value(filter_app_combo)};
}

void ManageDialog::update_group_form_header() {
    if (!radio_file) return;
    const bool is_new = !editing_group_id;
    QString title, subtitle;
    if (radio_filter && radio_filter->isChecked()) {
        title = is_new ? tr_text("New Filter Group") : tr_text("Edit Filter Group");
        subtitle = tr_text("Filter groups store no content; they show the clipboard history that matches the chosen type and source app");
    } else if (radio_hidden && radio_hidden->isChecked()) {
        title = is_new ? tr_text("New Hidden Group") : tr_text("Edit Hidden Group");
        subtitle = tr_text("Hidden groups are not displayed on the clipboard panel; changing to normal group restores visibility");
    } else if (radio_file->isChecked()) {
        title = is_new ? tr_text("New Quick Launch Group") : tr_text("Edit Quick Launch Group");
        subtitle = tr_text("Quick launch groups accept files, folder paths and links; selecting an item opens them");
    } else {
        title = is_new ? tr_text("New General Group") : tr_text("Edit General Group");
        subtitle = tr_text("General groups allow any content; selecting an item pastes content or files to the target");
    }
    set_detail_header(title, subtitle);
}

void ManageDialog::create_emoji_picker(const QString &current_icon) { create_group_icon_picker(this, current_icon); }

void ManageDialog::switch_emoji_group_checked(int group_idx, std::optional<int> form_token) {
    if (!is_current_detail_form_token(form_token)) return;
    if (!emoji_scroll) return;
    if (emoji_tab_buttons.isEmpty()) return;
    switch_emoji_group(this, group_idx);
}

void ManageDialog::on_icon_input_changed_checked(const QString &text, std::optional<int> form_token) {
    if (!is_current_detail_form_token(form_token)) return;
    if (!icon_input || !icon_preview) return;
    on_icon_input_changed(this, text);
}

void ManageDialog::on_preset_icon_clicked_checked(const QString &icon, std::optional<int> form_token) {
    if (!is_current_detail_form_token(form_token)) return;
    if (!icon_input || !icon_preview) return;
    on_preset_icon_clicked(this, icon);
}

bool ManageDialog::is_file_content_mode() {
    // Hidden groups behave like normal ones and use no file form.
    if (editing_item_id) {
        if (const auto item = manager->get_item(*editing_item_id)) return item->content_type == "file";
    }
    const auto selected_group = get_selected_group();
    if (!selected_group) return false;
    if (selected_group->group_type == GroupType::FILE) return new_content_kind_ != "link";
    return new_content_kind_ == "file";
}

void ManageDialog::show_new_content_form() {
    leave_creating_group();
    editing_item_id.reset();
    const auto group = get_selected_group();
    if (group && group->is_filter()) {
        show_edit_group_form(group->id);
        return;
    }
    current_mode = "content";
    clear_detail_layout();

    if (!group) {
        set_detail_header(tr_text("Add Content"));
        set_footer(std::nullopt, false);
        auto *hint = new QLabel(tr_text("Please create a group first"));
        apply_theme_text_style(hint, 12, false, {}, true);
        detail_layout->addWidget(hint);
        detail_layout->addStretch();
        finish_detail_form_setup();
        return;
    }
    set_detail_header(tr_text("Add Content"), tr_text("Saved to \"{group}\"").replace("{group}", group->name));
    set_footer(tr_text("Add"), false);
    if (group->group_type == GroupType::FILE) {
        if (new_content_kind_ != "link") new_content_kind_ = "file";
        add_content_kind_switch();
        if (new_content_kind_ == "file") build_file_content_form(this);
        else {
            build_text_content_form(this);
            title_input->setPlaceholderText(tr_text("Enter title..."));
            content_edit->setPlaceholderText("https://example.com");
        }
    } else {
        if (new_content_kind_ == "link") new_content_kind_ = "text";
        add_content_kind_switch();
        if (new_content_kind_ == "file") build_file_content_form(this);
        else if (new_content_kind_ == "image") build_new_image_form();
        else build_text_content_form(this);
    }
    finish_detail_form_setup();
}

void ManageDialog::add_content_kind_switch() {
    auto *kind_switch = new SegmentedWidget;
    const auto group = get_selected_group();
    if (group && group->group_type == GroupType::FILE) {
        kind_switch->addItem("link", tr_text("Link"));
    } else {
        kind_switch->addItem("text", tr_text("Text"));
        kind_switch->addItem("image", tr_text("Image"));
    }
    kind_switch->addItem("file", tr_text("File"));
    kind_switch->setCurrentItem(new_content_kind_);
    connect(kind_switch, &SegmentedWidget::currentItemChanged, this, [this](const QString &kind) { set_new_content_kind(kind); });
    content_kind_switch = kind_switch;
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, dialog_scaled(6));
    row->addWidget(kind_switch);
    row->addStretch();
    detail_layout->addLayout(row);
}

void ManageDialog::set_new_content_kind(const QString &kind) {
    if (kind == new_content_kind_) return;
    // Switching type carries the entered title to the new form.
    const QString title = title_input ? title_input->text() : QString();
    new_content_kind_ = kind;
    show_new_content_form();
    if (!title.isEmpty() && title_input) title_input->setText(title);
}

void ManageDialog::build_new_image_form() {
    pending_image_.reset();
    clipboard::build_new_image_form(this, manager->is_monitoring());
}

void ManageDialog::set_pending_image(const QImage &image) {
    pending_image_ = image.isNull() ? std::nullopt : std::optional<QImage>(image);
    if (image_drop_zone) image_drop_zone->set_image(pending_image_);
}

void ManageDialog::use_clipboard_image() {
    const QImage image = QApplication::clipboard()->image();
    if (image.isNull()) {
        show_warning_dialog(this, tr_text("Hint"), tr_text("There is no image on the clipboard"));
        return;
    }
    set_pending_image(image);
}

void ManageDialog::browse_image_file() {
    const auto [path, filter] =
        get_open_file_name(this, tr_text("Choose Image File"), "", tr_text("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.ico)"));
    if (path.isEmpty()) return;
    const QImage image(path);
    if (image.isNull()) {
        show_warning_dialog(this, tr_text("Hint"), tr_text("This file could not be opened as an image"));
        return;
    }
    set_pending_image(image);
}

void ManageDialog::set_selected_file_path(const QString &path) {
    const QString normalized_path = QDir::toNativeSeparators(QDir::cleanPath(path));
    selected_file_path = normalized_path;
    if (file_path_input) file_path_input->setText(normalized_path);
}

void ManageDialog::on_browse_file() {
    const auto [path, filter] = get_open_file_name(this, tr_text("Select File"), "", tr_text("All Files (*.*)"));
    if (!path.isEmpty()) set_selected_file_path(path);
}

void ManageDialog::on_browse_folder() {
    const QString path = get_existing_directory(this, tr_text("Select Folder"), "");
    if (!path.isEmpty()) set_selected_file_path(path);
}

void ManageDialog::show_edit_content_form(qint64 item_id) {
    const auto item = manager->get_item(item_id);
    if (!item) return;
    leave_creating_group();
    current_mode = "content";
    clear_detail_layout();
    set_detail_header(tr_text("Edit Content"));
    set_footer(tr_text("Save"), true);
    pin_btn->setVisible(can_pin(*item));
    editing_item_id = item_id;

    const int row = item_list->row_of_id(item_id);
    if (row >= 0 && item_list->currentRow() != row) {
        item_list->blockSignals(true);
        item_list->setCurrentRow(row);
        item_list->blockSignals(false);
    }

    if (item->content_type == "file") {
        build_edit_file_content_form(this, *item, item->content_type == "file" ? extract_first_file_path_from_content(item->content) : QString());
        detail_layout->addStretch();
    } else if (item->content_type == "image") {
        const auto image_data = item->image_id && !item->image_id->isEmpty() ? manager->get_image_data(*item->image_id) : std::nullopt;
        QImage image;
        if (image_data && !image_data->isEmpty()) image = QImage::fromData(*image_data);
        editing_image_size = image.isNull() ? std::nullopt : std::optional<QSize>(image.size());
        build_image_content_form(this, *item, image, image_data ? image_data->size() : 0);
    } else {
        build_edit_text_content_form(this, *item);
    }

    if (item->created_at.isValid()) {
        detail_meta->setText(tr_text("Created {time}").replace("{time}", item->created_at.toString("yyyy-MM-dd HH:mm")));
        detail_meta->show();
    }
    finish_detail_form_setup();
}

void ManageDialog::show_import_export_form() {
    leave_creating_group();
    current_mode = "import_export";
    clear_detail_layout();
    set_detail_header(tr_text("Import/Export"), tr_text("Back up saved text content as CSV, or import it on another computer"));
    set_footer(std::nullopt, false);
    item_list->clearSelection();
    build_import_export_form(this);
    finish_detail_form_setup();
}

// ------------------------------------------------------------------
// Save / delete
// ------------------------------------------------------------------

QString ManageDialog::get_selected_icon() const {
    // Only the first non-blank character of the field.
    if (icon_input) {
        const QString icon = first_icon_cluster(icon_input->text());
        if (!icon.isEmpty()) return icon;
    }
    return QString::fromUtf8("📁");
}

void ManageDialog::on_save_clicked() {
    if (current_mode == "group") save_group();
    else if (current_mode == "content") save_content();
}

void ManageDialog::save_group() {
    if (!group_name_input) return;
    const QString name = group_name_input->text().trimmed();
    if (name.isEmpty()) {
        show_warning_dialog(this, tr_text("Hint"), tr_text("Please enter group name"));
        return;
    }
    if (group_name_exists(manager->get_groups(), name, editing_group_id)) {
        show_warning_dialog(this, tr_text("Hint"), tr_text("A group with this name already exists"));
        return;
    }
    const QString icon = get_selected_icon();
    const int group_type = selected_group_type();

    const auto result = clipboard::save_group(*manager, editing_group_id, name, icon, group_type, selected_filter_rule());
    if (!result.success) {
        show_warning_dialog(this, tr_text("Failed"), result.error == "create_group_failed" ? tr_text("Failed to create group") : tr_text("Failed to update group"));
        return;
    }
    emit group_added();
    emit data_changed();
    if (result.action == "created" && result.entity_id) selected_group_id = result.entity_id;
    refresh_group_list();
    if (result.action == "created") {
        on_group_selected(selected_group_id);
    } else {
        refresh_content_list();
        if (editing_group_id) show_edit_group_form(*editing_group_id);
    }
}

void ManageDialog::save_content() {
    if (!selected_group_id) {
        show_warning_dialog(this, tr_text("Hint"), tr_text("Please select a group first"));
        return;
    }
    const auto item = editing_item_id ? manager->get_item(*editing_item_id) : std::nullopt;
    const auto group = get_selected_group();
    if (item && item->content_type == "image") save_image_title(*item);
    else if (!item && new_content_kind_ == "image" && group && group->group_type != GroupType::FILE) save_new_image();
    else if (is_file_content_mode()) save_file_content();
    else save_text_content();
}

void ManageDialog::save_image_title(const ClipboardItem &item) {
    // The image itself is not editable, only its title; the content is rewritten as [WxH] from the image, correcting a damaged record.
    const auto title = stripped_or_none(title_input ? title_input->text() : QString());
    const QString content = editing_image_size ? QString("[%1x%2]").arg(editing_image_size->width()).arg(editing_image_size->height()) : item.content;
    if (!manager->update_item(item.id, content, title)) {
        show_warning_dialog(this, tr_text("Failed"), tr_text("Failed to update content"));
        return;
    }
    emit content_added(selected_group_id.value_or(0));
    emit data_changed();
    refresh_content_list();
}

void ManageDialog::save_new_image() {
    // Written to the clipboard; once the watcher records it, that record moves into the current group.
    if (!pending_image_) {
        show_warning_dialog(this, tr_text("Hint"), tr_text("Please choose an image first"));
        return;
    }
    if (image_request_) return;
    const QImage image = *pending_image_;
    image_request_ = ImageRequest{selected_group_id, stripped_or_none(title_input ? title_input->text() : QString()),
                                  QString("[%1x%2]").arg(image.width()).arg(image.height()), QDateTime::currentSecsSinceEpoch(),
                                  monotonic_ms() + IMAGE_CAPTURE_TIMEOUT_MS};
    // Disabling the focused button moves focus on; landing in the item list would select its first row and switch the form.
    if (title_input) title_input->setFocus();
    save_btn->setEnabled(false);
    copy_image_to_clipboard(image);
    image_wait_timer_.start();
}

void ManageDialog::poll_captured_image() {
    if (!image_request_) {
        image_wait_timer_.stop();
        return;
    }
    const ImageRequest request = *image_request_;
    const auto item = find_captured_image(request);
    if (!item && monotonic_ms() < request.deadline_ms) return;
    image_wait_timer_.stop();
    image_request_.reset();
    save_btn->setEnabled(true);
    if (!item) {
        show_warning_dialog(this, tr_text("Failed"), tr_text("The image was not recorded. Make sure clipboard history is turned on."));
        return;
    }
    file_captured_image(*item, request);
}

std::optional<ClipboardItem> ManageDialog::find_captured_image(const ImageRequest &request) {
    // The new or de-duplicated record has updated_at no earlier than the write; the size is checked too so another image is not taken.
    // History sorts pinned items first; without the size filter, many pinned images would push the new record out of the first 30.
    for (const auto &item : manager->get_history(0, 30, request.content, FilterRule::of_kind("image")))
        if (item.content == request.content && item.updated_at.isValid() && item.updated_at.toSecsSinceEpoch() >= request.since) return item;
    return std::nullopt;
}

std::optional<Group> ManageDialog::group_of_item(qint64 item_id) {
    for (const auto &group : manager->get_groups())
        for (const auto &item : manager->get_by_group(group.id, 0, ITEMS_PER_GROUP))
            if (item.id == item_id) return group;
    return std::nullopt;
}

void ManageDialog::file_captured_image(const ClipboardItem &item, const ImageRequest &request) {
    if (!request.group_id) return;
    const qint64 target_id = *request.group_id;
    std::optional<Group> target;
    for (const auto &group : manager->get_groups())
        if (group.id == target_id) target = group;
    if (!target) return;
    // A new record cannot be in a group yet; de-duplication only refreshes updated_at, so created_at before the write marks an old record moved up.
    const bool is_new = item.created_at.isValid() && item.created_at.toSecsSinceEpoch() >= request.since;
    const auto current = is_new ? std::nullopt : group_of_item(item.id);
    // The library holds an image once (de-duplicated by content); in another group it can only move here, not be copied.
    if (current && current->id != target_id &&
        !show_confirm_dialog(this, tr_text("Image Already Saved"),
                             tr_text("This image is already in \"{group}\". Move it to \"{target}\"?").replace("{group}", current->name).replace("{target}", target->name)))
        return;
    if (request.title) manager->update_item(item.id, item.content, request.title);
    if ((!current || current->id != target_id) && !manager->move_to_group(item.id, target_id)) {
        show_warning_dialog(this, tr_text("Failed"), tr_text("Failed to move to group"));
        return;
    }

    emit content_added(target_id);
    emit data_changed();
    // While hidden the list and form were released and reload on show; refreshing here would clear that pending reload.
    if (needs_reload_ || selected_group_id != target_id) return;
    refresh_content_list();
    const int row = item_list->row_of_id(item.id);
    if (row >= 0) item_list->scrollToItem(item_list->item(row));
    const bool still_adding_image = current_mode == "content" && !editing_item_id && new_content_kind_ == "image";
    if (still_adding_image) {
        set_pending_image(QImage());
        if (title_input) title_input->clear();
    }
}

std::optional<ClipboardItem> ManageDialog::editing_image_item() {
    const auto item = editing_item_id ? manager->get_item(*editing_item_id) : std::nullopt;
    if (item && item->content_type == "image") return item;
    return std::nullopt;
}

void ManageDialog::copy_image_item() {
    const QImage image = load_item_image(*manager, editing_image_item());
    if (!image.isNull()) copy_image_to_clipboard(image);
}

void ManageDialog::pin_image_item() {
    if (const auto item = editing_image_item()) create_pin_from_clipboard_item(item->id, manager, this);
}

void ManageDialog::save_image_item_as() { clipboard::save_image_item_as(this, *manager, editing_image_item()); }

void ManageDialog::content_save_failed(const QString &error) {
    QString message = tr_text("Failed to update content");
    if (error == "move_to_group_failed") message = tr_text("Failed to move to group");
    else if (error == "add_content_failed") message = tr_text("Failed to add content");
    show_warning_dialog(this, tr_text("Failed"), message);
}

void ManageDialog::after_content_saved(const QString &action, std::optional<qint64> entity_id) {
    emit content_added(selected_group_id.value_or(0));
    emit data_changed();
    refresh_content_list();
    if (action == "created") {
        // After adding, the new item form stays for the next entry; the new item scrolls into view.
        const int row = item_list->row_of_id(entity_id);
        if (row >= 0) item_list->scrollToItem(item_list->item(row));
    }
}

void ManageDialog::save_file_content() {
    QString path = selected_file_path.value_or(QString());
    if (path.isEmpty() && file_path_input) path = file_path_input->text().trimmed();
    if (path.isEmpty()) {
        show_warning_dialog(this, tr_text("Hint"), tr_text("Please select a file"));
        return;
    }
    const auto title = title_input ? stripped_or_none(title_input->text()) : std::nullopt;
    const auto result = clipboard::save_file_content(*manager, editing_item_id, selected_group_id.value_or(0), path, title);
    if (!result.success) {
        content_save_failed(result.error);
        return;
    }
    if (result.action == "created") {
        if (title_input) title_input->clear();
        if (file_path_input) file_path_input->clear();
        selected_file_path.reset();
    }
    after_content_saved(result.action, result.entity_id);
}

void ManageDialog::save_text_content() {
    const QString content = content_edit ? content_edit->toPlainText().trimmed() : QString();
    if (content.isEmpty()) {
        show_warning_dialog(this, tr_text("Hint"), tr_text("Please enter content"));
        return;
    }
    if (!editing_item_id && new_content_kind_ == "link") {
        const QUrl url(content.startsWith("www.", Qt::CaseInsensitive) ? "https://" + content : content);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || url.isRelative() || url.authority().isEmpty() ||
            !QStringList{"http", "https", "ftp", "file"}.contains(scheme) ||
            std::any_of(content.cbegin(), content.cend(), [](QChar ch) { return ch.isSpace(); })) {
            show_warning_dialog(this, tr_text("Hint"), tr_text("Please enter a valid link"));
            return;
        }
    }
    const auto title = title_input ? stripped_or_none(title_input->text()) : std::nullopt;
    const auto result = clipboard::save_text_content(*manager, editing_item_id, selected_group_id.value_or(0), content, title);
    if (!result.success) {
        content_save_failed(result.error);
        return;
    }
    if (result.action == "created") {
        content_edit->clear();
        if (title_input) title_input->clear();
    }
    after_content_saved(result.action, result.entity_id);
}

void ManageDialog::delete_group(qint64 group_id) {
    if (!show_confirm_dialog(this, tr_text("Confirm Delete"), build_delete_group_confirm_message([](const char *text) { return tr_text(text); }))) return;
    if (!manager->delete_group(group_id)) {
        show_warning_dialog(this, tr_text("Failed"), tr_text("Failed to delete group"));
        return;
    }
    if (editing_group_id == group_id) editing_group_id.reset();
    if (selected_group_id == group_id) selected_group_id.reset();
    emit group_added();
    emit data_changed();
    refresh_group_list();
    on_group_selected(selected_group_id);
    show_new_group_form();
}

void ManageDialog::on_delete_clicked() {
    if (current_mode == "group" && editing_group_id && *editing_group_id) {
        delete_group(*editing_group_id);
    } else if (current_mode == "content" && editing_item_id && *editing_item_id) {
        if (!show_confirm_dialog(this, tr_text("Confirm Delete"), tr_text("Are you sure you want to delete this item?"))) return;
        if (manager->delete_item(*editing_item_id)) {
            editing_item_id.reset();
            emit content_added(selected_group_id.value_or(0));
            emit data_changed();
            refresh_content_list();
            show_new_content_form();
        } else {
            show_warning_dialog(this, tr_text("Failed"), tr_text("Failed to delete item"));
        }
    }
}

// ------------------------------------------------------------------
// Import / export
// ------------------------------------------------------------------

void ManageDialog::export_to_csv() {
    // Plain text items only.
    const auto [file_path, filter] = get_save_file_name(this, tr_text("Export to CSV"), "clipboard_export.csv", "CSV Files (*.csv);;All Files (*)");
    if (file_path.isEmpty()) return;
    try {
        const auto rows = collect_text_export_rows(*manager);
        QString encoding = "utf-8-sig";
        if (export_encoding_combo) {
            const QString chosen = export_encoding_combo->currentData().toString();
            if (!chosen.isEmpty()) encoding = chosen;
        }
        write_csv_rows(file_path, {tr_text("Group"), tr_text("Content"), tr_text("Title")}, rows, encoding);
        show_info_dialog(this, tr_text("Export Successful"), tr_text("Exported {count} items to CSV file.").replace("{count}", QString::number(rows.size())));
    } catch (const std::exception &e) {
        show_warning_dialog(this, tr_text("Export Failed"), tr_text("Failed to export: {error}").replace("{error}", QString::fromUtf8(e.what())));
    }
}

void ManageDialog::import_from_csv() {
    const auto [file_path, filter] = get_open_file_name(this, tr_text("Import from CSV"), "", "CSV Files (*.csv);;All Files (*)");
    if (file_path.isEmpty()) return;
    try {
        const auto rows = read_import_rows(file_path);
        if (rows.isEmpty()) {
            show_warning_dialog(this, tr_text("Import Failed"), tr_text("No valid data found in CSV file."));
            return;
        }
        const int imported_count = import_text_rows(*manager, rows);
        emit group_added();
        emit data_changed();
        refresh_group_list();
        refresh_content_list();
        show_info_dialog(this, tr_text("Import Successful"), tr_text("Imported {count} items.").replace("{count}", QString::number(imported_count)));
    } catch (const std::exception &e) {
        show_warning_dialog(this, tr_text("Import Failed"), tr_text("Failed to import: {error}").replace("{error}", QString::fromUtf8(e.what())));
    }
}
}
