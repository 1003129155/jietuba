#include "translation/paragraph_translator.h"
#include <QElapsedTimer>
#include <QTest>
#include <QThread>
#include <atomic>
#include <functional>
#include <mutex>

using namespace jietuba;

namespace {
// What every instance of the fake service shares: the calls it saw and how to answer them.
struct Service {
    struct Call {
        QStringList texts;
        QString context;
        bool batch = false;
        // Requests the service was handling when this one arrived, itself included.
        int in_flight = 0;
    };
    // nullopt answers "<text>!" for every text.
    std::function<std::optional<BatchTranslationResult>(const Call &, int call_index)> answer;
    int delay_ms = 0;
    TranslationProvider::BatchLimits limits;

    std::mutex mutex;
    QList<Call> calls;
    int in_flight = 0;
    int most_in_flight = 0;

    BatchTranslationResult handle(Call call) {
        int index = 0;
        {
            const std::lock_guard lock(mutex);
            index = static_cast<int>(calls.size());
            call.in_flight = ++in_flight;
            calls.append(call);
            most_in_flight = std::max(most_in_flight, in_flight);
        }
        if (delay_ms > 0) QThread::msleep(static_cast<unsigned long>(delay_ms));
        std::optional<BatchTranslationResult> reply = answer ? answer(call, index) : std::nullopt;
        if (!reply) {
            reply = BatchTranslationResult{true, {}, {}, std::nullopt, {}};
            for (const auto &text : call.texts) reply->translations.append(text + "!");
        }
        const std::lock_guard lock(mutex);
        --in_flight;
        return *reply;
    }
    int batches() {
        const std::lock_guard lock(mutex);
        return static_cast<int>(std::count_if(calls.begin(), calls.end(), [](const Call &call) { return call.batch; }));
    }
    int singles() {
        const std::lock_guard lock(mutex);
        return static_cast<int>(std::count_if(calls.begin(), calls.end(), [](const Call &call) { return !call.batch; }));
    }
};

class FakeProvider : public TranslationProvider {
public:
    FakeProvider(Service &service, int parallel) : service_(service) { parallel_requests = parallel; }
    QString provider_id() const override { return "fake"; }
    QString display_name() const override { return "Fake"; }
    bool is_configured() const override { return true; }
    BatchLimits batch_limits() const override { return service_.limits; }
    TranslationResult translate(const TranslationRequest &request) override {
        const auto reply = service_.handle({{request.text}, {}, false});
        if (!reply.success) return TranslationResult::failure(reply.error_code.value_or(TranslationErrorCode::UNKNOWN), reply.error_message);
        return TranslationResult{true, reply.translations.value(0), {}, std::nullopt, {}};
    }
    BatchTranslationResult translate_batch(const QStringList &texts, const TranslationRequest &request) override {
        return service_.handle({texts, request.options.value("context").toString(), true});
    }
private:
    Service &service_;
};

struct Fleet {
    Fleet(Service &service, int parallel) {
        for (int index = 0; index < parallel; ++index) owned.push_back(std::make_unique<FakeProvider>(service, parallel));
        for (const auto &provider : owned) providers.push_back(provider.get());
    }
    std::vector<std::unique_ptr<FakeProvider>> owned;
    std::vector<TranslationProvider *> providers;
};

QStringList numbered(int count) {
    QStringList texts;
    for (int index = 0; index < count; ++index) texts << QString("text %1").arg(index);
    return texts;
}

BatchTranslationResult refuse(TranslationErrorCode code, const QString &message) { return BatchTranslationResult::failure(code, message); }

const TranslationRequest PROTOTYPE(QString(), "zh-Hans");
}

class ParagraphTranslatorTests : public QObject {
    Q_OBJECT
private slots:
    // A screen repeats its labels: each distinct text costs one request, and every place it
    // appears gets the translation.
    void sendsEqualTextsOnceAndKeepsTheOrder() {
        Service service;
        Fleet fleet(service, 2);
        QList<int> progress;
        const auto result = translate_paragraphs({"Save", "Cancel", "Save", "  ", "Save", "Apply"}, fleet.providers, PROTOTYPE, nullptr,
                                                 [&](int done, int total) {
                                                     QCOMPARE(total, 6);
                                                     progress << done;
                                                 });
        QCOMPARE(result.translations, QStringList({"Save!", "Cancel!", "Save!", "", "Save!", "Apply!"}));
        QCOMPARE(result.failed, 0);
        QCOMPARE(service.singles(), 3);
        QCOMPARE(result.translated_chars, qint64(4 + 6 + 5));
        // The blank text counts as settled from the start, the repeated one settles three at once.
        QCOMPARE(progress.last(), 6);
        QVERIFY(std::is_sorted(progress.begin(), progress.end()));
    }

    void runsAsManyRequestsAtOnceAsTheServiceTakes() {
        Service service;
        service.delay_ms = 120;
        Fleet fleet(service, 4);
        QElapsedTimer timer;
        timer.start();
        const auto result = translate_paragraphs(numbered(8), fleet.providers, PROTOTYPE);
        const auto elapsed = timer.elapsed();
        QCOMPARE(result.failed, 0);
        QCOMPARE(service.most_in_flight, 4);
        // Two rounds of four; one at a time would take 960 ms.
        QVERIFY2(elapsed < 700, qPrintable(QString::number(elapsed)));
    }

    void spacesRequestsByTheServicesInterval() {
        Service service;
        Fleet fleet(service, 4);
        for (auto &provider : fleet.owned) provider->request_interval_ms = 100;
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(translate_paragraphs(numbered(4), fleet.providers, PROTOTYPE).failed, 0);
        // The first goes at once, the other three 100 ms apart.
        QVERIFY2(timer.elapsed() >= 290, qPrintable(QString::number(timer.elapsed())));
    }

    void packsTextsUpToBothBatchLimits() {
        Service service;
        service.limits = {3, 20};
        Fleet fleet(service, 1);
        // 6 + 6 + 6 fits three texts; the long one goes alone; then the last two.
        const QStringList texts{"aaaaaa", "bbbbbb", "cccccc", "a text of thirty characters ....", "dddddd", "eeeeee"};
        const auto result = translate_paragraphs(texts, fleet.providers, PROTOTYPE);
        QCOMPARE(result.failed, 0);
        QCOMPARE(result.translations.at(3), texts.at(3) + "!");
        QCOMPARE(service.calls.size(), 3);
        QCOMPARE(service.calls.at(0).texts, texts.mid(0, 3));
        QCOMPARE(service.calls.at(1).texts, texts.mid(3, 1));
        QVERIFY(!service.calls.at(1).batch);
        QCOMPARE(service.calls.at(2).texts, texts.mid(4, 2));
        // A batch is told what came just before it, sources only.
        QCOMPARE(service.calls.at(0).context, QString());
        QCOMPARE(service.calls.at(2).context, QString("cccccc\na text of thirty characters ...."));
    }

    // A reply that does not line up is never paired by guesswork: its halves are asked again.
    void splitsABatchWhoseReplyDoesNotLineUp() {
        Service service;
        service.limits = {8, 0};
        service.answer = [](const Service::Call &call, int) -> std::optional<BatchTranslationResult> {
            if (call.texts.size() != 8) return std::nullopt;
            return BatchTranslationResult{true, {"only", "seven", "came", "back", "from", "the", "model"}, {}, std::nullopt, {}};
        };
        Fleet fleet(service, 2);
        const auto texts = numbered(8);
        const auto result = translate_paragraphs(texts, fleet.providers, PROTOTYPE);
        QCOMPARE(result.failed, 0);
        for (int index = 0; index < 8; ++index) QCOMPARE(result.translations.at(index), texts.at(index) + "!");
        QCOMPARE(service.batches(), 3);
        QCOMPARE(service.singles(), 0);
    }

    void stopsAskingForBatchesFromAServiceThatNeverKeepsTheContract() {
        Service service;
        service.limits = {4, 0};
        service.answer = [](const Service::Call &call, int) -> std::optional<BatchTranslationResult> {
            if (!call.batch) return std::nullopt;
            return refuse(TranslationErrorCode::UNKNOWN, "not an array");
        };
        Fleet fleet(service, 1);
        const auto result = translate_paragraphs(numbered(12), fleet.providers, PROTOTYPE);
        QCOMPARE(result.failed, 0);
        // The first batch and its first half; everything after goes text by text.
        QCOMPARE(service.batches(), 2);
        QCOMPARE(service.singles(), 12);
    }

    void aTextThatFailsLeavesTheOthersTranslated() {
        Service service;
        service.answer = [](const Service::Call &call, int) -> std::optional<BatchTranslationResult> {
            if (call.texts.first() == "text 2") return refuse(TranslationErrorCode::INVALID_REQUEST, "text too long");
            return std::nullopt;
        };
        Fleet fleet(service, 3);
        const auto result = translate_paragraphs(numbered(5), fleet.providers, PROTOTYPE);
        QCOMPARE(result.failed, 1);
        QCOMPARE(result.error, QString("text too long"));
        QVERIFY(!result.fatal);
        QCOMPARE(result.translations, QStringList({"text 0!", "text 1!", "", "text 3!", "text 4!"}));
    }

    void aRejectedKeyEndsTheRunAtOnce() {
        Service service;
        service.answer = [](const Service::Call &, int) { return std::optional(refuse(TranslationErrorCode::AUTH_FAILED, "invalid key")); };
        Fleet fleet(service, 1);
        const auto result = translate_paragraphs(numbered(20), fleet.providers, PROTOTYPE);
        QVERIFY(result.fatal);
        QCOMPARE(result.error, QString("invalid key"));
        QCOMPARE(result.failed, 20);
        QCOMPARE(service.calls.size(), 1);
    }

    // A service that takes four requests at once is asked for eight: the pace drops to what
    // it takes, not all the way to one at a time.
    void aRateLimitHalvesHowManyRunAtOnce() {
        Service service;
        service.delay_ms = 40;
        service.answer = [](const Service::Call &call, int) -> std::optional<BatchTranslationResult> {
            if (call.in_flight > 4) return refuse(TranslationErrorCode::RATE_LIMITED, "too many requests");
            return std::nullopt;
        };
        Fleet fleet(service, 8);
        const auto result = translate_paragraphs(numbered(40), fleet.providers, PROTOTYPE);
        QCOMPARE(result.failed, 0);
        // Only requests started at the first pace are refused: the four over the limit, and
        // at most the four that replaced the ones answered before the first refusal came in.
        const auto over_limit = [](const Service::Call &call) { return call.in_flight > 4; };
        const auto refused = std::count_if(service.calls.begin(), service.calls.end(), over_limit);
        QVERIFY2(refused >= 4 && refused <= 8, qPrintable(QString::number(refused)));
        QCOMPARE(std::count_if(service.calls.begin() + 16, service.calls.end(), over_limit), 0);
        // Afterwards four still run side by side.
        QVERIFY(std::any_of(service.calls.begin() + 16, service.calls.end(), [](const Service::Call &call) { return call.in_flight == 4; }));
    }

    void aServiceThatKeepsRefusingEndsTheRun() {
        Service service;
        service.answer = [](const Service::Call &, int) { return std::optional(refuse(TranslationErrorCode::RATE_LIMITED, "too many requests")); };
        Fleet fleet(service, 2);
        const auto result = translate_paragraphs(numbered(10), fleet.providers, PROTOTYPE);
        QCOMPARE(result.failed, 10);
        QVERIFY(!result.fatal);
        QCOMPARE(result.error, QString("too many requests"));
        // Two at once, one at a time, one a second, once more: then it stops. The second
        // request of the first step may not have started before the first was refused.
        QVERIFY2(service.calls.size() == 4 || service.calls.size() == 5, qPrintable(QString::number(service.calls.size())));
    }

    // A refusal for going too fast is answered by going slower, not by giving up.
    void aRateLimitSendsTheRestOneAtATime() {
        Service service;
        service.delay_ms = 30;
        service.answer = [](const Service::Call &, int index) -> std::optional<BatchTranslationResult> {
            if (index < 3) return refuse(TranslationErrorCode::RATE_LIMITED, "too many requests");
            return std::nullopt;
        };
        Fleet fleet(service, 3);
        const auto result = translate_paragraphs(numbered(3), fleet.providers, PROTOTYPE);
        QCOMPARE(result.failed, 0);
        QCOMPARE(result.translations, QStringList({"text 0!", "text 1!", "text 2!"}));
        QCOMPARE(service.calls.size(), 6);
    }

    void aServiceThatStaysUnreachableEndsTheRun() {
        Service service;
        service.answer = [](const Service::Call &, int) { return std::optional(refuse(TranslationErrorCode::NETWORK_ERROR, "Network error: down")); };
        Fleet fleet(service, 4);
        const auto result = translate_paragraphs(numbered(40), fleet.providers, PROTOTYPE);
        QCOMPARE(result.failed, 40);
        QVERIFY(!result.fatal);
        QCOMPARE(result.error, QString("Network error: down"));
        // The four in flight, then one retry: not one timeout per text.
        QVERIFY2(service.calls.size() <= 5, qPrintable(QString::number(service.calls.size())));
    }

    void aCancelEndsTheRunWithoutSendingTheRest() {
        Service service;
        service.delay_ms = 150;
        Fleet fleet(service, 2);
        HttpCancellation cancellation;
        std::unique_ptr<QThread> canceller(QThread::create([&] {
            QThread::msleep(60);
            cancellation.cancel();
        }));
        canceller->start();
        const auto result = translate_paragraphs(numbered(30), fleet.providers, PROTOTYPE, &cancellation);
        canceller->wait();
        QVERIFY(result.cancelled);
        QCOMPARE(service.calls.size(), 2);
    }

    void withoutAProviderNothingIsTranslated() {
        const auto result = translate_paragraphs({"a", "b"}, {}, PROTOTYPE);
        QVERIFY(result.fatal);
        QCOMPARE(result.translations, QStringList({"", ""}));
    }
};

QTEST_GUILESS_MAIN(ParagraphTranslatorTests)
#include "test_paragraph_translator.moc"
