#pragma once
#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/controllers/selection_manager.h"
#include "clipboard/ui/mixins/frameless_mixin.h"
#include "clipboard/ui/theme/themes.h"
#include "clipboard/ui/widgets/quick_edit_popup.h"
#include "core/i18n.h"
#include "core/shortcut_manager.h"
#include <QAction>
#include <QDateEdit>
#include <QElapsedTimer>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <memory>
#include <optional>

class QMimeData;

namespace jietuba {
class LineEdit;
class ToolSettingsManager;
}

namespace jietuba::clipboard {
class ClipboardItemDelegate;
class ClipboardMouseController;
class ClipboardWindow;
class GroupBar;
class ManageDialog;

inline const QString QUICK_EDIT_SHORTCUT = "inapp_clipboard_quick_edit";

// Keys of the clipboard window (priority 60), while it is the active window.
class ClipboardShortcutHandler : public ShortcutHandler {
public:
    explicit ClipboardShortcutHandler(ClipboardWindow *window);
    // Re-reads the configurable keys; called each time the window shows.
    void reload_bindings();
    bool is_active() override;
    bool handle_key(QInputEvent *event) override;
    int priority() const override { return 60; }
    QString handler_name() const override { return "ClipboardWindow"; }
    QString pick_keys;

private:
    QPointer<ClipboardWindow> window_;
    QHash<QString, KeyBinding> bindings_;
};

// The direct pick character of a key: "1"-"9" or "a"-"z"; empty for other keys.
QString pick_char(int key);

// 将一条历史记录作为文件交给 Windows 拖放，不写入系统剪贴板。
std::unique_ptr<QMimeData> clipboard_drag_mime(ClipboardManager &manager, const ClipboardItem &item);

// The clipboard history window: the list, search and filters, the group bar and the gear menu.
class ClipboardWindow : public QWidget {
    Q_OBJECT
public:
    static QString tr_text(const char *text) { return i18n_tr(text, "ClipboardWindow"); }
    explicit ClipboardWindow(QWidget *parent = nullptr);
    ~ClipboardWindow() override;
    // Window geometry goes to this INI file instead of the registry; empty restores.
    static void set_geometry_file_for_test(const QString &ini_path);

    ClipboardManager &manager;
    ClipboardController *controller;
    ToolSettingsManager *config = nullptr;
    Theme current_theme;
    int window_opacity = 0;
    int font_size = 0;
    QString group_bar_position;

    QFrame *container = nullptr;
    QHBoxLayout *content_layout = nullptr;
    QWidget *left_widget = nullptr;
    QVBoxLayout *left_layout = nullptr;
    QListWidget *list_widget = nullptr;
    ClipboardItemDelegate *item_delegate = nullptr;
    SelectionManager *selection_manager = nullptr;
    ClipboardMouseController *mouse_controller = nullptr;
    QWidget *time_filter_bar = nullptr;
    QDateEdit *start_date_edit = nullptr;
    QLabel *time_filter_separator = nullptr;
    QDateEdit *end_date_edit = nullptr;
    QToolButton *apply_time_filter_btn = nullptr;
    QStringList type_filter_labels;
    int type_filter_index = 0;
    QToolButton *type_filter_btn = nullptr;
    QIcon type_filter_arrow_;
    QMenu *type_filter_menu = nullptr;
    QList<QAction *> type_filter_actions;
    QToolButton *clear_time_filter_btn = nullptr;
    QWidget *bottom_bar = nullptr;
    QToolButton *time_filter_toggle_btn = nullptr;
    LineEdit *search_input = nullptr;
    QPushButton *clear_search_btn = nullptr;
    QLabel *mark_count_label = nullptr;
    QPushButton *menu_btn = nullptr;
    GroupBar *group_bar = nullptr;
    QuickEditPopup *quick_edit_popup = nullptr;

    // Re-reads the settings the window keeps (after the settings window is accepted).
    void load_settings();
    // Reloads unless the window is hidden (the manage window and storage moves call this).
    void request_data_refresh(const QString &reason = {});
    void on_manage_data_changed();
    // Follows the toolbar and panel scale.
    void apply_scale();
    // The manage window's signals to this window's refreshes (unique connections).
    void connect_manage_dialog(ManageDialog *dialog);
    // The watcher's new item, queued from its thread.
    void notify_new_content(const std::optional<ClipboardItem> &item = std::nullopt);
    std::optional<ClipboardItem> get_item_data(qint64 item_id) const;

    // Actions behind keys, mouse gestures and menus.
    void on_paste_item(qint64 item_id);
    void paste_selected(bool plain_text = false);
    void create_pin_window(qint64 item_id);
    void quick_edit_item(qint64 item_id);
    void show_item_context_menu(qint64 item_id, const QPoint &pos);
    virtual void start_item_drag(qint64 item_id);
    void toggle_item_mark(qint64 item_id);
    void mark_items_to(qint64 item_id);
    // The quick edit key: text opens quick edit, other items in a group open the group editor.
    bool edit_selected();

signals:
    void item_pasted(qint64 item_id);
    void closed();
    void new_item_received(const std::optional<jietuba::clipboard::ClipboardItem> &item);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void on_data_loaded(const QList<ClipboardItem> &items, bool is_first_page);
    void on_item_inserted(const ClipboardItem &item, int row);
    void on_item_moved_to_top(qint64 item_id, int row);
    void on_item_removed(int row);
    void on_item_row_moved(int from_row, int to_row);
    void on_item_updated(int row, const ClipboardItem &item);
    void on_reload_required();
    void apply_scale_metrics();
    void on_theme_changed(const Theme &theme);
    void on_language_changed();
    void apply_theme();
    QLocale current_date_locale() const;
    void apply_date_filter_locale();
    void retranslate_ui();
    void apply_opacity();
    void load_window_geometry();
    void save_window_geometry();
    void setup_ui();
    QString menu_style() const;
    void show_main_menu();
    void toggle_time_filter_bar(bool checked);
    void apply_time_range_filter();
    void close_time_filter_bar();
    void clear_time_and_type_filters();
    void set_window_opacity_percent(int percent);
    void set_font_size(int size);
    void set_image_size(const QString &size);
    void set_display_lines(int lines);
    void on_font_size_changed(int size);
    void on_opacity_changed(int percent);
    void refresh_list();
    void on_load_completed();
    void check_and_load_more_if_needed();
    void append_items(const QList<ClipboardItem> &items);
    void on_scroll(int value);
    void on_search_changed();
    void on_selection_changed();
    void on_delegate_item_entered(QListWidgetItem *list_item);
    void set_highlighted_item(std::optional<qint64> item_id);
    void update_search_background(const QString &text);
    void apply_search_input_style(bool has_text = false);
    void apply_clear_search_btn_style();
    void apply_menu_btn_style();
    void apply_mark_count_style();
    void update_mark_count();
    void on_marks_changed();
    void apply_time_filter_styles();
    void clear_search();
    void update_type_filter_button(int index);
    void refresh_type_filter_button();
    void show_type_filter_menu();
    void set_type_filter(int index);
    void on_group_switched(std::optional<qint64> group_id);
    void on_sidebar_entered();
    void on_sidebar_exited(std::optional<qint64> prev_item_id);
    void enter_sidebar_mode();
    void on_top_group_switch(int delta);
    void on_add_group_clicked();
    void on_add_item_clicked();
    void on_clear_clicked();
    void set_group_bar_position(const QString &position);
    bool is_paste_picker_surface(Hwnd hwnd) const;
    ClipboardController::CloseCallback paste_close_callback();
    bool in_file_group();
    bool is_launch_item(qint64 item_id) const;
    void open_launch_item(qint64 item_id);
    void open_link(qint64 item_id);
    void paste_plain_text(qint64 item_id);
    void paste_marked(const QList<qint64> &item_ids, bool plain_text = false, const QString &layout = "vertical", bool explicit_paste = false);
    void show_context_menu(const QPoint &pos);
    void show_marked_context_menu(const QList<qint64> &item_ids, const QPoint &pos);
    void move_marked_to_group(const QList<qint64> &item_ids, std::optional<qint64> group_id);
    void delete_marked(const QList<qint64> &item_ids);
    QMap<QString, std::function<void()>> item_context_menu_handlers(qint64 item_id);
    std::function<void()> resolve_item_context_menu_handler(qint64 item_id, const QString &action_key);
    void special_paste(qint64 item_id, const QString &action_key);
    void file_special_paste(qint64 item_id, const QString &action_key);
    void edit_item(qint64 item_id);
    QPoint row_global_pos(qint64 item_id) const;
    void on_quick_edit_finished(const QuickEditResult &result);
    bool save_quick_edit(qint64 item_id, const QString &text);
    void delete_item(qint64 item_id);
    void open_file_item(qint64 item_id);
    void paste_item_to_clipboard(qint64 item_id);
    void open_file_location(qint64 item_id);
    void warmup_offscreen_once();
    void reveal_window();
    void position_at_cursor();
    bool owns_window(const QWidget *window) const;
    void check_and_hide();
    bool is_draggable_area(QObject *widget) const;
    bool is_date_filter_widget(QObject *obj) const;
    void apply_list_item_data(QListWidgetItem *list_item, const ClipboardItem &item) const;

    bool is_loading_ = false;
    // Refresh requests from the manage window while hidden are skipped; the next show reloads the list.
    bool ignore_manage_refresh_when_hidden_ = true;
    int auto_fill_max_pages_ = 3;
    int auto_fill_remaining_ = 3;
    bool offscreen_warmup_in_progress_ = false;
    bool dragging_ = false;
    inline static bool offscreen_warmup_done_ = false;
    FramelessMixin frameless_;
    std::unique_ptr<QSettings> qsettings_;
    std::optional<int> saved_x_, saved_y_;
    int saved_width_ = 450, saved_height_ = 600;
    QHBoxLayout *time_filter_layout_ = nullptr;
    QHBoxLayout *bottom_layout_ = nullptr;
    QPointer<QMenu> settings_menu_;
    std::optional<QElapsedTimer> menu_close_time_;
    QTimer *search_timer_ = nullptr;
    std::unique_ptr<ClipboardShortcutHandler> shortcut_handler_;
};

// Single and double clicks on clipboard rows, dispatched exclusively by the mouse settings.
class ClipboardMouseController : public QObject {
    Q_OBJECT
public:
    explicit ClipboardMouseController(ClipboardWindow *window);
    void cancel();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    struct Pressed {
        qint64 item_id;
        Qt::MouseButton button;
        Qt::KeyboardModifiers modifiers;
        QPoint global_pos;
    };
    struct Pending {
        QString action;
        qint64 item_id;
        QPoint position;
        Qt::MouseButton button;
        Qt::KeyboardModifiers modifiers;
    };
    bool available(const QString &action, qint64 item_id) const;
    QString match(QInputEvent *event, const QString &gesture, qint64 item_id) const;
    // Ctrl+left / Shift+left without a bound action mark items.
    std::function<void(qint64)> marking(QMouseEvent *event, const QString &gesture, qint64 item_id) const;
    void run(const QString &action, qint64 item_id, const QPoint &position);
    void finish_click();

    ClipboardWindow *window_;
    QListWidget *list_widget_;
    QWidget *viewport_;
    std::optional<Pressed> pressed_;
    std::optional<Pending> pending_;
    QTimer timer_;
};
}

Q_DECLARE_METATYPE(std::optional<jietuba::clipboard::ClipboardItem>)
