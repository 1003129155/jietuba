#include "clipboard/controllers/clipboard_controller.h"
#include "clipboard/controllers/paste_keystroke.h"
#include "clipboard/core/text_transform.h"
#include "clipboard/ui/dialogs/manage_dialog.h"
#include "core/crash_handler.h"
#include "core/logger.h"
#include "core/ui_scale.h"
#include "settings/color_formats.h"
#include "settings/tool_settings.h"
#include "ui/dialogs.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <thread>

namespace jietuba::clipboard {
namespace {
QString tr_window(const char *text) { return QCoreApplication::translate("ClipboardWindow", text); }

// Layout of the group bar at 100%; the bar's real size is already scaled.
constexpr int TOP_USED = 8 + 34 + 4 + 1 + 4 + 34 + 4;
constexpr int BOTTOM_RESERVED = 8 + 34 + 4 + 34 + 8;
constexpr int BTN_SLOT = 34 + 4;
// Top mode lays the buttons out horizontally.
constexpr int H_LEFT_USED = 2 + 34 + 4 + 1 + 4 + 34 + 4;
constexpr int H_RIGHT_RESERVED = 4 + 34 + 4 + 34 + 2;
constexpr int H_BTN_SLOT = 34 + 4;

// Digits of a regex capture; Python's int() also reads non-ASCII decimal digits.
quint64 digits_value(const QString &digits) {
    constexpr quint64 max = std::numeric_limits<quint64>::max();
    quint64 value = 0;
    for (char32_t c : digits.toUcs4()) {
        const int digit = QChar::digitValue(c);
        if (value > (max - static_cast<quint64>(digit)) / 10) return max;
        value = value * 10 + static_cast<quint64>(digit);
    }
    return value;
}

quint64 saturating_add(quint64 a, quint64 b) { return a > std::numeric_limits<quint64>::max() - b ? std::numeric_limits<quint64>::max() : a + b; }
}

int calc_sidebar_capacity(int right_bar_height) {
    if (right_bar_height <= 0) return -1;
    const int available = right_bar_height - scaled(TOP_USED) - scaled(BOTTOM_RESERVED);
    return available <= 0 ? 0 : available / scaled(BTN_SLOT);
}

int calc_topbar_capacity(int bar_width) {
    if (bar_width <= 0) return -1;
    const int available = bar_width - scaled(H_LEFT_USED) - scaled(H_RIGHT_RESERVED);
    return available <= 0 ? 0 : available / scaled(H_BTN_SLOT);
}

ClipboardController::ClipboardController(ClipboardManager &manager, QObject *parent) : QObject(parent), manager(manager), context_menu_controller_(this) {
    load_settings();
}

// ==================== Settings ====================

void ClipboardController::load_settings() {
    auto *config = get_tool_settings_manager();
    auto_paste_enabled = config->get_clipboard_auto_paste();
    paste_with_html = config->get_clipboard_paste_with_html();
}

void ClipboardController::set_auto_paste(bool enabled) {
    // Updated at once so the setting takes effect immediately.
    auto_paste_enabled = enabled;
    get_tool_settings_manager()->set_clipboard_auto_paste(enabled);
}

void ClipboardController::set_paste_with_html(bool enabled) {
    paste_with_html = enabled;
    get_tool_settings_manager()->set_clipboard_paste_with_html(enabled);
}

void ClipboardController::set_move_to_top_on_paste(bool enabled) { get_tool_settings_manager()->set_clipboard_move_to_top_on_paste(enabled); }

// ==================== Loading ====================

int ClipboardController::page_size() const { return page_size_; }

void ClipboardController::load_history() {
    if (is_loading_) {
        pending_reload_ = true;
        log_debug(T("⏸️ 正在加载中，标记待重新加载"), "Clipboard");
        return;
    }
    current_offset_ = 0;
    has_more_ = true;
    current_items.clear();
    pending_reload_ = false;
    last_scroll_value_ = 0;
    load_more_items();
}

void ClipboardController::load_more_items() {
    if (is_loading_ || !has_more_) return;
    is_loading_ = true;
    emit loading_state_changed(true);

    const int current_page_size = page_size();
    const bool is_first_page = current_offset_ == 0;
    const auto [new_items, raw_count] = fetch_items_page(current_page_size, current_offset_);
    current_offset_ += raw_count;
    log_info(T("加载完成 - 获取到 {count} 条记录", {{"count", static_cast<int>(new_items.size())}}), "Clipboard");
    // Judged by raw_count before filtering: searching inside a group filters on this side, so a page
    // may match far fewer rows, which would wrongly mark a group with more pages as finished.
    if (raw_count < current_page_size) has_more_ = false;
    current_items.append(new_items);
    emit data_loaded(new_items, is_first_page);

    is_loading_ = false;
    emit loading_state_changed(false);
    emit load_completed();
    if (pending_reload_) {
        pending_reload_ = false;
        QTimer::singleShot(0, this, [this] { load_history(); });
    }
}

void ClipboardController::load_more_if_needed() {
    if (!has_more_ || is_loading_) return;
    load_more_items();
}

std::optional<FilterRule> ClipboardController::current_filter_rule() const {
    if (!current_group_id) return std::nullopt;
    for (const auto &group : manager.get_groups())
        if (group.id == *current_group_id) return group.is_filter() ? std::optional(group.filter_rule.value_or(FilterRule{})) : std::nullopt;
    return std::nullopt;
}

bool ClipboardController::shows_history() const { return !current_group_id || current_filter_rule().has_value(); }

std::pair<QList<ClipboardItem>, int> ClipboardController::fetch_items_page(int limit, qint64 offset) {
    const auto filter_rule = current_filter_rule();
    if (!current_group_id || filter_rule) {
        std::optional<qint64> start_ts, end_ts;
        if (time_range_) {
            start_ts = time_range_->first.toSecsSinceEpoch();
            end_ts = time_range_->second.toSecsSinceEpoch();
        }
        // A filter group's kind is fixed; the type button still narrows a group that only picks a source app.
        FilterRule filter = filter_rule.value_or(FilterRule{});
        if (!filter.kind) filter.kind = content_type_;
        const auto items = manager.get_history(offset, limit, search_text_, filter, start_ts, end_ts);
        return {items, static_cast<int>(items.size())};
    }
    auto items = manager.get_by_group(current_group_id, offset, limit);
    const int raw_count = static_cast<int>(items.size());
    if (search_text_) {
        QList<ClipboardItem> matched;
        for (const auto &item : items)
            if (item.content.contains(*search_text_, Qt::CaseInsensitive) || (item.title && item.title->contains(*search_text_, Qt::CaseInsensitive)))
                matched.append(item);
        items = matched;
    }
    return {items, raw_count};
}

void ClipboardController::check_scroll_load(int scroll_value, int scroll_max) {
    if (!has_more_ || is_loading_) return;
    // No scroll bar, nothing to load.
    if (scroll_max <= 0) return;
    // Only when scrolling down.
    if (scroll_value <= last_scroll_value_) {
        last_scroll_value_ = scroll_value;
        return;
    }
    last_scroll_value_ = scroll_value;
    const int distance_to_bottom = scroll_max - scroll_value;
    const double scroll_percentage = static_cast<double>(scroll_value) / scroll_max * 100;
    // Within 50 pixels of the bottom and past 90% of the range.
    if (distance_to_bottom < 50 && scroll_percentage > 90) {
        log_debug(T("🔄 触发加载更多 - 距离底部: {distance_to_bottom}px, 滚动位置: {scroll_percentage:.1f}%",
                    {{"distance_to_bottom", distance_to_bottom}, {"scroll_percentage", scroll_percentage}}),
                  "Clipboard");
        load_more_items();
    }
}

// ==================== Search and filters ====================

void ClipboardController::set_search_text(const QString &text) {
    const QString stripped = text.trimmed();
    const std::optional<QString> search = stripped.isEmpty() ? std::nullopt : std::optional<QString>(stripped);
    if (search_text_ != search) {
        search_text_ = search;
        load_history();
    }
}

void ClipboardController::set_content_type_filter(const std::optional<QString> &kind) {
    if (content_type_ != kind) {
        content_type_ = kind;
        load_history();
    }
}

bool ClipboardController::set_time_range_text(const QString &range_text) {
    const auto parsed_range = parse_time_range_text(range_text);
    if (!parsed_range && !range_text.trimmed().isEmpty()) return false;
    if (time_range_ != parsed_range) {
        time_range_ = parsed_range;
        load_history();
    }
    return true;
}

void ClipboardController::clear_time_and_type_filters() {
    if (time_range_ || content_type_) {
        time_range_.reset();
        content_type_.reset();
        load_history();
    }
}

std::optional<std::pair<QDateTime, QDateTime>> ClipboardController::parse_time_range_text(const QString &range_text) {
    const QString text = range_text.trimmed();
    if (text.isEmpty()) return std::nullopt;
    static const QRegularExpression pattern(QString::fromUtf8(R"(^\s*(\d{4})[\/.-](\d{1,2})[\/.-](\d{1,2})\s*(?:-|~|至|到)\s*(\d{4})[\/.-](\d{1,2})[\/.-](\d{1,2})\s*$)"),
                                            QRegularExpression::UseUnicodePropertiesOption);
    const auto match = pattern.match(text);
    if (!match.hasMatch()) return std::nullopt;
    auto part = [&match](int index) { return static_cast<int>(std::min<quint64>(digits_value(match.captured(index)), 1'000'000)); };
    const QDate start_date(part(1), part(2), part(3));
    const QDate end_date(part(4), part(5), part(6));
    // Python's datetime has no year 0 either.
    if (!start_date.isValid() || !end_date.isValid() || part(1) < 1 || part(4) < 1) return std::nullopt;
    if (end_date < start_date) return std::nullopt;
    return std::make_pair(QDateTime(start_date, QTime(0, 0)), QDateTime(end_date.addDays(1), QTime(0, 0)));
}

// ==================== Groups ====================

void ClipboardController::switch_to_group(std::optional<qint64> group_id) {
    if (current_group_id != group_id) {
        current_group_id = group_id;
        load_history();
    }
}

bool ClipboardController::delete_group(qint64 group_id, QWidget *parent_widget, const std::function<QString(const char *)> &tr) {
    if (parent_widget) {
        QString title = "Confirm Delete";
        QString message = "Are you sure you want to delete this group?\nItems in the group will be moved back to clipboard history.";
        if (tr) {
            title = tr("Confirm Delete");
            message = tr("Are you sure you want to delete this group?") + "\n" + tr("Items in the group will be moved back to clipboard history.");
        }
        if (!show_confirm_dialog(parent_widget, title, message)) return false;
    }
    if (!manager.delete_group(group_id)) return false;
    // The deleted group was on show: back to the clipboard history.
    if (current_group_id == group_id) current_group_id.reset();
    if (auto *dialog = get_existing_manage_dialog()) dialog->refresh_after_external_change(group_id);
    emit reload_required();
    return true;
}

QList<Group> ClipboardController::get_groups() { return manager.get_groups(); }

std::pair<bool, bool> ClipboardController::get_group_move_state(qint64 group_id) {
    const auto groups = manager.get_groups();
    for (qsizetype index = 0; index < groups.size(); ++index)
        if (groups[index].id == group_id) return {index > 0, index < groups.size() - 1};
    return {false, false};
}

bool ClipboardController::move_group_order(qint64 group_id, int direction) {
    const auto groups = manager.get_groups();
    qsizetype current_index = -1;
    for (qsizetype i = 0; i < groups.size(); ++i)
        if (groups[i].id == group_id) {
            current_index = i;
            break;
        }
    if (current_index < 0) return false;
    const qsizetype new_index = current_index + (direction < 0 ? -1 : 1);
    if (new_index < 0 || new_index >= groups.size()) return false;
    auto temp_groups = groups;
    temp_groups.removeAt(current_index);
    const std::optional<qint64> before_id = new_index > 0 ? std::optional<qint64>(temp_groups[new_index - 1].id) : std::nullopt;
    const std::optional<qint64> after_id = new_index < temp_groups.size() ? std::optional<qint64>(temp_groups[new_index].id) : std::nullopt;
    return manager.move_group_between(group_id, before_id, after_id);
}

// ==================== Item actions ====================

void ClipboardController::send_paste_keystroke(bool explicit_paste) {
    // Paste chosen in a menu is itself a manual paste; the auto-paste switch does not apply.
    if (!explicit_paste && !auto_paste_enabled) return;
    const Hwnd target_hwnd = foreground_tracker_.target_hwnd();
    // Delayed so the clipboard window is closed or hidden and cannot take the focus back.
    QTimer::singleShot(50, [target_hwnd] { paste_to_target(target_hwnd); });
}

bool ClipboardController::paste_item(qint64 item_id, const CloseCallback &on_close_callback, bool explicit_paste) {
    bool move_to_top = get_tool_settings_manager()->get_clipboard_move_to_top_on_paste();
    // Only the history view moves it to the top; pasting in a group keeps the order.
    if (!shows_history()) move_to_top = false;
    if (!manager.paste_item(item_id, paste_with_html, move_to_top)) return false;
    log_info(T("已粘贴项 {item_id} (带格式: {with_html}, 移到最前: {move_to_top})", {{"item_id", item_id}, {"with_html", paste_with_html}, {"move_to_top", move_to_top}}),
             "Clipboard");
    if (move_to_top) move_item_to_top(item_id);
    if (on_close_callback) on_close_callback();
    send_paste_keystroke(explicit_paste);
    return true;
}

QList<qint64> ClipboardController::multi_paste_order(const QList<qint64> &item_ids) {
    const bool first_selected_first = get_tool_settings_manager()->get_clipboard_multi_paste_order() == "oldest_first";
    if (first_selected_first) return item_ids;
    QList<qint64> reversed(item_ids.crbegin(), item_ids.crend());
    return reversed;
}

bool ClipboardController::stitched_image_fits(const QList<qint64> &item_ids, const QString &layout) {
    static const QRegularExpression size_pattern(R"(^\[(\d+)x(\d+)\]$)", QRegularExpression::UseUnicodePropertiesOption);
    QList<std::pair<quint64, quint64>> sizes;
    for (qint64 item_id : item_ids) {
        const auto item = loaded_item(item_id);
        if (!item) return true;
        const auto match = size_pattern.match(item->content);
        if (!match.hasMatch()) return true;
        sizes.append({digits_value(match.captured(1)), digits_value(match.captured(2))});
    }
    if (sizes.isEmpty()) return true;
    quint64 width = 0, height = 0;
    for (const auto &[w, h] : sizes) {
        if (layout == "horizontal") {
            width = saturating_add(width, w);
            height = std::max(height, h);
        } else {
            width = std::max(width, w);
            height = saturating_add(height, h);
        }
    }
    const quint64 limit = ClipboardManager::max_stitched_pixels();
    if (width == 0 || height == 0) return true;
    return width <= limit / height;
}

bool ClipboardController::paste_items(const QList<qint64> &item_ids, const CloseCallback &on_close_callback, bool explicit_paste, bool plain_text,
                                      const QString &layout) {
    auto *config = get_tool_settings_manager();
    const auto ordered = multi_paste_order(item_ids);
    const bool move_to_top = config->get_clipboard_move_to_top_on_paste() && shows_history();
    const QString separator = config->get_clipboard_multi_paste_separator();
    const bool keep_in_history = config->get_clipboard_multi_paste_keep_merged();
    bool stitching = !plain_text && !ordered.isEmpty();
    for (qint64 item_id : ordered) {
        const auto item = loaded_item(item_id);
        if (!item || item->content_type != "image") stitching = false;
    }
    if (stitching) {
        // Stitching decodes and re-encodes whole images, so it runs in the background and the window closes first.
        if (on_close_callback) on_close_callback();
        QPointer<ClipboardController> guard(this);
        const bool with_html = paste_with_html;
        std::thread([guard, ordered, with_html, move_to_top, separator, plain_text, layout, keep_in_history, explicit_paste] {
            run_thread_guarded("clipboard-multi-paste", [&] {
                const auto used = ClipboardManager::instance().paste_items(ordered, with_html, move_to_top, separator, plain_text, layout, keep_in_history);
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [guard, used, move_to_top, explicit_paste] {
                        if (guard) guard->on_multi_paste_finished(used, move_to_top, explicit_paste);
                    },
                    Qt::QueuedConnection);
            });
        }).detach();
        return true;
    }
    const auto used = manager.paste_items(ordered, paste_with_html, move_to_top, separator, plain_text, layout, keep_in_history);
    if (used.isEmpty()) return false;
    log_info(T("已合并粘贴 {count} 项", {{"count", static_cast<int>(used.size())}}), "Clipboard");
    if (move_to_top) move_items_to_top(used);
    if (on_close_callback) on_close_callback();
    send_paste_keystroke(explicit_paste);
    return true;
}

void ClipboardController::on_multi_paste_finished(const QList<qint64> &used, bool move_to_top, bool explicit_paste) {
    if (used.isEmpty()) {
        log_error(T("合并粘贴没有写入任何内容"), "Clipboard");
        return;
    }
    log_info(T("已合并粘贴 {count} 项", {{"count", static_cast<int>(used.size())}}), "Clipboard");
    // A group may be on show by the time stitching ends; the history order must not reorder a group.
    if (move_to_top && shows_history()) move_items_to_top(used);
    send_paste_keystroke(explicit_paste);
}

void ClipboardController::move_items_to_top(const QList<qint64> &item_ids) {
    // Several rows move to the top keeping their relative order: start from the lowest.
    QList<std::pair<qint64, qint64>> loaded;
    for (qint64 item_id : item_ids)
        if (const auto index = loaded_index(item_id)) loaded.append({*index, item_id});
    std::stable_sort(loaded.begin(), loaded.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &[index, item_id] : loaded) move_item_to_top(item_id);
}

void ClipboardController::move_item_to_top(qint64 item_id) {
    // The backend only updates the database; a paste usually writes what the clipboard already holds, so the watcher stays quiet and the list moves itself.
    const auto index = loaded_index(item_id);
    if (!index) return;
    const ClipboardItem item = current_items[*index];
    // Pinned items sort before item_order, so their place does not change.
    if (item.is_pinned) return;
    const int row = first_unpinned_row();
    if (*index == row) return;
    current_items.removeAt(*index);
    current_items.insert(row, item);
    emit item_moved_to_top(item_id, row);
}

bool ClipboardController::paste_transformed_text(qint64 item_id, const QString &transform_key, const CloseCallback &on_close_callback, bool explicit_paste) {
    // "Paste keeping order": with formatting, never moved to the top.
    if (transform_key == "special_paste_in_order") {
        if (!manager.paste_item(item_id, paste_with_html, false)) return false;
        log_info(T("保持顺序粘贴项 {item_id}", {{"item_id", item_id}}), "Clipboard");
        if (on_close_callback) on_close_callback();
        send_paste_keystroke(explicit_paste);
        return true;
    }

    // "Paste as plain text": a paste without HTML formatting.
    if (transform_key == "special_paste_plain_text") {
        const auto clipboard_item = get_item(item_id);
        if (!clipboard_item || clipboard_item->content_type != "text") {
            log_error(T("项 {item_id} 不是文本类型，无法粘贴纯文本", {{"item_id", item_id}}), "Clipboard");
            return false;
        }
        QString error;
        if (!ClipboardManager::set_clipboard_text(clipboard_item->content, &error)) {
            log_exception(error, T("设置剪贴板纯文本失败"));
            return false;
        }
        log_info(T("粘贴纯文本项 {item_id}", {{"item_id", item_id}}), "Clipboard");
        if (on_close_callback) on_close_callback();
        send_paste_keystroke(explicit_paste);
        return true;
    }

    const auto &registry = TRANSFORM_REGISTRY();
    const auto transform = registry.find(transform_key);
    if (transform == registry.end()) {
        log_error(T("未知转换键: {transform_key}", {{"transform_key", transform_key}}), "Clipboard");
        return false;
    }
    const auto clipboard_item = get_item(item_id);
    if (!clipboard_item || clipboard_item->content_type != "text") {
        log_error(T("项 {item_id} 不是文本类型，无法特殊粘贴", {{"item_id", item_id}}), "Clipboard");
        return false;
    }
    QString transformed;
    try {
        transformed = transform->second(clipboard_item->content);
    } catch (const std::exception &e) {
        log_exception(e, T("文本转换失败: {transform_key}", {{"transform_key", transform_key}}));
        return false;
    }
    // To the system clipboard as plain text, without formatting.
    QString error;
    if (!ClipboardManager::set_clipboard_text(transformed, &error)) {
        log_exception(error, T("设置剪贴板文本失败"));
        return false;
    }
    log_info(T("特殊粘贴项 {item_id} ({transform_key})", {{"item_id", item_id}, {"transform_key", transform_key}}), "Clipboard");
    if (on_close_callback) on_close_callback();
    send_paste_keystroke(explicit_paste);
    return true;
}

bool ClipboardController::paste_file_text(qint64 item_id, const QString &transform_key, const CloseCallback &on_close_callback, bool explicit_paste) {
    const auto clipboard_item = get_item(item_id);
    if (!clipboard_item || clipboard_item->content_type != "file") {
        log_error(T("项 {item_id} 不是文件类型，无法特殊粘贴", {{"item_id", item_id}}), "Clipboard");
        return false;
    }
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(clipboard_item->content.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        log_exception(parse_error.errorString(), T("解析文件项内容失败"));
        return false;
    }
    QStringList files;
    if (document.isObject()) {
        for (const auto &value : document.object().value("files").toArray()) {
            const QString file_path = value.toString();
            if (!file_path.isEmpty()) files.append(QDir::toNativeSeparators(QDir::cleanPath(file_path)));
        }
    }
    if (files.isEmpty()) {
        log_error(T("文件项 {item_id} 没有可粘贴的文件路径", {{"item_id", item_id}}), "Clipboard");
        return false;
    }
    QString text;
    if (transform_key == "file_paste_names") {
        QStringList names;
        for (const auto &file_path : files) names.append(QFileInfo(file_path).fileName());
        text = names.join("\n");
    } else if (transform_key == "file_paste_links") {
        text = files.join("\n");
    } else {
        log_error(T("未知文件粘贴键: {transform_key}", {{"transform_key", transform_key}}), "Clipboard");
        return false;
    }
    QString error;
    if (!ClipboardManager::set_clipboard_text(text, &error)) {
        log_exception(error, T("设置剪贴板文件文本失败"));
        return false;
    }
    log_info(T("文件特殊粘贴项 {item_id} ({transform_key})", {{"item_id", item_id}, {"transform_key", transform_key}}), "Clipboard");
    if (on_close_callback) on_close_callback();
    send_paste_keystroke(explicit_paste);
    return true;
}

bool ClipboardController::paste_as_files(qint64 item_id, const CloseCallback &on_close_callback, bool explicit_paste) {
    if (!manager.paste_as_files(item_id)) return false;
    log_info(T("作为文件粘贴项 {item_id}", {{"item_id", item_id}}), "Clipboard");
    if (on_close_callback) on_close_callback();
    send_paste_keystroke(explicit_paste);
    return true;
}

QStringList ClipboardController::color_conversions(const ClipboardItem &item) {
    if (!item.color) return {};
    const QString original = item.content.trimmed();
    QStringList texts;
    for (const auto &format : color_formats::enabled_formats(color_formats::load(get_tool_settings_manager()))) {
        const QString text = format.render(*item.color);
        if (!text.isEmpty() && text.compare(original, Qt::CaseInsensitive) != 0 && !texts.contains(text)) texts.append(text);
    }
    return texts;
}

bool ClipboardController::paste_color_conversion(qint64 item_id, int index, const CloseCallback &on_close_callback, bool explicit_paste) {
    const auto clipboard_item = get_item(item_id);
    const QStringList texts = clipboard_item ? color_conversions(*clipboard_item) : QStringList();
    if (index < 0 || index >= texts.size()) return false;
    QString error;
    if (!ClipboardManager::set_clipboard_text(texts[index], &error)) {
        log_exception(error, T("设置剪贴板颜色文本失败"));
        return false;
    }
    log_info(T("按颜色格式粘贴项 {item_id}", {{"item_id", item_id}}), "Clipboard");
    if (on_close_callback) on_close_callback();
    send_paste_keystroke(explicit_paste);
    return true;
}

bool ClipboardController::delete_item(qint64 item_id) {
    if (!manager.delete_item(item_id)) return false;
    remove_loaded_item(item_id);
    return true;
}

void ClipboardController::toggle_pin(qint64 item_id) {
    manager.toggle_pin(item_id);
    load_history();
}

bool ClipboardController::move_to_group(qint64 item_id, std::optional<qint64> group_id) {
    if (!manager.move_to_group(item_id, group_id)) return false;
    log_info(T("已移动到分组 {group_id}", {{"group_id", group_id ? QVariant(*group_id) : QVariant()}}), "Clipboard");
    // History and filter groups do not filter by group, so the row stays; a group view has lost it.
    if (!shows_history() && group_id != current_group_id) remove_loaded_item(item_id);
    return true;
}

bool ClipboardController::update_item_content(qint64 item_id, const QString &content) {
    const auto item = get_item(item_id);
    if (!item) return false;
    if (!manager.update_item(item_id, content, item->title)) return false;
    const auto index = loaded_index(item_id);
    const auto updated = get_item(item_id);
    if (index && updated) {
        current_items[*index] = *updated;
        emit item_updated(static_cast<int>(*index), *updated);
    }
    return true;
}

int ClipboardController::delete_items(const QList<qint64> &item_ids) {
    int count = 0;
    for (qint64 item_id : item_ids) count += delete_item(item_id) ? 1 : 0;
    return count;
}

int ClipboardController::move_items_to_group(const QList<qint64> &item_ids, std::optional<qint64> group_id) {
    int count = 0;
    for (qint64 item_id : item_ids) count += move_to_group(item_id, group_id) ? 1 : 0;
    return count;
}

std::optional<qint64> ClipboardController::loaded_index(qint64 item_id) const {
    for (qsizetype i = 0; i < current_items.size(); ++i)
        if (current_items[i].id == item_id) return i;
    return std::nullopt;
}

std::optional<ClipboardItem> ClipboardController::loaded_item(qint64 item_id) const {
    const auto index = loaded_index(item_id);
    if (!index) return std::nullopt;
    return current_items[*index];
}

void ClipboardController::remove_loaded_item(qint64 item_id) {
    const auto index = loaded_index(item_id);
    if (!index) return;
    current_items.removeAt(*index);
    // Pages are fetched by offset; one row fewer loaded moves the next page back, or the row at the boundary is skipped.
    current_offset_ -= 1;
    emit item_removed(static_cast<int>(*index));
}

void ClipboardController::move_loaded_item(qint64 item_id, int direction) {
    const auto index = loaded_index(item_id);
    const std::optional<qint64> target = index ? std::optional<qint64>(*index + (direction < 0 ? -1 : 1)) : std::nullopt;
    // After a search, neighbouring rows need not be neighbours in the group, and unloaded neighbours give no position; both reload the list.
    if (search_text_ || !target || *target < 0 || *target >= current_items.size()) {
        load_history();
        return;
    }
    // Sorting is by pinned state first; moving across the pinned boundary only changes item_order, not the shown order.
    if (current_items[*index].is_pinned != current_items[*target].is_pinned) return;
    current_items.swapItemsAt(*index, *target);
    emit item_row_moved(static_cast<int>(*index), static_cast<int>(*target));
}

std::pair<bool, bool> ClipboardController::get_item_move_state(qint64 item_id, std::optional<qint64> group_id) {
    if (!group_id) return {false, false};
    const auto items = manager.get_by_group(group_id, 0, 1000);
    for (qsizetype index = 0; index < items.size(); ++index)
        if (items[index].id == item_id) return {index > 0, index < items.size() - 1};
    return {false, false};
}

bool ClipboardController::move_item_order(qint64 item_id, std::optional<qint64> group_id, int direction) {
    if (!group_id) return false;
    const auto items = manager.get_by_group(group_id, 0, 1000);
    qsizetype current_index = -1;
    for (qsizetype i = 0; i < items.size(); ++i)
        if (items[i].id == item_id) {
            current_index = i;
            break;
        }
    if (current_index < 0) return false;
    const qsizetype new_index = current_index + (direction < 0 ? -1 : 1);
    if (new_index < 0 || new_index >= items.size()) return false;
    auto temp_items = items;
    temp_items.removeAt(current_index);
    const std::optional<qint64> before_id = new_index > 0 ? std::optional<qint64>(temp_items[new_index - 1].id) : std::nullopt;
    const std::optional<qint64> after_id = new_index < temp_items.size() ? std::optional<qint64>(temp_items[new_index].id) : std::nullopt;
    if (!manager.move_item_between(item_id, before_id, after_id)) return false;
    if (current_group_id == group_id) move_loaded_item(item_id, direction);
    return true;
}

ManageDialog *ClipboardController::open_manage_dialog_for_group(qint64 group_id, const std::function<void(ManageDialog *)> &connect_signals) {
    auto *dialog = get_manage_dialog(&manager);
    if (connect_signals) connect_signals(dialog);
    dialog->open_group_editor(group_id);
    return dialog;
}

ManageDialog *ClipboardController::open_manage_dialog_for_item(qint64 item_id, std::optional<qint64> group_id,
                                                               const std::function<void(ManageDialog *)> &connect_signals) {
    auto *dialog = get_manage_dialog(&manager);
    if (connect_signals) connect_signals(dialog);
    dialog->open_item_editor(item_id, group_id);
    return dialog;
}

bool ClipboardController::clear_history(QWidget *parent_widget) {
    if (parent_widget && !show_confirm_dialog(parent_widget, tr_window("Confirm Clear"),
                                              tr_window("Are you sure you want to clear the clipboard history?\nPinned items and items in groups will be kept.\nThis action cannot be undone.")))
        return false;
    // Clearing from the window only hides the items; reclaiming disk space is left to the settings page.
    if (!manager.clear_unpinned_ungrouped(false)) return false;
    load_history();
    return true;
}

std::optional<ClipboardItem> ClipboardController::get_item(qint64 item_id) { return manager.get_item(item_id); }

// ==================== Window state ====================

void ClipboardController::set_paste_target_exclusion(std::function<bool(Hwnd)> predicate) { foreground_tracker_.set_excluded(std::move(predicate)); }

void ClipboardController::on_window_show() {
    // showEvent comes before the window gets focus, so the foreground is still the user's window.
    foreground_tracker_.start();
    load_history();
}

void ClipboardController::on_window_hide() { foreground_tracker_.stop(); }

void ClipboardController::on_new_content(bool is_window_visible, const std::optional<ClipboardItem> &item) {
    if (!is_window_visible) return;
    if (item && insert_item(*item)) return;
    load_history();
}

bool ClipboardController::insert_item(const ClipboardItem &item) {
    if (is_loading_ || !can_insert_incrementally()) return false;
    // A repeated copy is moved to the top by the backend instead of added; its old row is still in the list at another place.
    for (const auto &existing : current_items)
        if (existing.id == item.id) return false;
    const int row = first_unpinned_row();
    current_items.insert(row, item);
    // Pages are fetched by offset; one more row at the head moves the next page on, or the boundary row is fetched twice.
    current_offset_ += 1;
    emit item_inserted(item, row);
    return true;
}

bool ClipboardController::can_insert_incrementally() const { return !current_group_id && !search_text_ && !content_type_ && !time_range_; }

int ClipboardController::first_unpinned_row() const {
    // Sorted by is_pinned DESC, item_order DESC: new items go after the pinned block.
    for (qsizetype index = 0; index < current_items.size(); ++index)
        if (!current_items[index].is_pinned) return static_cast<int>(index);
    return static_cast<int>(current_items.size());
}

// ==================== Side bar overflow ====================

std::pair<QList<Group>, QList<Group>> ClipboardController::get_sidebar_overflow(int bar_size, bool is_top) {
    QList<Group> groups;
    for (const auto &group : get_groups())
        if (group.group_type != GroupType::HIDDEN) groups.append(group);
    const int cap = is_top ? calc_topbar_capacity(bar_size) : calc_sidebar_capacity(bar_size);
    if (cap == -1 || groups.size() <= cap) return {groups, {}};

    QList<Group> visible = groups.mid(0, cap);
    QList<Group> hidden = groups.mid(cap);
    // The current group stays visible.
    if (current_group_id && !visible.isEmpty()) {
        bool shown = false;
        for (const auto &group : visible) shown = shown || group.id == *current_group_id;
        if (!shown) {
            for (qsizetype i = 0; i < hidden.size(); ++i) {
                if (hidden[i].id != *current_group_id) continue;
                const Group selected = hidden.takeAt(i);
                hidden.insert(0, visible.back());
                visible.back() = selected;
                break;
            }
        }
    }
    return {visible, hidden};
}

// ==================== Menu data ====================

std::optional<ContextMenuData> ClipboardController::build_context_menu_data(qint64 item_id) { return context_menu_controller_.build_context_menu_data(item_id); }

QList<MenuAction> ClipboardController::build_group_context_menu_data(qint64 group_id) { return context_menu_controller_.build_group_context_menu_data(group_id); }

std::optional<ContextMenuData> ClipboardController::build_marked_context_menu_data(const QList<qint64> &item_ids) {
    return context_menu_controller_.build_marked_context_menu_data(item_ids);
}
}
