#pragma once
#include "clipboard/core/models.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

struct JbClipboard;

namespace jietuba::clipboard {
// Clipboard history over the native pyclipboard backend; one per process. Failures are
// logged and reported as empty results, as the reference wrapper does.
class ClipboardManager {
public:
    using Callback = std::function<void(const ClipboardItem &)>;

    static ClipboardManager &instance();
    ClipboardManager(const ClipboardManager &) = delete;
    ClipboardManager &operator=(const ClipboardManager &) = delete;

    bool is_available() const { return backend() != nullptr; }
    // Reopens the backend on db_path (empty: the saved path) and resumes monitoring.
    bool reset_storage(const QString &db_path = {});
    // Stops monitoring and closes the database before its files are moved.
    bool release_storage();
    std::optional<QString> get_db_path() const;
    std::optional<QString> get_images_dir() const;
    // Passes the history limit setting to the backend, which trims on insert.
    void apply_history_limit();

    // callback runs on the watcher thread for each recorded item.
    void start_monitoring(Callback callback = {});
    void stop_monitoring();
    // Drops the callback without stopping the watcher, for its owner going away.
    void clear_callback();
    bool is_monitoring() const;
    // Busy (false) while image encoding or storage writes are active.
    bool pause_for_update();
    void resume_after_update();
    size_t pending_writes() const;

    QList<ClipboardItem> get_history(qint64 offset = 0, qint64 limit = 50, const std::optional<QString> &search = std::nullopt, const FilterRule &filter = {},
                                     std::optional<qint64> start_time = std::nullopt, std::optional<qint64> end_time = std::nullopt);
    // Source apps in the history with their item counts, most first; this application's builds come as OWN_SOURCE_APP.
    QList<QPair<QString, qint64>> source_apps();
    qint64 get_total_count();
    QList<ClipboardItem> search(const QString &keyword, qint64 limit = 50);
    std::optional<ClipboardItem> get_item(qint64 item_id);
    bool delete_item(qint64 item_id);
    // keep_grouped keeps items that belong to a group.
    bool clear_history(bool keep_grouped = false);
    // Keeps pinned and grouped items. vacuum also shrinks the database file, which takes
    // time in proportion to what is kept; false only removes the rows.
    bool clear_unpinned_ungrouped(bool vacuum = true);
    std::optional<qint64> add_item(const QString &content, const QString &content_type = "text", const std::optional<QString> &title = std::nullopt);
    bool update_item(qint64 item_id, const QString &content, const std::optional<QString> &title = std::nullopt);
    // The new pinned state.
    bool toggle_pin(qint64 item_id);
    // Writes the item to the system clipboard and counts the paste.
    bool paste_item(qint64 item_id, bool with_html = true, bool move_to_top = false);
    // Writes the files a path item names to the clipboard; false when none of them exists.
    bool paste_as_files(qint64 item_id);
    // Merges several items into one clipboard write; the ids actually used, empty when nothing was written.
    QList<qint64> paste_items(const QList<qint64> &item_ids, bool with_html = true, bool move_to_top = false, const QString &separator = "\n",
                              bool plain_text = false, const QString &layout = "vertical", bool keep_in_history = false);
    std::optional<QByteArray> get_image_data(const QString &image_id);

    // filter_rule is required for a filter group and ignored for the others.
    std::optional<qint64> create_group(const QString &name, const std::optional<QString> &color = std::nullopt, const std::optional<QString> &icon = std::nullopt,
                                       int group_type = 0, const std::optional<FilterRule> &filter_rule = std::nullopt);
    QList<Group> get_groups();
    bool delete_group(qint64 group_id);
    bool rename_group(qint64 group_id, const QString &name);
    bool update_group(qint64 group_id, const QString &name, const std::optional<QString> &color = std::nullopt, const std::optional<QString> &icon = std::nullopt,
                      int group_type = 0, const std::optional<FilterRule> &filter_rule = std::nullopt);
    // The Rust core refuses items the group does not take (see group_accepts).
    bool move_to_group(qint64 item_id, std::optional<qint64> group_id = std::nullopt);
    bool move_group_between(qint64 group_id, std::optional<qint64> before_id = std::nullopt, std::optional<qint64> after_id = std::nullopt);
    bool move_item_between(qint64 item_id, std::optional<qint64> before_id = std::nullopt, std::optional<qint64> after_id = std::nullopt);
    QList<ClipboardItem> get_by_group(std::optional<qint64> group_id = std::nullopt, qint64 offset = 0, qint64 limit = 50);

    // Without a saved path the database lives under the application data folder.
    static QString default_db_path();
    static quint64 max_stitched_pixels();
    // pyclipboard.set_clipboard_text; false with *error on failure.
    static bool set_clipboard_text(const QString &text, QString *error = nullptr);

private:
    ClipboardManager();
    QString resolve_db_path(const QString &db_path) const;
    qint64 history_limit() const;
    void open(const QString &db_path);
    // Each call holds its own reference: a background paste or size scan still running when
    // the storage is released keeps the database open until it returns.
    std::shared_ptr<JbClipboard> backend() const;
    void set_backend(std::shared_ptr<JbClipboard> backend);
    void drop_backend();
    static void on_native_item(const char *item_json, void *user);
    void on_item(const ClipboardItem &item, qint64 encoded_sizes);

    mutable std::mutex backend_lock_;
    std::shared_ptr<JbClipboard> backend_;
    Callback callback_;
    bool resume_monitoring_after_reset_ = false;
};
}
