#pragma once
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <optional>

namespace jietuba {
class TranslationLoadingDialog;
class TranslationPopup;
class TranslationService;
class TranslationWorker;
struct UIThemeTokens;

// OCR for screenshot translation. Holds only a QImage (a value type), never a widget.
class TranslationOcrThread : public QThread {
    Q_OBJECT
public:
    explicit TranslationOcrThread(const QImage &image) : image_(image) {}
    // Requests cancellation; the recognition itself is one synchronous native call
    // that cannot be interrupted, so its result is dropped instead.
    void cancel() { cancelled_ = true; }
signals:
    // (success, recognized text or error message)
    void finished_signal(bool success, const QString &result);
protected:
    void run() override;
private:
    QImage image_;
    std::atomic_bool cancelled_ = false;
};

// Owns the single translation window and compact popup, so pin windows and
// screenshots need not manage them: windows are reused while open and released
// when closed.
//
// The params maps carry the optional keyword arguments of each call: api_key,
// target_lang (default "ZH"), source_lang, use_pro, split_sentences and
// preserve_formatting.
class TranslationManager : public QObject {
    Q_OBJECT
public:
    explicit TranslationManager(std::shared_ptr<TranslationService> translation_service = nullptr);
    static TranslationManager *instance();
    static bool has_instance();
    // Clears the singleton on exit.
    static void cleanup();

    // Configures the DeepL service for legacy callers. split_sentences: "0" none,
    // "1" automatic, "nonewlines" ignores line breaks.
    void configure(const QString &api_key, bool use_pro = false, const QString &split_sentences = "nonewlines", bool preserve_formatting = true);
    // Translates in the full window, creating or reusing it; empty text only shows it.
    void translate(const QString &text, std::optional<QPoint> position = std::nullopt, const QVariantMap &params = {});
    // Translates selected text in the compact result popup.
    void translate_compact(const QString &text, std::optional<QPoint> position = std::nullopt, const QVariantMap &params = {});
    // Shows the compact popup empty, with the caret in the input box.
    void open_compact_input(std::optional<QPoint> position = std::nullopt, const QVariantMap &params = {});
    // Shows the window at once, recognizes the image in the background, fills in the
    // source text and translates it.
    void translate_from_image(const QPixmap &pixmap, const QVariantMap &params = {});
    // Shows the OCR translation state immediately, for internal or external OCR tasks.
    void begin_ocr_translation(std::optional<QPoint> position = std::nullopt, const QVariantMap &params = {});
    // Feeds any OCR task's result into the same window and translates it.
    void complete_ocr_translation(bool success, const QString &result) { on_ocr_finished(success, result); }
    void close_dialog();
    // Closes the translation UI and lets network and OCR workers finish before exit.
    // timeout_ms defaults to the longest running request's timeout plus slack.
    void shutdown(std::optional<int> timeout_ms = std::nullopt, int ocr_timeout_ms = 3000);
    bool is_dialog_open() const;

signals:
    void translation_started(const QString &text);
    void translation_finished(bool success, const QString &translated_text, const QString &error);

private:
    static constexpr int SHUTDOWN_SLACK_MS = 1000;
    static constexpr int FALLBACK_NETWORK_TIMEOUT_MS = 10000;
    static QString api_key_error();
    // Legacy callers may pass a key; new callers let the service read the provider config.
    void resolve_api_key(const QVariantMap &params);
    // Applies use_pro / split_sentences / preserve_formatting from params, keeping
    // the configured values where absent.
    void apply_request_params(const QVariantMap &params);
    // Temporary DeepL overrides, only for legacy callers.
    QVariantMap provider_overrides() const;
    bool backend_ready() const;
    QString backend_name() const;
    // Keeps the full editor and compact popup mutually exclusive.
    void activate_surface(const QString &target);
    QString current_theme_name() const;
    void on_ui_theme_changed(const UIThemeTokens &tokens);
    void on_popup_input_changed();
    void on_popup_translate_requested(const QString &text);
    void on_popup_target_lang_changed(const QString &new_lang);
    TranslationPopup *ensure_popup();
    bool is_popup_valid() const { return !popup_.isNull(); }
    // Promotes compact content to the full editor without requesting it again.
    void open_full_from_popup(const QString &source_text, const QString &translated_text, const QString &error_text);
    void ensure_dialog(const QString &text, std::optional<QPoint> position, const QString &source_lang, const QString &target_lang);
    bool is_dialog_valid() const { return !dialog_.isNull(); }
    void start_translation(const QString &text, const QString &target_lang, const std::optional<QString> &source_lang, const QString &result_target);
    // Invalidates the active request without blocking the GUI thread.
    void stop_current_thread();
    // Drops superseded results and delivers only the latest request's.
    void on_thread_result(TranslationWorker *thread, int token, bool success, const QString &translated_text, const QString &error,
                          const QString &detected_lang);
    void release_thread(TranslationWorker *thread);
    void on_translation_finished(bool success, const QString &translated_text, const QString &error, const QString &detected_lang, const QString &result_target);
    void on_translate_requested(const QString &text, const QString &source_lang, const QString &target_lang);
    void on_dialog_destroyed();
    int network_shutdown_budget_ms(const QList<TranslationWorker *> &threads) const;
    void start_ocr_thread(const QPixmap &pixmap);
    // Handles screenshot and pin OCR results alike.
    void on_ocr_finished(bool success, const QString &result);

    QPointer<TranslationLoadingDialog> dialog_;
    QPointer<TranslationPopup> popup_;
    TranslationWorker *thread_ = nullptr;
    // Keeps superseded network workers alive until they exit.
    QSet<TranslationWorker *> threads_;
    int request_token_ = 0;
    QHash<int, QString> request_targets_;
    QString active_target_ = "dialog";
    QString target_lang_ = "ZH";
    QString api_key_;
    bool use_pro_ = false;
    bool legacy_provider_override_ = false;
    QString split_sentences_ = "nonewlines";
    bool preserve_formatting_ = true;
    QString pending_target_lang_ = "ZH";
    QPixmap pending_pixmap_;
    QPointer<TranslationOcrThread> ocr_thread_;
    std::shared_ptr<TranslationService> translation_service_;
};
}
