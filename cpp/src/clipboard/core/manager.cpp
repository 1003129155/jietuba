#include "clipboard/core/manager.h"
#include "core/constants.h"
#include "core/logger.h"
#include "core/native.h"
#include "core/text_utils.h"
#include "settings/tool_settings.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>

namespace jietuba::clipboard {
namespace {
constexpr size_t ERROR_LEN = 1024;
std::mutex callback_lock;

QByteArray utf8(const QString &text) { return text.toUtf8(); }

// Null for None.
const char *optional_arg(const std::optional<QString> &text, QByteArray &storage) {
    if (!text) return nullptr;
    storage = text->toUtf8();
    return storage.constData();
}

const qint64 *optional_id(const std::optional<qint64> &value) { return value ? &*value : nullptr; }

QString take_text(char *text) {
    if (!text) return {};
    const QString result = QString::fromUtf8(text);
    jb_clipboard_text_free(text);
    return result;
}

QJsonDocument take_json(char *text) {
    if (!text) return {};
    const auto document = QJsonDocument::fromJson(QByteArray(text));
    jb_clipboard_text_free(text);
    return document;
}

QList<ClipboardItem> items_from(char *json) {
    QList<ClipboardItem> items;
    for (const auto &value : take_json(json).array()) items.append(ClipboardItem::from_json(value.toObject()));
    return items;
}

QString format_bytes(qint64 n) {
    if (n < 1024) return QString("%1 B").arg(n);
    if (n < 1024 * 1024) return QString::number(static_cast<double>(n) / 1024, 'f', 1) + " KB";
    return QString::number(static_cast<double>(n) / 1024 / 1024, 'f', 2) + " MB";
}

// Closed when the last user lets go.
std::shared_ptr<JbClipboard> open_backend(const QString &db_path, char *error) {
    auto *handle = jb_clipboard_open(utf8(db_path).constData(), error, ERROR_LEN);
    if (!handle) return nullptr;
    return std::shared_ptr<JbClipboard>(handle, [](JbClipboard *backend) {
        jb_clipboard_stop_monitor(backend);
        jb_clipboard_free(backend);
    });
}
}

ClipboardManager &ClipboardManager::instance() {
    // Never destroyed: like the reference, the watcher simply ends with the process
    // instead of being joined during static destruction.
    static auto *manager = new ClipboardManager;
    return *manager;
}

ClipboardManager::ClipboardManager() {
    char error[ERROR_LEN] = {};
    auto backend = open_backend(resolve_db_path({}), error);
    if (!backend) {
        log_error(T("初始化失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return;
    }
    set_backend(std::move(backend));
    log_info(T("管理器初始化成功"), "Clipboard");
    // The history limit (the backend trims).
    apply_history_limit();
}

std::shared_ptr<JbClipboard> ClipboardManager::backend() const {
    std::lock_guard guard(backend_lock_);
    return backend_;
}

void ClipboardManager::set_backend(std::shared_ptr<JbClipboard> backend) {
    std::lock_guard guard(backend_lock_);
    backend_ = std::move(backend);
}

void ClipboardManager::drop_backend() {
    std::shared_ptr<JbClipboard> backend;
    {
        std::lock_guard guard(backend_lock_);
        backend.swap(backend_);
    }
    // The watcher stops now; a call still running on another thread keeps the database open until it returns.
    if (backend) jb_clipboard_stop_monitor(backend.get());
}

QString ClipboardManager::default_db_path() {
    // The backend's own default, which the reference gets by passing no path: the roaming
    // data folder (%APPDATA%), shared with the Python version.
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty()) base = QDir(QDir::homePath()).filePath("AppData/Roaming");
    return QDir::toNativeSeparators(QDir(base).filePath("pyclipboard/clipboard.db"));
}

QString ClipboardManager::resolve_db_path(const QString &db_path) const {
    if (!db_path.isEmpty()) return db_path;
    const QString saved = get_tool_settings_manager()->get_clipboard_db_path();
    return saved.isEmpty() ? default_db_path() : saved;
}

bool ClipboardManager::reset_storage(const QString &db_path) {
    const bool was_monitoring = is_monitoring() || resume_monitoring_after_reset_;
    resume_monitoring_after_reset_ = false;
    drop_backend();
    char error[ERROR_LEN] = {};
    auto backend = open_backend(resolve_db_path(db_path), error);
    if (!backend) {
        log_exception(QString::fromUtf8(error), T("重新打开剪贴板数据库"));
        return false;
    }
    set_backend(std::move(backend));
    apply_history_limit();
    if (was_monitoring) {
        Callback callback;
        {
            std::lock_guard guard(callback_lock);
            callback = callback_;
        }
        start_monitoring(callback);
    }
    return true;
}

bool ClipboardManager::release_storage() {
    resume_monitoring_after_reset_ = is_monitoring();
    drop_backend();
    return true;
}

std::optional<QString> ClipboardManager::get_db_path() const {
    const auto backend = this->backend();
    if (!backend) return std::nullopt;
    return take_text(jb_clipboard_db_path(backend.get()));
}

std::optional<QString> ClipboardManager::get_images_dir() const {
    const auto backend = this->backend();
    if (!backend) return std::nullopt;
    return take_text(jb_clipboard_images_dir(backend.get()));
}

qint64 ClipboardManager::history_limit() const { return get_tool_settings_manager()->get_clipboard_history_limit(); }

void ClipboardManager::apply_history_limit() {
    const auto backend = this->backend();
    if (!backend) return;
    const qint64 limit = history_limit();
    jb_clipboard_set_history_limit(backend.get(), limit);
    log_info(T("历史限制设置为: {limit}", {{"limit", limit}}), "Clipboard");
}

void ClipboardManager::on_native_item(const char *item_json, void *user) {
    // Called from the native watcher thread: an exception must not unwind into it.
    try {
        const auto object = QJsonDocument::fromJson(QByteArray(item_json)).object();
        const qint64 encoded = object.value("char_count").toInteger();
        static_cast<ClipboardManager *>(user)->on_item(ClipboardItem::from_json(object), encoded);
    } catch (const std::exception &e) {
        log_exception(e, LogMsg("ClipboardManager.on_native_item"));
    }
}

void ClipboardManager::on_item(const ClipboardItem &item, qint64 encoded) {
    // Line breaks removed so the log line stays whole.
    QString preview = left_chars(item.display_text(), 50);
    preview.replace("\r\n", " ").replace(u'\n', u' ').replace(u'\r', u' ');
    preview = preview.trimmed();
    // char_count encodes the format byte counts: raw * 10_000_000 + compressed.
    if (encoded > 0) {
        const qint64 raw_bytes = encoded / 10'000'000;
        const qint64 comp_bytes = encoded % 10'000'000;
        QString size_info;
        if (raw_bytes != comp_bytes && raw_bytes > 0) {
            const double ratio = comp_bytes > 0 ? static_cast<double>(raw_bytes) / static_cast<double>(comp_bytes) : 0;
            size_info = QString("%1 → %2 (%3x)").arg(format_bytes(raw_bytes), format_bytes(comp_bytes), QString::number(ratio, 'f', 1));
        } else {
            size_info = format_bytes(raw_bytes);
        }
        log_debug(T("新内容: {icon} {preview}  [{size_info}]", {{"icon", item.icon()}, {"preview", preview}, {"size_info", size_info}}), "Clipboard");
    } else {
        log_debug(T("新内容: {icon} {preview}", {{"icon", item.icon()}, {"preview", preview}}), "Clipboard");
    }
    Callback callback;
    {
        std::lock_guard guard(callback_lock);
        callback = callback_;
    }
    if (callback) callback(item);
}

void ClipboardManager::start_monitoring(Callback callback) {
    const auto backend = this->backend();
    if (!backend) {
        log_error(T("管理器不可用"), "Clipboard");
        return;
    }
    {
        std::lock_guard guard(callback_lock);
        callback_ = std::move(callback);
    }
    char error[ERROR_LEN] = {};
    if (jb_clipboard_start_monitor(backend.get(), &ClipboardManager::on_native_item, this, error, ERROR_LEN))
        log_info(T("开始监听剪贴板"), "Clipboard");
    else
        log_error(T("启动监听失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
}

void ClipboardManager::clear_callback() {
    std::lock_guard guard(callback_lock);
    callback_ = {};
}

void ClipboardManager::stop_monitoring() {
    const auto backend = this->backend();
    if (!backend) return;
    jb_clipboard_stop_monitor(backend.get());
    log_info(T("🛑 停止监听"), "Clipboard");
}

bool ClipboardManager::is_monitoring() const {
    const auto backend = this->backend();
    return backend && jb_clipboard_is_monitoring(backend.get());
}

bool ClipboardManager::pause_for_update() {
    const auto backend = this->backend();
    return !backend || jb_clipboard_pause_for_update(backend.get());
}

void ClipboardManager::resume_after_update() {
    if (const auto backend = this->backend()) jb_clipboard_resume_after_update(backend.get());
}

size_t ClipboardManager::pending_writes() const {
    const auto backend = this->backend();
    return backend ? jb_clipboard_pending_writes(backend.get()) : 0;
}

QList<ClipboardItem> ClipboardManager::get_history(qint64 offset, qint64 limit, const std::optional<QString> &search, const FilterRule &filter,
                                                   std::optional<qint64> start_time, std::optional<qint64> end_time) {
    const auto backend = this->backend();
    if (!backend) return {};
    QByteArray search_text, kind_text, app_text;
    char *json = nullptr;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_get_history(backend.get(), offset, limit, optional_arg(search, search_text), optional_arg(filter.kind, kind_text),
                                  optional_arg(filter.app, app_text), optional_id(start_time), optional_id(end_time), &json, error, ERROR_LEN)) {
        log_error(T("获取历史失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return {};
    }
    return items_from(json);
}

bool ClipboardManager::paste_as_files(qint64 item_id) {
    const auto backend = this->backend();
    if (!backend) return false;
    bool ok = false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_paste_as_files(backend.get(), item_id, &ok, error, ERROR_LEN)) {
        log_error(T("作为文件粘贴失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return ok;
}

QList<QPair<QString, qint64>> ClipboardManager::source_apps() {
    const auto backend = this->backend();
    if (!backend) return {};
    char *json = nullptr;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_source_apps(backend.get(), &json, error, ERROR_LEN)) {
        log_error(T("获取来源应用失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return {};
    }
    QList<QPair<QString, qint64>> apps;
    for (const auto &value : take_json(json).array()) apps.emplaceBack(value.toObject().value("app").toString(), value.toObject().value("count").toInteger());
    return apps;
}

qint64 ClipboardManager::get_total_count() {
    const auto backend = this->backend();
    if (!backend) return 0;
    qint64 count = 0;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_get_count(backend.get(), &count, error, ERROR_LEN)) {
        log_exception(QString::fromUtf8(error), T("获取总记录数"));
        return 0;
    }
    return count;
}

QList<ClipboardItem> ClipboardManager::search(const QString &keyword, qint64 limit) { return get_history(0, limit, keyword); }

std::optional<ClipboardItem> ClipboardManager::get_item(qint64 item_id) {
    const auto backend = this->backend();
    if (!backend) return std::nullopt;
    char *json = nullptr;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_get_item(backend.get(), item_id, &json, error, ERROR_LEN)) {
        log_error(T("获取项失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return std::nullopt;
    }
    const auto document = take_json(json);
    if (!document.isObject()) return std::nullopt;
    return ClipboardItem::from_json(document.object());
}

bool ClipboardManager::delete_item(qint64 item_id) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_delete_item(backend.get(), item_id, error, ERROR_LEN)) {
        log_error(T("删除失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return true;
}

bool ClipboardManager::clear_history(bool keep_grouped) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_clear_history(backend.get(), keep_grouped, error, ERROR_LEN)) {
        log_error(T("清空失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    log_info(T("🗑️ 历史已清空 (keep_grouped={keep_grouped})", {{"keep_grouped", keep_grouped}}), "Clipboard");
    return true;
}

bool ClipboardManager::clear_unpinned_ungrouped(bool vacuum) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_clear_unpinned_ungrouped(backend.get(), vacuum, error, ERROR_LEN)) {
        log_error(T("清空失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    log_info(T("🗑️ 已清理未置顶且未分组的历史"), "Clipboard");
    return true;
}

bool ClipboardManager::single_file() const {
    const auto backend = this->backend();
    return backend && jb_clipboard_single_file(backend.get());
}

bool ClipboardManager::set_single_file(bool on) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_set_single_file(backend.get(), on, error, ERROR_LEN)) {
        log_error(QString::fromUtf8(error), "Clipboard");
        return false;
    }
    log_info(T("存储方式已切换: single_file={on}", {{"on", on}}), "Clipboard");
    return true;
}

std::optional<qint64> ClipboardManager::add_item(const QString &content, const QString &content_type, const std::optional<QString> &title) {
    const auto backend = this->backend();
    if (!backend) return std::nullopt;
    QByteArray title_text;
    qint64 item_id = 0;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_add_item(backend.get(), utf8(content).constData(), utf8(content_type).constData(), optional_arg(title, title_text), &item_id, error, ERROR_LEN)) {
        log_error(T("添加内容失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return std::nullopt;
    }
    log_info(T("添加内容成功: ID={item_id}", {{"item_id", item_id}}), "Clipboard");
    return item_id;
}

bool ClipboardManager::update_item(qint64 item_id, const QString &content, const std::optional<QString> &title) {
    const auto backend = this->backend();
    if (!backend) return false;
    QByteArray title_text;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_update_item(backend.get(), item_id, utf8(content).constData(), optional_arg(title, title_text), error, ERROR_LEN)) {
        log_error(T("更新内容失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return true;
}

bool ClipboardManager::toggle_pin(qint64 item_id) {
    const auto backend = this->backend();
    if (!backend) return false;
    bool pinned = false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_toggle_pin(backend.get(), item_id, &pinned, error, ERROR_LEN)) {
        log_error(T("置顶失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return pinned;
}

bool ClipboardManager::paste_item(qint64 item_id, bool with_html, bool move_to_top) {
    const auto backend = this->backend();
    if (!backend) return false;
    bool ok = false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_paste_item(backend.get(), item_id, with_html, move_to_top, &ok, error, ERROR_LEN)) {
        log_error(T("粘贴失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return ok;
}

QList<qint64> ClipboardManager::paste_items(const QList<qint64> &item_ids, bool with_html, bool move_to_top, const QString &separator, bool plain_text,
                                            const QString &layout, bool keep_in_history) {
    const auto backend = this->backend();
    if (!backend) return {};
    int64_t *ids = nullptr;
    size_t count = 0;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_paste_items(backend.get(), item_ids.constData(), static_cast<size_t>(item_ids.size()), with_html, move_to_top, utf8(separator).constData(),
                                  plain_text, utf8(layout).constData(), keep_in_history, &ids, &count, error, ERROR_LEN)) {
        log_error(T("合并粘贴失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return {};
    }
    QList<qint64> used(ids, ids + count);
    jb_clipboard_ids_free(ids, count);
    return used;
}

std::optional<QByteArray> ClipboardManager::get_image_data(const QString &image_id) {
    const auto backend = this->backend();
    if (!backend) return std::nullopt;
    uint8_t *data = nullptr;
    size_t len = 0;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_get_image_data(backend.get(), utf8(image_id).constData(), &data, &len, error, ERROR_LEN)) {
        log_error(T("获取图片失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return std::nullopt;
    }
    if (!data) return std::nullopt;
    QByteArray bytes(reinterpret_cast<const char *>(data), static_cast<qsizetype>(len));
    jb_clipboard_bytes_free(data, len);
    return bytes;
}

std::optional<qint64> ClipboardManager::create_group(const QString &name, const std::optional<QString> &color, const std::optional<QString> &icon, int group_type,
                                                    const std::optional<FilterRule> &filter_rule) {
    const auto backend = this->backend();
    if (!backend) return std::nullopt;
    QByteArray color_text, icon_text;
    const QByteArray rule_text = filter_rule ? filter_rule->to_json() : QByteArray();
    qint64 group_id = 0;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_create_group(backend.get(), utf8(name).constData(), optional_arg(color, color_text), optional_arg(icon, icon_text), group_type,
                                   filter_rule ? rule_text.constData() : nullptr, &group_id, error, ERROR_LEN)) {
        log_error(T("创建分组失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return std::nullopt;
    }
    return group_id;
}

QList<Group> ClipboardManager::get_groups() {
    const auto backend = this->backend();
    if (!backend) return {};
    char *json = nullptr;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_get_groups(backend.get(), &json, error, ERROR_LEN)) {
        log_error(T("获取分组失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return {};
    }
    QList<Group> groups;
    for (const auto &value : take_json(json).array()) groups.append(Group::from_json(value.toObject()));
    return groups;
}

bool ClipboardManager::delete_group(qint64 group_id) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_delete_group(backend.get(), group_id, error, ERROR_LEN)) {
        log_error(T("删除分组失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return true;
}

bool ClipboardManager::rename_group(qint64 group_id, const QString &name) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_rename_group(backend.get(), group_id, utf8(name).constData(), error, ERROR_LEN)) {
        log_error(T("重命名分组失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return true;
}

bool ClipboardManager::update_group(qint64 group_id, const QString &name, const std::optional<QString> &color, const std::optional<QString> &icon, int group_type,
                                    const std::optional<FilterRule> &filter_rule) {
    const auto backend = this->backend();
    if (!backend) return false;
    QByteArray color_text, icon_text;
    const QByteArray rule_text = filter_rule ? filter_rule->to_json() : QByteArray();
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_update_group(backend.get(), group_id, utf8(name).constData(), optional_arg(color, color_text), optional_arg(icon, icon_text), group_type,
                                   filter_rule ? rule_text.constData() : nullptr, error, ERROR_LEN)) {
        log_error(T("更新分组失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return true;
}

bool ClipboardManager::move_to_group(qint64 item_id, std::optional<qint64> group_id) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    auto failed = [&error] {
        log_error(T("移动到分组失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    };
    if (!jb_clipboard_move_to_group(backend.get(), item_id, optional_id(group_id), error, ERROR_LEN)) return failed();
    return true;
}

bool ClipboardManager::move_group_between(qint64 group_id, std::optional<qint64> before_id, std::optional<qint64> after_id) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_move_group_between(backend.get(), group_id, optional_id(before_id), optional_id(after_id), error, ERROR_LEN)) {
        log_error(T("调整分组顺序失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return true;
}

bool ClipboardManager::move_item_between(qint64 item_id, std::optional<qint64> before_id, std::optional<qint64> after_id) {
    const auto backend = this->backend();
    if (!backend) return false;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_move_item_between(backend.get(), item_id, optional_id(before_id), optional_id(after_id), error, ERROR_LEN)) {
        log_error(T("调整内容顺序失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return false;
    }
    return true;
}

QList<ClipboardItem> ClipboardManager::get_by_group(std::optional<qint64> group_id, qint64 offset, qint64 limit) {
    const auto backend = this->backend();
    if (!backend) return {};
    char *json = nullptr;
    char error[ERROR_LEN] = {};
    if (!jb_clipboard_get_by_group(backend.get(), optional_id(group_id), offset, limit, &json, error, ERROR_LEN)) {
        log_error(T("按分组查询失败: {e}", {{"e", QString::fromUtf8(error)}}), "Clipboard");
        return {};
    }
    return items_from(json);
}

quint64 ClipboardManager::max_stitched_pixels() { return jb_clipboard_max_stitched_pixels(); }

bool ClipboardManager::set_clipboard_text(const QString &text, QString *error) {
    char message[ERROR_LEN] = {};
    if (jb_clipboard_set_text(utf8(text).constData(), message, ERROR_LEN)) return true;
    if (error) *error = QString::fromUtf8(message);
    return false;
}
}
