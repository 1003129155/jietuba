#pragma once
#include "clipboard/controllers/context_menu_controller.h"
#include "clipboard/controllers/foreground_tracker.h"
#include "clipboard/core/manager.h"
#include <QDateTime>
#include <QObject>
#include <functional>
#include <optional>

class QWidget;

namespace jietuba::clipboard {
class ManageDialog;

// How many group buttons fit in the side bar of this height (-1 before layout).
int calc_sidebar_capacity(int right_bar_height);
// How many group buttons fit in the top bar of this width (-1 before layout).
int calc_topbar_capacity(int bar_width);

// The clipboard window's logic: paging, search and filters, groups, item actions,
// paste target tracking and the data behind the context menus.
class ClipboardController : public QObject {
    Q_OBJECT
public:
    using CloseCallback = std::function<void()>;

    explicit ClipboardController(ClipboardManager &manager, QObject *parent = nullptr);

    ClipboardManager &manager;
    QList<ClipboardItem> current_items;
    // None shows the clipboard history.
    std::optional<qint64> current_group_id;
    bool auto_paste_enabled = true;
    bool paste_with_html = true;

    void load_settings();
    void set_auto_paste(bool enabled);
    void set_paste_with_html(bool enabled);
    void set_move_to_top_on_paste(bool enabled);

    // Reloads from the first page.
    void load_history();
    bool has_more_items() const { return has_more_ && !is_loading_; }
    void load_more_items();
    void load_more_if_needed();
    // Loads the next page when scrolled down near the end.
    void check_scroll_load(int scroll_value, int scroll_max);

    void set_search_text(const QString &text);
    // 0 all, 1 text, 2 image, 3 file.
    void set_content_type_filter(int filter_index);
    // "2026/06/20-2026/06/30"; false when the text is not a valid range.
    bool set_time_range_text(const QString &range_text);
    void clear_time_and_type_filters();
    // [start of first day, start of the day after the last).
    static std::optional<std::pair<QDateTime, QDateTime>> parse_time_range_text(const QString &range_text);

    // The content type of the selected group when it is a filter group.
    std::optional<QString> current_filter_type() const;
    // History and filter groups follow the history rules; real groups do not.
    bool shows_history() const;
    void switch_to_group(std::optional<qint64> group_id);
    // Asks first when parent_widget is given; tr translates in the caller's context.
    bool delete_group(qint64 group_id, QWidget *parent_widget = nullptr, const std::function<QString(const char *)> &tr = {});
    QList<Group> get_groups();
    std::pair<bool, bool> get_group_move_state(qint64 group_id);
    bool move_group_order(qint64 group_id, int direction);

    // explicit: the user chose Paste in a menu, so the auto-paste switch does not apply.
    bool paste_item(qint64 item_id, const CloseCallback &on_close_callback = {}, bool explicit_paste = false);
    // In selection order unless the order setting says last selected first.
    QList<qint64> multi_paste_order(const QList<qint64> &item_ids);
    // Whether the stitched image stays within the size limit; unknown sizes pass.
    bool stitched_image_fits(const QList<qint64> &item_ids, const QString &layout = "vertical");
    // Merged paste; stitching runs in the background, so true means started.
    bool paste_items(const QList<qint64> &item_ids, const CloseCallback &on_close_callback = {}, bool explicit_paste = false, bool plain_text = false,
                     const QString &layout = "vertical");
    void move_item_to_top(qint64 item_id);
    bool paste_transformed_text(qint64 item_id, const QString &transform_key, const CloseCallback &on_close_callback = {}, bool explicit_paste = false);
    bool paste_file_text(qint64 item_id, const QString &transform_key, const CloseCallback &on_close_callback = {}, bool explicit_paste = false);
    bool delete_item(qint64 item_id);
    void toggle_pin(qint64 item_id);
    bool move_to_group(qint64 item_id, std::optional<qint64> group_id);
    // Rewrites the body, keeps the title, refreshes the loaded row in place.
    bool update_item_content(qint64 item_id, const QString &content);
    int delete_items(const QList<qint64> &item_ids);
    int move_items_to_group(const QList<qint64> &item_ids, std::optional<qint64> group_id);
    std::optional<qint64> loaded_index(qint64 item_id) const;
    std::optional<ClipboardItem> loaded_item(qint64 item_id) const;
    std::pair<bool, bool> get_item_move_state(qint64 item_id, std::optional<qint64> group_id);
    bool move_item_order(qint64 item_id, std::optional<qint64> group_id, int direction);
    // connect_signals makes the caller's (unique) connections before the editor opens.
    ManageDialog *open_manage_dialog_for_group(qint64 group_id, const std::function<void(ManageDialog *)> &connect_signals = {});
    ManageDialog *open_manage_dialog_for_item(qint64 item_id, std::optional<qint64> group_id, const std::function<void(ManageDialog *)> &connect_signals = {});
    bool clear_history(QWidget *parent_widget = nullptr);
    std::optional<ClipboardItem> get_item(qint64 item_id);

    void set_paste_target_exclusion(std::function<bool(Hwnd)> predicate);
    void on_window_show();
    void on_window_hide();
    // item: the watcher's new record; without it the list is reloaded.
    void on_new_content(bool is_window_visible, const std::optional<ClipboardItem> &item = std::nullopt);
    // False when only a query can tell where the item belongs; the caller reloads.
    bool insert_item(const ClipboardItem &item);

    // (visible, hidden) groups for a side bar of this size; the current group stays visible.
    std::pair<QList<Group>, QList<Group>> get_sidebar_overflow(int bar_size, bool is_top = false);
    std::optional<ContextMenuData> build_context_menu_data(qint64 item_id);
    QList<MenuAction> build_group_context_menu_data(qint64 group_id);
    std::optional<ContextMenuData> build_marked_context_menu_data(const QList<qint64> &item_ids);

    ForegroundWindowTracker &foreground_tracker() { return foreground_tracker_; }

signals:
    void data_loaded(const QList<jietuba::clipboard::ClipboardItem> &items, bool is_first_page);
    void loading_state_changed(bool is_loading);
    void reload_required();
    // One load finished; is_loading is already false.
    void load_completed();
    void item_inserted(const jietuba::clipboard::ClipboardItem &item, int row);
    void item_moved_to_top(qint64 item_id, int row);
    void item_removed(int row);
    void item_row_moved(int from_row, int to_row);
    void item_updated(int row, const jietuba::clipboard::ClipboardItem &item);

private:
    int page_size() const;
    std::pair<QList<ClipboardItem>, int> fetch_items_page(int limit, qint64 offset);
    void send_paste_keystroke(bool explicit_paste);
    void on_multi_paste_finished(const QList<qint64> &used, bool move_to_top, bool explicit_paste);
    void move_items_to_top(const QList<qint64> &item_ids);
    void remove_loaded_item(qint64 item_id);
    void move_loaded_item(qint64 item_id, int direction);
    bool can_insert_incrementally() const;
    int first_unpinned_row() const;

    qint64 current_offset_ = 0;
    int page_size_ = 38;
    // More rows fit per page when time and source are hidden.
    int page_size_without_metadata_ = 65;
    bool is_loading_ = false;
    bool has_more_ = true;
    int last_scroll_value_ = 0;
    bool pending_reload_ = false;
    std::optional<QString> search_text_;
    std::optional<QString> content_type_;
    std::optional<std::pair<QDateTime, QDateTime>> time_range_;
    ForegroundWindowTracker foreground_tracker_;
    ClipboardContextMenuController context_menu_controller_;
};
}
