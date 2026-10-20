#include "ui/settings_ui/dialog.h"
#include "clipboard/ui/theme/themes.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/elevation.h"
#include "core/platform_utils.h"
#include "core/resource_manager.h"
#include "core/shortcut_manager.h"
#include "core/theme.h"
#include "core/ui_scale.h"
#include "core/ui_theme.h"
#include "ocr/ocr_manager.h"
#include "pin/pin_shortcut.h"
#include "settings/settings_transfer.h"
#include "settings/tool_settings.h"
#include "translation/registry.h"
#include "ui/dialogs.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/labels.h"
#include "ui/fluent_lite/navigation.h"
#include "ui/fluent_lite/segmented.h"
#include "ui/fluent_lite/switch.h"
#include "ui/fluent_lite/titlebar.h"
#include "ui/hotkey_edit.h"
#include "ui/inapp_key_edit.h"
#include "ui/settings_ui/components.h"
#include "ui/settings_ui/mouse_binding_editor.h"
#include "ui/settings_ui/pages.h"
#include "ui/toolbar_layout.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGuiApplication>
#include <QLabel>
#include <QMenu>
#include <QMetaMethod>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
// By content stack index.
QWidget *(*const PAGE_BUILDERS[])(SettingsDialog *) = {
    create_hotkey_page,         // 0
    create_capture_page,        // 1
    create_clipboard_page,      // 2
    create_appearance_page,     // 3
    create_translation_page,    // 4
    create_log_page,            // 5
    create_misc_page,           // 6
    create_developer_page,      // 7
    create_about_page,          // 8
    create_quick_actions_page,  // 9
    create_mouse_page,          // 10
    create_pin_page,            // 11
    create_screen_annotation_page,  // 12
};
constexpr int PAGE_COUNT = static_cast<int>(std::size(PAGE_BUILDERS));

// Value-change signals followed for the Apply button. Buttons opening a picker or editor may
// update state only after their child closes, without a value signal, so clicked counts too.
const QSet<QByteArray> TRACKED_SIGNALS = {"textChanged", "checkedChanged", "toggled", "currentIndexChanged", "valueChanged", "clicked"};

// The signals of a widget class to follow, one per name; cached per class.
const QList<QMetaMethod> &tracked_signals(const QMetaObject *meta) {
    static QHash<const QMetaObject *, QList<QMetaMethod>> cache;
    auto it = cache.find(meta);
    if (it == cache.end()) {
        QList<QMetaMethod> methods;
        QSet<QByteArray> seen;
        for (int i = 0; i < meta->methodCount(); ++i) {
            const auto method = meta->method(i);
            if (method.methodType() != QMetaMethod::Signal || !TRACKED_SIGNALS.contains(method.name()) || seen.contains(method.name())) continue;
            seen.insert(method.name());
            methods.append(method);
        }
        it = cache.insert(meta, methods);
    }
    return *it;
}

bool checked_of(QWidget *widget) {
    if (auto *card = qobject_cast<SwitchSettingCard *>(widget)) return card->isChecked();
    if (auto *button = qobject_cast<SwitchButton *>(widget)) return button->isChecked();
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) return button->isChecked();
    return false;
}

void set_checked(QWidget *widget, bool checked) {
    if (auto *card = qobject_cast<SwitchSettingCard *>(widget)) card->setChecked(checked);
    else if (auto *button = qobject_cast<SwitchButton *>(widget)) button->setChecked(checked);
    else if (auto *check = qobject_cast<QAbstractButton *>(widget)) check->setChecked(checked);
}

QString text_of(QWidget *widget) {
    if (auto *edit = qobject_cast<HotkeyEdit *>(widget)) return edit->text();
    if (auto *edit = qobject_cast<QLineEdit *>(widget)) return edit->text();
    if (auto *label = qobject_cast<QLabel *>(widget)) return label->text();
    return {};
}

void set_text(QWidget *widget, const QString &text) {
    if (auto *edit = qobject_cast<HotkeyEdit *>(widget)) edit->setText(text);
    else if (auto *line_edit = qobject_cast<QLineEdit *>(widget)) line_edit->setText(text);
    else if (auto *label = qobject_cast<QLabel *>(widget)) label->setText(text);
}

// A combo's data, a mouse binding editor's binding, or a switch state.
QVariant behavior_value(QWidget *control) {
    if (auto *editor = qobject_cast<MouseBindingEditor *>(control)) return editor->currentData();
    if (auto *combo = qobject_cast<QComboBox *>(control)) return combo->currentData();
    return checked_of(control);
}

void select_data(QComboBox *combo, const QVariant &value) {
    if (!combo) return;
    if (const int index = combo->findData(value); index >= 0) combo->setCurrentIndex(index);
}

QString hotkey_text(SettingsDialog *dialog, const QString &attr) { return text_of(dialog->attr_widget(attr)).trimmed(); }
}

QString settings_tr(const char *text) { return i18n_tr(text, "SettingsDialog"); }

void save_inapp_shortcut_edits(ToolSettingsManager *config_manager, const std::vector<std::pair<QString, InAppKeyEdit *>> &edits) {
    for (const auto &[cfg_key, edit] : edits) {
        const QString value = edit->text().trimmed();
        if (value.endsWith("+")) continue;
        config_manager->set_inapp_shortcut(cfg_key, is_reserved_inapp_shortcut(value) ? QString() : value);
    }
}

QSize settings_window_size(int available_width, int available_height, double factor) {
    const int width = std::min(1150, std::max(1050, static_cast<int>(1050 + (available_width / factor - 1920) * 100 / 640)));
    const int height = std::min(900, static_cast<int>(750 + (available_height / factor - 1032) * 150 / 360));
    return {std::min(static_cast<int>(python_round(width * factor)), available_width),
            std::min(static_cast<int>(python_round(height * factor)), available_height)};
}

SettingsDialog::SettingsDialog(ToolSettingsManager *manager, const QString &hotkey, QWidget *parent)
    : FrostedFramelessDialog(parent), config_manager(manager), current_hotkey(hotkey) {
    // The custom title bar comes before setWindowTitle so it receives the change.
    setup_titlebar();
    setWindowTitle("jietuba");
    fit_to_screen();
    // The same font stack as cards and navigation, so text styles without a family match.
    QFont window_font;
    window_font.setFamilies(FONT_FAMILIES());
    window_font.setPointSize(dialog_scaled(11));
    setFont(window_font);
    setObjectName("SettingsDialog");
    setup_ui();
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, [this] { on_ui_theme_changed(); });
}

void SettingsDialog::fit_to_screen() {
    if (isVisible()) return;
    auto *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect available = screen->availableGeometry();
    fit_to_available(available.size());
    // An explicit move() sets WA_Moved, after which show() no longer centres the dialog, so it is moved
    // only when no part of it is on a screen. Overlap, not the corner: a never-moved dialog sits at
    // 0,0, which is outside the available area when the taskbar is at the top or left.
    for (auto *candidate : QGuiApplication::screens())
        if (candidate->availableGeometry().intersects(frameGeometry())) return;
    move(available.x() + python_floordiv(available.width() - width(), 2),
         available.y() + python_floordiv(available.height() - height(), 2));
}

void SettingsDialog::fit_to_available(const QSize &available) {
    const bool user_sized = fitted_size_ && size() != *fitted_size_;
    const QSize target = user_sized ? size().boundedTo(available)
                                    : settings_window_size(available.width(), available.height(), get_dialog_scale().factor());
    if (target != size()) resize(target);
    if (!user_sized) fitted_size_ = size();
}

QWidget *SettingsDialog::behavior_control(const QString &key) const {
    for (const auto &[name, control] : behavior_controls)
        if (name == key) return control;
    return nullptr;
}

InAppKeyEdit *SettingsDialog::inapp_edit(const QString &key) const {
    for (const auto &[name, edit] : inapp_edits)
        if (name == key) return edit;
    return nullptr;
}

void SettingsDialog::setup_titlebar() {
    auto *title_bar = new FluentTitleBar(this);
    configure_dialog_control(*title_bar);
    setTitleBar(title_bar);
    title_bar->iconLabel->hide();
    title_bar->titleLabel->hide();
    title_bar->setFixedHeight(title_bar->buttonLayout->sizeHint().height());
    title_bar->hBoxLayout->setContentsMargins(0, 0, 0, 0);
    if (title_bar->maxBtn) title_bar->maxBtn->hide();
    title_bar->setDoubleClickEnabled(false);
    const QString icon_path = ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg"));
    if (QFileInfo::exists(icon_path)) setWindowIcon(QIcon(icon_path));
}

// ── UI ──

void SettingsDialog::setup_ui() {
    // Wide enough for the longest navigation item (Japanese "マウスショートカット" in bold when selected).
    const int sidebar_width = dialog_scaled(222);
    const int title_bar_height = titleBar() ? titleBar()->height() : dialog_scaled(32);

    auto *main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(dialog_scaled(10), title_bar_height + dialog_scaled(6), dialog_scaled(10), dialog_scaled(10));
    main_layout->setSpacing(dialog_scaled(10));

    // 1. Left panel.
    auto *left_panel = new QWidget;
    left_panel->setObjectName("SettingsLeftPanel");
    left_panel->setFixedWidth(sidebar_width);
    auto *left_v = new QVBoxLayout(left_panel);
    left_v->setContentsMargins(dialog_scaled(10), dialog_scaled(6), dialog_scaled(10), dialog_scaled(10));
    left_v->setSpacing(dialog_scaled(8));

    auto *logo_area = new QWidget;
    logo_area->setFixedHeight(dialog_scaled(78));
    auto *logo_layout = new QHBoxLayout(logo_area);
    logo_layout->setContentsMargins(dialog_scaled(12), dialog_scaled(10), dialog_scaled(12), dialog_scaled(10));
    logo_layout->setSpacing(dialog_scaled(10));
    logo_layout->setAlignment(Qt::AlignVCenter);
    auto *logo_icon_lbl = new QLabel;
    logo_icon_lbl->setFixedSize(dialog_scaled(36), dialog_scaled(36));
    logo_icon_lbl->setAlignment(Qt::AlignCenter);
    const QString icon_path = ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg"));
    if (QFileInfo::exists(icon_path)) logo_icon_lbl->setPixmap(QIcon(icon_path).pixmap(dialog_scaled(36), dialog_scaled(36)));
    logo_layout->addWidget(logo_icon_lbl, 0, Qt::AlignVCenter);
    auto *text_box = new QVBoxLayout;
    text_box->setContentsMargins(0, 0, 0, 0);
    text_box->setSpacing(dialog_scaled(2));
    auto *app_name_lbl = new BodyLabel(tr_text("jietuba"));
    apply_theme_text_style(app_name_lbl, 16, true);
    auto *app_desc_lbl = new QLabel(tr_text("Settings"));
    apply_theme_text_style(app_desc_lbl, 13, false, {}, true);
    text_box->addWidget(app_name_lbl);
    text_box->addWidget(app_desc_lbl);
    logo_layout->addLayout(text_box, 1);
    left_v->addWidget(logo_area);
    // The logo's context menu opens developer options.
    logo_area->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(logo_area, &QWidget::customContextMenuRequested, this, [this, logo_area](const QPoint &pos) { show_logo_context_menu(logo_area->mapTo(this, pos)); });

    nav_list_ = create_navigation(left_panel);
    left_v->addWidget(nav_list_, 1);
    main_layout->addWidget(left_panel);

    // 2. Right content area.
    right_area_ = new QWidget;
    right_area_->setObjectName("SettingsRightArea");
    auto *right_layout = new QVBoxLayout(right_area_);
    right_layout->setContentsMargins(dialog_scaled(24), dialog_scaled(16), dialog_scaled(24), dialog_scaled(18));
    right_layout->setSpacing(dialog_scaled(14));
    content_title_ = new QLabel(tr_text("Shortcut Settings"));
    apply_theme_text_style(content_title_, 21, true, QString("padding: 0 %1px %2px %1px;").arg(dialog_scaled(6)).arg(dialog_scaled(2)));

    // Pages are built on first visit (ensure_page): building all takes over a second and
    // would stall opening the window. Placeholders keep the indices.
    content_stack = new QStackedWidget;
    for (int i = 0; i < PAGE_COUNT; ++i) content_stack->addWidget(new QWidget);
    ensure_page(0);

    right_layout->addWidget(content_title_);
    right_layout->addWidget(content_stack);
    right_layout->setStretchFactor(content_stack, 1);
    right_layout->addLayout(create_button_area());
    main_layout->addWidget(right_area_, 1);

    apply_dialog_stylesheet();
    set_current_nav("shortcuts");
    // The snapshot also decides whether Apply is enabled; later pages join it in ensure_page.
    settings_snapshot_ = snapshot_settings();
    update_action_buttons();
}

NavigationInterface *SettingsDialog::create_navigation(QWidget *parent) {
    auto *nav = new NavigationInterface(parent);
    nav->setObjectName("SettingsNavigation");
    nav->setExpandWidth(dialog_scaled(198));
    nav->setMinimumExpandWidth(0);
    nav->expand();
    nav->setMinimumWidth(dialog_scaled(198));
    nav->setMaximumWidth(dialog_scaled(206));

    struct NavItem {
        QString route_key;
        IconSource icon;
        const char *text;
        int stack_index;
        NavigationItemPosition position;
    };
    std::vector<NavItem> items{
        {"shortcuts", ResourceManager::get_icon_path(QString::fromUtf8("热键.svg")), "Shortcuts", 0, NavigationItemPosition::TOP},
        {"mouse", ResourceManager::get_icon_path(QString::fromUtf8("鼠标线框.svg")), "Mouse Shortcuts", 10, NavigationItemPosition::TOP},
        {"quick_actions", FluentIcon::StopWatch, "Quick Actions", 9, NavigationItemPosition::TOP},
        {"capture", FluentIcon::Camera, "Capture Settings", 1, NavigationItemPosition::TOP},
        {"pin", FluentIcon::Pin, "Pin", 11, NavigationItemPosition::TOP},
        {"screen_annotation", ResourceManager::get_icon_path(QString::fromUtf8("画笔.svg")), "Screen Annotation", 12, NavigationItemPosition::TOP},
        {"clipboard", FluentIcon::Paste, "Clipboard", 2, NavigationItemPosition::TOP},
        {"translation", FluentIcon::Language, "Translation", 4, NavigationItemPosition::TOP},
        {"appearance", FluentIcon::Brush, "Appearance", 3, NavigationItemPosition::TOP},
        {"log", FluentIcon::History, "Log Settings", 5, NavigationItemPosition::TOP},
        {"other", FluentIcon::Application, "Other", 6, NavigationItemPosition::TOP},
        {"about", FluentIcon::Info, "About", 8, NavigationItemPosition::BOTTOM},
    };
    // Input methods / features / the application itself, with a thin line between groups.
    const QSet<QString> group_starts{"capture", "appearance"};
    const QMap<QString, QString> feature_tones{{"capture", "capture"}, {"pin", "pin"}, {"screen_annotation", "capture"}, {"clipboard", "clipboard"}, {"translation", "translate"}};
    for (const auto &item : items) {
        if (group_starts.contains(item.route_key)) nav->addSeparator();
        const int stack_index = item.stack_index;
        const QString route_key = item.route_key;
        nav->addItem(route_key, item.icon, tr_text(item.text), [this, stack_index, route_key] { on_nav_changed(stack_index, route_key); }, item.position, {},
                     feature_tones.value(route_key));
    }
    return nav;
}

void SettingsDialog::set_current_nav(const QString &route_key) {
    if (nav_list_) nav_list_->setCurrentItem(route_key);
}

void SettingsDialog::ensure_page(int index) {
    if (built_pages_.count(index) || !PAGE_BUILDERS[index]) return;
    built_pages_.insert(index);
    QWidget *page = PAGE_BUILDERS[index](this);
    // Removing the current page makes the next placeholder current, and an inserted page does
    // not take its place, so the index is restored by hand.
    const bool was_current = content_stack->currentIndex() == index;
    QWidget *placeholder = content_stack->widget(index);
    content_stack->removeWidget(placeholder);
    placeholder->deleteLater();
    content_stack->insertWidget(index, page);
    if (was_current) content_stack->setCurrentIndex(index);
    configure_dialog_controls(*page);
    disable_wheel_on_value_controls(page);
    connect_action_button_tracking(page);
    if (settings_snapshot_) {
        const auto current = snapshot_settings();
        for (auto it = current.begin(); it != current.end(); ++it)
            if (!settings_snapshot_->contains(it.key())) settings_snapshot_->insert(it.key(), it.value());
    }
}

void SettingsDialog::build_all_pages() {
    for (int index = 0; index < PAGE_COUNT; ++index) ensure_page(index);
}

// ── Navigation and developer entry ──

void SettingsDialog::on_nav_changed(int stack_index, const QString &route_key) {
    static const QMap<int, const char *> title_map{
        {0, "Shortcut Settings"}, {1, "Capture Settings"}, {2, "Clipboard Settings"}, {3, "Appearance Settings"},
        {4, "Translation Settings"}, {5, "Log Settings"}, {6, "Other Settings"}, {8, "Software Information"},
        {9, "Quick Actions"}, {10, "Mouse Shortcuts"}, {11, "Pin Settings"}, {12, "Screen Annotation Settings"},
    };
    if (!title_map.contains(stack_index)) return;
    ensure_page(stack_index);
    // The hotkey may have been edited on the shortcut page since.
    if (stack_index == 12) refresh_screen_annotation_hotkey();
    content_title_->setText(tr_text(title_map.value(stack_index)));
    content_stack->setCurrentIndex(stack_index);
    if (!route_key.isEmpty()) set_current_nav(route_key);
    refresh_after_page_change();
}

void SettingsDialog::refresh_clipboard_size(int delay_ms) {
    if (!has_attr("_clipboard_size_label")) return;
    if (delay_ms > 0) {
        QTimer::singleShot(delay_ms, this, [this] { refresh_clipboard_size(); });
        return;
    }
    const QString size = clipboard_storage_size_text();
    attr<QLabel>("_clipboard_size_label")->setText(size.isEmpty() ? QString::fromUtf8("—") : size);
}

void SettingsDialog::show_logo_context_menu(const QPoint &pos) {
    QMenu menu(this);
    menu.setStyleSheet(theme_menu_style());
    auto *action_dev = menu.addAction(tr_text("Developer Options"));
    if (menu.exec(mapToGlobal(pos)) == action_dev) open_developer_page();
}

void SettingsDialog::open_developer_page() {
    ensure_page(7);
    content_stack->setCurrentIndex(7);
    content_title_->setText(tr_text("Developer Options"));
    nav_list_->clearCurrentItem();
    refresh_after_page_change();
}

void SettingsDialog::refresh_after_page_change() {
    update();
    right_area_->update();
    content_stack->update();
    if (auto *current = content_stack->currentWidget()) current->update();
}

// ── Files and folders ──

void SettingsDialog::change_save_dir() {
    const QString new_dir = get_existing_directory(this, tr_text("Select Screenshot Save Folder"), config_manager->get_screenshot_save_path());
    if (!new_dir.isEmpty()) set_text(attr_widget("save_path_lbl"), new_dir);
}

void SettingsDialog::open_save_dir() {
    const QString path = config_manager->get_screenshot_save_path();
    QDir().mkpath(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void SettingsDialog::change_log_dir() {
    const QString new_dir = get_existing_directory(this, tr_text("Select Log Save Folder"), config_manager->get_log_dir());
    if (!new_dir.isEmpty()) set_text(attr_widget("path_lbl"), new_dir);
}

void SettingsDialog::open_log_dir() {
    const QString path = config_manager->get_log_dir();
    QDir().mkpath(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// ── Footer ──

QLayout *SettingsDialog::create_button_area() {
    auto *layout = new QHBoxLayout;
    layout->setSpacing(dialog_scaled(10));
    // The footer is outside content_stack, so its controls opt in to dialog scaling themselves.
    auto *reset_btn = new TransparentPushButton(tr_text("Reset This Page"));
    configure_dialog_control(*reset_btn);
    reset_btn->setCursor(Qt::PointingHandCursor);
    reset_btn->setIcon(FluentIcon::Sync);
    connect(reset_btn, &QPushButton::clicked, this, &SettingsDialog::reset_current_page);
    connect(reset_btn, &QPushButton::clicked, this, &SettingsDialog::update_action_buttons);

    // Separate from the unsaved-changes prompt's Cancel ("stay in settings"): this one closes
    // the window, so it has its own source text and translation.
    auto *cancel_btn = new PushButton(tr_text("Cancel / Close"));
    configure_dialog_control(*cancel_btn);
    footer_cancel_btn_ = cancel_btn;
    cancel_btn->setFixedHeight(dialog_scaled(42));
    cancel_btn->setMinimumWidth(dialog_scaled(120));
    cancel_btn->setCursor(Qt::PointingHandCursor);
    connect(cancel_btn, &QPushButton::clicked, this, &QDialog::reject);

    auto *ok_btn = new PrimaryPushButton(tr_text("Apply"));
    configure_dialog_control(*ok_btn);
    footer_ok_btn_ = ok_btn;
    ok_btn->setFixedHeight(dialog_scaled(42));
    ok_btn->setMinimumWidth(dialog_scaled(120));
    ok_btn->setCursor(Qt::PointingHandCursor);
    ok_btn->setBaseIconSize(16);
    connect(ok_btn, &QPushButton::clicked, this, [this] { apply_settings(); });
    apply_footer_styles();

    layout->addWidget(reset_btn);
    layout->addStretch();
    layout->addWidget(cancel_btn);
    layout->addWidget(ok_btn);
    return layout;
}

void SettingsDialog::apply_footer_styles() {
    const auto &t = get_ui_theme()->tokens();
    if (footer_cancel_btn_)
        footer_cancel_btn_->setStyleSheet(QString(R"(
            QPushButton {
                min-height: %1px;
                padding: 0 %2px;
                color: %3;
                background: %4;
                border: 1px solid %5;
                border-radius: %6px;
                font-size: %7px;
                font-weight: 500;
                outline: none;
            }
            QPushButton:hover { background: %8; }
            QPushButton:pressed { background: %9; }
        )")
                                              .arg(dialog_scaled(40))
                                              .arg(dialog_scaled(22))
                                              .arg(t.text, t.surface, t.border_hover)
                                              .arg(dialog_scaled(12))
                                              .arg(dialog_scaled(15))
                                              .arg(t.surface_hover, t.surface_subtle));
    if (footer_ok_btn_) {
        // The check mark asset is white; the disabled light background takes one in the text colour.
        QIcon check;
        check.addPixmap(fluent_icon(FluentIcon::Check).pixmap(64, 64), QIcon::Normal);
        check.addPixmap(tinted_icon(FluentIcon::Check, t.text_disabled).pixmap(64, 64), QIcon::Disabled);
        footer_ok_btn_->setIcon(check);
        footer_ok_btn_->setStyleSheet(QString(R"(
            QPushButton {
                min-height: %1px;
                padding: 0 %2px;
                color: #FFFFFF;
                background: %3;
                border: 1px solid %3;
                border-radius: %4px;
                font-size: %5px;
                font-weight: 600;
                outline: none;
            }
            QPushButton:hover { background: %6; border-color: %6; }
            QPushButton:pressed { background: %7; border-color: %7; }
            QPushButton:disabled { color: %8; background: %9; border-color: %10; }
        )")
                                          .arg(dialog_scaled(40))
                                          .arg(dialog_scaled(22))
                                          .arg(t.accent_strong)
                                          .arg(dialog_scaled(12))
                                          .arg(dialog_scaled(15))
                                          .arg(t.accent_strong_hover, t.accent_strong_pressed, t.text_disabled, t.surface_subtle, t.border));
    }
}

void SettingsDialog::connect_action_button_tracking(QWidget *page) {
    static const QMetaMethod slot = SettingsDialog::staticMetaObject.method(SettingsDialog::staticMetaObject.indexOfMethod("update_action_buttons()"));
    for (auto *widget : page->findChildren<QWidget *>())
        for (const auto &signal : tracked_signals(widget->metaObject())) connect(widget, signal, this, slot);
}

void SettingsDialog::update_action_buttons() {
    if (!footer_ok_btn_) return;
    const bool dirty = has_unsaved_changes();
    footer_ok_btn_->setEnabled(dirty);
    footer_ok_btn_->setCursor(dirty ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

// ── Reset ──

void SettingsDialog::apply_startup() {
    auto *autostart_toggle = attr_widget("autostart_toggle");
    const bool autostart = checked_of(autostart_toggle);
    const bool was_admin = config_manager->get_app_setting("run_as_admin").toBool();
    auto *admin_toggle = attr_widget("run_as_admin_toggle");
    config_manager->set_app_setting("autostart_enabled", autostart);
    if (!admin_toggle) {
        // Without the card (an account that cannot elevate) admin mode is left as it is.
        elevation::apply_startup(autostart, was_admin, was_admin);
        return;
    }
    const bool wanted = autostart && checked_of(admin_toggle);
    const bool admin = elevation::apply_startup(autostart, wanted, was_admin, true);
    if (admin != wanted) {
        set_checked(admin_toggle, admin);
        if (admin) {
            // The task stayed, so it still starts at sign-in.
            set_checked(autostart_toggle, true);
            config_manager->set_app_setting("autostart_enabled", true);
        }
        show_warning_dialog(this, tr_text("Run as Administrator"),
                            admin ? tr_text("Run as administrator could not be turned off. It still starts as administrator at sign-in.")
                                  : tr_text("Run as administrator was not turned on."));
    }
    config_manager->set_app_setting("run_as_admin", admin);
    // After this apply finishes.
    if (admin && !was_admin && elevation::elevation_state() != elevation::ELEVATED)
        QTimer::singleShot(0, this, &SettingsDialog::admin_mode_enabled);
}

void SettingsDialog::save_magnifier_color_formats() {
    // Edited in the manager window and kept on the instance; Apply saves them.
    if (magnifier_color_formats) color_formats::save(config_manager, *magnifier_color_formats);
}

void SettingsDialog::save_special_paste_entries() {
    if (clipboard_special_paste_entries) special_paste::save(config_manager, *clipboard_special_paste_entries);
}

void SettingsDialog::reset_current_page() {
    switch (content_stack->currentIndex()) {
    case 0: reset_hotkey_page(); break;
    case 1: reset_screenshot_settings_page(); break;
    case 2: reset_clipboard_page(); break;
    case 3: reset_appearance_page(); break;
    case 4: reset_translation_page(); break;
    case 5: reset_log_page(); break;
    case 6: reset_misc_page(); break;
    case 7: reset_long_screenshot_page(); break;
    case 9: reset_quick_actions_page(); break;
    case 10: reset_mouse_page(); break;
    case 11: reset_pin_page(); break;
    case 12: reset_screen_annotation_page(); break;
    default: break;
    }
}

void SettingsDialog::reset_pin_page() {
    const auto &defaults = config_manager->app_defaults();
    refresh_behavior_controls(true, {}, PIN_BEHAVIOR_KEYS);
    if (auto *toggle = attr_widget("pin_auto_toolbar_toggle")) set_checked(toggle, defaults.value("pin_auto_toolbar").toBool());
    if (auto *toggle = attr_widget("ocr_enable_toggle"); toggle && toggle->isEnabled()) set_checked(toggle, defaults.value("ocr_enabled").toBool());
}

void SettingsDialog::show_pin_shortcuts(bool mouse) {
    on_nav_changed(mouse ? 10 : 0, mouse ? "mouse" : "shortcuts");
    SegmentedWidget *tabs = mouse ? mouse_inapp_tabs.data() : inapp_shortcut_tabs.data();
    if (!tabs) return;
    tabs->setCurrentItem("pin");
    if (auto *scroll = qobject_cast<QScrollArea *>(content_stack->currentWidget())) {
        // The page may be new; scroll after layout so the tab row and its section title reach the top.
        QPointer<QScrollArea> guarded_scroll(scroll);
        QPointer<SegmentedWidget> guarded_tabs(tabs);
        QTimer::singleShot(0, this, [guarded_scroll, guarded_tabs] {
            if (guarded_scroll && guarded_tabs)
                guarded_scroll->verticalScrollBar()->setValue(guarded_tabs->mapTo(guarded_scroll->widget(), QPoint(0, 0)).y() - dialog_scaled(60));
        });
    }
}

void SettingsDialog::reset_screen_annotation_page() { refresh_behavior_controls(true, {}, SCREEN_ANNOTATION_BEHAVIOR_KEYS); }

void SettingsDialog::show_hotkey_row(const QString &attr) {
    on_nav_changed(0, "shortcuts");
    auto *scroll = qobject_cast<QScrollArea *>(content_stack->currentWidget());
    QPointer<QWidget> edit(attr_widget(attr));
    if (!scroll || !edit) return;
    // The page may be new; scroll after layout.
    QPointer<QScrollArea> guarded_scroll(scroll);
    QTimer::singleShot(0, this, [guarded_scroll, edit] {
        if (guarded_scroll && edit) guarded_scroll->ensureWidgetVisible(edit, 0, dialog_scaled(80));
    });
}

void SettingsDialog::refresh_screen_annotation_hotkey() {
    auto *card = attr<SettingCard>("screen_annotation_hotkey_card");
    if (!card) return;
    QStringList values;
    bool editors = false;
    for (const auto &name : global_hotkey_edit_attrs("screen_annotation_hotkey_edit")) {
        auto *edit = attr<HotkeyEdit>(name);
        if (!edit) continue;
        editors = true;
        values.append(edit->text().trimmed());
    }
    if (!editors) values = config_manager->get_global_hotkeys("screen_annotation");
    QStringList shown;
    for (const auto &value : values) {
        if (value.isEmpty()) continue;
        QStringList keys = value.split('+');
        for (auto &key : keys) key = key.left(1).toUpper() + key.mid(1);
        shown.append(keys.join(" + "));
    }
    card->setContent(shown.isEmpty() ? tr_text("Not set. Screen annotation also starts from the tray menu.")
                                     : tr_text("Currently %1").arg(shown.join(QString::fromUtf8("  /  "))));
}

void SettingsDialog::reset_mouse_page() {
    refresh_behavior_controls(true, "mouse_");
    refresh_behavior_controls(true, "quick_capture_");
}

void SettingsDialog::reset_hotkey_page() {
    const auto &defaults = config_manager->app_defaults();
    for (const auto &[function, edits] : global_hotkey_edit_rows(this)) {
        QString default_name;
        for (const auto &entry : GLOBAL_HOTKEY_FUNCTIONS())
            if (function == QLatin1String(entry.function)) default_name = entry.default_name;
        for (int slot = 0; slot < static_cast<int>(edits.size()); ++slot)
            if (edits[slot]) edits[slot]->setText(defaults.value(default_name + global_hotkey_slot_suffix(slot)).toString());
    }
    // In-app shortcuts.
    for (const auto &[cfg_key, edit] : inapp_edits) edit->setText(cfg_key == "inapp_pin_reset_size" ? QString() : defaults.value(cfg_key, "").toString());
    select_data(attr<QComboBox>("cursor_move_combo"), defaults.value("inapp_cursor_move_mode"));
    select_data(attr<QComboBox>("clipboard_pick_combo"), defaults.value("inapp_clipboard_pick_mode"));
}

// Resets the developer options page.
void SettingsDialog::reset_long_screenshot_page() {
    const auto &defaults = config_manager->app_defaults();
    select_data(attr<QComboBox>("engine_combo"), defaults.value("long_stitch_engine"));
    if (auto *spin = attr<QSpinBox>("ignore_top_pixels_spinbox")) spin->setValue(defaults.value("long_stitch_ignore_top_pixels").toInt());
    for (const char *key : {"preload_screenshot", "preload_toolbar", "preload_ocr", "preload_settings", "preload_clipboard"})
        if (auto *toggle = attr_widget(QString(key) + "_toggle")) set_checked(toggle, defaults.value(key).toBool());
    if (auto *toggle = attr_widget("info_hide_on_drag_toggle")) set_checked(toggle, defaults.value("screenshot_info_hide_on_drag").toBool());
}

void SettingsDialog::reset_appearance_page() {
    const auto &defaults = config_manager->app_defaults();
    select_data(attr<QComboBox>("_ui_theme_combo"), defaults.value("ui_theme_mode", "system"));
    select_data(attr<QComboBox>("_ui_scale_combo"), defaults.value("ui_scale_percent", 100));
    select_data(attr<QComboBox>("_dialog_scale_combo"), defaults.value("dialog_scale_percent", 100));
    if (appearance_theme_color) {
        appearance_theme_color = QColor(defaults.value("theme_color").toString());
        update_color_btn(attr_widget("_theme_color_btn"), *appearance_theme_color);
    }
    if (appearance_mask_color) {
        appearance_mask_color = QColor(defaults.value("mask_color_r").toInt(), defaults.value("mask_color_g").toInt(), defaults.value("mask_color_b").toInt());
        update_color_btn(attr_widget("_mask_color_btn"), *appearance_mask_color);
    }
    select_data(attr<QComboBox>("_selection_border_combo"), defaults.value("selection_border_width"));
    select_data(attr<QComboBox>("_selection_handle_combo"), defaults.value("selection_handle_style"));
    select_data(attr<QComboBox>("_selection_handle_size_combo"), defaults.value("selection_handle_size"));
    select_data(attr<QComboBox>("_color_picker_style_combo"), defaults.value("color_picker_style"));
}

void SettingsDialog::reset_quick_actions_page() {
    refresh_behavior_controls(true, {}, std::set<QString>{"capture_fullscreen_crosshair", "capture_include_cursor"});
    const auto &defaults = config_manager->app_defaults();
    for (const auto &[attr_name, key] : {std::make_pair("ocr_copy_directly_toggle", "ocr_copy_directly"), std::make_pair("barcode_copy_single_toggle", "barcode_copy_single"),
                                         std::make_pair("cross_tool_selection_toggle", "cross_tool_selection"), std::make_pair("text_always_on_top_toggle", "text_always_on_top")})
        if (auto *toggle = attr_widget(attr_name)) set_checked(toggle, defaults.value(key).toBool());
    if (auto *combo = attr<QComboBox>("shift_line_angle_combo")) combo->setCurrentIndex(SHIFT_LINE_ANGLES.indexOf(defaults.value("shift_line_angle").toInt()));
}

void SettingsDialog::reset_screenshot_settings_page() {
    const auto &defaults = config_manager->app_defaults();
    if (auto *combo = attr<QComboBox>("capture_engine_combo")) combo->setCurrentIndex(CAPTURE_ENGINE_CHOICES.indexOf(defaults.value("capture_engine").toString()));
    if (auto *combo = attr<QComboBox>("smart_mode_combo"))
        combo->setCurrentIndex(SMART_SELECTION_MODES.indexOf(defaults.value("smart_selection").toBool() ? defaults.value("smart_selection_mode").toString() : QString("off")));
    for (const auto &[attr_name, key] : {std::make_pair("smart_animation_toggle", "smart_selection_animation"), std::make_pair("save_toggle", "screenshot_save_enabled"),
                                         std::make_pair("screenshot_pin_save_toggle", "screenshot_pin_save_enabled"),
                                         std::make_pair("screenshot_save_original_toggle", "screenshot_save_original_enabled"),
                                         std::make_pair("clipboard_file_reference_toggle", "clipboard_file_reference_enabled"),
                                         std::make_pair("watermark_auto_toggle", "screenshot_watermark_auto"), std::make_pair("magnifier_enabled_toggle", "magnifier_enabled"),
                                         std::make_pair("magnifier_grid_toggle", "magnifier_grid"), std::make_pair("magnifier_hint_toggle", "magnifier_hint")})
        if (auto *toggle = attr_widget(attr_name)) set_checked(toggle, defaults.value(key).toBool());
    if (auto *label = attr_widget("save_path_lbl")) set_text(label, defaults.value("screenshot_save_path").toString());
    if (auto *combo = attr<QComboBox>("screenshot_format_combo"))
        combo->setCurrentIndex(std::max<int>(0, QStringList{"PNG", "JPG", "BMP", "WEBP", "PDF"}.indexOf(defaults.value("screenshot_format").toString().toUpper())));
    select_data(attr<QComboBox>("save_dialog_style_combo"), defaults.value("save_dialog_style"));
    if (magnifier_color_formats) magnifier_color_formats = color_formats::default_formats();
    fill_ocr_engines(this, defaults.value("ocr_engine").toString(), defaults.value("ppocr_model").toString());
    select_data(attr<QComboBox>("ocr_result_position_combo"), defaults.value("ocr_result_position"));
}

void SettingsDialog::reset_log_page() {
    const auto &defaults = config_manager->app_defaults();
    if (auto *toggle = attr_widget("log_toggle")) set_checked(toggle, defaults.value("log_enabled").toBool());
    if (auto *combo = attr<QComboBox>("log_level_combo")) combo->setCurrentText(defaults.value("log_level").toString());
    if (auto *combo = attr<QComboBox>("log_retention_combo")) {
        int index = combo->findData(defaults.value("log_retention_days"));
        if (index < 0) index = combo->findData(7);
        if (index >= 0) combo->setCurrentIndex(index);
    }
    if (auto *label = attr_widget("path_lbl")) set_text(label, defaults.value("log_dir").toString());
}

void SettingsDialog::reset_misc_page() {
    const auto &defaults = config_manager->app_defaults();
    if (auto *toggle = attr_widget("autostart_toggle")) set_checked(toggle, defaults.value("autostart_enabled").toBool());
    if (auto *toggle = attr_widget("run_as_admin_toggle")) set_checked(toggle, defaults.value("run_as_admin").toBool());
    if (auto *toggle = attr_widget("show_main_window_toggle")) set_checked(toggle, defaults.value("show_main_window").toBool());
}

void SettingsDialog::reset_translation_page() {
    const auto &defaults = config_manager->app_defaults();
    select_data(attr<QComboBox>("translation_provider_combo"), defaults.value("translation_provider"));
    // Every provider's credentials and switches reset from the registry's declarations.
    if (provider_field_widgets) provider_fields::reset_to_defaults(defaults, provider_fields::all_fields(*translation_registry), *provider_field_widgets);
    select_data(attr<QComboBox>("translation_target_combo"), defaults.value("translation_target_lang"));
    if (auto *toggle = attr_widget("split_sentences_toggle")) set_checked(toggle, defaults.value("translation_split_sentences").toBool());
    if (auto *toggle = attr_widget("preserve_formatting_toggle")) set_checked(toggle, defaults.value("translation_preserve_formatting").toBool());
}

void SettingsDialog::reset_clipboard_page() {
    const auto &defaults = config_manager->app_defaults();
    if (auto *toggle = attr_widget("clipboard_enabled_toggle")) set_checked(toggle, defaults.value("clipboard_enabled").toBool());
    if (auto *toggle = attr_widget("clipboard_auto_paste_toggle")) set_checked(toggle, defaults.value("clipboard_auto_paste").toBool());
    if (auto *spin = attr<QSpinBox>("clipboard_history_limit_spin")) spin->setValue(defaults.value("clipboard_history_limit").toInt());
    if (clipboard_special_paste_entries) clipboard_special_paste_entries = special_paste::default_entries();
}

// ── Apply ──

bool SettingsDialog::apply_settings(bool close_after) {
    dialog_scale_changed_on_accept_ = false;
    language_changed_on_apply_ = false;
    close_after_apply_ = close_after;

    // Every global hotkey passes validation first, before any set_*, so conflicting values
    // are never written and the window stays open.
    if (!validate_global_hotkey_edits(this, true)) {
        content_stack->setCurrentIndex(0);
        set_current_nav("shortcuts");
        show_warning_dialog(this, tr_text("Shortcut Conflict"), tr_text("Some global hotkeys are duplicated or unavailable. Please fix them before applying."));
        return false;
    }
    const auto mouse_conflicts = mouse_binding_conflicts(this);
    if (!mouse_conflicts.empty()) {
        on_nav_changed(10, "mouse");
        show_warning_dialog(this, tr_text("Shortcut Conflict"), mouse_binding_conflict_message(this, mouse_conflicts));
        return false;
    }

    // Saving may rebuild windows (a language switch); that must not trigger the unsaved prompt.
    skip_unsaved_close_prompt_ = true;

    // 0. Hotkeys.
    for (const auto &[function, edits] : global_hotkey_edit_rows(this)) {
        if (std::find(edits.begin(), edits.end(), nullptr) != edits.end()) continue;
        QStringList values;
        for (auto *edit : edits) values.append(edit->text().trimmed());
        config_manager->set_global_hotkeys(function, values);
    }

    // 1. Behaviour switches and capture interaction (smart selection).
    for (const auto &[key, control] : behavior_controls)
        if (control) config_manager->set_app_setting(key, behavior_value(control));
    if (auto *capture_copy = qobject_cast<MouseBindingEditor *>(behavior_control("mouse_capture_copy")))
        config_manager->set_double_click_copy_close_enabled(capture_copy->currentData() == "doubleleft");
    if (auto *toggle = attr_widget("ocr_copy_directly_toggle")) config_manager->set_ocr_copy_directly_enabled(checked_of(toggle));
    if (auto *toggle = attr_widget("barcode_copy_single_toggle")) config_manager->set_barcode_copy_single_enabled(checked_of(toggle));
    if (auto *toggle = attr_widget("cross_tool_selection_toggle")) config_manager->set_cross_tool_selection_enabled(checked_of(toggle));
    if (auto *combo = attr<QComboBox>("shift_line_angle_combo")) config_manager->set_shift_line_angle(combo->currentData().toInt());
    if (auto *toggle = attr_widget("text_always_on_top_toggle")) config_manager->set_text_always_on_top_enabled(checked_of(toggle));
    if (auto *combo = attr<QComboBox>("smart_mode_combo")) config_manager->set_smart_selection_mode(combo->currentData().toString());
    if (auto *toggle = attr_widget("smart_animation_toggle")) config_manager->set_smart_selection_animation(checked_of(toggle));

    // 2. Logging.
    if (auto *log_toggle = attr_widget("log_toggle")) {
        const bool log_enabled = checked_of(log_toggle);
        config_manager->set_log_enabled(log_enabled);
        if (auto *combo = attr<QComboBox>("log_level_combo")) {
            const QString log_level = combo->currentText();
            config_manager->set_log_level(log_level);
            static const QMap<QString, int> level_map{{"DEBUG", LogLevel::Debug}, {"INFO", LogLevel::Info}, {"WARNING", LogLevel::Warning}, {"ERROR", LogLevel::Error}};
            if (level_map.contains(log_level)) {
                get_logger().set_level(level_map.value(log_level));
                get_logger().set_console_level(level_map.value(log_level));
            }
        }
        if (auto *combo = attr<QComboBox>("log_retention_combo")) {
            const qint64 retention_days = combo->currentData().toLongLong();
            const qint64 old_retention = config_manager->get_log_retention_days();
            config_manager->set_log_retention_days(retention_days);
            if (retention_days > 0 && retention_days < old_retention) cleanup_old_logs(config_manager->get_log_dir(), retention_days);
        }
        if (auto *path_lbl = attr_widget("path_lbl")) {
            const QString old_log_dir = config_manager->get_log_dir();
            const QString new_log_dir = text_of(path_lbl);
            config_manager->set_log_dir(new_log_dir);
            get_logger().set_enabled(log_enabled);
            if (new_log_dir != old_log_dir) {
                get_logger().set_log_dir(new_log_dir);
                show_info_dialog(this, tr_text("Log"), tr_text("Log save location changed.") + "\n" + tr_text("*Changes will fully take effect after restart."));
            }
            refresh_latest_log_label();
        }
    }

    // 3. Saving.
    if (auto *toggle = attr_widget("save_toggle")) config_manager->set_screenshot_save_enabled(checked_of(toggle));
    if (auto *toggle = attr_widget("screenshot_pin_save_toggle")) config_manager->set_screenshot_pin_save_enabled(checked_of(toggle));
    if (auto *toggle = attr_widget("screenshot_save_original_toggle")) config_manager->set_screenshot_save_original_enabled(checked_of(toggle));
    if (auto *toggle = attr_widget("clipboard_file_reference_toggle")) config_manager->set_clipboard_file_reference_enabled(checked_of(toggle));
    if (auto *toggle = attr_widget("watermark_auto_toggle")) config_manager->set_screenshot_watermark_auto(checked_of(toggle));
    if (auto *label = attr_widget("save_path_lbl")) config_manager->set_screenshot_save_path(text_of(label));
    if (auto *combo = attr<QComboBox>("screenshot_format_combo")) config_manager->set_screenshot_format(combo->currentData().toString());
    if (auto *combo = attr<QComboBox>("save_dialog_style_combo")) config_manager->set_app_setting("save_dialog_style", combo->currentData());

    // 3.5 Magnifier.
    for (const auto &[attr_name, key] : {std::make_pair("magnifier_enabled_toggle", "magnifier_enabled"), std::make_pair("magnifier_grid_toggle", "magnifier_grid"),
                                         std::make_pair("magnifier_hint_toggle", "magnifier_hint")})
        if (auto *toggle = attr_widget(attr_name)) config_manager->set_app_setting(key, checked_of(toggle));

    // 4. OCR.
    if (auto *toggle = attr_widget("ocr_enable_toggle")) config_manager->set_ocr_enabled(checked_of(toggle));
    if (auto *combo = attr<QComboBox>("ocr_engine_combo")) {
        const auto [engine, model] = ocr_choice(combo->currentData().toString());
        if (model && *model != config_manager->get_ppocr_model()) {
            config_manager->set_ppocr_model(*model);
            set_ppocr_model(*model);
        }
        if (engine != config_manager->get_ocr_engine()) {
            config_manager->set_ocr_engine(engine);
            set_ocr_engine(engine);
            request_trim_working_set();
        }
    }
    if (auto *combo = attr<QComboBox>("ocr_result_position_combo")) config_manager->set_ocr_result_position(combo->currentData().toString());

    // 5. Translation. Every provider's credentials and switches are saved from the registry's declarations.
    if (auto *combo = attr<QComboBox>("translation_provider_combo")) config_manager->set_translation_provider(combo->currentData().toString());
    if (provider_field_widgets) provider_fields::save_from(config_manager, provider_fields::all_fields(*translation_registry), *provider_field_widgets);
    if (auto *combo = attr<QComboBox>("translation_target_combo")) config_manager->set_translation_target_lang(combo->currentData().toString());
    if (auto *toggle = attr_widget("split_sentences_toggle")) config_manager->set_translation_split_sentences(checked_of(toggle));
    if (auto *toggle = attr_widget("preserve_formatting_toggle")) config_manager->set_translation_preserve_formatting(checked_of(toggle));

    // 6. Miscellaneous.
    if (attr_widget("autostart_toggle")) apply_startup();
    if (auto *toggle = attr_widget("show_main_window_toggle")) config_manager->set_show_main_window(checked_of(toggle));
    if (auto *toggle = attr_widget("pin_auto_toolbar_toggle")) config_manager->set_pin_auto_toolbar(checked_of(toggle));
    save_magnifier_color_formats();

    // Interface language.
    if (auto *combo = attr<QComboBox>("language_combo")) {
        const QString new_lang = combo->currentData().toString();
        const QString old_lang = config_manager->get_app_setting("language", "ja").toString();
        config_manager->qsettings.setValue("app/language", new_lang);
        if (new_lang != old_lang) {
            language_changed_on_apply_ = true;
            I18nManager::load_language(new_lang);
        }
    }

    // 7. Clipboard.
    save_special_paste_entries();
    if (auto *toggle = attr_widget("clipboard_enabled_toggle")) config_manager->set_clipboard_enabled(checked_of(toggle));
    if (auto *spin = attr<QSpinBox>("clipboard_history_limit_spin")) config_manager->set_clipboard_history_limit(spin->value());

    // 7.5 In-app shortcuts.
    if (!inapp_edits.empty()) {
        save_inapp_shortcut_edits(config_manager, inapp_edits);
        // The pin shortcut handlers reload their bindings.
        try {
            if (auto *pin_ctrl = PinShortcutController::existing()) pin_ctrl->reload_bindings();
        } catch (const std::exception &e) {
            log_exception(e, T("重载 Pin 快捷键绑定"));
        }
    }
    if (auto *combo = attr<QComboBox>("cursor_move_combo")) config_manager->set_inapp_cursor_move_mode(combo->currentData().toString());
    if (auto *combo = attr<QComboBox>("clipboard_pick_combo")) config_manager->set_inapp_clipboard_pick_mode(combo->currentData().toString());

    // 8. Long screenshot / developer.
    if (auto *combo = attr<QComboBox>("capture_engine_combo")) config_manager->set_capture_engine(combo->currentData().toString());
    if (auto *combo = attr<QComboBox>("engine_combo")) config_manager->set_long_stitch_engine(combo->currentData().toString());
    if (auto *spin = attr<QSpinBox>("ignore_top_pixels_spinbox")) config_manager->set_long_stitch_ignore_top_pixels(spin->value());

    // 9. Preload switches (effective after restart).
    for (const char *key : {"preload_screenshot", "preload_toolbar", "preload_ocr", "preload_settings", "preload_clipboard"})
        if (auto *toggle = attr_widget(QString(key) + "_toggle")) config_manager->set_app_setting(key, checked_of(toggle));

    // 10. Selection info panel behaviour.
    if (auto *toggle = attr_widget("info_hide_on_drag_toggle")) config_manager->set_app_setting("screenshot_info_hide_on_drag", checked_of(toggle));

    // 10.5 Clipboard appearance.
    apply_clipboard_appearance();

    // 11. Appearance (theme colour, mask colour, selection look, scales).
    if (auto *combo = attr<QComboBox>("_ui_theme_combo")) get_ui_theme()->set_mode(combo->currentData());
    // Built toolbars and panels recompute on scale_changed.
    if (auto *combo = attr<QComboBox>("_ui_scale_combo")) get_ui_scale().set_percent(combo->currentData());
    // The window scale applies to standalone windows created afterwards, at their own base
    // sizes; this window is not stretched after the fact, which would squeeze fixed-height
    // controls against laid-out content.
    if (auto *combo = attr<QComboBox>("_dialog_scale_combo")) dialog_scale_changed_on_accept_ = get_dialog_scale().set_percent(combo->currentData());
    auto &theme = get_theme();
    if (appearance_theme_color) theme.set_theme_color(*appearance_theme_color);
    if (appearance_mask_color) theme.set_mask_color(*appearance_mask_color);
    if (auto *combo = attr<QComboBox>("_selection_border_combo")) theme.set_selection_border_width(combo->currentData());
    if (auto *combo = attr<QComboBox>("_selection_handle_combo")) theme.set_selection_handle_style(combo->currentData().toString());
    if (auto *combo = attr<QComboBox>("_selection_handle_size_combo")) theme.set_selection_handle_size(combo->currentData().toString());
    if (auto *combo = attr<QComboBox>("_color_picker_style_combo")) config_manager->set_app_setting("color_picker_style", combo->currentData());

    for (const auto &[key, layout] : imported_toolbar_layouts_) config_manager->set_app_setting(key, layout);

    log_info(T("所有设置已保存"), "Settings");
    import_unsaved_ = false;
    imported_toolbar_layouts_.clear();
    settings_snapshot_ = snapshot_settings();
    update_action_buttons();
    config_manager->qsettings.sync();
    skip_unsaved_close_prompt_ = true;
    emit settings_applied();
    skip_unsaved_close_prompt_ = false;
    return true;
}

void SettingsDialog::accept() {
    if (apply_settings(true)) FrostedFramelessDialog::accept();
}

QString SettingsDialog::get_hotkey() const { return hotkey_text(const_cast<SettingsDialog *>(this), "hotkey_input"); }

void SettingsDialog::update_hotkey(const QString &new_hotkey) { set_text(attr_widget("hotkey_input"), new_hotkey); }

void SettingsDialog::refresh_latest_log_label() { jietuba::refresh_latest_log_label(this); }

void SettingsDialog::apply_dialog_stylesheet() {
    const auto &tokens = get_ui_theme()->tokens();
    const QString sheet = QString(R"(
            #SettingsDialog {
                background: transparent;
                border: none;
            }
            QWidget#SettingsLeftPanel {
                /* The sidebar sits on the window background, not as a block of its own. */
                background-color: transparent;
                border: none;
                border-radius: 12px;
            }
            QWidget#SettingsRightArea {
                background: %1;
                border: 1px solid %2;
                border-radius: 12px;
            }
            QLabel {
                color: %3;
            }
            QStackedWidget {
                background: transparent;
            }
        )")
                              .arg(theme_surface_color(), tokens.separator, tokens.text) +
                          scrollbar_qss(this) + R"(
            QScrollBar::handle:vertical:disabled { background: transparent; }
        )";
    // Runs on every show; an unchanged sheet is not set again, which would restyle every control.
    if (sheet != styleSheet()) setStyleSheet(sheet);
}

void SettingsDialog::on_ui_theme_changed() {
    apply_dialog_stylesheet();
    refresh_theme_widget_styles(this);
    apply_footer_styles();
    content_title_->setStyleSheet(theme_text_style(21, true, "padding: 0 6px 2px 6px;"));
    update();
}

void SettingsDialog::resizeEvent(QResizeEvent *event) {
    FrostedFramelessDialog::resizeEvent(event);
    if (titleBar()) titleBar()->resize(width(), titleBar()->height());
}

void SettingsDialog::showEvent(QShowEvent *event) {
    skip_unsaved_close_prompt_ = false;
    apply_dialog_stylesheet();
    refresh_settings();
    settings_snapshot_ = snapshot_settings();
    update_action_buttons();
    FrostedFramelessDialog::showEvent(event);
    apply_taskbar_icon();
}

void SettingsDialog::hideEvent(QHideEvent *event) {
    // Title bar buttons go back to their normal state when hidden.
    if (titleBar())
        for (auto *button : titleBar()->findChildren<TitleBarButton *>()) {
            button->setDown(false);
            button->update();
        }
    FrostedFramelessDialog::hideEvent(event);
}

// ── Unsaved changes ──

void SettingsDialog::update_snapshot(const QString &name, const QVariant &value) {
    if (settings_snapshot_ && settings_snapshot_->contains(name)) (*settings_snapshot_)[name] = value;
}

QVariantMap SettingsDialog::snapshot_settings() const {
    QVariantMap snap;
    for (const auto &[key, control] : behavior_controls)
        if (control) snap[key] = behavior_value(control);
    // Text.
    for (const auto &name : GLOBAL_HOTKEY_EDIT_ATTRS + QStringList{"save_path_lbl", "path_lbl"})
        if (auto *widget = attr_widget(name)) snap[name] = text_of(widget);
    // Translation credentials by declaration, so every provider's edits count as unsaved.
    if (provider_field_widgets)
        for (const auto &f : provider_fields::all_fields(*translation_registry))
            if (QWidget *widget = provider_field_widgets->value(f.config_key)) snap[f.config_key] = provider_fields::widget_value(f, widget);
    // Switches.
    for (const char *name : {"ocr_copy_directly_toggle", "barcode_copy_single_toggle", "cross_tool_selection_toggle", "text_always_on_top_toggle", "smart_animation_toggle",
                             "save_toggle", "screenshot_pin_save_toggle", "clipboard_file_reference_toggle", "screenshot_save_original_toggle", "watermark_auto_toggle",
                             "magnifier_enabled_toggle", "magnifier_grid_toggle", "magnifier_hint_toggle", "ocr_enable_toggle", "ocr_grayscale_toggle",
                             "ocr_upscale_toggle", "split_sentences_toggle", "preserve_formatting_toggle", "log_toggle", "clipboard_enabled_toggle",
                             "clipboard_auto_paste_toggle", "autostart_toggle", "run_as_admin_toggle", "show_main_window_toggle", "pin_auto_toolbar_toggle", "info_hide_on_drag_toggle",
                             "preload_screenshot_toggle", "preload_toolbar_toggle", "preload_ocr_toggle", "preload_settings_toggle", "preload_clipboard_toggle"})
        if (auto *widget = attr_widget(name)) snap[name] = checked_of(widget);
    // Combos.
    for (const char *name : {"screenshot_format_combo", "save_dialog_style_combo", "shift_line_angle_combo", "ocr_result_position_combo", "translation_provider_combo", "translation_target_combo",
                             "log_level_combo", "language_combo", "engine_combo", "cursor_move_combo", "clipboard_pick_combo", "log_retention_combo", "_ui_theme_combo",
                             "_ui_scale_combo", "_dialog_scale_combo", "_selection_border_combo", "_selection_handle_combo", "_selection_handle_size_combo",
                             "_color_picker_style_combo", "smart_mode_combo", "capture_engine_combo"})
        if (auto *combo = attr<QComboBox>(name)) snap[name] = combo->currentIndex();
    // The OCR list reorders as models are downloaded or deleted, so it compares by the item's value.
    if (auto *combo = attr<QComboBox>("ocr_engine_combo")) snap["ocr_engine_combo"] = combo->currentData();
    // Numbers.
    for (const char *name : {"clipboard_history_limit_spin", "ignore_top_pixels_spinbox", "ocr_scale_spinbox"}) {
        if (auto *spin = attr<QSpinBox>(name)) snap[name] = spin->value();
        else if (auto *double_spin = attr<QDoubleSpinBox>(name)) snap[name] = double_spin->value();
    }
    // In-app shortcuts.
    for (const auto &[cfg_key, edit] : inapp_edits) snap["inapp_" + cfg_key] = edit->text();
    // Colours.
    if (appearance_theme_color) snap["theme_color"] = appearance_theme_color->name();
    if (appearance_mask_color) snap["mask_color"] = appearance_mask_color->name();
    // Colour formats are edited in their own window, with no control to read.
    if (magnifier_color_formats) {
        QVariantList formats;
        for (const auto &fmt : *magnifier_color_formats) formats.append(QVariantList{fmt.name, fmt.template_string, fmt.enabled});
        snap["magnifier_color_formats"] = formats;
    }
    if (clipboard_special_paste_entries) {
        QVariantList entries;
        for (const auto &[key, enabled] : *clipboard_special_paste_entries) entries.append(QVariant(QVariantList{key, enabled}));
        snap["clipboard_special_paste_entries"] = entries;
    }
    for (const char *name : {"_clip_font_combo", "_clip_opacity_combo"})
        if (auto *combo = attr<QComboBox>(name)) snap[name] = combo->currentData();
    if (clip_theme_name) snap["_clip_theme_name"] = *clip_theme_name;
    return snap;
}

bool SettingsDialog::has_unsaved_changes() const {
    if (import_unsaved_) return true;
    if (!settings_snapshot_) return false;
    return snapshot_settings() != *settings_snapshot_;
}

QString SettingsDialog::confirm_close_with_unsaved_changes() {
    const std::vector<DialogButton> buttons{
        {"save", tr_text("Save"), QDialogButtonBox::AcceptRole},
        {"discard", tr_text("Don't Save"), QDialogButtonBox::DestructiveRole},
        {"cancel", tr_text("Cancel"), QDialogButtonBox::RejectRole, true},
    };
    return show_custom_confirm_dialog(this, tr_text("Unsaved Changes"), tr_text("You have unsaved changes. Do you want to save before closing?"), buttons).toString();
}

void SettingsDialog::closeEvent(QCloseEvent *event) {
    event->ignore();
    if (skip_unsaved_close_prompt_) {
        event->accept();
        return;
    }
    if (!has_unsaved_changes()) {
        event->accept();
        return;
    }
    const QString action = confirm_close_with_unsaved_changes();
    if (action == "save") {
        if (apply_settings(true)) event->accept();
        else event->ignore();
    } else if (action == "discard") {
        event->accept();
    } else {
        event->ignore();
    }
}

void SettingsDialog::apply_taskbar_icon() {
    const QString icon_path = ResourceManager::get_resource_path(QString::fromUtf8("svg/托盘.svg"));
    if (!QFileInfo::exists(icon_path)) return;
    QPixmap pix(32, 32);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    QIcon(icon_path).paint(&painter, 0, 0, 32, 32);
    painter.end();
    // The previous icon is released once the window holds the new one.
    static HICON current = nullptr;
    HICON hicon = pix.toImage().toHICON();
    if (!hicon) return;
    const auto hwnd = reinterpret_cast<HWND>(winId());
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hicon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hicon));
    if (current) DestroyIcon(current);
    current = hicon;
}

// ── Refresh ──

void SettingsDialog::refresh_behavior_controls(bool defaults, const QString &prefix, const std::optional<std::set<QString>> &keys) {
    for (const auto &[key, control] : behavior_controls) {
        if (!control) continue;
        if (keys && !keys->count(key)) continue;
        if (!keys && !key.startsWith(prefix)) continue;
        const QVariant default_value = config_manager->app_defaults().value(key);
        QVariant value;
        if (defaults) value = default_value;
        else if (key.startsWith("mouse_capture_")) value = get_capture_mouse_binding(config_manager, key.mid(QString("mouse_capture_").size()));
        else if (key.startsWith("mouse_pin_")) value = get_pin_mouse_binding(config_manager, key.mid(QString("mouse_pin_").size()));
        else value = config_manager->get_app_setting(key, default_value);
        if (auto *editor = qobject_cast<MouseBindingEditor *>(control)) {
            editor->setBinding(value.toString());
        } else if (auto *combo = qobject_cast<QComboBox *>(control)) {
            const int index = combo->findData(value);
            combo->setCurrentIndex(index >= 0 ? index : combo->findData(default_value));
        } else {
            set_checked(control, value.toBool());
        }
    }
}

void SettingsDialog::refresh_settings() {
    import_unsaved_ = false;
    imported_toolbar_layouts_.clear();
    refresh_behavior_controls();
    refresh_screen_annotation_hotkey();
    for (const auto &[function, edits] : global_hotkey_edit_rows(this)) {
        if (std::all_of(edits.begin(), edits.end(), [](HotkeyEdit *edit) { return !edit; })) continue;
        const auto values = config_manager->get_global_hotkeys(function);
        for (int slot = 0; slot < static_cast<int>(edits.size()); ++slot)
            if (edits[slot]) edits[slot]->setText(values[slot]);
    }

    // In-app shortcuts.
    for (const auto &[cfg_key, edit] : inapp_edits) {
        QString value = config_manager->get_inapp_shortcut(cfg_key);
        if (cfg_key == "inapp_pin_reset_size" && is_inapp_mouse_shortcut(value)) value = "";
        edit->setText(is_reserved_inapp_shortcut(value) ? QString() : value);
    }
    select_data(attr<QComboBox>("cursor_move_combo"), config_manager->get_inapp_cursor_move_mode());
    select_data(attr<QComboBox>("clipboard_pick_combo"), config_manager->get_inapp_clipboard_pick_mode());
    if (auto *combo = attr<QComboBox>("capture_engine_combo")) combo->setCurrentIndex(CAPTURE_ENGINE_CHOICES.indexOf(config_manager->get_capture_engine()));

    select_data(attr<QComboBox>("engine_combo"), config_manager->get_long_stitch_engine());
    if (auto *spin = attr<QSpinBox>("ignore_top_pixels_spinbox")) spin->setValue(static_cast<int>(config_manager->get_long_stitch_ignore_top_pixels()));
    if (auto *combo = attr<QComboBox>("smart_mode_combo")) combo->setCurrentIndex(SMART_SELECTION_MODES.indexOf(config_manager->get_smart_selection_mode()));
    if (auto *toggle = attr_widget("smart_animation_toggle")) set_checked(toggle, config_manager->get_smart_selection_animation());
    if (auto *toggle = attr_widget("ocr_copy_directly_toggle")) set_checked(toggle, config_manager->get_ocr_copy_directly_enabled());
    if (auto *toggle = attr_widget("barcode_copy_single_toggle")) set_checked(toggle, config_manager->get_barcode_copy_single_enabled());
    if (auto *toggle = attr_widget("cross_tool_selection_toggle")) set_checked(toggle, config_manager->get_cross_tool_selection_enabled());
    if (auto *combo = attr<QComboBox>("shift_line_angle_combo")) combo->setCurrentIndex(SHIFT_LINE_ANGLES.indexOf(config_manager->get_shift_line_angle()));
    if (auto *toggle = attr_widget("text_always_on_top_toggle")) set_checked(toggle, config_manager->get_text_always_on_top_enabled());

    if (auto *toggle = attr_widget("save_toggle")) set_checked(toggle, config_manager->get_screenshot_save_enabled());
    if (auto *toggle = attr_widget("screenshot_pin_save_toggle")) set_checked(toggle, config_manager->get_screenshot_pin_save_enabled());
    if (auto *toggle = attr_widget("screenshot_save_original_toggle")) set_checked(toggle, config_manager->get_screenshot_save_original_enabled());
    if (auto *toggle = attr_widget("clipboard_file_reference_toggle")) set_checked(toggle, config_manager->get_clipboard_file_reference_enabled());
    if (auto *toggle = attr_widget("watermark_auto_toggle")) set_checked(toggle, config_manager->get_screenshot_watermark_auto());
    if (auto *label = attr_widget("save_path_lbl")) set_text(label, config_manager->get_screenshot_save_path());
    if (auto *combo = attr<QComboBox>("screenshot_format_combo"))
        combo->setCurrentIndex(std::max<int>(0, QStringList{"PNG", "JPG", "BMP", "WEBP", "PDF"}.indexOf(config_manager->get_screenshot_format().toUpper())));
    select_data(attr<QComboBox>("save_dialog_style_combo"), config_manager->get_app_setting("save_dialog_style"));
    for (const auto &[attr_name, key] : {std::make_pair("magnifier_enabled_toggle", "magnifier_enabled"), std::make_pair("magnifier_grid_toggle", "magnifier_grid"),
                                         std::make_pair("magnifier_hint_toggle", "magnifier_hint")})
        if (auto *toggle = attr_widget(attr_name)) set_checked(toggle, config_manager->get_app_setting(key).toBool());
    if (magnifier_color_formats) magnifier_color_formats = color_formats::load(config_manager);
    if (auto *toggle = attr_widget("ocr_enable_toggle")) set_checked(toggle, config_manager->get_ocr_enabled());
    fill_ocr_engines(this, config_manager->get_ocr_engine(), config_manager->get_ppocr_model());
    select_data(attr<QComboBox>("ocr_result_position_combo"), config_manager->get_ocr_result_position());

    select_data(attr<QComboBox>("translation_provider_combo"), config_manager->get_translation_provider());
    // Every provider's credentials and switches reload from the declarations.
    if (provider_field_widgets) provider_fields::load_into(config_manager, provider_fields::all_fields(*translation_registry), *provider_field_widgets);
    select_data(attr<QComboBox>("translation_target_combo"), config_manager->get_app_setting("translation_target_lang", ""));
    if (auto *toggle = attr_widget("split_sentences_toggle")) set_checked(toggle, config_manager->get_translation_split_sentences());
    if (auto *toggle = attr_widget("preserve_formatting_toggle")) set_checked(toggle, config_manager->get_translation_preserve_formatting());

    if (auto *toggle = attr_widget("log_toggle")) set_checked(toggle, config_manager->get_log_enabled());
    if (auto *combo = attr<QComboBox>("log_level_combo")) combo->setCurrentText(config_manager->get_log_level());
    if (auto *combo = attr<QComboBox>("log_retention_combo")) {
        int index = combo->findData(config_manager->get_log_retention_days());
        if (index < 0) index = combo->findData(7);
        if (index >= 0) combo->setCurrentIndex(index);
    }
    if (auto *label = attr_widget("path_lbl")) set_text(label, config_manager->get_log_dir());

    if (auto *toggle = attr_widget("clipboard_enabled_toggle")) set_checked(toggle, config_manager->get_clipboard_enabled());
    if (auto *toggle = attr_widget("clipboard_auto_paste_toggle")) set_checked(toggle, config_manager->get_clipboard_auto_paste());
    if (auto *spin = attr<QSpinBox>("clipboard_history_limit_spin")) spin->setValue(static_cast<int>(config_manager->get_clipboard_history_limit()));
    if (clipboard_special_paste_entries) clipboard_special_paste_entries = special_paste::load(config_manager);

    if (auto *toggle = attr_widget("autostart_toggle")) set_checked(toggle, config_manager->get_app_setting("autostart_enabled").toBool());
    if (auto *toggle = attr_widget("run_as_admin_toggle")) set_checked(toggle, config_manager->get_app_setting("run_as_admin").toBool());
    if (auto *toggle = attr_widget("show_main_window_toggle")) set_checked(toggle, config_manager->get_show_main_window());
    if (auto *toggle = attr_widget("pin_auto_toolbar_toggle")) set_checked(toggle, config_manager->get_pin_auto_toolbar());
    select_data(attr<QComboBox>("language_combo"), config_manager->get_app_setting("language", "ja"));

    // Preload switches (defaults from the app defaults).
    for (const char *key : {"preload_screenshot", "preload_toolbar", "preload_ocr", "preload_settings", "preload_clipboard"})
        if (auto *toggle = attr_widget(QString(key) + "_toggle")) set_checked(toggle, config_manager->get_app_setting(key).toBool());
    if (auto *toggle = attr_widget("info_hide_on_drag_toggle")) set_checked(toggle, config_manager->get_app_setting("screenshot_info_hide_on_drag").toBool());

    // Clipboard appearance: also shows what was changed in the clipboard window itself.
    if (auto *button = attr_widget("_clip_theme_btn")) {
        clip_theme_name = config_manager->get_clipboard_theme();
        apply_clip_theme_btn_style(button, *clip_theme_name);
    }
    if (auto *combo = attr<QComboBox>("_clip_font_combo"); combo && combo->findData(config_manager->get_clipboard_font_size()) >= 0)
        combo->setCurrentIndex(combo->findData(config_manager->get_clipboard_font_size()));
    if (auto *combo = attr<QComboBox>("_clip_opacity_combo"); combo && combo->findData(config_manager->get_clipboard_window_opacity()) >= 0)
        combo->setCurrentIndex(combo->findData(config_manager->get_clipboard_window_opacity()));
    select_data(attr<QComboBox>("_color_picker_style_combo"), config_manager->get_app_setting("color_picker_style"));

    refresh_appearance_controls(&SettingsDialog::runtime_appearance);
}

QVariant SettingsDialog::runtime_appearance(const QString &key) {
    if (key == "ui_theme_mode") return ui_theme_mode_value(get_ui_theme()->mode());
    if (key == "ui_scale_percent") return get_ui_scale().percent();
    if (key == "dialog_scale_percent") return get_dialog_scale().percent();
    auto &theme = get_theme();
    if (key == "theme_color") return theme.theme_color();
    if (key == "mask_color") return theme.mask_color();
    if (key == "selection_border_width") return theme.selection_border_width();
    if (key == "selection_handle_style") return theme.selection_handle_style();
    if (key == "selection_handle_size") return theme.selection_handle_size();
    return {};
}

std::function<QVariant(const QString &)> SettingsDialog::configured_appearance(ToolSettingsManager *config) {
    return [config](const QString &key) -> QVariant {
        if (key == "ui_theme_mode") return ui_theme_mode_value(coerce_ui_theme_mode(config->get_app_setting(key, "system")));
        if (key == "ui_scale_percent" || key == "dialog_scale_percent") return UIScaleManager::normalize_percent(config->get_app_setting(key));
        if (key == "theme_color") {
            const QColor color(config->get_app_setting(key).toString());
            return color.isValid() ? color : QColor(config->app_defaults().value(key).toString());
        }
        if (key == "mask_color")
            return QColor(config->get_app_setting("mask_color_r").toInt(), config->get_app_setting("mask_color_g").toInt(), config->get_app_setting("mask_color_b").toInt());
        if (key == "selection_border_width") return ThemeManager::clamp_border_width(config->get_app_setting(key));
        return config->get_app_setting(key);
    };
}

void SettingsDialog::refresh_appearance_controls(const std::function<QVariant(const QString &)> &value_of) {
    for (const auto &[attr_name, key] : {std::make_pair("_ui_theme_combo", "ui_theme_mode"), std::make_pair("_ui_scale_combo", "ui_scale_percent"),
                                         std::make_pair("_dialog_scale_combo", "dialog_scale_percent"), std::make_pair("_selection_border_combo", "selection_border_width"),
                                         std::make_pair("_selection_handle_combo", "selection_handle_style"),
                                         std::make_pair("_selection_handle_size_combo", "selection_handle_size")})
        select_data(attr<QComboBox>(attr_name), value_of(key));
    if (has_attr("_theme_color_btn")) {
        appearance_theme_color = value_of("theme_color").value<QColor>();
        const QColor mc = value_of("mask_color").value<QColor>();
        appearance_mask_color = QColor(mc.red(), mc.green(), mc.blue());
        update_color_btn(attr_widget("_theme_color_btn"), *appearance_theme_color);
        update_color_btn(attr_widget("_mask_color_btn"), *appearance_mask_color);
    }
}

void SettingsDialog::apply_clipboard_appearance() {
    auto *config = config_manager;
    auto *clip_theme = clipboard::get_theme_manager();
    if (clip_theme_name) {
        config->set_clipboard_theme(*clip_theme_name);
        if (clip_theme->get_current_theme().name != *clip_theme_name) clip_theme->set_theme(*clip_theme_name);
    }
    if (auto *combo = attr<QComboBox>("_clip_font_combo"); combo && combo->currentData().isValid()) {
        const qint64 size = combo->currentData().toLongLong();
        if (size != config->get_clipboard_font_size()) {
            config->set_clipboard_font_size(size);
            clip_theme->notify_font_size_changed(static_cast<int>(size));
        }
    }
    if (auto *combo = attr<QComboBox>("_clip_opacity_combo"); combo && combo->currentData().isValid()) {
        const qint64 percent = combo->currentData().toLongLong();
        if (percent != config->get_clipboard_window_opacity()) {
            config->set_clipboard_window_opacity(percent);
            clip_theme->notify_opacity_changed(static_cast<int>(percent));
        }
    }
}

// ── Import ──

void SettingsDialog::load_imported_settings(const QVariantMap &values) {
    auto *config = config_manager;
    // Pages not yet built would later be built from the real configuration and miss the import.
    build_all_pages();
    {
        QTemporaryDir tmp;
        auto preview = settings_transfer::preview_config(config, values, tmp.filePath("import.ini"));
        config_manager = preview.config.get();
        try {
            refresh_settings();
            refresh_appearance_controls(configured_appearance(preview.config.get()));
        } catch (...) {
            config_manager = config;
            throw;
        }
        config_manager = config;
    }
    // Absent from the file, or not text (edited by hand): the current layout stays.
    imported_toolbar_layouts_.clear();
    for (const auto &key : toolbar_layout::setting_keys()) {
        const QVariant layout = values.value(ToolSettingsManager::app_setting_path(key));
        if (layout.typeId() == QMetaType::QString) imported_toolbar_layouts_.emplace_back(key, layout.toString());
    }
    import_unsaved_ = true;
    update_action_buttons();
}

std::set<QString> SettingsDialog::settings_keys() {
    // Unbuilt pages read none of their keys, and export would miss them.
    build_all_pages();
    QSet<QString> keys;
    {
        settings_transfer::RecordingReads recording(config_manager);
        refresh_settings();
        // Appearance values normally come from their managers, not the configuration; refreshing
        // them from the configuration once reads their keys too.
        refresh_appearance_controls(configured_appearance(config_manager));
        keys = recording.keys;
    }
    std::set<QString> result(keys.begin(), keys.end());
    // The toolbar layout is edited in the toolbar's own dialog and no page reads it, yet it is a customisation worth carrying.
    for (const auto &key : toolbar_layout::setting_keys()) result.insert(ToolSettingsManager::app_setting_path(key));
    return result;
}
}
