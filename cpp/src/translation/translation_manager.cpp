#include "translation/translation_manager.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/platform_utils.h"
#include "core/ui_theme.h"
#include "ocr/ocr_manager.h"
#include "settings/tool_settings.h"
#include "translation/service.h"
#include "translation/translation_dialog.h"
#include "translation/translation_popup.h"
#include "translation/worker.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <stdexcept>

namespace jietuba {
namespace {
// Service names appear on the translation window's badge and share its context.
QString tr_text(const char *text) { return i18n_tr(text, "TranslationDialog"); }

TranslationManager *&singleton() {
    static TranslationManager *manager = nullptr;
    return manager;
}

// text[:50] in code points.
QString preview(const QString &text) {
    const auto code_points = text.toUcs4();
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(code_points.constData()), std::min<qsizetype>(code_points.size(), 50));
}
}

void TranslationOcrThread::run() {
    try {
        if (cancelled_) return;
        if (!is_ocr_available()) {
            emit finished_signal(false, tr_text("OCR is not available"));
            image_ = QImage();
            return;
        }
        // Full result with coordinates, so lines come out in reading order.
        const auto result = recognize_text(image_);
        if (cancelled_) {
            image_ = QImage();
            return;
        }
        if (result.code == 100) {
            // Reading order, same-line fragments merged.
            const auto text = format_ocr_result_text(result);
            if (!text.trimmed().isEmpty()) emit finished_signal(true, text);
            else emit finished_signal(false, tr_text("No text recognized"));
        } else {
            emit finished_signal(false, tr_text("No text recognized"));
        }
    } catch (const std::exception &e) {
        emit finished_signal(false, tr_text("OCR failed:") + " " + QString::fromUtf8(e.what()));
    }
    image_ = QImage();
}

TranslationManager::TranslationManager(std::shared_ptr<TranslationService> translation_service) {
    if (!translation_service) translation_service = create_default_translation_service();
    translation_service_ = std::move(translation_service);
    connect(get_ui_theme(), &UIThemeManager::theme_changed, this, &TranslationManager::on_ui_theme_changed);
    log_debug(T("TranslationManager 已初始化"), "Translation");
}

TranslationManager *TranslationManager::instance() {
    if (!singleton()) singleton() = new TranslationManager();
    return singleton();
}

bool TranslationManager::has_instance() { return singleton() != nullptr; }

void TranslationManager::cleanup() {
    if (singleton()) {
        singleton()->shutdown();
        delete singleton();
        singleton() = nullptr;
        log_debug(T("TranslationManager 已清理"), "Translation");
    }
}

QString TranslationManager::api_key_error() { return QCoreApplication::translate("TranslationDialog", "API key not configured"); }

void TranslationManager::resolve_api_key(const QVariantMap &params) {
    if (!params.contains("api_key")) return;
    legacy_provider_override_ = true;
    api_key_ = params.value("api_key").toString();
}

void TranslationManager::apply_request_params(const QVariantMap &params) {
    if (params.contains("use_pro")) use_pro_ = params.value("use_pro").toBool();
    if (params.contains("split_sentences")) split_sentences_ = params.value("split_sentences").toString();
    if (params.contains("preserve_formatting")) preserve_formatting_ = params.value("preserve_formatting").toBool();
}

QVariantMap TranslationManager::provider_overrides() const {
    if (translation_service_->active_provider_id() != "deepl") return {};
    if (!legacy_provider_override_ && api_key_.isEmpty()) return {};
    return {{"api_key", api_key_}, {"use_pro", use_pro_}};
}

bool TranslationManager::backend_ready() const { return translation_service_->is_configured({}, provider_overrides()); }

QString TranslationManager::backend_name() const {
    try {
        return translation_service_->provider_label([](const QString &text) { return i18n_tr(text, "TranslationDialog"); }, {}, provider_overrides());
    } catch (const std::invalid_argument &) {
        return tr_text("Engine not configured");
    }
}

void TranslationManager::activate_surface(const QString &target) {
    active_target_ = target;
    if (target == "dialog") {
        if (is_popup_valid()) popup_->hide();
    } else if (target == "compact") {
        if (is_dialog_valid()) dialog_->hide();
    }
}

QString TranslationManager::current_theme_name() const { return get_ui_theme()->is_dark() ? "dark" : "light"; }

void TranslationManager::on_ui_theme_changed(const UIThemeTokens &tokens) {
    // Refreshes translation surfaces already created.
    const QString theme_name = tokens.is_dark() ? "dark" : "light";
    if (is_dialog_valid()) dialog_->set_theme(theme_name);
    if (is_popup_valid()) popup_->set_theme(theme_name);
}

void TranslationManager::configure(const QString &api_key, bool use_pro, const QString &split_sentences, bool preserve_formatting) {
    api_key_ = api_key;
    use_pro_ = use_pro;
    legacy_provider_override_ = true;
    split_sentences_ = split_sentences;
    preserve_formatting_ = preserve_formatting;
    log_debug(T("翻译服务已配置 (Pro: {use_pro}, split_sentences: {split_sentences}, preserve_formatting: {preserve_formatting})",
                {{"use_pro", use_pro}, {"split_sentences", split_sentences}, {"preserve_formatting", preserve_formatting}}),
              "Translation");
}

void TranslationManager::translate(const QString &text, std::optional<QPoint> position, const QVariantMap &params) {
    resolve_api_key(params);
    apply_request_params(params);
    const auto target_lang = params.value("target_lang", "ZH").toString();
    const auto source_lang = params.value("source_lang").toString();
    target_lang_ = target_lang;

    stop_current_thread();
    activate_surface("dialog");
    ensure_dialog(text, position, source_lang, target_lang);

    // With text, translate; otherwise only show the window.
    if (!text.trimmed().isEmpty()) {
        if (!backend_ready()) {
            log_error(T("翻译引擎未配置"), "Translation");
            if (is_dialog_valid()) dialog_->set_translation_error(api_key_error());
            return;
        }
        log_info(T("开始翻译: {text_preview}...", {{"text_preview", preview(text)}}), "Translation");
        emit translation_started(text);
        // The target language the user picked in the window wins over the default.
        auto actual_target_lang = target_lang;
        if (is_dialog_valid()) {
            const auto chosen = dialog_->get_target_lang();
            if (!chosen.isEmpty()) actual_target_lang = chosen;
        }
        start_translation(text, actual_target_lang, source_lang.isEmpty() ? std::nullopt : std::optional<QString>(source_lang), "dialog");
    } else {
        log_info(T("打开翻译窗口（待用户输入）"), "Translation");
        if (is_dialog_valid()) {
            if (backend_ready()) dialog_->target_edit->clear();
            else dialog_->set_translation_error(api_key_error());
            dialog_->show();
            dialog_->raise();
            dialog_->activateWindow();
            QPointer<SourceEdit> edit = dialog_->source_edit;
            QTimer::singleShot(0, edit, [edit] { edit->setFocus(); });
        }
    }
}

void TranslationManager::translate_compact(const QString &raw_text, std::optional<QPoint> position, const QVariantMap &params) {
    const auto text = raw_text.trimmed();
    if (text.isEmpty()) {
        translate({}, position, params);
        return;
    }
    resolve_api_key(params);
    apply_request_params(params);
    const auto target_lang = params.value("target_lang", "ZH").toString();
    const auto source_lang = params.value("source_lang").toString();
    target_lang_ = target_lang;

    stop_current_thread();
    activate_surface("compact");
    auto *popup = ensure_popup();
    popup->set_target_lang(target_lang);
    popup->set_backend_status(backend_name(), backend_ready());
    // Selection translation must not take focus, or the source application's
    // selection and caret are lost.
    popup->show_popup(text, position, false);

    if (!backend_ready()) {
        popup->show_error(api_key_error());
        return;
    }
    log_info(T("开始划词翻译: {text_preview}...", {{"text_preview", preview(text)}}), "Translation");
    emit translation_started(text);
    start_translation(text, target_lang, source_lang.isEmpty() ? std::nullopt : std::optional<QString>(source_lang), "compact");
}

void TranslationManager::open_compact_input(std::optional<QPoint> position, const QVariantMap &params) {
    resolve_api_key(params);
    apply_request_params(params);
    const auto target_lang = params.value("target_lang", "ZH").toString();
    target_lang_ = target_lang;

    stop_current_thread();
    activate_surface("compact");
    auto *popup = ensure_popup();
    popup->set_target_lang(target_lang);
    popup->set_backend_status(backend_name(), backend_ready());
    // Nothing to translate yet: focus goes straight to the input box.
    popup->show_popup({}, position, true);
    if (!backend_ready()) popup->show_error(api_key_error());
}

void TranslationManager::on_popup_input_changed() {
    // Invalidates an in-flight result as soon as the typed text changes.
    if (active_target_ == "compact" && thread_) stop_current_thread();
}

void TranslationManager::on_popup_translate_requested(const QString &raw_text) {
    const auto text = raw_text.trimmed();
    if (text.isEmpty()) return;
    stop_current_thread();
    activate_surface("compact");
    if (!backend_ready()) {
        if (is_popup_valid()) popup_->show_error(api_key_error());
        return;
    }
    log_info(T("开始小窗输入翻译: {text_preview}...", {{"text_preview", preview(text)}}), "Translation");
    emit translation_started(text);
    // The popup's currently selected target language.
    const auto target_lang = is_popup_valid() ? popup_->current_target_lang() : target_lang_;
    start_translation(text, target_lang, QString("auto"), "compact");
}

void TranslationManager::on_popup_target_lang_changed(const QString &new_lang) {
    target_lang_ = new_lang;
    get_tool_settings_manager()->set_app_setting("translation_target_lang", new_lang);
    log_debug(T("小窗目标语言已更新: {new_lang}", {{"new_lang", new_lang}}), "Translation");
}

TranslationPopup *TranslationManager::ensure_popup() {
    // Created lazily and reused so later calls are fast.
    if (!is_popup_valid()) {
        popup_ = new TranslationPopup();
        popup_->set_theme(current_theme_name());
        connect(popup_, &TranslationPopup::open_full_requested, this, &TranslationManager::open_full_from_popup);
        connect(popup_, &TranslationPopup::manual_input_changed, this, &TranslationManager::on_popup_input_changed);
        connect(popup_, &TranslationPopup::manual_translate_requested, this, &TranslationManager::on_popup_translate_requested);
        connect(popup_, &TranslationPopup::target_lang_changed, this, &TranslationManager::on_popup_target_lang_changed);
        popup_->set_target_lang(target_lang_);
    }
    return popup_;
}

void TranslationManager::open_full_from_popup(const QString &source_text, const QString &translated_text, const QString &error_text) {
    activate_surface("dialog");
    if (thread_ && thread_->isRunning()) request_targets_[request_token_] = "dialog";
    const auto target_lang = get_tool_settings_manager()->get_translation_target_lang();
    ensure_dialog(source_text, std::nullopt, "auto", target_lang);
    if (!is_dialog_valid()) return;
    if (!translated_text.isEmpty()) dialog_->set_translation_result(translated_text);
    else if (!error_text.isEmpty()) dialog_->set_translation_error(error_text);
    else if (thread_ && thread_->isRunning()) dialog_->set_loading();
    else dialog_->target_edit->clear();
    dialog_->show();
    dialog_->raise();
    dialog_->activateWindow();
}

void TranslationManager::ensure_dialog(const QString &text, std::optional<QPoint> position, const QString &source_lang, const QString &target_lang) {
    if (!is_dialog_valid()) {
        log_debug(T("创建新翻译窗口"), "Translation");
        dialog_ = new TranslationLoadingDialog(text, nullptr, position, source_lang.isEmpty() ? QString("auto") : source_lang, target_lang);
        dialog_->set_theme(current_theme_name());
        connect(dialog_, &QObject::destroyed, this, &TranslationManager::on_dialog_destroyed);
        // (text, source_lang, target_lang)
        connect(dialog_, &TranslationDialog::translate_requested, this, &TranslationManager::on_translate_requested);
        dialog_->set_backend_badge(backend_name(), backend_ready());
        dialog_->show();
    } else {
        log_debug(T("复用现有翻译窗口"), "Translation");
        // The target language is left alone: keep the user's choice in the combo box.
        dialog_->update_content(text, {}, source_lang.isEmpty() ? QString("auto") : source_lang);
        // Loading state only when there is text.
        if (!text.trimmed().isEmpty()) dialog_->set_loading();
        dialog_->set_backend_badge(backend_name(), backend_ready());
        dialog_->show();
        dialog_->raise();
        dialog_->activateWindow();
    }
}

void TranslationManager::start_translation(const QString &text, const QString &target_lang, const std::optional<QString> &source_lang, const QString &result_target) {
    const auto provider_name = backend_name();
    log_info(T("调用翻译引擎 {provider_name}: target={target_lang}, preserve_formatting={preserve_formatting}",
               {{"provider_name", provider_name}, {"target_lang", target_lang}, {"preserve_formatting", preserve_formatting_}}),
             "Translation");

    request_token_ += 1;
    const int token = request_token_;
    request_targets_[token] = result_target;
    const TranslationRequest request(text, target_lang, source_lang, preserve_formatting_, 10, {{"split_sentences", split_sentences_}});
    auto *thread = new TranslationWorker(*translation_service_, request, {}, provider_overrides());
    thread_ = thread;
    threads_.insert(thread);
    connect(thread, &TranslationWorker::finished_signal, this, [this, thread, token](const TranslationResult &result) {
        on_thread_result(thread, token, result.success, result.translated_text, result.error_message, result.detected_source_lang);
    });
    connect(thread, &QThread::finished, this, [this, thread] { release_thread(thread); });
    thread->start();
}

void TranslationManager::stop_current_thread() {
    const int old_token = request_token_;
    request_token_ += 1;
    request_targets_.remove(old_token);
    auto *thread = thread_;
    thread_ = nullptr;
    // Its result would be ignored anyway; aborting frees the thread now rather than at the timeout.
    if (thread && thread->isRunning()) thread->abort();
}

void TranslationManager::on_thread_result(TranslationWorker *thread, int token, bool success, const QString &translated_text, const QString &error,
                                          const QString &detected_lang) {
    if (token != request_token_ || thread != thread_) {
        request_targets_.remove(token);
        log_debug(T("忽略已被新请求替代的翻译结果"), "Translation");
        return;
    }
    const auto result_target = request_targets_.contains(token) ? request_targets_.take(token) : active_target_;
    on_translation_finished(success, translated_text, error, detected_lang, result_target);
}

void TranslationManager::release_thread(TranslationWorker *thread) {
    threads_.remove(thread);
    if (thread_ == thread) thread_ = nullptr;
    thread->deleteLater();
}

void TranslationManager::on_translation_finished(bool success, const QString &translated_text, const QString &error, const QString &detected_lang,
                                                 const QString &result_target) {
    log_debug(T("翻译完成: success={success}, detected_lang={detected_lang}", {{"success", success}, {"detected_lang", detected_lang}}), "Translation");
    if (result_target == "compact" && is_popup_valid()) {
        if (success) popup_->show_result(translated_text, detected_lang);
        else popup_->show_error(error.isEmpty() ? tr_text("Translation failed") : error);
    } else if (result_target == "dialog" && is_dialog_valid()) {
        dialog_->on_translation_finished(success, translated_text, error, detected_lang);
    }
    emit translation_finished(success, translated_text, error);
}

void TranslationManager::on_translate_requested(const QString &text, const QString &source_lang, const QString &target_lang) {
    // From the window's translate button.
    activate_surface("dialog");
    if (!backend_ready()) {
        if (is_dialog_valid()) dialog_->set_translation_error(api_key_error());
        return;
    }
    if (text.trimmed().isEmpty()) {
        if (is_dialog_valid()) dialog_->set_translation_error(tr_text("Please enter text to translate"));
        return;
    }
    log_debug(T("翻译请求: -> {target_lang}", {{"target_lang", target_lang}}), "Translation");
    stop_current_thread();
    start_translation(text, target_lang, source_lang, "dialog");
}

void TranslationManager::on_dialog_destroyed() {
    log_debug(T("翻译窗口已关闭，清理资源"), "Translation");
    dialog_ = nullptr;
    if (active_target_ == "dialog") stop_current_thread();
}

void TranslationManager::close_dialog() {
    if (is_dialog_valid()) dialog_->close();
    dialog_ = nullptr;
    // The popup is kept for reuse while open; once dropped here nothing else owns it.
    if (is_popup_valid()) {
        popup_->close();
        popup_->deleteLater();
    }
    popup_ = nullptr;
    stop_current_thread();
}

int TranslationManager::network_shutdown_budget_ms(const QList<TranslationWorker *> &threads) const {
    int budget = 0;
    for (auto *thread : threads)
        if (thread->isRunning()) budget = std::max(budget, thread->effective_timeout_ms());
    return budget ? budget + SHUTDOWN_SLACK_MS : 0;
}

void TranslationManager::shutdown(std::optional<int> timeout_ms, int ocr_timeout_ms) {
    close_dialog();
    const auto threads = threads_.values();
    for (auto *thread : threads)
        if (thread->isRunning()) thread->abort();

    // Aborted requests end at once; the budget only caps a request that does not.
    const int budget = timeout_ms.value_or(network_shutdown_budget_ms(threads));
    QElapsedTimer clock;
    clock.start();
    for (auto *thread : threads) {
        if (!thread->isRunning()) continue;
        const int remaining_ms = std::max<qint64>(0, budget - clock.elapsed());
        if (remaining_ms && thread->wait(remaining_ms)) continue;
        if (thread->isRunning()) log_warning(T("退出时翻译网络线程未在期限内结束"), "Translation");
    }

    // The OCR thread must be waited for as well: process teardown racing a thread
    // still inside the native recognizer aborts the process. cancel() only sets a
    // flag and cannot interrupt a running recognition; the wait is what makes exit
    // safe. Local recognition is usually far faster than a network request, so the
    // budget is shorter.
    if (ocr_thread_ && ocr_thread_->isRunning()) {
        ocr_thread_->cancel();
        if (!ocr_thread_->wait(std::max(0, ocr_timeout_ms))) log_warning(T("退出时 OCR 线程未在期限内结束"), "Translation");
    }
}

bool TranslationManager::is_dialog_open() const { return is_dialog_valid() && dialog_->isVisible(); }

void TranslationManager::translate_from_image(const QPixmap &pixmap, const QVariantMap &params) {
    begin_ocr_translation(std::nullopt, params);
    // Keeps the image until the OCR thread is done.
    pending_pixmap_ = pixmap;
    start_ocr_thread(pixmap);
}

void TranslationManager::begin_ocr_translation(std::optional<QPoint> position, const QVariantMap &params) {
    resolve_api_key(params);
    apply_request_params(params);
    const auto target_lang = params.value("target_lang", "ZH").toString();
    // Used once OCR completes.
    pending_target_lang_ = target_lang;
    stop_current_thread();
    activate_surface("dialog");

    log_info(T("OCR 翻译模式：显示窗口并等待识别结果"), "Translation");
    ensure_dialog({}, position, "auto", target_lang);

    if (is_dialog_valid()) {
        dialog_->source_edit->setPlainText(i18n_tr("Recognizing...", "TranslationLoadingDialog"));
        // No editing while recognizing.
        dialog_->source_edit->setEnabled(false);
    }
}

void TranslationManager::start_ocr_thread(const QPixmap &pixmap) {
    // QPixmap must stay on the GUI thread; the worker gets a deep-copied QImage.
    if (pixmap.isNull()) {
        log_debug(T("传入 pixmap 为空，跳过OCR"), "Translation");
        return;
    }
    const auto image = pixmap.toImage().copy();
    if (image.isNull()) {
        log_debug(T("QImage 转换失败，跳过OCR"), "Translation");
        return;
    }
    // An older thread's result is dropped by disconnecting it; it is left to finish
    // in the background, never terminated or waited for.
    if (ocr_thread_ && ocr_thread_->isRunning()) {
        ocr_thread_->cancel();
        disconnect(ocr_thread_, &TranslationOcrThread::finished_signal, nullptr, nullptr);
    }
    auto *thread = new TranslationOcrThread(image);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &TranslationOcrThread::finished_signal, this, &TranslationManager::on_ocr_finished);
    ocr_thread_ = thread;
    thread->start();
    log_debug(T("OCR线程已启动"), "Translation");
}

void TranslationManager::on_ocr_finished(bool success, const QString &result) {
    log_debug(T("OCR完成: success={success}, result_len={result_len}", {{"success", success}, {"result_len", static_cast<int>(result.toUcs4().size())}}),
              "Translation");
    pending_pixmap_ = QPixmap();
    request_trim_working_set();

    if (!is_dialog_valid()) {
        log_debug(T("翻译窗口已关闭，忽略OCR结果"), "Translation");
        return;
    }
    dialog_->source_edit->setEnabled(true);
    if (success && !result.isEmpty()) {
        dialog_->source_edit->setPlainText(result);
        log_info(T("OCR识别成功: {result_preview}...", {{"result_preview", preview(result)}}), "Translation");
        const auto chosen = dialog_->get_target_lang();
        start_translation(result, chosen.isEmpty() ? pending_target_lang_ : chosen, QString("auto"), "dialog");
    } else {
        dialog_->source_edit->setPlainText("");
        dialog_->source_edit->setPlaceholderText(result.isEmpty() ? tr_text("Recognition failed") : result);
        log_error(T("OCR识别失败: {result}", {{"result", result}}), "Translation");
    }
}
}
