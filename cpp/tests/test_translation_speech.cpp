#include "settings/tool_settings.h"
#include "translation/speech_service.h"
#include "translation/translation_dialog.h"
#include "translation/translation_popup.h"
#include "ui/fluent_lite/inputs.h"
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>

using namespace jietuba;

namespace {
// Never touches SAPI or makes a sound; every call can be made to fail.
class FakeSpeechBackend : public SpeechBackend {
public:
    explicit FakeSpeechBackend(const QStringList &locales = {"en_US", "fi_FI"}) {
        for (const auto &name : locales) add_voice(name);
    }
    void add_voice(const QString &locale_name) { voice_list.push_back({"Fake " + locale_name, QLocale(locale_name), locale_name}); }

    bool voices(std::vector<VoiceInfo> &found, QString &error) override {
        if (!fail_voices.isEmpty()) return error = fail_voices, false;
        found = voice_list;
        return true;
    }
    bool speak(const VoiceInfo &voice, const QString &text, QString &error) override {
        if (!fail_speak.isEmpty()) return error = fail_speak, false;
        spoken.push_back({voice.name, text});
        done = false;
        position = 0;
        return true;
    }
    bool purge(QString &error) override {
        ++purges;
        if (!fail_purge.isEmpty()) return error = fail_purge, false;
        done = true;
        return true;
    }
    bool poll(SpeechStatus &status, QString &error) override {
        if (!fail_poll.isEmpty()) return error = fail_poll, false;
        status = {done, position, hresult};
        return true;
    }
    bool close(QString &error) override {
        closed = true;
        return purge(error);
    }

    std::vector<VoiceInfo> voice_list;
    std::vector<std::pair<QString, QString>> spoken;
    int purges = 0;
    bool done = true;
    qint64 position = 0;
    qint32 hresult = 0;
    bool closed = false;
    QString fail_voices, fail_speak, fail_purge, fail_poll;
};

// An SPERR_* style failure code.
constexpr qint32 SPEECH_FAILURE = -2147200925;

QString spoken_text(const FakeSpeechBackend &backend) { return backend.spoken.empty() ? QString() : backend.spoken.back().second; }

QString spoken_voice(const FakeSpeechBackend &backend) { return backend.spoken.empty() ? QString() : backend.spoken.back().first; }

// Types at the end of the box the way a user edit arrives.
void append_text(QTextEdit *edit, const QString &text) {
    edit->moveCursor(QTextCursor::End);
    edit->insertPlainText(text);
}
}

// Expected values for the pure logic come from tests/fixtures/translation.json.
class TranslationSpeechTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir dir_;
    QSettings *settings_ = nullptr;
    QJsonObject fixtures_;
    std::shared_ptr<FakeSpeechBackend> backend_;
    double now_ = 1000.0;

    SpeechService *service() { return SpeechService::instance(); }

    TranslationDialog *make_dialog(const QString &lang = "EN") {
        auto *dialog = new TranslationDialog({}, {}, nullptr, std::nullopt, "auto", lang);
        dialog->target_language->setCurrentIndex(dialog->target_language->findData(lang));
        return dialog;
    }

    std::unique_ptr<TranslationPopup> make_popup() {
        auto popup = std::make_unique<TranslationPopup>();
        popup->set_target_lang("EN");
        return popup;
    }

    void advance(double seconds) { now_ += seconds; }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        settings_ = new QSettings(dir_.filePath("settings.ini"), QSettings::IniFormat, this);
        get_tool_settings_manager(settings_);
        QFile file(JIETUBA_TRANSLATION_FIXTURE_PATH);
        QVERIFY(file.open(QIODevice::ReadOnly));
        fixtures_ = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!fixtures_["speech_lcid"].toArray().isEmpty());
    }

    void init() {
        settings_->clear();
        backend_ = std::make_shared<FakeSpeechBackend>();
        SpeechService::set_backend_factory([backend = backend_](QString &) { return backend; });
        now_ = 1000.0;
        SpeechService::set_clock([this] { return now_; });
    }

    void cleanup() {
        SpeechService::reset_instance();
        SpeechService::set_backend_factory(nullptr);
        SpeechService::set_clock(nullptr);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // ---- Voice matching ----

    void lcidAttributesMatchReference() {
        for (const auto &value : fixtures_["speech_lcid"].toArray()) {
            const auto item = value.toObject();
            const auto locale = locale_from_lcid(item["input"].toString());
            const QString name = locale ? locale->name() : QString();
            QVERIFY2(name == item["expected"].toString(), qPrintable(item["input"].toString() + " -> " + name));
            QCOMPARE(locale.has_value(), !item["expected"].isNull());
        }
    }

    void voiceChoiceMatchesReference() {
        for (const auto &value : fixtures_["speech_voice_for"].toArray()) {
            const auto item = value.toObject();
            QStringList locales;
            for (const auto &locale : item["voices"].toArray()) locales << locale.toString();
            auto backend = std::make_shared<FakeSpeechBackend>(QStringList{});
            for (const auto &locale : locales) backend->voice_list.push_back({locale, QLocale(locale), locale});
            SpeechService::set_backend_factory([backend](QString &) { return backend; });
            SpeechService service;
            const auto chosen = item["chosen"].toObject();
            for (auto it = chosen.begin(); it != chosen.end(); ++it) {
                const auto voice = service.voice_for(it.key());
                const QString name = voice ? voice->name : QString();
                QVERIFY2(name == it.value().toString(), qPrintable(locales.join(',') + " / " + it.key() + " -> " + name));
            }
        }
    }

    void voiceMustMatchTheLanguage() {
        QCOMPARE(service()->voice_for("EN")->locale.language(), QLocale::English);
        QVERIFY(service()->voice_for("FI"));
        // Without a Chinese voice another language must not read it.
        QVERIFY(!service()->voice_for("ZH"));
        QVERIFY(!service()->can_speak(""));
    }

    void sameTerritoryIsPreferred() {
        backend_->voice_list.insert(backend_->voice_list.begin(), {"UK", QLocale("en_GB"), "uk"});
        QCOMPARE(service()->voice_for("EN")->name, QString("Fake en_US"));
    }

    void newlyInstalledVoiceIsFoundAfterRescan() {
        QVERIFY(!service()->can_speak("ZH"));
        backend_->add_voice("zh_CN");
        advance(1);
        QVERIFY(!service()->can_speak("ZH"));  // not re-enumerated on every refresh
        advance(SpeechService::VOICE_RESCAN_SECONDS);
        QVERIFY(service()->can_speak("ZH"));
    }

    // ---- Reading state ----

    void ownerIsClearedWhenReadingFinishes() {
        const auto owner = SpeechService::new_owner();
        QList<bool> changes;
        connect(service(), &SpeechService::state_changed, this, [&] { changes << service()->is_speaking(owner); });

        QVERIFY(service()->speak(owner, "hello world", "EN"));
        QVERIFY(service()->is_speaking(owner));
        service()->poll();
        QVERIFY(service()->is_speaking(owner));

        backend_->done = true;
        QTRY_VERIFY_WITH_TIMEOUT(!service()->is_speaking(owner), 3000);
        QCOMPARE(changes, (QList<bool>{true, false}));
        QVERIFY(!service()->polling());
    }

    void newRequestReplacesTheOldOne() {
        const auto first = SpeechService::new_owner(), second = SpeechService::new_owner();
        service()->speak(first, "alpha", "EN");
        service()->speak(second, "omega", "EN");
        QVERIFY(service()->is_speaking(second));
        QVERIFY(!service()->is_speaking(first));
        QCOMPARE(backend_->spoken.size(), size_t(2));
        QCOMPARE(backend_->spoken[0].second, QString("alpha"));
        QCOMPARE(backend_->spoken[1].second, QString("omega"));
    }

    void stopIgnoresOtherOwners() {
        const auto owner = SpeechService::new_owner(), other = SpeechService::new_owner();
        service()->speak(owner, "one two", "EN");
        service()->stop(other);
        QVERIFY(service()->is_speaking(owner));
        QCOMPARE(backend_->purges, 0);

        service()->stop(owner);
        QVERIFY(!service()->is_speaking(owner));
        QCOMPARE(backend_->purges, 1);
    }

    void toggleStartsThenStops() {
        const auto owner = SpeechService::new_owner();
        service()->toggle(owner, "one two", "EN");
        QVERIFY(service()->is_speaking(owner));
        service()->toggle(owner, "one two", "EN");
        QVERIFY(!service()->is_speaking(owner));
    }

    void unspeakableRequestsDoNothing() {
        const auto owner = SpeechService::new_owner();
        QVERIFY(!service()->speak(owner, "   ", "EN"));
        // U+3000 counts as whitespace when stripping.
        QVERIFY(!service()->speak(owner, QString::fromUtf8("　\n"), "EN"));
        QVERIFY(!service()->speak(owner, QString::fromUtf8("你好"), "ZH"));
        QVERIFY(!service()->is_speaking(owner));
        QVERIFY(backend_->spoken.empty());
        QVERIFY(!service()->is_speaking(0));
    }

    void textIsStrippedBeforeReading() {
        service()->speak(SpeechService::new_owner(), "\n  hello \t", "EN");
        QCOMPARE(spoken_text(*backend_), QString("hello"));
    }

    // ---- Failures ----

    void missingEngineMeansNoVoiceAndNoRetry() {
        int calls = 0;
        SpeechService::set_backend_factory([&calls](QString &error) -> std::shared_ptr<SpeechBackend> {
            ++calls;
            error = "SAPI not registered";
            return nullptr;
        });
        QVERIFY(!service()->can_speak("EN"));
        QVERIFY(!service()->speak(SpeechService::new_owner(), "hello", "EN"));
        service()->stop();
        advance(SpeechService::VOICE_RESCAN_SECONDS);
        QVERIFY(!service()->can_speak("EN"));
        QCOMPARE(calls, 1);
    }

    void voiceEnumerationFailureIsNoVoice() {
        backend_->fail_voices = "registry broken";
        QVERIFY(!service()->can_speak("EN"));
    }

    void startFailureResetsAndSilencesThePreviousReading() {
        const auto first = SpeechService::new_owner(), second = SpeechService::new_owner();
        service()->speak(first, "alpha", "EN");
        backend_->fail_speak = "voice engine missing";
        QSignalSpy changed(service(), &SpeechService::state_changed);

        QVERIFY(!service()->speak(second, "omega", "EN"));
        QCOMPARE(service()->owner(), quint64(0));
        QCOMPARE(backend_->purges, 1);
        QVERIFY(!service()->polling());
        QCOMPARE(changed.count(), 1);
    }

    void statusFailureResets() {
        const auto owner = SpeechService::new_owner();
        service()->speak(owner, "alpha", "EN");
        backend_->fail_poll = "RPC server unavailable";
        service()->poll();
        QVERIFY(!service()->is_speaking(owner));
        QCOMPARE(backend_->purges, 1);
    }

    void stopFailureStillResets() {
        const auto owner = SpeechService::new_owner();
        service()->speak(owner, "alpha", "EN");
        backend_->fail_purge = "busy";
        service()->stop(owner);
        QVERIFY(!service()->is_speaking(owner));
        QVERIFY(!service()->polling());
    }

    void streamEndingWithAnErrorResets() {
        const auto owner = SpeechService::new_owner();
        service()->speak(owner, "alpha", "EN");
        backend_->done = true;
        backend_->hresult = SPEECH_FAILURE;
        service()->poll();
        QVERIFY(!service()->is_speaking(owner));
    }

    void staleErrorCodeDoesNotCutARunningReading() {
        const auto owner = SpeechService::new_owner();
        service()->speak(owner, "alpha", "EN");
        backend_->hresult = SPEECH_FAILURE;
        service()->poll();
        QVERIFY(service()->is_speaking(owner));
    }

    void stalledReadingGivesUp() {
        const auto owner = SpeechService::new_owner();
        service()->speak(owner, "alpha beta gamma", "EN");

        advance(SpeechService::STALL_SECONDS - 1);
        backend_->position = 6;
        service()->poll();  // still moving, the timer restarts
        advance(SpeechService::STALL_SECONDS - 1);
        service()->poll();
        QVERIFY(service()->is_speaking(owner));

        advance(2);
        service()->poll();
        QVERIFY(!service()->is_speaking(owner));
        QCOMPARE(backend_->purges, 1);
    }

    void shutdownClosesTheEngineForGood() {
        const auto owner = SpeechService::new_owner();
        service()->speak(owner, "alpha", "EN");
        service()->shutdown();
        QVERIFY(backend_->closed);
        QVERIFY(!service()->is_speaking(owner));
        QVERIFY(!service()->speak(owner, "again", "EN"));
        QVERIFY(!service()->polling());
    }

    // ---- Translation window ----

    void buttonFollowsTheTargetState() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog());
        auto *button = dialog->speak_target_button;
        QVERIFY(!button->isEnabled());  // no translation yet

        dialog->set_loading();
        QVERIFY(!button->isEnabled());

        dialog->set_translation_result("hello world");
        QVERIFY(button->isEnabled());

        dialog->set_translation_error("boom");
        QVERIFY(!button->isEnabled());
    }

    void buttonIsDisabledWithoutAVoice() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog("ZH"));
        dialog->set_translation_result(QString::fromUtf8("你好"));
        QVERIFY(!dialog->speak_target_button->isEnabled());
        QVERIFY(dialog->speak_target_button->toolTip().contains("Windows"));
    }

    void clickShowsStopUntilReadingEnds() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog());
        auto *button = dialog->speak_target_button;
        dialog->set_translation_result("one two three");
        button->click();
        QCOMPARE(button->icon_name(), QString("stop"));
        QCOMPARE(spoken_text(*backend_), QString("one two three"));

        backend_->done = true;
        QTRY_COMPARE_WITH_TIMEOUT(button->icon_name(), QString("speak"), 3000);
        QVERIFY(button->isEnabled());
    }

    void twoWindowsNeverBothShowStop() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog());
        std::unique_ptr<TranslationDialog> other(make_dialog());
        dialog->set_translation_result("alpha");
        other->set_translation_result("beta");
        dialog->speak_target_button->click();
        other->speak_target_button->click();
        QCOMPARE(dialog->speak_target_button->icon_name(), QString("speak"));
        QCOMPARE(other->speak_target_button->icon_name(), QString("stop"));
    }

    void readingStopsWhenTheTranslationGoesAway_data() {
        QTest::addColumn<QString>("change");
        for (const char *change : {"loading", "error", "result", "close"}) QTest::newRow(change) << QString(change);
    }

    void readingStopsWhenTheTranslationGoesAway() {
        QFETCH(QString, change);
        QPointer<TranslationDialog> dialog = make_dialog();
        dialog->set_translation_result("alpha");
        dialog->speak_target_button->click();
        QVERIFY(service()->is_speaking(dialog->speech_token()));

        if (change == "loading") dialog->set_loading();
        else if (change == "error") dialog->set_translation_error("boom");
        else if (change == "result") dialog->set_translation_result("new text");
        else dialog->close();

        QCOMPARE(service()->owner(), quint64(0));
        QCOMPARE(backend_->purges, 1);
        delete dialog.data();
    }

    void swappedTextIsReadInItsOwnLanguage() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog("ZH"));
        dialog->source_language->setCurrentIndex(dialog->source_language->findData("EN"));
        dialog->source_edit->setPlainText("good day");
        dialog->set_translation_result(QString::fromUtf8("你好"));
        QVERIFY(!dialog->speak_target_button->isEnabled());

        dialog->swap_button->click();

        QCOMPARE(dialog->target_edit->toPlainText(), QString("good day"));
        QCOMPARE(dialog->target_text_lang(), QString("EN"));
        QVERIFY(dialog->speak_target_button->isEnabled());
    }

    void closedWindowsAreReleased() {
        // The service outlives the windows; a closed window must go away and its
        // connection with it.
        QList<QPointer<TranslationDialog>> dialogs;
        for (int i = 0; i < 20; ++i) {
            auto *dialog = make_dialog();
            dialog->set_translation_result("one two three four");
            dialog->speak_target_button->click();
            dialogs << dialog;
            dialog->close();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
        QCOMPARE(service()->owner(), quint64(0));
        for (const auto &dialog : dialogs) QVERIFY(dialog.isNull());
        // Later state changes reach no deleted window.
        service()->speak(SpeechService::new_owner(), "alpha", "EN");
        service()->stop();
    }

    // ---- Source text language ----

    void guessTextLanguage_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expected");
        const std::pair<const char *, const char *> cases[] = {
            {"", ""},
            {"123 !? ...", ""},
            {"\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac", ""},  // Greek is not covered
            {"hello world", "EN"},
            {"Gr\xc3\xbc\xc3\x9f" "e", "EN"},
            {"\xec\x95\x88\xeb\x85\x95\xed\x95\x98\xec\x84\xb8\xec\x9a\x94", "KO"},
            {"\xec\x95\x88\xeb\x85\x95 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae", "KO"},  // Hangul wins over kana and Han
            {"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe6\x96\x87", "JA"},
            {"\xe3\x82\xab\xe3\x82\xbf\xe3\x82\xab\xe3\x83\x8a", "JA"},
            {"\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab\xe3\x81\xa1\xe3\x81\xaf \xe4\xbd\xa0\xe5\xa5\xbd", "JA"},
            {"\xe4\xbd\xa0\xe5\xa5\xbd", "ZH"},
            {"\xe4\xbd\xa0\xe5\xa5\xbd hello", "ZH"},
            {"\xf0\xa0\x80\x80", "ZH"},  // U+20000, outside the BMP
            {"\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "RU"},
            {"hello \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "RU"},
            {"\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd1\x96\xd1\x82", "UK"},  // і
            {"\xd0\x87\xd0\xb6\xd0\xb0\xd0\xba", "UK"},  // Ї
            {"\xd1\x94", "UK"},  // є
            {"\xd2\x91\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xba", "UK"},  // ґ
            {"\xe0\xb8\xaa\xe0\xb8\xa7\xe0\xb8\xb1\xe0\xb8\xaa\xe0\xb8\x94\xe0\xb8\xb5 abc", "TH"},
            {"\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7", "AR"},
            {"\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 hello", "AR"},
        };
        for (const auto &[text, expected] : cases) QTest::newRow(text) << QString::fromUtf8(text) << QString(expected);
    }

    void guessTextLanguage() {
        QFETCH(QString, text);
        QFETCH(QString, expected);
        QCOMPARE(guess_text_language(text), expected);
    }

    void sourceReadsInTheSelectedLanguage() {
        backend_->add_voice("de_DE");
        std::unique_ptr<TranslationDialog> dialog(make_dialog());
        dialog->source_language->setCurrentIndex(dialog->source_language->findData("DE"));
        dialog->source_edit->setPlainText("guten Tag");
        dialog->speak_source_button->click();
        QCOMPARE(spoken_voice(*backend_), QString("Fake de_DE"));
        QCOMPARE(spoken_text(*backend_), QString("guten Tag"));
    }

    void sourceUsesTheDetectedLanguageUntilEdited() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog());
        dialog->source_edit->setPlainText("hyvaa paivaa");
        // "fi" is not a combo entry, so the combo stays on automatic.
        dialog->set_translation_result("good day", "fi");
        QCOMPARE(dialog->get_source_lang(), QString("auto"));
        QCOMPARE(dialog->source_text_lang(), QString("fi"));
        dialog->speak_source_button->click();
        QCOMPARE(spoken_voice(*backend_), QString("Fake fi_FI"));

        dialog->source_edit->setPlainText("hyvaa paivaa!");
        QCOMPARE(dialog->source_text_lang(), QString("EN"));
    }

    void sourceButtonFollowsTheTextAndVoices() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog());
        auto *button = dialog->speak_source_button;
        QVERIFY(!button->isEnabled());
        dialog->source_edit->setPlainText("hello");
        QVERIFY(button->isEnabled());
        dialog->source_edit->setPlainText(QString::fromUtf8("你好"));
        QVERIFY(!button->isEnabled());
        QVERIFY(button->toolTip().contains("Windows"));
        dialog->source_edit->setPlainText("   ");
        QVERIFY(!button->isEnabled());
    }

    void sourceAndTargetButtonsReplaceEachOther() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog());
        dialog->source_edit->setPlainText("hello");
        dialog->set_translation_result("bonjour tout le monde");
        dialog->speak_source_button->click();
        QCOMPARE(dialog->speak_source_button->icon_name(), QString("stop"));
        QCOMPARE(dialog->speak_target_button->icon_name(), QString("speak"));

        dialog->speak_target_button->click();
        QCOMPARE(dialog->speak_source_button->icon_name(), QString("speak"));
        QCOMPARE(dialog->speak_target_button->icon_name(), QString("stop"));
        QVERIFY(service()->is_speaking(dialog->speech_token()));

        dialog->speak_source_button->click();
        QVERIFY(service()->is_speaking(dialog->source_speech_token()));
        QCOMPARE(dialog->speak_target_button->icon_name(), QString("speak"));
        QCOMPARE(spoken_text(*backend_), QString("hello"));
    }

    void editingTheSourceStopsOnlyItsReading() {
        std::unique_ptr<TranslationDialog> dialog(make_dialog());
        dialog->source_edit->setPlainText("hello");
        dialog->speak_source_button->click();
        dialog->source_edit->insertPlainText(" world");
        QCOMPARE(service()->owner(), quint64(0));
        QCOMPARE(backend_->purges, 1);
        QCOMPARE(dialog->speak_source_button->icon_name(), QString("speak"));

        dialog->set_translation_result("bonjour");
        dialog->speak_target_button->click();
        dialog->source_edit->insertPlainText("!");
        QVERIFY(service()->is_speaking(dialog->speech_token()));
    }

    void closingStopsTheSourceReading() {
        QPointer<TranslationDialog> dialog = make_dialog();
        dialog->source_edit->setPlainText("hello");
        dialog->speak_source_button->click();
        dialog->close();
        QCOMPARE(service()->owner(), quint64(0));
        delete dialog.data();
    }

    // ---- Popup ----

    void popupButtonsFollowTheText() {
        auto popup = make_popup();
        popup->show_popup("hello world");
        QVERIFY(popup->speak_source_button->isEnabled());
        QVERIFY(!popup->speak_result_button->isHidden());
        QVERIFY(!popup->speak_result_button->isEnabled());  // still translating

        popup->show_result("bonjour");
        QVERIFY(popup->speak_result_button->isEnabled());
        popup->show_error("boom");
        QVERIFY(!popup->speak_result_button->isEnabled());

        // A translation into a language without a voice.
        popup->set_target_lang("ZH");
        popup->show_result(QString::fromUtf8("你好"));
        QVERIFY(!popup->speak_result_button->isEnabled());
        QVERIFY(popup->speak_result_button->toolTip().contains("Windows"));

        popup->show_popup();
        QVERIFY(popup->speak_result_button->isHidden());
        QVERIFY(!popup->speak_source_button->isEnabled());
    }

    void popupReadsTheTranslationInItsTargetLanguage() {
        backend_->add_voice("de_DE");
        auto popup = make_popup();
        popup->show_popup("good day");
        popup->set_target_lang("DE");
        popup->show_result("guten Tag", "en");
        popup->speak_result_button->click();
        QCOMPARE(spoken_voice(*backend_), QString("Fake de_DE"));
        QCOMPARE(spoken_text(*backend_), QString("guten Tag"));
        // The source uses the detected language while it is unchanged.
        QCOMPARE(popup->source_text_lang(), QString("en"));
    }

    void popupSourceUsesDetectedLanguageUntilEdited() {
        auto popup = make_popup();
        popup->show_popup("hyvaa paivaa");
        popup->show_result("good day", "fi");
        popup->speak_source_button->click();
        QCOMPARE(spoken_voice(*backend_), QString("Fake fi_FI"));
        append_text(popup->source_edit, "!");
        QCOMPARE(popup->source_text_lang(), QString("EN"));
        QCOMPARE(service()->owner(), quint64(0));
    }

    void popupButtonsReplaceEachOther() {
        auto popup = make_popup();
        popup->show_popup("hello");
        popup->show_result("bonjour");
        popup->speak_source_button->click();
        popup->speak_result_button->click();
        QCOMPARE(popup->speak_source_button->icon_name(), QString("speak"));
        QCOMPARE(popup->speak_result_button->icon_name(), QString("stop"));
        popup->speak_source_button->click();
        QCOMPARE(popup->speak_source_button->icon_name(), QString("stop"));
        QCOMPARE(popup->speak_result_button->icon_name(), QString("speak"));
    }

    void popupStopsTheTranslationReading_data() {
        QTest::addColumn<QString>("change");
        for (const char *change : {"loading", "error", "result", "new popup"}) QTest::newRow(change) << QString(change);
    }

    void popupStopsTheTranslationReading() {
        QFETCH(QString, change);
        auto popup = make_popup();
        popup->show_popup("hello");
        popup->show_result("bonjour");
        popup->speak_result_button->click();
        QVERIFY(service()->is_speaking(popup->result_speech_token()));

        if (change == "loading") {
            // Typing retranslates after a pause; the reading goes on until the request starts.
            append_text(popup->source_edit, " there");
            QVERIFY(service()->is_speaking(popup->result_speech_token()));
            QTRY_COMPARE_WITH_TIMEOUT(service()->owner(), quint64(0), 3000);
        } else if (change == "error") popup->show_error("boom");
        else if (change == "result") popup->show_result("salut");
        else popup->show_popup("other text");

        QCOMPARE(service()->owner(), quint64(0));
        QCOMPARE(popup->speak_result_button->icon_name(), QString("speak"));
    }

    void popupEditStopsOnlyTheSourceReading() {
        auto popup = make_popup();
        popup->show_popup("hello");
        popup->show_result("bonjour");
        popup->speak_source_button->click();
        append_text(popup->source_edit, "x");
        QCOMPARE(service()->owner(), quint64(0));
        QCOMPARE(popup->speak_source_button->icon_name(), QString("speak"));
    }

    void hidingThePopupStopsReading_data() {
        QTest::addColumn<QString>("button");
        QTest::addColumn<QString>("how");
        for (const char *button : {"source", "result"})
            for (const char *how : {"hide", "close button", "close"}) QTest::newRow(QByteArray(button) + " " + how) << QString(button) << QString(how);
    }

    void hidingThePopupStopsReading() {
        QFETCH(QString, button);
        QFETCH(QString, how);
        auto popup = make_popup();
        popup->show_popup("hello");
        popup->show_result("bonjour");
        (button == "source" ? popup->speak_source_button : popup->speak_result_button)->click();
        QVERIFY(service()->owner() != 0);
        if (how == "hide") popup->hide();
        else if (how == "close button") popup->close_button->click();
        else popup->close();
        QCOMPARE(service()->owner(), quint64(0));
        QCOMPARE(backend_->purges, 1);
    }

    // ---- Real SAPI, synthesised into memory without sound ----

    void sapiReadsMarkupLikeTextAsPlainText() {
        QString error;
        auto backend = SapiBackend::create(error);
        if (!backend) QSKIP(qPrintable("SAPI unavailable: " + error));
        std::vector<VoiceInfo> voices;
        QVERIFY2(backend->voices(voices, error), qPrintable(error));
        if (voices.empty()) QSKIP("no SAPI voices installed");
        QVERIFY2(backend->use_memory_output(error), qPrintable(error));

        QVERIFY2(backend->speak(voices.front(), "<pitch middle='x'>1 < 2 & 3", error), qPrintable(error));
        SpeechStatus status;
        // The first synthesis on a freshly started machine loads the voice, which can take far longer than later ones.
        QVERIFY(QTest::qWaitFor([&] { return backend->poll(status, error) && status.done; }, 60000));
        QVERIFY(status.hresult >= 0);
        QVERIFY(backend->memory_output_size() > 0);
        QVERIFY(backend->close(error));
    }

    void sapiPurgeEndsALongReading() {
        QString error;
        auto backend = SapiBackend::create(error);
        if (!backend) QSKIP(qPrintable("SAPI unavailable: " + error));
        std::vector<VoiceInfo> voices;
        backend->voices(voices, error);
        if (voices.empty()) QSKIP("no SAPI voices installed");
        QVERIFY2(backend->use_memory_output(error), qPrintable(error));

        QVERIFY(backend->speak(voices.front(), QString("one two three four five six seven eight nine ten ").repeated(50), error));
        SpeechStatus status;
        QVERIFY(backend->poll(status, error));
        QVERIFY(!status.done);
        QVERIFY(backend->purge(error));
        QVERIFY(QTest::qWaitFor([&] { return backend->poll(status, error) && status.done; }, 10000));
    }

    void sapiVoicesHaveLocales() {
        QString error;
        auto backend = SapiBackend::create(error);
        if (!backend) QSKIP(qPrintable("SAPI unavailable: " + error));
        std::vector<VoiceInfo> voices;
        QVERIFY(backend->voices(voices, error));
        if (voices.empty()) QSKIP("no SAPI voices installed");
        QStringList names;
        for (const auto &voice : voices) {
            QVERIFY(voice.locale.language() != QLocale::C);
            names << voice.name;
        }
        QCOMPARE(QSet<QString>(names.begin(), names.end()).size(), names.size());
        qInfo() << "SAPI voices:" << names;
    }

    void sapiCallsFailCleanlyAfterClose() {
        QString error;
        auto backend = SapiBackend::create(error);
        if (!backend) QSKIP(qPrintable("SAPI unavailable: " + error));
        std::vector<VoiceInfo> voices;
        backend->voices(voices, error);
        // close() purges through the audio output, which CI runners without an audio device cannot open.
        QVERIFY2(backend->use_memory_output(error), qPrintable(error));
        QVERIFY2(backend->close(error), qPrintable(error));
        SpeechStatus status;
        QVERIFY(!backend->poll(status, error));
        QVERIFY(!backend->purge(error));
        if (!voices.empty()) QVERIFY(!backend->speak(voices.front(), "hello", error));
        QVERIFY(!backend->voices(voices, error));
    }
};

QTEST_MAIN(TranslationSpeechTests)
#include "test_translation_speech.moc"
