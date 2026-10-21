#include "core/ui_theme.h"
#include "settings/tool_settings.h"
#include "translation/provider.h"
#include "translation/providers/volcengine.h"
#include "translation/registry.h"
#include "translation/service.h"
#include "translation/worker.h"
#include "translation/smart_translation_controller.h"
#include "translation/translation_dialog.h"
#include "translation/translation_manager.h"
#include "translation/translation_popup.h"
#include "translation/ui/widgets.h"
#include "ui/fluent_lite/inputs.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDate>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <atomic>

using namespace jietuba;

namespace {
std::atomic_int provider_calls = 0;
std::atomic_int provider_delay_ms = 0;

// Answers "[target] text" without a network; text containing "fail" fails.
class FakeProvider : public TranslationProvider {
public:
    explicit FakeProvider(const ProviderConfig &config) : config_(config) { min_timeout = 2; }
    QString provider_id() const override { return "fake"; }
    QString display_name() const override { return "Fake Engine"; }
    bool is_configured() const override { return !config_.value("api_key").toString().isEmpty(); }
    TranslationResult translate(const TranslationRequest &request) override {
        ++provider_calls;
        if (provider_delay_ms > 0) QThread::msleep(provider_delay_ms);
        if (request.text.contains("fail")) return TranslationResult::failure(TranslationErrorCode::NETWORK_ERROR, "network down");
        return TranslationResult{true, QString("[%1] %2").arg(request.target_lang, request.text), "EN", std::nullopt, {}};
    }
private:
    ProviderConfig config_;
};

class FakeConfig : public TranslationConfig {
public:
    QString api_key = "secret";
    QString get_translation_provider() const override { return "fake"; }
    QVariantMap get_translation_provider_config(const QString &) const override { return {{"api_key", api_key}}; }
};

std::atomic_int quota_requests = 0;
std::atomic<QThread *> quota_request_thread = nullptr;

// Volcengine with its declared quota but without a network.
class QuotaProvider : public TranslationProvider {
public:
    explicit QuotaProvider(const ProviderConfig &) { quota_limit_chars = VolcengineTranslateProvider::QUOTA_LIMIT_CHARS; }
    QString provider_id() const override { return "volcengine"; }
    QString display_name() const override { return "Volcengine"; }
    bool is_configured() const override { return true; }
    TranslationResult translate(const TranslationRequest &) override {
        ++quota_requests;
        quota_request_thread = QThread::currentThread();
        return TranslationResult{true, "hi", "zh-Hans", std::nullopt, {}};
    }
};

// The settings configuration, noting the thread each usage write runs on.
class RecordingConfig : public TranslationConfig {
public:
    explicit RecordingConfig(std::shared_ptr<TranslationConfig> inner) : inner_(std::move(inner)) {}
    QString get_translation_provider() const override { return inner_->get_translation_provider(); }
    QVariantMap get_translation_provider_config(const QString &provider_id) const override { return inner_->get_translation_provider_config(provider_id); }
    qint64 get_translation_usage_chars(const QString &provider_id) const override { return inner_->get_translation_usage_chars(provider_id); }
    void add_translation_usage_chars(const QString &provider_id, qint64 chars) override {
        add_threads << QThread::currentThread();
        inner_->add_translation_usage_chars(provider_id, chars);
    }
    bool get_translation_quota_enabled(const QString &provider_id) const override { return inner_->get_translation_quota_enabled(provider_id); }
    qint64 get_translation_quota_limit(const QString &provider_id) const override { return inner_->get_translation_quota_limit(provider_id); }
    QList<QThread *> add_threads;
private:
    std::shared_ptr<TranslationConfig> inner_;
};

// The default service's settings configuration over a registry holding QuotaProvider.
std::shared_ptr<TranslationService> quota_service(std::shared_ptr<TranslationConfig> config = nullptr) {
    auto registry = std::make_shared<ProviderRegistry>();
    registry->register_provider("volcengine", [](const ProviderConfig &config) { return std::make_unique<QuotaProvider>(config); });
    return std::make_shared<TranslationService>(registry, config ? config : create_default_translation_service()->config);
}

TranslationResult run_worker(const TranslationService &service, const QString &text = QString::fromUtf8("你好世界")) {
    TranslationWorker worker(service, TranslationRequest(text, "EN"), "volcengine");
    std::optional<TranslationResult> received;
    QObject receiver;
    QObject::connect(&worker, &TranslationWorker::finished_signal, &receiver, [&received](const TranslationResult &result) { received = result; });
    worker.start();
    const bool finished = QTest::qWaitFor([&received] { return received.has_value(); }, 5000);
    worker.wait(3000);
    // The bookkeeping is queued to this thread ahead of the result.
    QCoreApplication::processEvents();
    return finished ? *received : TranslationResult::failure(TranslationErrorCode::UNKNOWN, "worker timed out");
}

}

class TranslationUiTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    std::shared_ptr<FakeConfig> config_;
    std::unique_ptr<TranslationManager> manager_;

    TranslationManager *manager() {
        if (!manager_) {
            auto registry = std::make_shared<ProviderRegistry>();
            ProviderMetadata metadata;
            metadata.provider_id = "fake";
            metadata.display_name = "Fake Engine";
            registry->register_provider("fake", [](const ProviderConfig &config) { return std::make_unique<FakeProvider>(config); }, metadata);
            manager_ = std::make_unique<TranslationManager>(std::make_shared<TranslationService>(registry, config_));
        }
        return manager_.get();
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        get_tool_settings_manager(settings_);
    }

    void init() {
        // The windows remember the last chosen languages.
        settings_->clear();
        config_ = std::make_shared<FakeConfig>();
        provider_calls = 0;
        provider_delay_ms = 0;
        quota_requests = 0;
        quota_request_thread = nullptr;
    }

    void cleanup() {
        if (manager_) manager_->shutdown(3000);
        manager_.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ---- Monthly quota guard ----

    void quotaCredentialsMapToProviderConfig() {
        auto *config = get_tool_settings_manager();
        config->set_volcengine_translate_access_key("ak");
        config->set_volcengine_translate_secret_key("sk");
        config->set_tencent_translate_secret_id("  id  ");
        config->set_tencent_translate_secret_key("key");
        QCOMPARE(config->get_translation_provider_config("volcengine"), (QVariantMap{{"access_key_id", "ak"}, {"secret_access_key", "sk"}}));
        QCOMPARE(config->get_translation_provider_config("tencent"), (QVariantMap{{"secret_id", "id"}, {"secret_key", "key"}}));
    }

    void usageIsBucketedByMonth() {
        auto *config = get_tool_settings_manager();
        config->add_translation_usage_chars("volcengine", 10);
        config->add_translation_usage_chars("volcengine", 5);
        config->add_translation_usage_chars("volcengine", 0);
        config->add_translation_usage_chars("volcengine", -3);
        QCOMPARE(config->get_translation_usage_chars("volcengine"), qint64(15));
        const auto month = QDate::currentDate().toString("yyyyMM");
        QCOMPARE(settings_->value("translation/usage/" + month + "/volcengine").toLongLong(), qint64(15));
        // Other months' counters are kept but not read.
        settings_->setValue("translation/usage/" + QDate::currentDate().addMonths(-1).toString("yyyyMM") + "/volcengine", 999);
        QCOMPARE(config->get_translation_usage_chars("volcengine"), qint64(15));
        QCOMPARE(config->get_translation_usage_chars("tencent"), qint64(0));
    }

    void theServiceBlocksAtTheLimitWithoutARequest() {
        get_tool_settings_manager()->add_translation_usage_chars("volcengine", 1990000);
        const auto result = quota_service()->translate(TranslationRequest(QString::fromUtf8("你好"), "EN"), "volcengine");
        QCOMPARE(result.error_code, std::optional(TranslationErrorCode::QUOTA_EXCEEDED));
        QVERIFY(result.error_message.contains("1990000/1990000"));
        QCOMPARE(quota_requests.load(), 0);
    }

    void theServiceRecordsCodePointsAfterSuccess() {
        auto service = quota_service();
        QVERIFY(service->translate(TranslationRequest(QString::fromUtf8("你好世界"), "EN"), "volcengine").success);
        QCOMPARE(get_tool_settings_manager()->get_translation_usage_chars("volcengine"), qint64(4));
        // An emoji counts once, not as two UTF-16 units.
        QVERIFY(service->translate(TranslationRequest(QString::fromUtf8("\U0001F600a"), "EN"), "volcengine").success);
        QCOMPARE(get_tool_settings_manager()->get_translation_usage_chars("volcengine"), qint64(6));
    }

    void aCustomLimitAppliesAndTheGuardCanBeOff() {
        auto *config = get_tool_settings_manager();
        auto service = quota_service();
        config->set_volcengine_translate_quota_limit("10");
        config->add_translation_usage_chars("volcengine", 10);
        const TranslationRequest request(QString::fromUtf8("你好"), "EN");
        const auto blocked = service->translate(request, "volcengine");
        config->set_volcengine_translate_quota_enabled(false);
        const auto allowed = service->translate(request, "volcengine");
        QCOMPARE(blocked.error_code, std::optional(TranslationErrorCode::QUOTA_EXCEEDED));
        QVERIFY(allowed.success);
        QCOMPARE(quota_requests.load(), 1);
        // Counted with the guard off too, so the figure is right when it is back on.
        QCOMPARE(config->get_translation_usage_chars("volcengine"), qint64(12));
    }

    void providersWithoutAQuotaAreNotTracked() {
        auto service = create_default_translation_service();
        const auto deepl = service->provider("deepl");
        QVERIFY(!service->check_quota(*deepl));
        service->record_usage(*deepl, 100);
        QCOMPARE(get_tool_settings_manager()->get_translation_usage_chars("deepl"), qint64(0));
        // Both declared quotas are guarded.
        for (const QString provider_id : {"volcengine", "tencent"}) QVERIFY(service->provider(provider_id)->quota_limit_chars > 0);
    }

    void theWorkerBlocksAtTheLimitWithoutARequest() {
        get_tool_settings_manager()->add_translation_usage_chars("volcengine", 1990000);
        const auto result = run_worker(*quota_service());
        QCOMPARE(result.error_code, std::optional(TranslationErrorCode::QUOTA_EXCEEDED));
        QCOMPARE(quota_requests.load(), 0);
    }

    void theWorkerRequestsOffThreadAndRecordsOnTheGuiThread() {
        auto config = std::make_shared<RecordingConfig>(create_default_translation_service()->config);
        const auto result = run_worker(*quota_service(config));
        QVERIFY(result.success);
        QCOMPARE(quota_requests.load(), 1);
        QVERIFY(quota_request_thread.load() && quota_request_thread.load() != QThread::currentThread());
        // Compared as pointers only: the worker thread is gone by now.
        QCOMPARE(config->add_threads.size(), 1);
        QVERIFY(config->add_threads.first() == QThread::currentThread());
        QCOMPARE(get_tool_settings_manager()->get_translation_usage_chars("volcengine"), qint64(4));
    }

    // ---- The full window through the manager ----

    void translatingFillsTheWindowWithTheResult() {
        auto *m = manager();
        QSignalSpy started(m, &TranslationManager::translation_started);
        QSignalSpy finished(m, &TranslationManager::translation_finished);
        m->translate("hello", QPoint(20, 20));
        QVERIFY(m->is_dialog_open());
        QCOMPARE(started.count(), 1);
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toBool(), true);
        QCOMPARE(finished.first().at(1).toString(), QString("[zh-Hans] hello"));
        TranslationDialog *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationDialog *>(widget); found && found->isVisible()) window = found;
        QVERIFY(window);
        QCOMPARE(window->source_edit->toPlainText(), QString("hello"));
        QCOMPARE(window->target_edit->toPlainText(), QString("[zh-Hans] hello"));
        QVERIFY(!window->target_edit->property("error").toBool());
        QVERIFY(window->backend_badge->text().contains("Fake Engine"));

        // The window is reused for the next text.
        m->translate("again");
        QTRY_COMPARE(finished.count(), 2);
        QCOMPARE(window->target_edit->toPlainText(), QString("[zh-Hans] again"));
        int dialogs = 0;
        for (auto *widget : QApplication::topLevelWidgets()) dialogs += qobject_cast<TranslationDialog *>(widget) && widget->isVisible();
        QCOMPARE(dialogs, 1);
        m->close_dialog();
        QVERIFY(!m->is_dialog_open());
    }

    void failuresAndAMissingEngineShowInTheWindow() {
        auto *m = manager();
        QSignalSpy finished(m, &TranslationManager::translation_finished);
        m->translate("please fail");
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toBool(), false);
        TranslationDialog *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationDialog *>(widget); found && found->isVisible()) window = found;
        QVERIFY(window);
        QVERIFY(window->target_edit->property("error").toBool());
        QVERIFY(window->target_edit->toPlainText().contains("network down"));

        config_->api_key.clear();
        const int calls = provider_calls;
        m->translate("hello");
        QCOMPARE(provider_calls.load(), calls);
        QVERIFY(window->target_edit->toPlainText().contains("API key not configured"));
        QVERIFY(window->target_edit->property("error").toBool());
        // Opening the window empty still shows why nothing will translate.
        m->translate("");
        QVERIFY(window->target_edit->toPlainText().contains("API key not configured"));
    }

    void theWindowsTranslateButtonAndLanguagesDriveRequests() {
        auto *m = manager();
        QSignalSpy finished(m, &TranslationManager::translation_finished);
        m->translate("");
        TranslationDialog *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationDialog *>(widget); found && found->isVisible()) window = found;
        QVERIFY(window);
        window->source_edit->setPlainText("bonjour");
        QVERIFY(window->character_count->text().contains("7"));
        window->target_language->setCurrentIndex(window->target_language->findData("EN"));
        QCOMPARE(window->get_target_lang(), QString("EN"));
        window->translate_button->click();
        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(window->target_edit->toPlainText(), QString("[en] bonjour"));
        // Swapping moves the detected source into the target and the result into the source.
        window->source_language->setCurrentIndex(window->source_language->findData("JA"));
        window->swap_button->click();
        QCOMPARE(window->get_source_lang(), QString("EN"));
        QCOMPARE(window->get_target_lang(), QString("JA"));
        // Clearing empties the source.
        window->clear_source_button->click();
        QVERIFY(window->source_edit->toPlainText().isEmpty());
    }

    // ---- The compact popup through the manager ----

    // Closing the translation surfaces destroys the popup; the next request makes a new one.
    void closingTheSurfacesDestroysThePopup() {
        auto *m = manager();
        m->open_compact_input(QPoint(10, 10));
        QPointer<TranslationPopup> popup;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationPopup *>(widget); found && found->isVisible()) popup = found;
        QVERIFY(popup);
        m->close_dialog();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!popup);
    }

    void selectionTranslationUsesTheCompactPopup() {
        auto *m = manager();
        QSignalSpy finished(m, &TranslationManager::translation_finished);
        m->translate_compact("  selected text  ", QPoint(10, 10));
        TranslationPopup *popup = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationPopup *>(widget); found && found->isVisible()) popup = found;
        QVERIFY(popup);
        QVERIFY(!m->is_dialog_open());
        QCOMPARE(popup->source_edit->toPlainText(), QString("selected text"));
        // Selection translation never steals focus, but the text stays editable.
        QVERIFY(popup->testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY(!popup->source_edit->isReadOnly());
        QTRY_COMPARE(popup->result_edit->toPlainText(), QString("[zh-Hans] selected text"));
        QVERIFY(popup->copy_button->isEnabled());
        // Copying puts the translation on the clipboard.
        popup->copy_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("[zh-Hans] selected text"));

        // Manual entry reuses the same popup and takes focus.
        m->open_compact_input(QPoint(10, 10));
        TranslationPopup *again = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationPopup *>(widget); found && found->isVisible()) again = found;
        QCOMPARE(again, popup);
        QVERIFY(!popup->testAttribute(Qt::WA_ShowWithoutActivating));
        const int before = finished.count();
        popup->source_edit->setPlainText("typed by hand");
        QTRY_COMPARE(finished.count(), before + 1);
        QCOMPARE(popup->result_edit->toPlainText(), QString("[zh-Hans] typed by hand"));

        // Opening the full window promotes the content without asking again.
        const int calls = provider_calls;
        popup->full_button->click();
        QVERIFY(m->is_dialog_open());
        QVERIFY(!popup->isVisible());
        QCOMPARE(provider_calls.load(), calls);
        TranslationDialog *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationDialog *>(widget); found && found->isVisible()) window = found;
        QVERIFY(window);
        QCOMPARE(window->source_edit->toPlainText(), QString("typed by hand"));
        QCOMPARE(window->target_edit->toPlainText(), QString("[zh-Hans] typed by hand"));
    }

    void aMissingEngineShowsInThePopup() {
        config_->api_key.clear();
        auto *m = manager();
        m->open_compact_input(QPoint(10, 10));
        TranslationPopup *popup = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationPopup *>(widget); found && found->isVisible()) popup = found;
        QVERIFY(popup);
        QVERIFY(popup->result_edit->toPlainText().contains("API key not configured"));
        QVERIFY(popup->result_edit->property("error").toBool());
        m->translate_compact("selected", QPoint(10, 10));
        QVERIFY(popup->result_edit->property("error").toBool());
        QCOMPARE(provider_calls.load(), 0);
    }

    void aNewerRequestSupersedesTheRunningOne() {
        provider_delay_ms = 300;
        auto *m = manager();
        QSignalSpy finished(m, &TranslationManager::translation_finished);
        m->translate("first");
        m->translate("second");
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() >= 1, 5000);
        QTest::qWait(500);
        // Only the latest request's result is delivered.
        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).toString(), QString("[zh-Hans] second"));
        // Shutdown waits for the superseded worker too.
        m->shutdown(3000);
        QCOMPARE(provider_calls.load(), 2);
    }

    void themeChangesReachOpenSurfaces() {
        auto *m = manager();
        m->translate("");
        m->open_compact_input(QPoint(10, 10));
        m->translate("");
        TranslationDialog *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *found = qobject_cast<TranslationDialog *>(widget); found && found->isVisible()) window = found;
        QVERIFY(window);
        const auto before = window->styleSheet();
        const bool dark = get_ui_theme()->is_dark();
        get_ui_theme()->set_mode(dark ? "light" : "dark", false);
        QVERIFY(window->styleSheet() != before);
        get_ui_theme()->set_mode(dark ? "dark" : "light", false);
    }

    // ---- The popup and window on their own ----

    void thePopupShowsResultsErrorsAndRequestsTheFullWindow() {
        TranslationPopup popup;
        QSignalSpy full(&popup, &TranslationPopup::open_full_requested);
        QSignalSpy manual(&popup, &TranslationPopup::manual_translate_requested);
        popup.set_backend_ready(true);
        popup.show_popup("hello", QPoint(10, 10));
        QCOMPARE(popup.source_edit->toPlainText(), QString("hello"));
        QVERIFY(!popup.copy_button->isEnabled());
        // A programmatic fill does not start the typing debounce.
        QTest::qWait(600);
        QCOMPARE(manual.count(), 0);
        popup.show_result(QString::fromUtf8("你好"), "EN");
        QCOMPARE(popup.result_edit->toPlainText(), QString::fromUtf8("你好"));
        QVERIFY(popup.copy_button->isEnabled());
        popup.show_error("network error");
        QVERIFY(popup.result_edit->property("error").toBool());
        QVERIFY(!popup.copy_button->isEnabled());
        popup.full_button->click();
        QCOMPARE(full.last(), (QList<QVariant>{"hello", "", "network error"}));
        // An empty popup takes no extra height.
        popup.show_popup("", QPoint(10, 10));
        QCOMPARE(popup.height(), popup.sizeHint().height());
        popup.source_edit->setPlainText("manual text");
        QTRY_COMPARE(manual.count(), 1);
        QCOMPARE(manual.last().at(0).toString(), QString("manual text"));
        popup.set_target_lang("JA");
        QCOMPARE(popup.current_target_lang(), QString("JA"));
        popup.set_backend_status("Some Very Long Translation Engine Name For Eliding", true);
        QVERIFY(popup.backend_badge->text().size() <= QString("Some Very Long Translation Engine Name For Eliding").size());
        popup.set_theme("light");
        popup.set_theme("dark");
    }

    void thePopupLanguageMenuSwitchesTheTarget() {
        TranslationPopup popup;
        QSignalSpy changed(&popup, &TranslationPopup::target_lang_changed);
        popup.show_popup("hello", QPoint(10, 10));
        QTimer::singleShot(0, &popup, [] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (auto *action : menu->actions())
                if (action->data().toString() == "EN") menu->setActiveAction(action);
            QTest::keyClick(menu, Qt::Key_Return);
        });
        popup.lang_button->click();
        QCOMPARE(changed.count(), 1);
        QCOMPARE(changed.first().at(0).toString(), QString("EN"));
        QCOMPARE(popup.current_target_lang(), QString("EN"));
    }

    void theWindowTracksLoadingErrorsAndSize() {
        TranslationDialog dialog("source", {}, nullptr, QPoint(10, 10));
        dialog.show();
        dialog.set_loading();
        const auto loading = dialog.target_edit->placeholderText() + dialog.target_edit->toPlainText();
        QVERIFY(!loading.isEmpty());
        dialog.on_translation_finished(false, {}, "boom");
        QVERIFY(dialog.target_edit->property("error").toBool());
        dialog.on_translation_finished(true, "done", {}, "FR");
        QCOMPARE(dialog.target_edit->toPlainText(), QString("done"));
        QVERIFY(!dialog.target_edit->property("error").toBool());
        dialog.update_content("new text", "EN", "JA");
        QCOMPARE(dialog.source_edit->toPlainText(), QString("new text"));
        QCOMPARE(dialog.get_target_lang(), QString("EN"));
        QCOMPARE(dialog.get_source_lang(), QString("JA"));
        dialog.set_backend_badge("Engine", false);
        QVERIFY(dialog.backend_badge->text().contains("Engine"));
        // The window starts at its default size where the screen allows it, never below the minimum.
        const auto roomy = TranslationDialog::initial_size_for_available_geometry(QRect(0, 0, 1920, 1040));
        QVERIFY(roomy == std::make_pair(TranslationDialog::DEFAULT_WIDTH, TranslationDialog::DEFAULT_HEIGHT));
        const auto small = TranslationDialog::initial_size_for_available_geometry(QRect(0, 0, 900, 600));
        QVERIFY(small.first <= 900 && small.second <= 600);
        QVERIFY(small.first >= std::min(TranslationDialog::MINIMUM_WIDTH, 900));
        // Copy buttons put each side on the clipboard.
        dialog.copy_source_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("new text"));
        dialog.copy_target_button->click();
        QCOMPARE(QApplication::clipboard()->text(), QString("done"));
        dialog.set_theme("light");
        dialog.set_theme("dark");
    }

    void loadingDotsCycleAndStop() {
        QStringList ticks;
        EllipsisAnimator animator(nullptr, [&](const QString &text) { ticks.append(text); });
        animator.start();
        QVERIFY(animator.is_running());
        QTRY_VERIFY_WITH_TIMEOUT(ticks.size() >= 4, 3000);
        animator.stop();
        QVERIFY(!animator.is_running());
        const int count = ticks.size();
        QTest::qWait(EllipsisAnimator::INTERVAL_MS * 2);
        QCOMPARE(ticks.size(), count);
        QCOMPARE(EllipsisAnimator::text_for(0).endsWith('.'), false);
        QVERIFY(EllipsisAnimator::text_for(3).endsWith("..."));
        // Restarting begins from no dots, so the first tick shows one.
        ticks.clear();
        animator.start();
        QTRY_VERIFY(!ticks.isEmpty());
        QCOMPARE(ticks.first(), EllipsisAnimator::text_for(1));
        animator.stop();
        EllipsisAnimator silent;
        silent.start();
        QTest::qWait(EllipsisAnimator::INTERVAL_MS + 50);
        silent.stop();
    }

    // ---- The selection probe behind the translation hotkey ----

    // The probe uses the application's manager; DeepL without a key answers locally,
    // so nothing goes to the network.
    void theSelectionProbeOpensThePopupWithTheCopiedText() {
        get_tool_settings_manager()->set_translation_provider("deepl");
        get_tool_settings_manager()->set_accessor_value("deepl_api_key", "");
        const auto visible_popup = []() -> TranslationPopup * {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *found = qobject_cast<TranslationPopup *>(widget); found && found->isVisible()) return found;
            return nullptr;
        };
        SmartTranslationController controller;
        // Text from a caller that owns its selection goes straight to the popup.
        controller.translate_selection("   ");
        QVERIFY(!visible_popup());
        controller.translate_selection("  owned text ");
        QVERIFY(visible_popup());
        QCOMPARE(visible_popup()->source_edit->toPlainText(), QString("owned text"));
        visible_popup()->hide();
        QVERIFY(controller.hotkey().owner == &controller);

        // With clipboard monitoring, the copied text arrives as a clipboard item.
        controller.clipboard_monitor_available = [] { return true; };
        controller.on_clipboard_item("text", "ignored before a probe");
        QVERIFY(!visible_popup());
        controller.trigger();
        QVERIFY(controller.probe_active());
        // Items before the copy is sent belong to someone else.
        controller.on_clipboard_item("text", "too early");
        QVERIFY(controller.probe_active());
        QTest::qWait(SmartTranslationController::COPY_DELAY_MS + 40);
        controller.on_clipboard_item("text", "  copied text  ");
        QVERIFY(!controller.probe_active());
        QCOMPARE(visible_popup()->source_edit->toPlainText(), QString("copied text"));
        visible_popup()->hide();

        // An image instead of text opens the popup for typing.
        controller.trigger();
        QTest::qWait(SmartTranslationController::COPY_DELAY_MS + 40);
        controller.on_clipboard_item("image", "");
        QVERIFY(!controller.probe_active());
        QVERIFY(visible_popup());
        QVERIFY(visible_popup()->source_edit->toPlainText().isEmpty());
        visible_popup()->hide();

        // Nothing at all: typing after the timeout.
        controller.trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.probe_active(), SmartTranslationController::PROBE_TIMEOUT_MS + 500);
        QVERIFY(visible_popup());
        visible_popup()->hide();

        // Without monitoring the clipboard is read once after the copy; what it holds depends on
        // the foreground program, so either the text or typing opens.
        controller.clipboard_monitor_available = nullptr;
        QApplication::clipboard()->setText("clipboard text");
        controller.trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!controller.probe_active(), SmartTranslationController::PROBE_TIMEOUT_MS + 500);
        QVERIFY(visible_popup());
        // A newer probe makes the older one's timers do nothing.
        visible_popup()->hide();
        controller.trigger();
        controller.translate_selection("direct");
        QTest::qWait(SmartTranslationController::PROBE_TIMEOUT_MS + 100);
        QCOMPARE(visible_popup()->source_edit->toPlainText(), QString("direct"));
        TranslationManager::cleanup();
    }
};

QTEST_MAIN(TranslationUiTests)
#include "test_translation_ui.moc"
