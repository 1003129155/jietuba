#include "translation/smart_translation_controller.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include "translation/translation_manager.h"
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QTimer>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba {
namespace {
int char_count(const QString &text) { return static_cast<int>(text.toUcs4().size()); }
}

SmartTranslationController::SmartTranslationController(QObject *parent) : QObject(parent), config_(get_tool_settings_manager()) {}

void SmartTranslationController::trigger() {
    probe_token_ += 1;
    const int token = probe_token_;
    probe_active_ = true;
    copy_dispatched_ = false;
    cursor_position_ = QCursor::pos();
    log_debug(T("智能翻译探测开始: token={token}", {{"token", token}}), "Translation");
    QTimer::singleShot(COPY_DELAY_MS, this, [this, token] { dispatch_copy(token); });
    QTimer::singleShot(PROBE_TIMEOUT_MS, this, [this, token] { on_timeout(token); });
}

void SmartTranslationController::translate_selection(const QString &raw_text) {
    const auto text = raw_text.trimmed();
    if (text.isEmpty()) return;
    probe_token_ += 1;
    probe_active_ = false;
    copy_dispatched_ = false;
    cursor_position_ = QCursor::pos();
    log_debug(T("外部选区直接翻译: {char_count} 字符", {{"char_count", char_count(text)}}), "Translation");
    open_compact(text);
}

void SmartTranslationController::dispatch_copy(int token) {
    if (!is_current(token)) return;
    copy_dispatched_ = true;
    send_copy_shortcut();
    if (!(clipboard_monitor_available && clipboard_monitor_available()))
        QTimer::singleShot(FALLBACK_READ_DELAY_MS, this, [this, token] { read_clipboard_fallback(token); });
}

void SmartTranslationController::read_clipboard_fallback(int token) {
    // Reads once when clipboard history monitoring is intentionally unavailable.
    if (!is_current(token)) return;
    const auto text = QApplication::clipboard()->text().trimmed();
    if (!text.isEmpty()) {
        probe_active_ = false;
        log_debug(T("剪贴板监听未启用，直接读取当前文本: {char_count} 字符", {{"char_count", char_count(text)}}), "Translation");
        open_compact(text);
        return;
    }
    open_compact_input(token, "empty-clipboard");
}

void SmartTranslationController::send_copy_shortcut() {
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event(VK_INSERT, 0, 0, 0);
    keybd_event(VK_INSERT, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
}

void SmartTranslationController::on_clipboard_item(const QString &content_type, const QString &content) {
    if (!probe_active_ || !copy_dispatched_) return;
    const int token = probe_token_;
    const auto text = content_type == "text" ? content.trimmed() : QString();
    if (content_type == "text" && !text.isEmpty()) {
        probe_active_ = false;
        log_debug(T("智能翻译获取文本成功: {char_count} 字符", {{"char_count", char_count(text)}}), "Translation");
        open_compact(text);
        return;
    }
    log_debug(T("智能翻译忽略非文本内容: {content_type_display}", {{"content_type_display", content_type.isEmpty() ? QString("unknown") : content_type}}),
              "Translation");
    open_compact_input(token, "non-text");
}

void SmartTranslationController::on_timeout(int token) {
    if (!is_current(token)) return;
    open_compact_input(token, "timeout");
}

QVariantMap SmartTranslationController::translation_params() const { return config_->get_translation_request_params(); }

void SmartTranslationController::open_compact(const QString &text) {
    TranslationManager::instance()->translate_compact(text, cursor_position_, translation_params());
}

void SmartTranslationController::open_compact_input(int token, const QString &reason) {
    if (!is_current(token)) return;
    probe_active_ = false;
    log_debug(T("智能翻译转入小窗手动输入: {reason}", {{"reason", reason}}), "Translation");
    TranslationManager::instance()->open_compact_input(cursor_position_, translation_params());
}
}
