#include "clipboard/ui/windows/clipboard_window.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "clipboard/ui/image_item_actions.h"
#include "clipboard/ui/menus/action_menu.h"
#include "clipboard/ui/panels/setting_panel.h"
#include "clipboard/ui/theme/theme_styles.h"
#include "clipboard/ui/widgets/group_bar.h"
#include "clipboard/ui/widgets/item_delegate.h"
#include "clipboard/ui/widgets/item_drag.h"
#include "clipboard/ui/widgets/preview_popup.h"
#include "clipboard/ui/widgets/selection_bar.h"
#include "clipboard/ui/windows/pin_window.h"
#include "core/constants.h"
#include "core/data_location.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/resource_manager.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/fluent_lite/theme.h"
#include <QApplication>
#include <QCalendarWidget>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QIcon>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPainter>
#include <QMouseEvent>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStyle>
#include <QUrl>
#include <QWheelEvent>

namespace jietuba::clipboard {
namespace {
// Direct picks take bare keys only: this app's simulated Ctrl+V comes back here while the window stays open, and as "paste item N" it would loop.
constexpr Qt::KeyboardModifiers DIRECT_PICK_BLOCKERS = Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;

QString translate_window(const QString &text) { return ClipboardWindow::tr_text(text.toUtf8().constData()); }

}

QString pick_char(int key) {
    if (key >= Qt::Key_1 && key <= Qt::Key_9) return QString::number(key - Qt::Key_1 + 1);
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return QString(QChar(u'a' + (key - Qt::Key_A)));
    return {};
}

// ==================== Shortcuts ====================

ClipboardShortcutHandler::ClipboardShortcutHandler(ClipboardWindow *window) : window_(window) { reload_bindings(); }

void ClipboardShortcutHandler::reload_bindings() {
    // Read on each show, so changed settings apply next time.
    bindings_ = load_inapp_bindings({QUICK_EDIT_SHORTCUT});
    pick_keys = clipboard_pick_keys(get_tool_settings_manager()->get_inapp_clipboard_pick_mode());
}

bool ClipboardShortcutHandler::owns_window(const QWidget *window) const { return window_ && window_->owns_window(window); }

bool ClipboardShortcutHandler::is_active() {
    ClipboardWindow *w = window_;
    // An open popup menu takes its own keys.
    return w && w->isVisible() && !QApplication::activePopupWidget();
}

bool ClipboardShortcutHandler::handle_key(QInputEvent *event) {
    ClipboardWindow *w = window_;
    if (!w) return false;
    if (w->mouse_controller) w->mouse_controller->cancel();
    const int key = event_key(event);
    const auto modifiers = event->modifiers();

    // The quick edit popup shares activation with this window; pass-through keys like Esc and Tab belong to the editor.
    if (w->quick_edit_popup && w->quick_edit_popup->is_open()) return false;

    // Configured keys come before the fixed Enter and direct pick letters below.
    if (match_inapp_binding(event, QUICK_EDIT_SHORTCUT, bindings_) && w->edit_selected()) return true;

    if (key == Qt::Key_Escape) {
        // The cursor only points, so there is nothing else to undo before closing.
        if (!w->selection_manager->clear_marks()) w->close();
        return true;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        w->paste_selected(modifiers & Qt::ShiftModifier);
        return true;
    }
    // Delete is only in the context menu, so the Delete key next to Enter cannot remove items by mistake.
    if (key == Qt::Key_Delete) return true;
    if (key == Qt::Key_F && modifiers == Qt::ControlModifier) {
        w->search_input->setFocus();
        return true;
    }
    if (modifiers & DIRECT_PICK_BLOCKERS) return false;

    const QString character = pick_char(key);
    const qsizetype index = character.isEmpty() ? -1 : pick_keys.indexOf(character);
    if (index >= 0 && QApplication::focusWidget() != w->search_input) {
        // During a multiple selection the rows show circles instead of their keys.
        if (w->selection_manager->has_marks()) return true;
        const auto &items = w->controller->current_items;
        if (index < items.size()) {
            w->on_paste_item(items[index].id);
            return true;
        }
    }
    return false;
}

// ==================== Window ====================

namespace {
// Entries of the type filter menu: the kind it selects (see FilterRule::kind), empty for every type.
const QStringList &type_filter_kinds() {
    static const QStringList kinds = {QString(), "text", SUB_KIND_URL, SUB_KIND_COLOR, SUB_KIND_PATH, "image", "file"};
    return kinds;
}

QStringList type_filter_label_texts() {
    return {ClipboardWindow::tr_text("All"),  ClipboardWindow::tr_text("Text"),  ClipboardWindow::tr_text("Link"), ClipboardWindow::tr_text("Color"),
            ClipboardWindow::tr_text("Path"), ClipboardWindow::tr_text("Image"), ClipboardWindow::tr_text("File")};
}

QString &geometry_file_for_test() {
    static QString path;
    return path;
}

std::unique_ptr<QSettings> geometry_settings() {
    if (!geometry_file_for_test().isEmpty()) return std::make_unique<QSettings>(geometry_file_for_test(), QSettings::IniFormat);
    auto settings = std::make_unique<QSettings>(data_location().settings_file(), QSettings::IniFormat);
    settings->beginGroup(CLIPBOARD_WINDOW_SETTINGS_GROUP);
    return settings;
}
}

void ClipboardWindow::set_geometry_file_for_test(const QString &ini_path) { geometry_file_for_test() = ini_path; }

ClipboardWindow::ClipboardWindow(QWidget *parent)
    : QWidget(parent), manager(ClipboardManager::instance()), controller(new ClipboardController(manager, this)), qsettings_(geometry_settings()) {
    connect(controller, &ClipboardController::data_loaded, this, [this](const QList<ClipboardItem> &items, bool first) { on_data_loaded(items, first); });
    connect(controller, &ClipboardController::loading_state_changed, this, [this](bool loading) { is_loading_ = loading; });
    connect(controller, &ClipboardController::reload_required, this, [this] { on_reload_required(); });
    connect(controller, &ClipboardController::load_completed, this, [this] { on_load_completed(); });
    connect(controller, &ClipboardController::item_inserted, this, [this](const ClipboardItem &item, int row) { on_item_inserted(item, row); });
    connect(controller, &ClipboardController::item_moved_to_top, this, [this](qint64 item_id, int row) { on_item_moved_to_top(item_id, row); });
    connect(controller, &ClipboardController::item_removed, this, [this](int row) { on_item_removed(row); });
    connect(controller, &ClipboardController::item_row_moved, this, [this](int from, int to) { on_item_row_moved(from, to); });
    connect(controller, &ClipboardController::item_updated, this, [this](int row, const ClipboardItem &item) { on_item_updated(row, item); });

    connect(this, &ClipboardWindow::new_item_received, this, [this](const std::optional<ClipboardItem> &item) { controller->on_new_content(isVisible(), item); });
    frameless_.is_draggable_area = [this](QObject *widget, const QPoint &) { return is_draggable_area(widget); };
    frameless_.init_frameless(this, 8);

    auto *theme_manager = get_theme_manager();
    current_theme = theme_manager->get_current_theme();
    connect(theme_manager, &ThemeManager::theme_changed, this, [this](const Theme &theme) { on_theme_changed(theme); });
    connect(theme_manager, &ThemeManager::font_size_changed, this, [this](int size) { on_font_size_changed(size); });
    connect(theme_manager, &ThemeManager::opacity_changed, this, [this](int percent) { on_opacity_changed(percent); });
    connect(&get_ui_scale(), &UIScaleManager::scale_changed, this, [this] { apply_scale(); });
    connect(I18nManager::instance().language_signals(), &I18nSignals::language_changed, this, [this] { on_language_changed(); });

    load_settings();
    load_window_geometry();
    setup_ui();
    apply_opacity();
    shortcut_handler_ = std::make_unique<ClipboardShortcutHandler>(this);

    controller->set_paste_target_exclusion([this](Hwnd hwnd) { return is_paste_picker_surface(hwnd); });
    PreviewPopup::instance()->set_manager(&manager);
    warmup_offscreen_once();
}

ClipboardWindow::~ClipboardWindow() {
    ShortcutManager::instance()->unregister_handler(shortcut_handler_.get());
    PreviewPopup::instance()->set_display_enabled(false);
}

void ClipboardWindow::apply_list_item_data(QListWidgetItem *list_item, const ClipboardItem &item) const {
    list_item->setData(ROLE_ITEM_ID, item.id);
    list_item->setData(ROLE_ITEM_DATA, QVariant::fromValue(item));
}

void ClipboardWindow::on_data_loaded(const QList<ClipboardItem> &items, bool is_first_page) {
    if (is_first_page) {
        auto_fill_remaining_ = auto_fill_max_pages_;
        if (!items.isEmpty() || !controller->current_items.isEmpty()) refresh_list();
        else list_widget->clear();
    } else {
        append_items(items);
    }
}

void ClipboardWindow::on_item_inserted(const ClipboardItem &item, int row) {
    // One new item goes into the list: no rebuild, no scroll, selection and preview untouched.
    auto *list_item = new QListWidgetItem;
    apply_list_item_data(list_item, item);
    list_widget->insertItem(row, list_item);
    selection_manager->row_inserted(row);
}

void ClipboardWindow::on_item_moved_to_top(qint64 item_id, int row) {
    // Moves the existing row without rebuilding the list.
    for (int index = 0; index < list_widget->count(); ++index) {
        if (list_widget->item(index)->data(ROLE_ITEM_ID).toLongLong() != item_id) continue;
        if (index == row) return;
        list_widget->insertItem(row, list_widget->takeItem(index));
        selection_manager->row_moved(index, row);
        return;
    }
}

// The selection manager does not follow Qt's current item; row_removed keeps its cursor row in step.
void ClipboardWindow::on_item_removed(int row) {
    QListWidgetItem *list_item = list_widget->item(row);
    const std::optional<qint64> removed_id = list_item ? std::optional<qint64>(list_item->data(ROLE_ITEM_ID).toLongLong()) : std::nullopt;
    list_widget->blockSignals(true);
    delete list_widget->takeItem(row);
    list_widget->blockSignals(false);
    selection_manager->row_removed(row);
    if (removed_id) selection_manager->unmark(*removed_id);
    // One row fewer may leave the window unfilled; load the next page if so.
    QTimer::singleShot(0, this, [this] { check_and_load_more_if_needed(); });
}

void ClipboardWindow::on_item_row_moved(int from_row, int to_row) {
    list_widget->blockSignals(true);
    list_widget->insertItem(to_row, list_widget->takeItem(from_row));
    list_widget->blockSignals(false);
    selection_manager->row_moved(from_row, to_row);
}

void ClipboardWindow::on_item_updated(int row, const ClipboardItem &item) {
    QListWidgetItem *list_item = list_widget->item(row);
    if (list_item && list_item->data(ROLE_ITEM_ID).toLongLong() == item.id) list_item->setData(ROLE_ITEM_DATA, QVariant::fromValue(item));
}

void ClipboardWindow::on_reload_required() {
    controller->load_history();
    group_bar->refresh_buttons();
}

void ClipboardWindow::request_data_refresh(const QString &reason) {
    if (ignore_manage_refresh_when_hidden_ && !isVisible()) {
        const QString reason_text = reason.isEmpty() ? QString("Clipboard data") : reason;
        log_debug(T("{reason_text} 当前窗口不可见，跳过刷新界面", {{"reason_text", reason_text}}), "Clipboard");
        return;
    }
    controller->load_history();
}

void ClipboardWindow::on_manage_data_changed() { request_data_refresh("Manage dialog data"); }

void ClipboardWindow::apply_scale() {
    // Fixed sizes reset, then styles and the list redone at the new scale.
    apply_scale_metrics();
    group_bar->apply_scale();
    PreviewPopup::instance()->apply_scale();
    apply_theme();
}

void ClipboardWindow::apply_scale_metrics() {
    setMinimumSize(scaled(390), scaled(400));
    for (const auto &[bar, layout] : {std::make_pair(time_filter_bar, time_filter_layout_), std::make_pair(bottom_bar, bottom_layout_)}) {
        bar->setFixedHeight(scaled(36));
        layout->setContentsMargins(scaled(8), scaled(4), scaled(8), scaled(4));
    }
    time_filter_layout_->setSpacing(scaled(4));
    bottom_layout_->setSpacing(scaled(8));
    const std::tuple<QWidget *, int, int> sizes[] = {
        {start_date_edit, 108, 26}, {end_date_edit, 108, 26},         {apply_time_filter_btn, 36, 26}, {type_filter_btn, 80, 26},
        {clear_time_filter_btn, 26, 26}, {time_filter_toggle_btn, 28, 28}, {clear_search_btn, 24, 24},  {menu_btn, 28, 28},
    };
    for (const auto &[widget, width, height] : sizes) widget->setFixedSize(scaled(width), scaled(height));
}

void ClipboardWindow::on_theme_changed(const Theme &theme) {
    current_theme = theme;
    apply_theme();
}

void ClipboardWindow::on_language_changed() {
    apply_date_filter_locale();
    retranslate_ui();
}

void ClipboardWindow::apply_theme() {
    if (item_delegate) {
        item_delegate->set_theme(current_theme);
        item_delegate->set_window_opacity(window_opacity);
    }
    apply_opacity();
    group_bar->set_theme(current_theme);
    if (search_input) {
        apply_search_input_style(!search_input->text().trimmed().isEmpty());
        apply_clear_search_btn_style();
        apply_menu_btn_style();
        apply_time_filter_styles();
        selection_bar->apply_style(current_theme);
    }
    refresh_list();
    log_debug(T("主题已切换到: {theme_name}", {{"theme_name", current_theme.display_name}}), "Clipboard");
}

void ClipboardWindow::load_settings() {
    config = get_tool_settings_manager();
    window_opacity = static_cast<int>(config->get_clipboard_window_opacity());
    font_size = static_cast<int>(config->get_clipboard_font_size());
    group_bar_position = config->get_clipboard_group_bar_position();
}

QLocale ClipboardWindow::current_date_locale() const {
    const QString lang_code = I18nManager::get_current_language();
    if (lang_code == "ja") return QLocale(QLocale::Japanese, QLocale::Japan);
    if (lang_code == "ko") return QLocale(QLocale::Korean, QLocale::SouthKorea);
    if (lang_code == "en") return QLocale(QLocale::English, QLocale::UnitedStates);
    if (lang_code == "zh_TW") return QLocale(QLocale::Chinese, QLocale::Taiwan);
    return QLocale(QLocale::Chinese, QLocale::China);
}

void ClipboardWindow::apply_date_filter_locale() {
    if (!start_date_edit) return;
    const QLocale locale = current_date_locale();
    for (QDateEdit *date_edit : {start_date_edit, end_date_edit}) {
        date_edit->setLocale(locale);
        if (QCalendarWidget *calendar = date_edit->calendarWidget()) calendar->setLocale(locale);
    }
}

void ClipboardWindow::retranslate_ui() {
    if (!search_input) return;
    setWindowTitle(tr_text("Clipboard History"));
    start_date_edit->setToolTip(tr_text("Start date"));
    end_date_edit->setToolTip(tr_text("End date"));
    apply_time_filter_btn->setToolTip(tr_text("Apply time filter"));
    type_filter_btn->setToolTip(tr_text("Filter Type"));
    clear_time_filter_btn->setToolTip(tr_text("Close filters"));
    search_input->setPlaceholderText(QString::fromUtf8("🔍 ") + tr_text("Search"));
    clear_search_btn->setToolTip(tr_text("Clear search"));
    selection_bar->retranslate();

    type_filter_labels = type_filter_label_texts();
    for (int index = 0; index < type_filter_actions.size(); ++index) type_filter_actions[index]->setText(type_filter_labels[index]);
    update_type_filter_button(type_filter_index);
    if (group_bar) group_bar->refresh_buttons();
}

void ClipboardWindow::apply_opacity() {
    const ThemeStyleGenerator generator(current_theme);
    if (container) container->setStyleSheet(generator.generate_window_style(window_opacity));
    if (list_widget) list_widget->setStyleSheet(generator.generate_list_widget_style(window_opacity));
    if (bottom_bar) bottom_bar->setStyleSheet(generator.generate_search_bar_style(window_opacity));
    if (time_filter_bar) time_filter_bar->setStyleSheet(generator.generate_time_filter_bar_style(window_opacity));
    if (group_bar && group_bar->bar_widget) {
        const QString border = group_bar_position == "left" ? "border-right:" : group_bar_position == "top" ? "border-bottom:" : "border-left:";
        group_bar->bar_widget->setStyleSheet(generator.generate_search_bar_style(window_opacity).replace("border-top:", border));
    }
}

void ClipboardWindow::load_window_geometry() {
    const int default_width = config->get_clipboard_window_width();
    const int default_height = config->get_clipboard_window_height(600);
    const QVariant x = qsettings_->value("window/x"), y = qsettings_->value("window/y");
    if (x.isValid()) saved_x_ = x.toInt();
    if (y.isValid()) saved_y_ = y.toInt();
    saved_width_ = qsettings_->value("window/width", default_width).toInt();
    saved_height_ = qsettings_->value("window/height", default_height).toInt();
}

void ClipboardWindow::save_window_geometry() {
    saved_x_ = x();
    saved_y_ = y();
    qsettings_->setValue("window/x", x());
    qsettings_->setValue("window/y", y());
    qsettings_->setValue("window/width", width());
    qsettings_->setValue("window/height", height());
    qsettings_->sync();
}

void ClipboardWindow::setup_ui() {
    setWindowTitle(tr_text("Clipboard History"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(saved_width_, saved_height_);

    container = new QFrame(this);
    container->setStyleSheet(R"(
            QFrame#mainContainer {
                background: #FFFFFF;
                border: 1px solid #E0E0E0;
                border-radius: 4px;
            }
            QToolTip {
                background: #FFFFFF;
                color: #333333;
                border: 1px solid #E0E0E0;
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 12px;
            }
        )");
    container->setObjectName("mainContainer");

    auto *main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->addWidget(container);

    content_layout = new QHBoxLayout(container);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);

    left_widget = new QWidget;
    set_own_style(left_widget, "background: transparent;");
    left_layout = new QVBoxLayout(left_widget);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(0);

    list_widget = new QListWidget;
    list_widget->setStyleSheet(R"(
            QListWidget {
                background: #FFFFFF;
                border: none;
                outline: none;
                color: #333333;
            }
            QListWidget::item {
                padding: 0px;
                border: none;
                background: transparent;
            }
            QListWidget::item:selected {
                background: transparent;
            }
            QListWidget::item:hover {
                background: transparent;
            }
        )");
    list_widget->setSpacing(0);
    list_widget->setResizeMode(QListView::Adjust);
    list_widget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_widget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    list_widget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_widget, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) { show_context_menu(pos); });

    item_delegate = new ClipboardItemDelegate(list_widget, current_theme, font_size, window_opacity, config->get_clipboard_show_metadata(),
                                              static_cast<int>(config->get_clipboard_line_height_padding()), true, config->get_clipboard_image_size(), config->get_clipboard_display_lines());
    list_widget->setItemDelegate(item_delegate);
    list_widget->setMouseTracking(true);
    list_widget->viewport()->setMouseTracking(true);
    list_widget->viewport()->installEventFilter(this);

    selection_manager = new SelectionManager(list_widget, [this](qint64 item_id) { return get_item_data(item_id); });
    selection_manager->setParent(this);
    selection_manager->group_bar_position = group_bar_position;
    connect(selection_manager, &SelectionManager::item_activated, this, [this](qint64 item_id) { on_paste_item(item_id); });
    connect(selection_manager, &SelectionManager::marks_changed, this, [this] { on_marks_changed(); });
    connect(selection_manager, &SelectionManager::cursor_changed, this, [this](std::optional<qint64> item_id) {
        item_delegate->set_cursor_id(item_id);
        list_widget->viewport()->update();
    });
    connect(selection_manager, &SelectionManager::request_sidebar_focus, this, [this] { enter_sidebar_mode(); });
    connect(selection_manager, &SelectionManager::request_group_switch, this, [this](int delta) { on_top_group_switch(delta); });
    mouse_controller = new ClipboardMouseController(this);

    QScrollBar *scrollbar = list_widget->verticalScrollBar();
    connect(scrollbar, &QScrollBar::valueChanged, this, [this](int value) { on_scroll(value); });
    connect(scrollbar, &QScrollBar::rangeChanged, this, [this] { list_widget->viewport()->update(); });
    left_layout->addWidget(list_widget, 1);

    time_filter_bar = new QWidget;
    time_filter_bar->hide();
    time_filter_layout_ = new QHBoxLayout(time_filter_bar);

    const QDate today = QDate::currentDate();
    start_date_edit = new QDateEdit(today.addDays(-7));
    start_date_edit->setCalendarPopup(true);
    start_date_edit->setDisplayFormat("yyyy/MM/dd");
    start_date_edit->setToolTip(tr_text("Start date"));
    start_date_edit->setFocusPolicy(Qt::ClickFocus);
    time_filter_layout_->addWidget(start_date_edit);

    time_filter_separator = new QLabel(QString::fromUtf8("–"));
    time_filter_separator->setAlignment(Qt::AlignCenter);
    time_filter_layout_->addWidget(time_filter_separator);

    end_date_edit = new QDateEdit(today);
    end_date_edit->setCalendarPopup(true);
    end_date_edit->setDisplayFormat("yyyy/MM/dd");
    end_date_edit->setToolTip(tr_text("End date"));
    end_date_edit->setFocusPolicy(Qt::ClickFocus);
    time_filter_layout_->addWidget(end_date_edit);
    apply_date_filter_locale();

    apply_time_filter_btn = new QToolButton;
    apply_time_filter_btn->setText(tr_text("OK"));
    apply_time_filter_btn->setCursor(Qt::PointingHandCursor);
    apply_time_filter_btn->setToolTip(tr_text("Apply time filter"));
    apply_time_filter_btn->setFocusPolicy(Qt::NoFocus);
    connect(apply_time_filter_btn, &QToolButton::clicked, this, [this] { apply_time_range_filter(); });
    time_filter_layout_->addWidget(apply_time_filter_btn);
    time_filter_layout_->addStretch(1);

    type_filter_labels = type_filter_label_texts();
    type_filter_index = 0;
    type_filter_btn = new QToolButton;
    type_filter_btn->setCursor(Qt::PointingHandCursor);
    type_filter_btn->setToolTip(tr_text("Filter Type"));
    type_filter_btn->setFocusPolicy(Qt::NoFocus);
    type_filter_btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(type_filter_btn, &QToolButton::clicked, this, [this] { show_type_filter_menu(); });
    type_filter_menu = new QMenu(type_filter_btn);
    for (int idx = 0; idx < type_filter_labels.size(); ++idx) {
        QAction *action = type_filter_menu->addAction(type_filter_labels[idx]);
        action->setCheckable(true);
        action->setChecked(idx == type_filter_index);
        connect(action, &QAction::triggered, this, [this, idx] { set_type_filter(idx); });
        type_filter_actions.append(action);
    }
    update_type_filter_button(0);
    time_filter_layout_->addWidget(type_filter_btn);

    clear_time_filter_btn = new QToolButton;
    clear_time_filter_btn->setText(QString::fromUtf8("×"));
    clear_time_filter_btn->setCursor(Qt::PointingHandCursor);
    clear_time_filter_btn->setToolTip(tr_text("Close filters"));
    clear_time_filter_btn->setFocusPolicy(Qt::NoFocus);
    connect(clear_time_filter_btn, &QToolButton::clicked, this, [this] { close_time_filter_bar(); });
    time_filter_layout_->addWidget(clear_time_filter_btn);
    left_layout->addWidget(time_filter_bar);

    selection_bar = new SelectionBar;
    selection_bar->apply_style(current_theme);
    connect(selection_bar, &SelectionBar::paste_requested, this, [this] { paste_selected(false); });
    connect(selection_bar, &SelectionBar::save_requested, this, &ClipboardWindow::save_marked_to_folder);
    connect(selection_bar, &SelectionBar::more_requested, this, [this] {
        const QPoint at = list_widget->mapFromGlobal(selection_bar->more_button->mapToGlobal(QPoint(0, 0)));
        show_marked_context_menu(selection_manager->marked_ids(true), at);
    });
    connect(selection_bar, &SelectionBar::exit_requested, this, [this] { selection_manager->clear_marks(); });
    left_layout->addWidget(selection_bar);

    bottom_bar = new QWidget;
    bottom_bar->setStyleSheet(R"(
            QWidget {
                background: #FAFAFA;
                border-top: 1px solid #E0E0E0;
            }
        )");
    bottom_layout_ = new QHBoxLayout(bottom_bar);

    // This button and the gear get SVG icons: the style's arrow and the ⚙ character differ
    // between Windows versions (a chevron or a triangle, a colour or a plain gear).
    time_filter_toggle_btn = new QToolButton;
    time_filter_toggle_btn->setCheckable(true);
    time_filter_toggle_btn->setCursor(Qt::PointingHandCursor);
    time_filter_toggle_btn->setFocusPolicy(Qt::NoFocus);
    connect(time_filter_toggle_btn, &QToolButton::clicked, this, [this](bool checked) { toggle_time_filter_bar(checked); });
    bottom_layout_->addWidget(time_filter_toggle_btn);

    search_input = new LineEdit(nullptr, false);
    search_input->setPlaceholderText(QString::fromUtf8("🔍 ") + tr_text("Search"));
    apply_search_input_style();
    connect(search_input, &QLineEdit::textChanged, this, [this] { on_search_changed(); });
    connect(search_input, &QLineEdit::textChanged, this, [this](const QString &text) { update_search_background(text); });
    bottom_layout_->addWidget(search_input, 1);
    selection_manager->set_search_input(search_input);

    clear_search_btn = new QPushButton(QString::fromUtf8("×"));
    clear_search_btn->setCursor(Qt::PointingHandCursor);
    clear_search_btn->setToolTip(tr_text("Clear search"));
    clear_search_btn->setFocusPolicy(Qt::NoFocus);
    apply_clear_search_btn_style();
    connect(clear_search_btn, &QPushButton::clicked, this, [this] { clear_search(); });
    clear_search_btn->hide();
    bottom_layout_->addWidget(clear_search_btn);

    menu_btn = new QPushButton;
    menu_btn->setCursor(Qt::PointingHandCursor);
    menu_btn->setFocusPolicy(Qt::NoFocus);
    apply_menu_btn_style();
    connect(menu_btn, &QPushButton::clicked, this, [this] { show_main_menu(); });
    bottom_layout_->addWidget(menu_btn);

    apply_time_filter_styles();
    left_layout->addWidget(bottom_bar);
    content_layout->addWidget(left_widget, 1);

    group_bar = new GroupBar(controller, &manager, current_theme, group_bar_position, this);
    connect(group_bar, &GroupBar::close_requested, this, [this] { close(); });
    connect(group_bar, &GroupBar::group_switched, this, [this](std::optional<qint64> group_id) { on_group_switched(group_id); });
    connect(group_bar, &GroupBar::sidebar_entered, this, [this] { on_sidebar_entered(); });
    connect(group_bar, &GroupBar::sidebar_exited, this, [this](std::optional<qint64> prev) { on_sidebar_exited(prev); });
    connect(group_bar, &GroupBar::manage_groups_requested, this, [this] { on_add_group_clicked(); });
    connect(group_bar, &GroupBar::manage_items_requested, this, [this] { on_add_item_clicked(); });
    group_bar->build(content_layout, left_widget, left_layout);
    apply_scale_metrics();

    frameless_.setup_mouse_tracking_recursive(this);
}

QString ClipboardWindow::menu_style() const { return ThemeStyleGenerator(current_theme).generate_menu_style(); }

void ClipboardWindow::show_main_menu() {
    if (settings_menu_ && settings_menu_->isVisible()) {
        settings_menu_->close();
        settings_menu_ = nullptr;
        return;
    }
    // A click on the button that closed the menu already fired aboutToHide; it must not open the menu again.
    if (menu_close_time_ && menu_close_time_->elapsed() < 300) return;

    SettingMenuOptions options;
    options.menu_style = menu_style();
    options.tr = [](const char *text) { return tr_text(text); };
    options.paste_with_html = controller->paste_with_html;
    options.auto_paste = config->get_clipboard_auto_paste();
    options.close_after_paste = config->get_clipboard_close_after_paste();
    options.move_to_top = config->get_clipboard_move_to_top_on_paste();
    options.show_metadata = config->get_clipboard_show_metadata();
    options.preserve_search = config->get_clipboard_preserve_search();
    options.window_opacity = window_opacity;
    options.current_font_size = static_cast<int>(config->get_clipboard_font_size());
    options.current_image_size = config->get_clipboard_image_size();
    options.current_display_lines = config->get_clipboard_display_lines();
    options.current_theme_name = get_theme_manager()->get_current_theme().name;
    options.current_group_bar_position = group_bar_position;
    options.opacity_options = config->get_clipboard_window_opacity_options().toList();
    options.font_size_options = config->get_clipboard_font_size_options().toList();
    options.on_toggle_paste_html = [this](bool checked) { controller->set_paste_with_html(checked); };
    options.on_toggle_auto_paste = [this](bool checked) { controller->set_auto_paste(checked); };
    options.on_toggle_close_after_paste = [this](bool checked) { config->set_clipboard_close_after_paste(checked); };
    options.on_toggle_move_to_top = [this](bool checked) { controller->set_move_to_top_on_paste(checked); };
    options.on_toggle_show_metadata = [this](bool checked) {
        config->set_clipboard_show_metadata(checked);
        item_delegate->set_show_metadata(checked);
        controller->load_history();
    };
    options.on_toggle_preserve_search = [this](bool checked) { config->set_clipboard_preserve_search(checked); };
    options.on_set_opacity = [this](int percent) { set_window_opacity_percent(percent); };
    options.on_set_font_size = [this](int size) { set_font_size(size); };
    options.on_set_image_size = [this](const QString &size) { set_image_size(size); };
    options.on_set_display_lines = [this](int lines) { set_display_lines(lines); };
    options.on_set_theme = [](const QString &name) { get_theme_manager()->set_theme(name); };
    options.on_add_item = [this] { on_add_item_clicked(); };
    options.on_clear_history = [this] { on_clear_clicked(); };
    options.on_set_group_bar_position = [this](const QString &position) { set_group_bar_position(position); };
    options.open_position = config->get_clipboard_open_position();
    options.on_set_open_position = [this](const QString &position) { config->set_clipboard_open_position(position); };
    options.multi_paste_separator = config->get_clipboard_multi_paste_separator();
    options.multi_paste_order = config->get_clipboard_multi_paste_order();
    options.multi_paste_keep_merged = config->get_clipboard_multi_paste_keep_merged();
    options.on_set_multi_paste_separator = [this](const QString &value) { config->set_clipboard_multi_paste_separator(value); };
    options.on_set_multi_paste_order = [this](const QString &value) { config->set_clipboard_multi_paste_order(value); };
    options.on_toggle_multi_paste_keep_merged = [this](bool checked) { config->set_clipboard_multi_paste_keep_merged(checked); };
    options.anchor_pos = menu_btn->mapToGlobal(QPoint(0, 0));
    settings_menu_ = show_setting_menu(this, options);
    connect(settings_menu_, &QMenu::aboutToHide, this, [this] {
        menu_close_time_.emplace();
        menu_close_time_->start();
        settings_menu_ = nullptr;
    });
}

void ClipboardWindow::toggle_time_filter_bar(bool checked) {
    if (!checked) clear_time_and_type_filters();
    time_filter_bar->setVisible(checked);
    apply_time_filter_styles();
}

void ClipboardWindow::apply_time_range_filter() {
    const QString range_text = start_date_edit->date().toString("yyyy/MM/dd") + "-" + end_date_edit->date().toString("yyyy/MM/dd");
    const bool ok = controller->set_time_range_text(range_text);
    for (QDateEdit *edit : {start_date_edit, end_date_edit}) {
        edit->setProperty("hasError", !ok);
        edit->style()->unpolish(edit);
        edit->style()->polish(edit);
    }
    apply_time_filter_styles();
}

void ClipboardWindow::close_time_filter_bar() {
    time_filter_toggle_btn->setChecked(false);
    toggle_time_filter_bar(false);
}

void ClipboardWindow::clear_time_and_type_filters() {
    const QDate today = QDate::currentDate();
    start_date_edit->setDate(today.addDays(-7));
    end_date_edit->setDate(today);
    start_date_edit->setProperty("hasError", false);
    end_date_edit->setProperty("hasError", false);
    if (type_filter_btn) update_type_filter_button(0);
    controller->clear_time_and_type_filters();
}

void ClipboardWindow::set_window_opacity_percent(int percent) {
    window_opacity = percent;
    config->set_clipboard_window_opacity(percent);
    item_delegate->set_window_opacity(percent);
    apply_opacity();
    refresh_list();
}

void ClipboardWindow::set_font_size(int size) {
    font_size = size;
    config->set_clipboard_font_size(size);
    item_delegate->set_font_size(size);
    refresh_list();
}

void ClipboardWindow::set_image_size(const QString &size) {
    config->set_clipboard_image_size(size);
    item_delegate->set_image_size(size);
    refresh_list();
}

void ClipboardWindow::set_display_lines(int lines) {
    config->set_clipboard_display_lines(lines);
    item_delegate->set_display_lines(config->get_clipboard_display_lines());
    refresh_list();
}

void ClipboardWindow::on_font_size_changed(int size) {
    font_size = size;
    item_delegate->set_font_size(size);
    refresh_list();
}

void ClipboardWindow::on_opacity_changed(int percent) {
    window_opacity = percent;
    item_delegate->set_window_opacity(percent);
    apply_opacity();
    refresh_list();
}

void ClipboardWindow::refresh_list() {
    PreviewPopup::instance()->hide_preview();
    selection_manager->reset();

    list_widget->setUpdatesEnabled(false);
    list_widget->clear();
    item_delegate->set_show_metadata(config->get_clipboard_show_metadata());
    item_delegate->set_font_size(font_size);
    item_delegate->set_display_lines(config->get_clipboard_display_lines());
    item_delegate->set_window_opacity(window_opacity);
    item_delegate->set_theme(current_theme);
    for (const auto &item : controller->current_items) {
        auto *list_item = new QListWidgetItem;
        apply_list_item_data(list_item, item);
        list_widget->addItem(list_item);
    }
    list_widget->setUpdatesEnabled(true);
    list_widget->scrollToTop();
    list_widget->viewport()->update();
}

void ClipboardWindow::on_load_completed() {
    // Editing the current group's type in the manage window reloads; the button follows.
    refresh_type_filter_button();
    QTimer::singleShot(0, this, [this] { check_and_load_more_if_needed(); });
}

void ClipboardWindow::check_and_load_more_if_needed() {
    QScrollBar *scrollbar = list_widget->verticalScrollBar();
    if ((!scrollbar->isVisible() || scrollbar->maximum() == 0) && controller->has_more_items()) {
        if (auto_fill_remaining_ <= 0) return;
        auto_fill_remaining_ -= 1;
        controller->load_more_items();
    }
}

void ClipboardWindow::append_items(const QList<ClipboardItem> &items) {
    log_debug(T("📝 _append_items 被调用，准备追加 {count} 条数据", {{"count", static_cast<int>(items.size())}}), "Clipboard");
    list_widget->setUpdatesEnabled(false);
    for (const auto &item : items) {
        auto *list_item = new QListWidgetItem;
        apply_list_item_data(list_item, item);
        list_widget->addItem(list_item);
    }
    list_widget->setUpdatesEnabled(true);
    list_widget->viewport()->update();
}

void ClipboardWindow::on_scroll(int value) { controller->check_scroll_load(value, list_widget->verticalScrollBar()->maximum()); }

void ClipboardWindow::on_search_changed() {
    if (search_timer_) {
        search_timer_->stop();
    } else {
        search_timer_ = new QTimer(this);
        search_timer_->setSingleShot(true);
        connect(search_timer_, &QTimer::timeout, this, [this] { controller->set_search_text(search_input->text()); });
    }
    search_timer_->start(300);
}

void ClipboardWindow::on_pointer_moved(const QPoint &viewport_pos, const QPoint &global_pos) {
    // Qt reports the row under a still mouse again when the list scrolls; only a moved mouse points at something.
    if (pointer_pos_ == global_pos) return;
    point_at(viewport_pos, global_pos);
}

void ClipboardWindow::point_at(const QPoint &viewport_pos, const QPoint &global_pos) {
    pointer_pos_ = global_pos;
    QListWidgetItem *list_item = list_widget->itemAt(viewport_pos);
    if (!list_item) {
        pointer_row_ = -1;
        PreviewPopup::instance()->hide_preview();
        return;
    }
    // The preview follows the row the mouse comes onto, and comes back when the mouse takes the cursor back from the keys.
    const int row = list_widget->row(list_item);
    const bool preview = row != pointer_row_ || row != selection_manager->cursor_row();
    pointer_row_ = row;
    selection_manager->point_at(row);
    const QVariant item_data = list_item->data(ROLE_ITEM_DATA);
    if (preview && item_data.canConvert<ClipboardItem>()) PreviewPopup::instance()->show_preview(item_data.value<ClipboardItem>(), global_pos, 5);
}

void ClipboardWindow::update_search_background(const QString &text) {
    const bool has_text = !text.trimmed().isEmpty();
    apply_search_input_style(has_text);
    clear_search_btn->setVisible(has_text);
}

void ClipboardWindow::apply_search_input_style(bool has_text) { search_input->setStyleSheet(ThemeStyleGenerator(current_theme).generate_search_input_style(has_text)); }

void ClipboardWindow::apply_clear_search_btn_style() { clear_search_btn->setStyleSheet(ThemeStyleGenerator(current_theme).generate_clear_search_btn_style()); }

void ClipboardWindow::apply_menu_btn_style() {
    menu_btn->setStyleSheet(ThemeStyleGenerator(current_theme).generate_menu_btn_style());
    menu_btn->setIcon(tinted_icon(FluentIcon::Setting, current_theme.colors.text_secondary));
    menu_btn->setIconSize(QSize(scaled(16), scaled(16)));
}

void ClipboardWindow::on_marks_changed() {
    item_delegate->set_marked_ids(selection_manager->marked_ids());
    if (config->get_clipboard_display_lines() > 1) list_widget->doItemsLayout();
    list_widget->viewport()->update();
    selection_bar->set_count(static_cast<int>(selection_manager->marked_ids().size()));
}

void ClipboardWindow::toggle_item_mark(qint64 item_id) { selection_manager->toggle_mark(item_id); }

void ClipboardWindow::mark_items_to(qint64 item_id) { selection_manager->mark_range_to(item_id); }

void ClipboardWindow::apply_time_filter_styles() {
    const ThemeStyleGenerator generator(current_theme);
    if (time_filter_toggle_btn) {
        time_filter_toggle_btn->setStyleSheet(generator.generate_filter_toggle_btn_style());
        // Points down to close the bar while it is open.
        const bool open = time_filter_toggle_btn->isChecked();
        const auto &colors = current_theme.colors;
        time_filter_toggle_btn->setIcon(tinted_icon(ResourceManager::get_icon_path(open ? "step_down.svg" : "step_up.svg"), open ? colors.text_primary : colors.text_secondary));
        time_filter_toggle_btn->setIconSize(QSize(scaled(12), scaled(12)));
    }
    if (start_date_edit) {
        const QString date_style = generator.generate_time_filter_date_edit_style();
        const QString calendar_style = generator.generate_time_filter_calendar_style();
        start_date_edit->setStyleSheet(date_style);
        end_date_edit->setStyleSheet(date_style);
        start_date_edit->calendarWidget()->setStyleSheet(calendar_style);
        end_date_edit->calendarWidget()->setStyleSheet(calendar_style);
    }
    if (time_filter_separator)
        set_own_style(time_filter_separator,
                      QString("background: transparent; border: none; color: %1; font-size: %2px;").arg(current_theme.colors.text_secondary).arg(scaled(12)));
    if (apply_time_filter_btn) apply_time_filter_btn->setStyleSheet(generator.generate_time_filter_action_btn_style(true));
    if (type_filter_btn) {
        type_filter_btn->setStyleSheet(generator.generate_time_filter_type_btn_style());
        type_filter_menu->setStyleSheet(generator.generate_menu_style());
        refresh_type_filter_button();
    }
    if (clear_time_filter_btn) clear_time_filter_btn->setStyleSheet(generator.generate_time_filter_close_btn_style());
}

void ClipboardWindow::clear_search() {
    search_input->clear();
    search_input->setFocus();
}

void ClipboardWindow::update_type_filter_button(int index) {
    type_filter_index = index;
    for (int action_index = 0; action_index < type_filter_actions.size(); ++action_index) type_filter_actions[action_index]->setChecked(action_index == index);
    refresh_type_filter_button();
}

void ClipboardWindow::refresh_type_filter_button() {
    // A filter group's kind is fixed: the button shows it greyed out; other views show the user's choice.
    if (!type_filter_btn) return;
    const auto rule = controller->current_filter_rule();
    const auto fixed = rule ? rule->kind : std::nullopt;
    const int index = fixed ? std::max<int>(0, type_filter_kinds().indexOf(*fixed)) : type_filter_index;
    type_filter_btn->setText(type_filter_labels[index]);
    type_filter_btn->setEnabled(!fixed);
    type_filter_btn->setToolTip(fixed ? tr_text("Fixed by this filter group") : tr_text("Filter Type"));
}

void ClipboardWindow::show_type_filter_menu() { type_filter_menu->exec(type_filter_btn->mapToGlobal(QPoint(0, type_filter_btn->height()))); }

void ClipboardWindow::set_type_filter(int index) {
    update_type_filter_button(index);
    const QString kind = type_filter_kinds().value(index);
    controller->set_content_type_filter(kind.isEmpty() ? std::nullopt : std::optional(kind));
}

void ClipboardWindow::on_group_switched(std::optional<qint64> group_id) {
    group_bar->switch_to_group(group_id);
    bool is_quick_launch = false;
    if (group_id) {
        for (const auto &group : controller->manager.get_groups())
            if (group.id == *group_id) is_quick_launch = group.group_type == GroupType::FILE;
    }
    item_delegate->set_hide_file_icon(is_quick_launch);
    if (config->get_clipboard_display_lines() > 1) list_widget->doItemsLayout();
    refresh_type_filter_button();
    list_widget->viewport()->update();
}

void ClipboardWindow::on_sidebar_entered() {
    selection_manager->clear_cursor();
    list_widget->setFocus();
}

void ClipboardWindow::on_sidebar_exited(std::optional<qint64> prev_item_id) {
    list_widget->setFocus();
    if (!selection_manager->move_cursor_to(prev_item_id)) selection_manager->move_cursor(1);
}

void ClipboardWindow::enter_sidebar_mode() { group_bar->enter_sidebar_mode(selection_manager->cursor_id()); }

void ClipboardWindow::on_top_group_switch(int delta) { group_bar->handle_top_group_switch(delta); }

void ClipboardWindow::on_add_group_clicked() {
    auto *dialog = get_manage_dialog(&manager);
    connect_manage_dialog(dialog);
    dialog->switch_page(0);
    dialog->show_and_activate();
}

void ClipboardWindow::on_add_item_clicked() {
    auto *dialog = get_manage_dialog(&manager);
    connect_manage_dialog(dialog);
    dialog->switch_page(1);
    dialog->show_and_activate();
}

void ClipboardWindow::on_clear_clicked() { controller->clear_history(this); }

void ClipboardWindow::connect_manage_dialog(ManageDialog *dialog) {
    connect(dialog, &ManageDialog::group_added, group_bar, &GroupBar::refresh_buttons, Qt::UniqueConnection);
    connect(dialog, &ManageDialog::data_changed, this, &ClipboardWindow::on_manage_data_changed, Qt::UniqueConnection);
}

void ClipboardWindow::set_group_bar_position(const QString &position) {
    group_bar_position = position;
    selection_manager->group_bar_position = position;
    group_bar->set_position(position);
    config->set_clipboard_group_bar_position(position);
    apply_opacity();
}

bool ClipboardWindow::is_paste_picker_surface(Hwnd hwnd) const {
    // Only this picker window and windows it owns (quick edit, confirmations); other windows of this process are valid paste targets.
    const QWidget *widget = QWidget::find(static_cast<WId>(hwnd));
    return widget && owns_window(widget->window());
}

ClipboardController::CloseCallback ClipboardWindow::paste_close_callback() {
    // With the switch off the window stays open for pasting several times.
    if (!config->get_clipboard_close_after_paste()) return {};
    return [this] { close(); };
}

std::optional<ClipboardItem> ClipboardWindow::get_item_data(qint64 item_id) const { return controller->loaded_item(item_id); }

bool ClipboardWindow::is_launch_item(qint64 item_id) const {
    const auto item = get_item_data(item_id);
    return item && group_accepts(static_cast<int>(GroupType::FILE), item->content_type, item->sub_kind);
}

void ClipboardWindow::open_launch_item(qint64 item_id) {
    const auto item = get_item_data(item_id);
    if (item && item->content_type == "text" && item->sub_kind == SUB_KIND_URL) open_link(item_id);
    else open_file_item(item_id);
}

bool ClipboardWindow::in_file_group() {
    if (!controller->current_group_id) return false;
    for (const auto &group : controller->manager.get_groups())
        if (group.id == *controller->current_group_id) return group.group_type == GroupType::FILE;
    return false;
}

void ClipboardWindow::on_paste_item(qint64 item_id) {
    if (mouse_controller) mouse_controller->cancel();
    // Clicking a row of the multiple selection pastes them all; another row pastes only itself.
    const auto marked = selection_manager->marked_ids(true);
    if (marked.size() > 1 && marked.contains(item_id)) {
        paste_marked(marked);
        return;
    }
    selection_manager->clear_marks();
    // Other items retained after a group type change still paste as usual.
    if (in_file_group() && is_launch_item(item_id)) {
        open_launch_item(item_id);
        return;
    }
    if (controller->paste_item(item_id, paste_close_callback())) emit item_pasted(item_id);
}

void ClipboardWindow::paste_selected(bool plain_text) {
    const auto marked = selection_manager->marked_ids(true);
    if (marked.size() > 1) {
        paste_marked(marked, plain_text);
        return;
    }
    const std::optional<qint64> item_id = !marked.isEmpty() ? std::optional<qint64>(marked.front()) : selection_manager->cursor_id();
    if (!item_id || !*item_id) return;
    if (plain_text) paste_plain_text(*item_id);
    else on_paste_item(*item_id);
}

void ClipboardWindow::paste_plain_text(qint64 item_id) {
    // Shift+Enter: text without formatting, files as paths, images as usual.
    const auto item = get_item_data(item_id);
    if (!item) return;
    selection_manager->clear_marks();
    const auto close = paste_close_callback();
    if (item->content_type == "text") controller->paste_transformed_text(item_id, "special_paste_plain_text", close);
    else if (item->content_type == "file") controller->paste_file_text(item_id, "file_paste_links", close);
    else on_paste_item(item_id);
}

void ClipboardWindow::paste_marked(const QList<qint64> &item_ids, bool plain_text, const QString &layout, bool explicit_paste) {
    // A multiple selection of launchable items opens each; explicit paste keeps its usual behavior.
    if (mouse_controller) mouse_controller->cancel();
    if (!explicit_paste && in_file_group() && std::all_of(item_ids.begin(), item_ids.end(), [this](qint64 item_id) { return is_launch_item(item_id); })) {
        selection_manager->clear_marks();
        for (qint64 item_id : item_ids) open_launch_item(item_id);
        return;
    }
    bool stitching = !plain_text;
    for (qint64 item_id : item_ids) {
        const auto item = get_item_data(item_id);
        if (!item || item->content_type != "image") stitching = false;
    }
    if (stitching && !controller->stitched_image_fits(item_ids, layout)) {
        show_warning_dialog(this, tr_text("Images Too Large"), tr_text("The stitched image would be too large. Select fewer images."));
        return;
    }
    if (controller->paste_items(item_ids, paste_close_callback(), explicit_paste, plain_text, layout)) selection_manager->clear_marks();
}

void ClipboardWindow::show_context_menu(const QPoint &pos) {
    QListWidgetItem *item = list_widget->itemAt(pos);
    if (!item) return;
    const qint64 item_id = item->data(Qt::UserRole).toLongLong();
    if (!item_id) return;
    show_item_context_menu(item_id, pos);
}

void ClipboardWindow::show_item_context_menu(qint64 item_id, const QPoint &pos) {
    if (mouse_controller) mouse_controller->cancel();
    const auto marked = selection_manager->marked_ids(true);
    if (marked.size() > 1 && marked.contains(item_id)) {
        show_marked_context_menu(marked, pos);
        return;
    }
    selection_manager->clear_marks();
    const auto ctx = controller->build_context_menu_data(item_id);
    if (!ctx) return;
    ClipboardItemContextMenu(this, menu_style(), translate_window, item_context_menu_handlers(item_id),
                             {[this, item_id](const QString &action_key) { return resolve_item_context_menu_handler(item_id, action_key); }})
        .show(list_widget, pos, *ctx);
}

void ClipboardWindow::show_marked_context_menu(const QList<qint64> &item_ids, const QPoint &pos) {
    const auto ctx = controller->build_marked_context_menu_data(item_ids);
    if (!ctx) return;
    QMap<QString, ActionHandler> handlers{
        {"paste_marked", [this, item_ids] { paste_marked(item_ids, false, "vertical", true); }},
        {"paste_marked_plain", [this, item_ids] { paste_marked(item_ids, true, "vertical", true); }},
        {"paste_marked_vertical", [this, item_ids] { paste_marked(item_ids, false, "vertical", true); }},
        {"paste_marked_horizontal", [this, item_ids] { paste_marked(item_ids, false, "horizontal", true); }},
        {"pin_marked", [this, item_ids] {
             QList<ClipboardItem> items;
             for (qint64 item_id : item_ids)
                 if (const auto item = controller->get_item(item_id)) items.append(*item);
             selection_manager->clear_marks();
             create_pins_from_clipboard_items(items, this);
         }},
        {"remove_from_group", [this, item_ids] { move_marked_to_group(item_ids, std::nullopt); }},
        {"clear_marks", [this] { selection_manager->clear_marks(); }},
        {"delete_marked", [this, item_ids] { delete_marked(item_ids); }},
    };
    const DynamicHandlerResolver resolver = [this, item_ids](const QString &action_key) -> ActionHandler {
        const QString prefix = "move_to_group_";
        if (!action_key.startsWith(prefix)) return {};
        bool ok = false;
        const qint64 group_id = action_key.mid(prefix.size()).toLongLong(&ok);
        if (!ok) return {};
        return [this, item_ids, group_id] { move_marked_to_group(item_ids, group_id); };
    };
    ClipboardItemContextMenu(this, menu_style(), translate_window, handlers, {resolver}).show(list_widget, pos, *ctx);
}

void ClipboardWindow::move_marked_to_group(const QList<qint64> &item_ids, std::optional<qint64> group_id) {
    selection_manager->clear_marks();
    controller->move_items_to_group(item_ids, group_id);
}

void ClipboardWindow::delete_marked(const QList<qint64> &item_ids) {
    const bool confirmed = show_confirm_dialog(this, tr_text("Confirm Delete"),
                                               tr_text("Are you sure you want to delete the {count} selected items?").replace("{count}", QString::number(item_ids.size())));
    if (confirmed) {
        selection_manager->clear_marks();
        controller->delete_items(item_ids);
    }
}

QMap<QString, std::function<void()>> ClipboardWindow::item_context_menu_handlers(qint64 item_id) {
    return {
        {"paste", [this, item_id] { paste_item_to_clipboard(item_id); }},
        {"start_marking", [this, item_id] { selection_manager->mark(item_id); }},
        {"pin_image", [this, item_id] { create_pin_window(item_id); }},
        {"save_image_as", [this, item_id] { save_image_item_as(this, manager, controller->get_item(item_id)); }},
        {"save_item_as", [this, item_id] { save_item_as(item_id); }},
        {"toggle_pin", [this, item_id] { controller->toggle_pin(item_id); }},
        {"open_file_location", [this, item_id] { open_file_location(item_id); }},
        {"open_link", [this, item_id] { open_link(item_id); }},
        {"path_paste_as_files", [this, item_id] {
             if (controller->paste_as_files(item_id, paste_close_callback(), true)) emit item_pasted(item_id);
         }},
        {"quick_edit_item", [this, item_id] { quick_edit_item(item_id); }},
        {"edit_item", [this, item_id] { edit_item(item_id); }},
        {"move_item_up", [this, item_id] { controller->move_item_order(item_id, controller->current_group_id, -1); }},
        {"move_item_down", [this, item_id] { controller->move_item_order(item_id, controller->current_group_id, 1); }},
        {"delete_item", [this, item_id] { delete_item(item_id); }},
    };
}

std::function<void()> ClipboardWindow::resolve_item_context_menu_handler(qint64 item_id, const QString &action_key) {
    if (action_key == "remove_from_group") return [this, item_id] { controller->move_to_group(item_id, std::nullopt); };
    const QString prefix = "move_to_group_";
    if (action_key.startsWith(prefix)) {
        bool ok = false;
        const qint64 group_id = action_key.mid(prefix.size()).toLongLong(&ok);
        if (!ok) return {};
        return [this, item_id, group_id] { controller->move_to_group(item_id, group_id); };
    }
    // Special Paste submenu entries.
    if (action_key.startsWith("special_paste_") || action_key.startsWith("transform_")) return [this, item_id, action_key] { special_paste(item_id, action_key); };
    // Special Paste entries of file items.
    if (action_key.startsWith("file_paste_")) return [this, item_id, action_key] { file_special_paste(item_id, action_key); };
    // A color item's other formats.
    if (const QString color_prefix = "color_paste_"; action_key.startsWith(color_prefix)) {
        bool ok = false;
        const int index = action_key.mid(color_prefix.size()).toInt(&ok);
        if (!ok) return {};
        return [this, item_id, index] {
            if (controller->paste_color_conversion(item_id, index, paste_close_callback(), true)) emit item_pasted(item_id);
        };
    }
    return {};
}

void ClipboardWindow::special_paste(qint64 item_id, const QString &action_key) {
    controller->paste_transformed_text(item_id, action_key, paste_close_callback(), true);
    emit item_pasted(item_id);
}

void ClipboardWindow::file_special_paste(qint64 item_id, const QString &action_key) {
    controller->paste_file_text(item_id, action_key, paste_close_callback(), true);
    emit item_pasted(item_id);
}

void ClipboardWindow::edit_item(qint64 item_id) {
    controller->open_manage_dialog_for_item(item_id, controller->current_group_id, [this](ManageDialog *dialog) { connect_manage_dialog(dialog); });
}

bool ClipboardWindow::edit_selected() {
    const auto item_id = selection_manager->cursor_id();
    const auto item = item_id && *item_id ? get_item_data(*item_id) : std::nullopt;
    if (!item) return false;
    if (is_quick_editable(*item)) {
        quick_edit_item(*item_id);
        return true;
    }
    if (!controller->shows_history()) {
        edit_item(*item_id);
        return true;
    }
    return false;
}

void ClipboardWindow::quick_edit_item(qint64 item_id) {
    if (mouse_controller) mouse_controller->cancel();
    const auto item = get_item_data(item_id);
    if (!item) return;
    if (!quick_edit_popup) {
        quick_edit_popup = new QuickEditPopup(this);
        connect(quick_edit_popup, &QuickEditPopup::finished, this, [this](const QuickEditResult &result) { on_quick_edit_finished(result); });
    }
    PreviewPopup::instance()->set_display_enabled(false);
    quick_edit_popup->open_for(*item, row_global_pos(item_id), frameGeometry(), current_theme);
}

QPoint ClipboardWindow::row_global_pos(qint64 item_id) const {
    for (int row = 0; row < list_widget->count(); ++row) {
        QListWidgetItem *list_item = list_widget->item(row);
        if (list_item->data(ROLE_ITEM_ID).toLongLong() != item_id) continue;
        const QRect rect = list_widget->visualItemRect(list_item);
        if (!rect.isNull()) return list_widget->viewport()->mapToGlobal(rect.topLeft());
    }
    return QCursor::pos();
}

void ClipboardWindow::on_quick_edit_finished(const QuickEditResult &result) {
    if (isVisible()) PreviewPopup::instance()->set_display_enabled(true);
    const bool saved = !result.text || save_quick_edit(result.item_id, *result.text);
    if (result.paste && saved) {
        paste_item_to_clipboard(result.item_id);
    } else if (result.focus_left) {
        // A click in another program hides it by the focus rule; a click back here keeps it.
        QTimer::singleShot(100, this, [this] { check_and_hide(); });
    } else if (isVisible()) {
        activateWindow();
        list_widget->setFocus();
    }
}

bool ClipboardWindow::save_quick_edit(qint64 item_id, const QString &text) {
    const auto item = get_item_data(item_id);
    if (!item) return false;
    if (text == item->content) return true;
    return controller->update_item_content(item_id, text);
}

void ClipboardWindow::create_pin_window(qint64 item_id) { create_pin_from_clipboard_item(item_id, controller, this); }

void ClipboardWindow::start_item_drag(qint64 item_id) {
    if (dragging_) return;
    // A marked row carries every marked row, in the order a paste of them uses.
    QList<qint64> ids = selection_manager->marked_ids(true);
    if (!ids.contains(item_id)) ids = {item_id};
    const QString images_dir = manager.get_images_dir().value_or(QString());
    QList<ClipboardItem> items;
    for (const qint64 id : ids)
        if (const auto item = get_item_data(id); item && ItemDragMimeData::can_drag(*item, images_dir)) items.append(*item);
    if (items.isEmpty()) return;
    PreviewPopup::instance()->hide_preview();
    QPoint hot_spot;
    const QPixmap pixmap = drag_pixmap(item_id, static_cast<int>(items.size()), &hot_spot);
    dragging_ = true;
    exec_drag_out(list_widget, new ItemDragMimeData(items, images_dir, ItemDragMimeData::default_temp_root()), pixmap, hot_spot);
    dragging_ = false;
    check_and_hide();
}

QString ClipboardWindow::last_save_folder() const {
    // Shared with the save image window: the last folder anything was saved to.
    const QString folder = config->get_save_dialog_last_dir();
    return !folder.isEmpty() && QFileInfo(folder).isDir() ? folder : QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

bool ClipboardWindow::save_items_to_folder(const QList<ClipboardItem> &items) {
    if (items.isEmpty()) return false;
    PreviewPopup::instance()->hide_preview();
    const QString start = last_save_folder();
    const QString folder = get_existing_directory(this, tr_text("Choose a Folder to Save To"), start);
    if (folder.isEmpty()) return false;
    config->set_save_dialog_last_dir(QDir::toNativeSeparators(folder));
    // Text and images become files the way a drag makes them; they stay until the copy that reads them is done.
    auto staged = std::make_shared<ItemDragMimeData>(items, manager.get_images_dir().value_or(QString()), ItemDragMimeData::default_temp_root());
    const QStringList sources = staged->file_paths();
    log_info(T("另存 {count} 项到文件夹: {folder}", {{"count", sources.size()}, {"folder", QDir::toNativeSeparators(folder)}}), "Clipboard");
    // A file row records only where the files were; they may have been moved or deleted since.
    copy_into_folder(sources, folder, [staged, window = QPointer<ClipboardWindow>(this), total = sources.size()](bool, const QStringList &missing) {
        if (missing.isEmpty()) return;
        log_warning(T("另存时跳过已不存在的文件: {paths}", {{"paths", missing.join("; ")}}), "Clipboard");
        QWidget *parent = window && window->isVisible() ? window.data() : nullptr;
        if (missing.size() == total) show_info_dialog(parent, tr_text("Save as"), tr_text("These files are no longer where they were, so there is nothing to save."));
        else show_info_dialog(parent, tr_text("Save as"), tr_text("{count} files are no longer where they were and were skipped.").replace("{count}", QString::number(missing.size())));
    });
    return true;
}

void ClipboardWindow::save_marked_to_folder() {
    const QString images_dir = manager.get_images_dir().value_or(QString());
    QList<ClipboardItem> items;
    for (const qint64 id : selection_manager->marked_ids(true))
        if (const auto item = get_item_data(id); item && ItemDragMimeData::can_drag(*item, images_dir)) items.append(*item);
    // A cancelled picker keeps the selection for another try.
    if (save_items_to_folder(items)) selection_manager->clear_marks();
}

void ClipboardWindow::save_item_as(qint64 item_id) {
    const auto item = get_item_data(item_id);
    if (!item) return;
    if (item->content_type == "image") {
        save_image_item_as(this, manager, item);
        return;
    }
    if (item->content_type == "file") {
        save_items_to_folder({*item});
        return;
    }
    if (item->content_type != "text") return;
    PreviewPopup::instance()->hide_preview();
    const QString suggested = QDir(last_save_folder()).filePath(ItemDragMimeData::file_name(*item));
    QString path = get_save_file_name(this, tr_text("Save as"), suggested, tr_text("Text Files (*.txt)")).first;
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path += ".txt";
    config->set_save_dialog_last_dir(QDir::toNativeSeparators(QFileInfo(path).absolutePath()));
    QSaveFile file(path);
    const QByteArray bytes = item->content.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        log_error(T("保存文本失败: {path}: {e}", {{"path", QDir::toNativeSeparators(path)}, {"e", file.errorString()}}), "Clipboard");
        show_warning_dialog(this, tr_text("Save Failed"), tr_text("Failed to save the file."));
    }
}

QPixmap ClipboardWindow::drag_pixmap(qint64 item_id, int count, QPoint *hot_spot) const {
    QListWidgetItem *row = nullptr;
    for (int i = 0; i < list_widget->count() && !row; ++i)
        if (list_widget->item(i)->data(Qt::UserRole).toLongLong() == item_id) row = list_widget->item(i);
    QWidget *viewport = list_widget->viewport();
    const QRect rect = row ? list_widget->visualItemRect(row).intersected(viewport->rect()) : QRect();
    if (rect.isEmpty()) return {};
    QPixmap pixmap = viewport->grab(rect);
    const qreal ratio = pixmap.devicePixelRatio();
    const int max_width = scaled(240);
    const qreal factor = rect.width() > max_width ? static_cast<qreal>(max_width) / rect.width() : 1.0;
    if (factor < 1.0) {
        pixmap = pixmap.scaledToWidth(qRound(max_width * ratio), Qt::SmoothTransformation);
        pixmap.setDevicePixelRatio(ratio);
    }
    const QSize size = pixmap.deviceIndependentSize().toSize();
    const QPoint offset = viewport->mapFromGlobal(QCursor::pos()) - rect.topLeft();
    *hot_spot = QPoint(std::clamp(qRound(offset.x() * factor), 0, size.width() - 1), std::clamp(qRound(offset.y() * factor), 0, size.height() - 1));
    if (count > 1) {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const int diameter = scaled(20);
        const QRect badge(size.width() - diameter - scaled(4), scaled(4), diameter, diameter);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(current_theme.colors.accent_primary));
        painter.drawEllipse(badge);
        QFont font = painter.font();
        font.setPixelSize(scaled(11));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, QString::number(count));
    }
    return pixmap;
}

void ClipboardWindow::delete_item(qint64 item_id) {
    if (show_confirm_dialog(this, tr_text("Confirm Delete"), tr_text("Are you sure you want to delete this item?"))) controller->delete_item(item_id);
}

void ClipboardWindow::open_file_item(qint64 item_id) {
    const auto clipboard_item = controller->get_item(item_id);
    if (!clipboard_item) return;
    if (clipboard_item->content_type == "file") {
        QJsonParseError error;
        QJsonDocument::fromJson(clipboard_item->content.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError) {
            log_warning(T("打开文件失败: {e}", {{"e", error.errorString()}}), "Clipboard");
            return;
        }
    }
    const QStringList files = item_paths(*clipboard_item);
    if (files.isEmpty()) return;
    const QString file_path = QDir::toNativeSeparators(QDir::cleanPath(files.front()));
    if (QFileInfo::exists(file_path)) {
        QTimer::singleShot(0, this, [file_path] { QDesktopServices::openUrl(QUrl::fromLocalFile(file_path)); });
        QTimer::singleShot(50, this, [this] { hide(); });
    }
}

void ClipboardWindow::paste_item_to_clipboard(qint64 item_id) {
    // Paste from the context menu: the user asked for one paste, so the auto-paste switch is skipped.
    if (controller->paste_item(item_id, paste_close_callback(), true)) emit item_pasted(item_id);
}

void ClipboardWindow::open_file_location(qint64 item_id) {
    const auto clipboard_item = controller->get_item(item_id);
    if (!clipboard_item) return;
    if (clipboard_item->content_type == "file") {
        QJsonParseError error;
        QJsonDocument::fromJson(clipboard_item->content.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError) {
            log_warning(T("打开文件位置失败: {e}", {{"e", error.errorString()}}), "Clipboard");
            return;
        }
    }
    const QStringList files = item_paths(*clipboard_item);
    if (files.isEmpty()) return;
    const QString file_path = QDir::toNativeSeparators(QDir::cleanPath(files.front()));
    const QFileInfo info(file_path);
    if (info.isDir()) {
        QTimer::singleShot(0, this, [file_path] { QProcess::startDetached("explorer", {file_path}); });
    } else if (info.exists()) {
        QTimer::singleShot(0, this, [file_path] { QProcess::startDetached("explorer", {"/select,", file_path}); });
    } else if (const QString parent = QDir::toNativeSeparators(info.path()); QFileInfo::exists(parent)) {
        QTimer::singleShot(0, this, [parent] { QProcess::startDetached("explorer", {parent}); });
    }
}

void ClipboardWindow::open_link(qint64 item_id) {
    const auto clipboard_item = controller->get_item(item_id);
    if (!clipboard_item || clipboard_item->sub_kind != SUB_KIND_URL) return;
    QString text = clipboard_item->content.trimmed();
    if (text.startsWith("www.", Qt::CaseInsensitive)) text.prepend("https://");
    const QUrl url(text);
    if (!url.isValid()) return;
    QTimer::singleShot(0, this, [url] { QDesktopServices::openUrl(url); });
    QTimer::singleShot(50, this, [this] { hide(); });
}

void ClipboardWindow::notify_new_content(const std::optional<ClipboardItem> &item) { emit new_item_received(item); }

void ClipboardWindow::warmup_offscreen_once() {
    if (offscreen_warmup_done_) return;
    offscreen_warmup_done_ = true;
    QScreen *screen = QApplication::primaryScreen();
    if (!screen) return;
    offscreen_warmup_in_progress_ = true;
    const QPoint old_pos = pos();
    const double old_opacity = windowOpacity();
    const QRect geo = screen->availableGeometry();
    setWindowOpacity(0);
    move(geo.right() + 20000, geo.bottom() + 20000);
    show();
    QApplication::processEvents();
    hide();
    log_debug(T("剪贴板窗口离屏预热完成"), "Clipboard");
    move(old_pos);
    setWindowOpacity(old_opacity);
    offscreen_warmup_in_progress_ = false;
}

void ClipboardWindow::showEvent(QShowEvent *event) {
    if (offscreen_warmup_in_progress_) {
        QWidget::showEvent(event);
        return;
    }
    PreviewPopup::instance()->set_display_enabled(true);
    setWindowOpacity(0);

    shortcut_handler_->reload_bindings();
    item_delegate->set_pick_keys(shortcut_handler_->pick_keys);
    ShortcutManager::instance()->register_handler(shortcut_handler_.get());

    frameless_.reset();
    selection_manager->reset();
    controller->on_window_show();

    QElapsedTimer timer;
    timer.start();
    QWidget::showEvent(event);
    list_widget->setFocus();
    position_on_show();
    QTimer::singleShot(0, group_bar, [this] { group_bar->refresh_buttons(); });
    QTimer::singleShot(0, this, [this] { reveal_window(); });
    log_debug(T("⏱️ 打开窗口耗时: {elapsed_ms:.1f} ms", {{"elapsed_ms", timer.nsecsElapsed() / 1e6}}), "Clipboard");
}

void ClipboardWindow::reveal_window() { setWindowOpacity(1); }

void ClipboardWindow::position_on_show() {
    if (config->get_clipboard_open_position() != "previous" || !restore_previous_position()) position_at_cursor();
    keep_area_clear();
}

void ClipboardWindow::keep_area_clear() {
    const QRect area = area_to_keep_clear_;
    area_to_keep_clear_ = {};
    if (area.isEmpty() || !geometry().intersects(area)) return;
    QScreen *screen = QGuiApplication::screenAt(area.center());
    if (!screen) return;
    const QRect available = screen->availableGeometry();
    constexpr int GAP = 10;
    // Below the area, else above it; with room on neither side the window stays where it was.
    if (area.bottom() + GAP + height() <= available.bottom()) move(x(), area.bottom() + GAP);
    else if (area.top() - GAP - height() >= available.top()) move(x(), area.top() - GAP - height());
}

// A saved position whose screen is gone, or that no longer fits on it, falls back to the cursor.
bool ClipboardWindow::restore_previous_position() {
    if (!saved_x_ || !saved_y_) return false;
    const QRect frame(QPoint(*saved_x_, *saved_y_), size());
    QScreen *screen = QGuiApplication::screenAt(frame.center());
    if (!screen) return false;
    const QRect available = screen->availableGeometry();
    if (frame.width() > available.width() || frame.height() > available.height()) return false;
    const int x = std::clamp(frame.x(), available.left(), available.right() + 1 - frame.width());
    const int y = std::clamp(frame.y(), available.top(), available.bottom() + 1 - frame.height());
    move(x, y);
    return true;
}

void ClipboardWindow::position_at_cursor() {
    const QPoint cursor_pos = QCursor::pos();
    QScreen *screen = QApplication::screenAt(cursor_pos);
    if (!screen) return;
    const QRect screen_geo = screen->availableGeometry();
    int x = cursor_pos.x() + 10;
    int y = cursor_pos.y() + 10;
    if (x + width() > screen_geo.right()) x = cursor_pos.x() - width() - 10;
    if (y + height() > screen_geo.bottom()) y = cursor_pos.y() - height() - 10;
    if (x < screen_geo.left()) x = screen_geo.left();
    if (y < screen_geo.top()) y = screen_geo.top();
    move(x, y);
}

void ClipboardWindow::hideEvent(QHideEvent *event) {
    if (offscreen_warmup_in_progress_) {
        QWidget::hideEvent(event);
        return;
    }
    // The preview is disabled first: a failure below or late key and hover events in the queue must not bring it back after the window hides.
    PreviewPopup::instance()->set_display_enabled(false);
    if (quick_edit_popup) quick_edit_popup->close_for_focus_loss();
    frameless_.reset();
    controller->on_window_hide();
    if (!config->get_clipboard_preserve_search() && !search_input->text().isEmpty()) search_input->clear();
    QWidget::hideEvent(event);
    save_window_geometry();
    if (QWidget *active_popup = QApplication::activePopupWidget()) active_popup->close();
}

void ClipboardWindow::closeEvent(QCloseEvent *event) {
    // hideEvent usually follows a close, but disabling here covers a close that does not reach it.
    PreviewPopup::instance()->set_display_enabled(false);
    ShortcutManager::instance()->unregister_handler(shortcut_handler_.get());
    save_window_geometry();
    if (QWidget *active_popup = QApplication::activePopupWidget()) active_popup->close();
    emit closed();
    QWidget::closeEvent(event);
}

void ClipboardWindow::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) QTimer::singleShot(100, this, [this] { check_and_hide(); });
}

bool ClipboardWindow::owns_window(const QWidget *window) const {
    for (const QWidget *current = window; current; current = current->parentWidget())
        if (current == this) return true;
    return false;
}

void ClipboardWindow::check_and_hide() {
    if (dragging_) return;
    if (isActiveWindow()) return;
    // In resident mode the user closes it; pasting itself hands focus to the target window.
    if (!config->get_clipboard_close_after_paste()) return;
    // Confirmations and menus activate their own top-level windows, which still belong to the clipboard window.
    for (const QWidget *surface : {QApplication::activeWindow(), QApplication::activeModalWidget(), QApplication::activePopupWidget()})
        if (owns_window(surface)) return;
    hide();
}

bool ClipboardWindow::is_draggable_area(QObject *widget) const { return group_bar && group_bar->bar_widget && widget == group_bar->bar_widget; }

bool ClipboardWindow::is_date_filter_widget(QObject *obj) const {
    for (QDateEdit *widget : {start_date_edit, end_date_edit}) {
        if (!widget) continue;
        if (obj == widget || (obj->isWidgetType() && widget->isAncestorOf(static_cast<QWidget *>(obj)))) return true;
        if (QCalendarWidget *calendar = widget->calendarWidget())
            if (obj == calendar || (obj->isWidgetType() && calendar->isAncestorOf(static_cast<QWidget *>(obj)))) return true;
    }
    return qobject_cast<QCalendarWidget *>(obj) != nullptr;
}

bool ClipboardWindow::eventFilter(QObject *obj, QEvent *event) {
    const QEvent::Type event_type = event->type();

    const bool on_viewport = (event_type == QEvent::MouseMove || event_type == QEvent::Leave) && list_widget && obj == list_widget->viewport();
    if (on_viewport && event_type == QEvent::MouseMove) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        on_pointer_moved(mouse->position().toPoint(), mouse->globalPosition().toPoint());
    }
    // Leaving keeps the cursor where it was, like a menu.
    if (on_viewport && event_type == QEvent::Leave) {
        pointer_row_ = -1;
        PreviewPopup::instance()->hide_preview();
    }
    // Scrolling with the wheel is the mouse at work: the row it brings under the still mouse is pointed at.
    if (event_type == QEvent::Wheel && list_widget && obj == list_widget->viewport()) {
        const auto *wheel = static_cast<QWheelEvent *>(event);
        const QPoint viewport_pos = wheel->position().toPoint();
        const QPoint global_pos = wheel->globalPosition().toPoint();
        const int scroll = list_widget->verticalScrollBar()->value();
        // Queued: the list scrolls only after this filter lets the event through.
        QMetaObject::invokeMethod(this, [this, viewport_pos, global_pos, scroll] {
            if (list_widget->verticalScrollBar()->value() != scroll) point_at(viewport_pos, global_pos);
        }, Qt::QueuedConnection);
    }
    if (event_type == QEvent::FocusIn && obj == list_widget && !group_bar->sidebar_mode()) group_bar->clear_sidebar_focus();

    if (event_type == QEvent::KeyPress && group_bar_position != "top") {
        const int key = static_cast<QKeyEvent *>(event)->key();
        const int k_enter = group_bar_position == "right" ? Qt::Key_Right : Qt::Key_Left;
        const int k_exit = group_bar_position == "right" ? Qt::Key_Left : Qt::Key_Right;
        if (group_bar->sidebar_mode()) {
            if (key == k_exit) {
                group_bar->exit_sidebar_mode();
                return true;
            }
            if (key == Qt::Key_Up) {
                group_bar->move_sidebar_selection(-1);
                return true;
            }
            if (key == Qt::Key_Down) {
                group_bar->move_sidebar_selection(1);
                return true;
            }
            if (key == k_enter) return true;
        } else if (key == k_enter) {
            QWidget *focus_widget = QApplication::focusWidget();
            if (focus_widget == list_widget || (focus_widget && list_widget->isAncestorOf(focus_widget))) {
                enter_sidebar_mode();
                return true;
            }
        }
    }

    if ((event_type == QEvent::MouseButtonPress || event_type == QEvent::MouseButtonRelease || event_type == QEvent::MouseMove) && is_date_filter_widget(obj))
        return QWidget::eventFilter(obj, event);

    if (frameless_.handle_event(obj, event)) return true;
    return QWidget::eventFilter(obj, event);
}

void ClipboardWindow::leaveEvent(QEvent *event) {
    if (!frameless_.is_dragging() && frameless_.resize_edge().isEmpty()) unsetCursor();
    QWidget::leaveEvent(event);
}

void ClipboardWindow::keyPressEvent(QKeyEvent *event) {
    const int key = event->key();
    if ((key == Qt::Key_Up || key == Qt::Key_Down) && !group_bar->sidebar_mode()) {
        QWidget *focus_widget = QApplication::focusWidget();
        if (!focus_widget || focus_widget == search_input || !(focus_widget == list_widget || list_widget->isAncestorOf(focus_widget))) {
            list_widget->setFocus();
            selection_manager->move_cursor(key == Qt::Key_Up ? -1 : 1);
            return;
        }
    }
    QWidget::keyPressEvent(event);
}

void ClipboardWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    list_widget->viewport()->update();
    if (group_bar && group_bar->bar_widget) group_bar->refresh_buttons();
}

// ==================== Mouse gestures ====================

ClipboardMouseController::ClipboardMouseController(ClipboardWindow *window)
    : QObject(window), window_(window), list_widget_(window->list_widget), viewport_(window->list_widget->viewport()) {
    timer_.setSingleShot(true);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, [this] { finish_click(); });
    window->installEventFilter(this);
    list_widget_->installEventFilter(this);
    viewport_->installEventFilter(this);
    connect(list_widget_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { cancel(); });
    connect(list_widget_->model(), &QAbstractItemModel::modelReset, this, [this] { cancel(); });
}

void ClipboardMouseController::cancel() {
    timer_.stop();
    pending_.reset();
    pressed_.reset();
}

bool ClipboardMouseController::available(const QString &action, qint64 item_id) const {
    const auto item = window_->get_item_data(item_id);
    if (!item) return false;
    if (action == "pin") return can_pin(*item);
    if (action == "quick_edit") return is_quick_editable(*item);
    return true;
}

QString ClipboardMouseController::match(QInputEvent *event, const QString &gesture, qint64 item_id) const {
    for (const auto &action : CLIPBOARD_MOUSE_ACTIONS()) {
        if (mouse_gesture_binding_matches(get_clipboard_mouse_binding(window_->config, action.key), event, gesture) && available(action.key, item_id))
            return action.key;
    }
    return {};
}

std::function<void(qint64)> ClipboardMouseController::marking(QMouseEvent *event, const QString &gesture, qint64 item_id) const {
    if (gesture != "left" || !match(event, gesture, item_id).isEmpty() || !match(event, "doubleleft", item_id).isEmpty()) return {};
    if (event->modifiers() == Qt::ControlModifier) return [window = window_](qint64 id) { window->toggle_item_mark(id); };
    if (event->modifiers() == Qt::ShiftModifier) return [window = window_](qint64 id) { window->mark_items_to(id); };
    return {};
}

void ClipboardMouseController::run(const QString &action, qint64 item_id, const QPoint &position) {
    if (action.isEmpty() || !window_->isVisible() || !available(action, item_id)) return;
    if (action == "paste") window_->on_paste_item(item_id);
    else if (action == "pin") window_->create_pin_window(item_id);
    else if (action == "quick_edit") window_->quick_edit_item(item_id);
    else if (action == "menu") window_->show_item_context_menu(item_id, position);
}

void ClipboardMouseController::select_by_click(Qt::MouseButton button, Qt::KeyboardModifiers modifiers, qint64 item_id) {
    // Either button picks or drops the row, as with check boxes; the actions for the marks are under the bar's ··· button.
    if (button != Qt::LeftButton && button != Qt::RightButton) return;
    SelectionManager *selection = window_->selection_manager;
    if (modifiers == Qt::ShiftModifier) selection->mark_range_to(item_id);
    else selection->toggle_mark(item_id);
}

void ClipboardMouseController::finish_click() {
    const auto pending = pending_;
    cancel();
    if (pending) run(pending->action, pending->item_id, pending->position);
}

bool ClipboardMouseController::eventFilter(QObject *obj, QEvent *event) {
    const QEvent::Type kind = event->type();
    if (kind == QEvent::Hide || kind == QEvent::WindowDeactivate || kind == QEvent::FocusOut || kind == QEvent::KeyPress || kind == QEvent::Wheel) cancel();
    // The system's context menu event must not bypass a rebound right button.
    if (kind == QEvent::ContextMenu) {
        if (static_cast<QContextMenuEvent *>(event)->reason() == QContextMenuEvent::Mouse) return obj == list_widget_ || obj == viewport_;
        cancel();
    }
    if (obj != viewport_) return false;
    if (kind == QEvent::MouseMove) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (pressed_ && (mouse->globalPosition().toPoint() - pressed_->global_pos).manhattanLength() >= QApplication::startDragDistance()) {
            const Pressed pressed = *pressed_;
            cancel();
            // 越过系统拖动阈值才准备数据，同时取消单击/双击待执行动作。
            if (pressed.button == Qt::LeftButton && pressed.modifiers == Qt::NoModifier && mouse->buttons() == Qt::LeftButton &&
                mouse->modifiers() == Qt::NoModifier) {
                window_->start_item_drag(pressed.item_id);
                return true;
            }
        }
        return false;
    }
    if (kind != QEvent::MouseButtonPress && kind != QEvent::MouseButtonRelease && kind != QEvent::MouseButtonDblClick) return false;
    auto *mouse = static_cast<QMouseEvent *>(event);
    const Qt::MouseButton button = mouse->button();
    QString gesture;
    if (button == Qt::LeftButton) gesture = "left";
    else if (button == Qt::MiddleButton) gesture = "middle";
    else if (button == Qt::RightButton) gesture = "right";
    else return false;

    QListWidgetItem *row = list_widget_->itemAt(mouse->position().toPoint());
    const std::optional<qint64> item_id = row ? std::optional<qint64>(row->data(Qt::UserRole).toLongLong()) : std::nullopt;
    const auto modifiers = mouse->modifiers();
    const QPoint position = mouse->position().toPoint();
    if (kind == QEvent::MouseButtonPress || kind == QEvent::MouseButtonDblClick) {
        const auto pending = pending_;
        cancel();
        if (!item_id) return true;
        // The press points at the row; a plain left press also starts the next Shift range there.
        window_->selection_manager->point_at(list_widget_->row(row));
        if (button == Qt::LeftButton && !marking(mouse, gesture, *item_id)) window_->selection_manager->set_anchor(*item_id);
        list_widget_->setFocus();
        if (kind == QEvent::MouseButtonDblClick && pending && pending->item_id == *item_id && pending->button == button && pending->modifiers == modifiers) {
            const QString action = match(mouse, "double" + gesture, *item_id);
            if (!action.isEmpty()) {
                run(action, *item_id, position);
                return true;
            }
        }
        pressed_ = Pressed{*item_id, button, modifiers, mouse->globalPosition().toPoint()};
    } else if (pressed_) {
        const Pressed pressed = *pressed_;
        pressed_.reset();
        if (item_id && pressed.item_id == *item_id && pressed.button == button && pressed.modifiers == modifiers &&
            (mouse->globalPosition().toPoint() - pressed.global_pos).manhattanLength() < QApplication::startDragDistance()) {
            if (window_->selection_manager->has_marks()) {
                select_by_click(button, modifiers, *item_id);
                return true;
            }
            if (const auto mark = marking(mouse, gesture, *item_id)) {
                mark(*item_id);
                return true;
            }
            const QString action = match(mouse, gesture, *item_id);
            if (!match(mouse, "double" + gesture, *item_id).isEmpty()) {
                pending_ = Pending{action, *item_id, position, button, modifiers};
                timer_.start(QApplication::doubleClickInterval());
            } else {
                run(action, *item_id, position);
            }
        }
    }
    // Keeps QListWidget's itemClicked from activating the row again.
    return true;
}
}
