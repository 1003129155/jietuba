#include "core/http_client.h"
#include "translation/deepl_service.h"
#include "translation/providers/azure.h"
#include "translation/providers/baidu.h"
#include "translation/providers/custom_llm.h"
#include "translation/providers/deepl.h"
#include "translation/providers/google.h"
#include "translation/providers/tencent.h"
#include "translation/providers/volcengine.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <deque>

using namespace jietuba;

namespace {
struct Exchange {
    QByteArray method, path;
    QMap<QByteArray, QByteArray> headers;  // lower-case names
    QByteArray body;
};

struct Reply {
    int status = 200;
    QByteArray reason = "OK";
    QByteArray body;
    int delay_ms = 0;
    bool drop = false;  // close without answering
};

// A plain HTTP/1.1 server on its own thread: the client under test blocks the calling thread.
class FakeServer {
public:
    FakeServer() {
        context_ = new QObject;
        context_->moveToThread(&thread_);
        thread_.start();
        QMetaObject::invokeMethod(
            context_,
            [this] {
                server_ = new QTcpServer(context_);
                QObject::connect(server_, &QTcpServer::newConnection, server_, [this] {
                    while (auto *socket = server_->nextPendingConnection()) serve(socket);
                });
                server_->listen(QHostAddress::LocalHost);
                port_ = server_->serverPort();
            },
            Qt::BlockingQueuedConnection);
    }
    ~FakeServer() {
        QMetaObject::invokeMethod(context_, [this] { delete context_; }, Qt::BlockingQueuedConnection);
        thread_.quit();
        thread_.wait();
    }
    QString url(const QString &path) const { return QString("http://127.0.0.1:%1%2").arg(port_).arg(path); }
    void queue(const Reply &reply) {
        QMutexLocker lock(&mutex_);
        replies_.push_back(reply);
    }
    QList<Exchange> exchanges() const {
        QMutexLocker lock(&mutex_);
        return exchanges_;
    }
    void clear() {
        QMutexLocker lock(&mutex_);
        exchanges_.clear();
        replies_.clear();
    }
private:
    void serve(QTcpSocket *socket) {
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
            buffer->append(socket->readAll());
            const int header_end = buffer->indexOf("\r\n\r\n");
            if (header_end < 0) return;
            Exchange exchange;
            const auto lines = buffer->left(header_end).split('\n');
            const auto request_line = lines.first().trimmed().split(' ');
            exchange.method = request_line.value(0);
            exchange.path = request_line.value(1);
            for (int i = 1; i < lines.size(); ++i) {
                const auto line = lines[i].trimmed();
                const int colon = line.indexOf(':');
                if (colon > 0) exchange.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
            }
            const int length = exchange.headers.value("content-length", "0").toInt();
            if (buffer->size() < header_end + 4 + length) return;
            exchange.body = buffer->mid(header_end + 4, length);
            buffer->clear();
            Reply reply;
            {
                QMutexLocker lock(&mutex_);
                exchanges_ << exchange;
                if (!replies_.empty()) {
                    reply = replies_.front();
                    replies_.pop_front();
                }
            }
            const auto answer = [socket = QPointer<QTcpSocket>(socket), reply] {
                if (!socket) return;
                if (reply.drop) {
                    socket->abort();
                    return;
                }
                QByteArray response = "HTTP/1.1 " + QByteArray::number(reply.status) + " " + reply.reason + "\r\n";
                response += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n";
                response += reply.body;
                socket->write(response);
                socket->disconnectFromHost();
            };
            if (reply.delay_ms > 0) QTimer::singleShot(reply.delay_ms, socket, answer);
            else answer();
        });
    }
    QThread thread_;
    QObject *context_ = nullptr;
    QTcpServer *server_ = nullptr;
    quint16 port_ = 0;
    mutable QMutex mutex_;
    std::deque<Reply> replies_;
    QList<Exchange> exchanges_;
};

QByteArray json(const QJsonValue &value) {
    return value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact) : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
}

QByteArray chat_reply(const QString &content) {
    return json(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", QJsonObject{{"role", "assistant"}, {"content", content}}}}}}});
}
}

// The HTTP client and the providers whose address is configurable, against a local server.
class HttpProviderTests : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<FakeServer> server_;
private slots:
    void initTestCase() {
        server_ = std::make_unique<FakeServer>();
    }
    void init() { server_->clear(); }
    void cleanupTestCase() { server_.reset(); }



    void theClientSendsAndClassifiesOutcomes() {
        server_->queue({200, "OK", "{\"ok\":true}"});
        auto response = http_request("POST", server_->url("/echo?x=1"), {{"X-Test", "yes"}}, "payload", 5);
        QCOMPARE(response.kind, HttpResponse::Ok);
        QCOMPARE(response.status, 200);
        QCOMPARE(response.body, QByteArray("{\"ok\":true}"));
        auto sent = server_->exchanges().last();
        QCOMPARE(sent.method, QByteArray("POST"));
        QCOMPARE(sent.path, QByteArray("/echo?x=1"));
        QCOMPARE(sent.headers.value("x-test"), QByteArray("yes"));
        QCOMPARE(sent.body, QByteArray("payload"));
        // 400 and above are HTTP errors with their body.
        server_->queue({429, "Too Many Requests", "{\"error\":\"slow down\"}"});
        response = http_request("GET", server_->url("/limited"));
        QCOMPARE(response.kind, HttpResponse::HttpError);
        QCOMPARE(response.status, 429);
        QCOMPARE(response.reason, QString("Too Many Requests"));
        QVERIFY(response.body.contains("slow down"));
        // A reply later than the timeout. The system checks timeouts coarsely: short ones end after about four seconds.
        server_->queue({200, "OK", "{}", 7000});
        QElapsedTimer waited;
        waited.start();
        response = http_request("GET", server_->url("/slow"), {}, {}, 1);
        QVERIFY2(waited.elapsed() < 6500, qPrintable(QString::number(waited.elapsed())));
        QCOMPARE(response.kind, HttpResponse::Timeout);
        // A dropped connection and an unparseable address are network errors.
        server_->queue({200, "OK", {}, 0, true});
        QCOMPARE(http_request("GET", server_->url("/drop"), {}, {}, 3).kind, HttpResponse::NetworkError);
        QCOMPARE(http_request("GET", "not a url").kind, HttpResponse::NetworkError);
        QCOMPARE(python_urlencode({{"q", "a b&c"}, {"to", QString::fromUtf8("中")}}), QString("q=a+b%26c&to=%E4%B8%AD"));
    }

    // A cancel from another thread ends a waiting exchange at once; later ones on that thread send nothing.
    void aCancelAbortsTheExchangeAtOnce() {
        server_->queue({200, "OK", "{}", 8000});
        HttpCancellation cancellation;
        HttpResponse first, second;
        std::unique_ptr<QThread> worker(QThread::create([&] {
            const HttpCancellationScope scope(&cancellation);
            first = http_request("GET", server_->url("/hang"), {}, {}, 30);
            second = http_request("GET", server_->url("/after"), {}, {}, 30);
        }));
        worker->start();
        QTRY_COMPARE(server_->exchanges().size(), 1);
        QElapsedTimer waited;
        waited.start();
        cancellation.cancel();
        QVERIFY(worker->wait(5000));
        QVERIFY2(waited.elapsed() < 1000, qPrintable(QString::number(waited.elapsed())));
        QCOMPARE(first.kind, HttpResponse::NetworkError);
        QVERIFY2(first.reason.contains("12017"), qPrintable(first.reason));
        QCOMPARE(second.kind, HttpResponse::NetworkError);
        QCOMPARE(server_->exchanges().size(), 1);
        // The scope ended with that thread: this one is unaffected.
        server_->queue({200, "OK", "{\"ok\":true}"});
        QCOMPARE(http_request("GET", server_->url("/plain")).kind, HttpResponse::Ok);
    }

    void azureTranslatesThroughItsEndpoint() {
        AzureTranslateProvider azure({{"api_key", "secret"}, {"region", "eastasia"}, {"endpoint", server_->url("/azure/")}});
        QVERIFY(azure.is_configured());
        server_->queue({200, "OK",
                        json(QJsonArray{QJsonObject{{"detectedLanguage", QJsonObject{{"language", "en"}, {"score", 1}}},
                                                    {"translations", QJsonArray{QJsonObject{{"text", QString::fromUtf8("你好")}, {"to", "zh-Hans"}}}}}})});
        auto result = azure.translate(TranslationRequest("hello", "zh-Hans", QString("en")));
        QVERIFY2(result.success, qPrintable(result.error_message));
        QCOMPARE(result.translated_text, QString::fromUtf8("你好"));
        QCOMPARE(result.detected_source_lang, QString("en"));
        const auto sent = server_->exchanges().last();
        QVERIFY(sent.path.startsWith("/azure/translate?"));
        const QUrlQuery query(QUrl(QString::fromLatin1(sent.path)).query());
        QCOMPARE(query.queryItemValue("to"), QString("zh-Hans"));
        QCOMPARE(query.queryItemValue("from"), QString("en"));
        QCOMPARE(sent.headers.value("ocp-apim-subscription-key"), QByteArray("secret"));
        QCOMPARE(sent.headers.value("ocp-apim-subscription-region"), QByteArray("eastasia"));
        QCOMPARE(QJsonDocument::fromJson(sent.body).array().first().toObject().value("Text").toString(), QString("hello"));

        const auto failure = [&](const Reply &reply) {
            server_->queue(reply);
            return azure.translate(TranslationRequest("hello", "ja"));
        };
        result = failure({401, "Unauthorized", json(QJsonObject{{"error", QJsonObject{{"code", "401000"}, {"message", "bad key"}}}})});
        QCOMPARE(result.error_code, std::optional(TranslationErrorCode::AUTH_FAILED));
        QCOMPARE(result.error_message, QString("bad key"));
        QCOMPARE(failure({429, "Too Many Requests", {}}).error_code, std::optional(TranslationErrorCode::RATE_LIMITED));
        QCOMPARE(failure({400, "Bad Request", json(QJsonObject{{"error", QJsonObject{{"code", "400036"}, {"message", "The target language is not valid"}}}})}).error_code,
                 std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(failure({503, "Service Unavailable", {}}).error_code, std::optional(TranslationErrorCode::NETWORK_ERROR));
        QCOMPARE(failure({200, "OK", "not json"}).error_code, std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(failure({200, "OK", "[]"}).error_code, std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(failure({200, "OK", json(QJsonArray{QJsonObject{{"translations", QJsonArray{}}}})}).error_code, std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(failure({200, "OK", {}, 0, true}).error_code, std::optional(TranslationErrorCode::NETWORK_ERROR));
        QCOMPARE(azure.translate(TranslationRequest("   ", "ja")).error_code, std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(AzureTranslateProvider({}).translate(TranslationRequest("hi", "ja")).error_code, std::optional(TranslationErrorCode::NOT_CONFIGURED));
    }

    // A capture's paragraphs go to Azure as one array and come back in the same order.
    void azureTakesSeveralTextsInOneRequest() {
        AzureTranslateProvider azure({{"api_key", "secret"}, {"endpoint", server_->url("/azure")}});
        QVERIFY(azure.supports_batch());
        const auto entry = [](const QString &text) { return QJsonObject{{"translations", QJsonArray{QJsonObject{{"text", text}}}}}; };
        QJsonObject first = entry(QString::fromUtf8("设置"));
        first.insert("detectedLanguage", QJsonObject{{"language", "en"}});
        server_->queue({200, "OK", json(QJsonArray{first, entry(QString::fromUtf8("取消")), entry(QString::fromUtf8("应用"))})});
        const auto batch = azure.translate_batch({"Settings", "Cancel", "Apply"}, TranslationRequest(QString(), "zh-Hans"));
        QVERIFY2(batch.success, qPrintable(batch.error_message));
        QCOMPARE(batch.translations, QStringList({QString::fromUtf8("设置"), QString::fromUtf8("取消"), QString::fromUtf8("应用")}));
        QCOMPARE(batch.detected_source_lang, QString("en"));
        QCOMPARE(server_->exchanges().size(), 1);
        const auto sent = QJsonDocument::fromJson(server_->exchanges().last().body).array();
        QCOMPARE(sent.size(), 3);
        QCOMPARE(sent.at(2).toObject().value("Text").toString(), QString("Apply"));

        const auto failure = [&](const Reply &reply) {
            server_->queue(reply);
            return azure.translate_batch({"One", "Two"}, TranslationRequest(QString(), "ja")).error_code;
        };
        // Fewer results than texts cannot be paired up.
        QCOMPARE(failure({200, "OK", json(QJsonArray{entry("Eins")})}), std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(failure({200, "OK", "{}"}), std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(failure({200, "OK", "not json"}), std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(failure({429, "Too Many Requests", {}}), std::optional(TranslationErrorCode::RATE_LIMITED));
        QCOMPARE(failure({200, "OK", {}, 0, true}), std::optional(TranslationErrorCode::NETWORK_ERROR));
        QCOMPARE(azure.translate_batch({"One", " "}, TranslationRequest(QString(), "ja")).error_code, std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(azure.translate_batch({}, TranslationRequest(QString(), "ja")).error_code, std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(AzureTranslateProvider({}).translate_batch({"One", "Two"}, TranslationRequest(QString(), "ja")).error_code,
                 std::optional(TranslationErrorCode::NOT_CONFIGURED));
    }

    // DeepL takes the text parameter once per text.
    void deeplTakesSeveralTextsInOneRequest() {
        DeepLService deepl("secret:fx");
        deepl.api_url = server_->url("/v2/translate");
        server_->queue({200, "OK", json(QJsonObject{{"translations", QJsonArray{QJsonObject{{"text", "Hallo"}, {"detected_source_language", "EN"}},
                                                                                QJsonObject{{"text", "Welt"}, {"detected_source_language", "EN"}}}}})});
        const auto batch = deepl.translate_batch({"Hello", "World & more"}, "de", std::nullopt, "nonewlines", true, 5);
        QVERIFY2(batch.success, qPrintable(batch.error));
        QCOMPARE(batch.translated_texts, QStringList({"Hallo", "Welt"}));
        QCOMPARE(batch.detected_source_lang, QString("EN"));
        QUrlQuery form;
        form.setQuery(QString::fromUtf8(server_->exchanges().last().body).replace('+', "%20"));
        QCOMPARE(form.allQueryItemValues("text", QUrl::FullyDecoded), QStringList({"Hello", "World & more"}));
        QCOMPARE(form.queryItemValue("target_lang"), QString("DE"));
        // One translation short: nothing is paired by guesswork.
        server_->queue({200, "OK", json(QJsonObject{{"translations", QJsonArray{QJsonObject{{"text", "Hallo"}}}}})});
        QCOMPARE(deepl.translate_batch({"Hello", "World"}, "de").error, QString("Invalid API response format"));
        QCOMPARE(deepl.translate_batch({"Hello", "  "}, "de").error, QString("Text is empty"));
        QCOMPARE(deepl.translate_batch({}, "de").error, QString("Text is empty"));

        DeepLProvider provider({});
        QVERIFY(provider.supports_batch());
        const auto unconfigured = provider.translate_batch({"Hello", "World"}, TranslationRequest(QString(), "de", QString("en")));
        QCOMPARE(unconfigured.error_code, std::optional(TranslationErrorCode::AUTH_FAILED));
        QCOMPARE(unconfigured.error_message, QString("API key not configured"));
    }

    // The services with a fixed address: what a batch sends and how its reply is read.
    void googleAndVolcenginePairABatchWithItsReply() {
        GoogleTranslateProvider google({{"api_key", "k"}});
        QVERIFY(google.supports_batch());
        const auto google_body = QJsonDocument::fromJson(google.batch_payload({"One", "Two"}, TranslationRequest(QString(), "zh-Hans", QString("en")))).object();
        QCOMPARE(google_body.value("q").toArray(), QJsonArray({"One", "Two"}));
        QCOMPARE(google_body.value("target").toString(), QString("zh-CN"));
        QCOMPARE(google_body.value("source").toString(), QString("en"));
        const QJsonObject google_reply{
            {"data", QJsonObject{{"translations", QJsonArray{QJsonObject{{"translatedText", "A &amp; B"}, {"detectedSourceLanguage", "en"}},
                                                             QJsonObject{{"translatedText", "C"}}}}}}};
        const auto google_batch = GoogleTranslateProvider::parse_batch(google_reply, 2);
        QVERIFY(google_batch.success);
        QCOMPARE(google_batch.translations, QStringList({"A & B", "C"}));
        QCOMPARE(google_batch.detected_source_lang, QString("en"));
        QCOMPARE(GoogleTranslateProvider::parse_batch(google_reply, 3).error_code, std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(google.translate_batch({"One", ""}, TranslationRequest(QString(), "ja")).error_code, std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(google.translate_batch({}, TranslationRequest(QString(), "ja")).error_code, std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(GoogleTranslateProvider({}).translate_batch({"One", "Two"}, TranslationRequest(QString(), "ja")).error_code,
                 std::optional(TranslationErrorCode::NOT_CONFIGURED));

        VolcengineTranslateProvider volcengine({{"access_key_id", "ak"}, {"secret_access_key", "sk"}});
        QCOMPARE(volcengine.batch_limits().max_texts, 16);
        const auto volc_body = QJsonDocument::fromJson(volcengine.batch_payload({"One", "Two"}, TranslationRequest(QString(), "zh-Hans"))).object();
        QCOMPARE(volc_body.value("TextList").toArray(), QJsonArray({"One", "Two"}));
        QCOMPARE(volc_body.value("TargetLanguage").toString(), QString("zh"));
        const QJsonObject volc_reply{{"TranslationList", QJsonArray{QJsonObject{{"Translation", "Yi"}, {"DetectedSourceLanguage", "en"}},
                                                                    QJsonObject{{"Translation", "Er"}, {"DetectedSourceLanguage", "en"}}}}};
        const auto volc_batch = volcengine.parse_batch(volc_reply, 2);
        QVERIFY(volc_batch.success);
        QCOMPARE(volc_batch.translations, QStringList({"Yi", "Er"}));
        QCOMPARE(volc_batch.detected_source_lang, QString("en"));
        QCOMPARE(volcengine.parse_batch(volc_reply, 3).error_code, std::optional(TranslationErrorCode::UNKNOWN));
        const QJsonObject limited{{"ResponseMetadata", QJsonObject{{"Error", QJsonObject{{"Code", "-429"}, {"Message", "too frequent"}}}}}};
        QCOMPARE(volcengine.parse_batch(limited, 2).error_code, std::optional(TranslationErrorCode::RATE_LIMITED));
        QCOMPARE(volcengine.translate_batch({"One", " "}, TranslationRequest(QString(), "ja")).error_code, std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(volcengine.translate_batch({}, TranslationRequest(QString(), "ja")).error_code, std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(VolcengineTranslateProvider({}).translate_batch({"One", "Two"}, TranslationRequest(QString(), "ja")).error_code,
                 std::optional(TranslationErrorCode::NOT_CONFIGURED));

        // Tencent keeps one text a request, paced under its five requests a second.
        TencentTranslateProvider tencent({{"secret_id", "id"}, {"secret_key", "key"}});
        QVERIFY(!tencent.supports_batch());
        QVERIFY(tencent.parallel_requests * 1000 / tencent.request_interval_ms <= 8);
        QVERIFY(1000 / tencent.request_interval_ms < 5);
    }

    // Baidu answers one entry per line of q, which is how several texts share a request.
    void baiduTellsABatchApartByItsLines() {
        BaiduTranslateProvider baidu({{"appid", "id"}, {"secret_key", "key"}});
        QVERIFY(baidu.supports_batch());
        QVector<int> counts;
        const auto lines = BaiduTranslateProvider::batch_lines({"Save", "A wrapped\nparagraph", "Cancel"}, &counts);
        QVERIFY(lines.has_value());
        QCOMPARE(*lines, QStringList({"Save", "A wrapped", "paragraph", "Cancel"}));
        QCOMPARE(counts, QVector<int>({1, 2, 1}));
        const auto translations = BaiduTranslateProvider::batch_translations({"S", "W", "P", "C"}, counts);
        QVERIFY(translations.has_value());
        QCOMPARE(*translations, QStringList({"S", "W\nP", "C"}));
        // An entry short cannot be paired up; a blank line would be missing from the reply.
        QVERIFY(!BaiduTranslateProvider::batch_translations({"S", "W", "C"}, counts).has_value());
        QVector<int> unused;
        QVERIFY(!BaiduTranslateProvider::batch_lines({"Save", "two\n\nlines"}, &unused).has_value());

        QStringList entries;
        const QJsonObject reply{{"from", "en"},
                                {"trans_result", QJsonArray{QJsonObject{{"src", "Save"}, {"dst", "S"}}, QJsonObject{{"src", "Cancel"}, {"dst", "C"}}}}};
        const auto parsed = baidu.parse_response(reply, &entries);
        QVERIFY2(parsed.success, qPrintable(parsed.error_message));
        QCOMPARE(parsed.translated_text, QString("S\nC"));
        QCOMPARE(entries, QStringList({"S", "C"}));
        // Over the account's requests per second: a reason to slow down, not a failed text.
        QCOMPARE(baidu.parse_response(QJsonObject{{"error_code", 59004}, {"error_msg", "qps limit"}}).error_code,
                 std::optional(TranslationErrorCode::RATE_LIMITED));

        QCOMPARE(baidu.translate_batch({"Save", "two\n\nlines"}, TranslationRequest(QString(), "ja")).error_code,
                 std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(baidu.translate_batch({}, TranslationRequest(QString(), "ja")).error_code, std::optional(TranslationErrorCode::INVALID_REQUEST));
        QCOMPARE(BaiduTranslateProvider({}).translate_batch({"Save", "Cancel"}, TranslationRequest(QString(), "ja")).error_code,
                 std::optional(TranslationErrorCode::NOT_CONFIGURED));
    }

    void anOpenAiCompatibleServiceTranslatesAndListsModels() {
        CustomLLMProvider llm({{"api_key", "k"}, {"base_url", server_->url("/v1/")}, {"model", "local-model"}});
        QVERIFY(llm.is_configured());
        server_->queue({200, "OK", chat_reply("{\"translation\":\"Bonjour\",\"detected_source_lang\":\"en\"}")});
        auto result = llm.translate(TranslationRequest("Hello", "fr"));
        QVERIFY2(result.success, qPrintable(result.error_message));
        QCOMPARE(result.translated_text, QString("Bonjour"));
        auto sent = server_->exchanges().last();
        QCOMPARE(sent.path, QByteArray("/v1/chat/completions"));
        QCOMPARE(sent.headers.value("authorization"), QByteArray("Bearer k"));
        const auto body = QJsonDocument::fromJson(sent.body).object();
        QCOMPARE(body.value("model").toString(), QString("local-model"));
        QCOMPARE(body.value("messages").toArray().last().toObject().value("content").toString(), QString("Hello"));

        // A service that rejects JSON mode is asked again without it.
        server_->clear();
        server_->queue({400, "Bad Request", json(QJsonObject{{"error", "response_format is not supported"}})});
        server_->queue({200, "OK", chat_reply("Hallo")});
        result = llm.translate(TranslationRequest("Hello", "de"));
        QVERIFY2(result.success, qPrintable(result.error_message));
        QCOMPARE(result.translated_text, QString("Hallo"));
        const auto attempts = server_->exchanges();
        QCOMPARE(attempts.size(), 2);
        QVERIFY(QJsonDocument::fromJson(attempts[0].body).object().contains("response_format"));
        QVERIFY(!QJsonDocument::fromJson(attempts[1].body).object().contains("response_format"));

        const auto failure = [&](const Reply &reply) {
            server_->queue(reply);
            return llm.translate(TranslationRequest("Hello", "de"));
        };
        result = failure({401, "Unauthorized", json(QJsonObject{{"error", QJsonObject{{"message", "invalid key"}, {"type", "authentication_error"}}}})});
        QCOMPARE(result.error_code, std::optional(TranslationErrorCode::AUTH_FAILED));
        QCOMPARE(result.error_message, QString("invalid key"));
        QCOMPARE(failure({402, "Payment Required", {}}).error_code, std::optional(TranslationErrorCode::QUOTA_EXCEEDED));
        QCOMPARE(failure({429, "Too Many Requests", {}}).error_code, std::optional(TranslationErrorCode::RATE_LIMITED));
        QCOMPARE(failure({500, "Internal Server Error", {}}).error_code, std::optional(TranslationErrorCode::NETWORK_ERROR));
        QCOMPARE(failure({404, "Not Found", {}}).error_code, std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(failure({200, "OK", "<html>"}).error_code, std::optional(TranslationErrorCode::UNKNOWN));
        QCOMPARE(failure({200, "OK", {}, 0, true}).error_code, std::optional(TranslationErrorCode::NETWORK_ERROR));

        // Model list: ids sorted case-insensitively, duplicates and junk dropped.
        QJsonArray models;
        for (const char *id : {"beta", "Alpha", "beta"}) models.append(QJsonObject{{"id", id}});
        models.append(3);
        models.append(QJsonObject());
        server_->queue({200, "OK", json(QJsonObject{{"data", models}})});
        QCOMPARE(llm.list_models(), QStringList({"Alpha", "beta"}));
        QCOMPARE(server_->exchanges().last().path, QByteArray("/v1/models"));
        const auto list_error = [&](const Reply &reply) {
            server_->queue(reply);
            try {
                llm.list_models();
            } catch (const std::exception &e) {
                return QString::fromUtf8(e.what());
            }
            return QString();
        };
        QCOMPARE(list_error({401, "Unauthorized", json(QJsonObject{{"error", QJsonObject{{"message", "no access"}}}})}), QString("no access"));
        QCOMPARE(list_error({200, "OK", "{\"data\":5}"}), QString("Invalid model list response"));
        QCOMPARE(list_error({200, "OK", "garbage"}), QString("Invalid model list response"));
        QVERIFY(list_error({200, "OK", {}, 0, true}).startsWith("Network error"));
        CustomLLMProvider unconfigured({});
        QCOMPARE(unconfigured.translate(TranslationRequest("Hello", "de")).error_code, std::optional(TranslationErrorCode::NOT_CONFIGURED));
    }

    // A page of paragraphs goes out as one request and comes back in the same order: this is
    // what keeps a long capture from costing one round trip per paragraph.
    void aBatchTranslationKeepsTheInputOrder() {
        CustomLLMProvider llm({{"api_key", "k"}, {"base_url", server_->url("/v1")}, {"model", "m"}});
        QVERIFY(llm.supports_batch());
        QCOMPARE(llm.parallel_requests, 8);
        const QStringList texts{"Hello", "World", "Good morning"};
        server_->queue({200, "OK", chat_reply("{\"translations\":[\"Bonjour\",\"Monde\",\"Bonjour\"]}")});
        const auto batch = llm.translate_batch(texts, TranslationRequest(QString(), "fr"));
        QVERIFY2(batch.success, qPrintable(batch.error_message));
        QCOMPARE(batch.translations, QStringList({"Bonjour", "Monde", "Bonjour"}));
        const auto sent = server_->exchanges();
        QCOMPARE(sent.size(), 1);
        const auto messages = QJsonDocument::fromJson(sent.last().body).object().value("messages").toArray();
        QVERIFY(messages.first().toObject().value("content").toString().contains("exactly 3 strings"));
        // The texts travel as a JSON array, so a paragraph holding quotes stays data.
        const auto payload = QJsonDocument::fromJson(messages.last().toObject().value("content").toString().toUtf8()).array();
        QCOMPARE(payload.size(), 3);
        QCOMPARE(payload.at(1).toString(), QString("World"));

        // One capture, one page: the entries are described as reading order rather than as
        // unrelated strings, which is what lets a term or a pronoun resolve across them.
        const auto system = messages.first().toObject().value("content").toString();
        QVERIFY2(system.contains("reading order"), qPrintable(system));
        QVERIFY(!system.contains("independent"));

        // The paragraphs before the batch reach the model as reference, not as entries.
        server_->clear();
        server_->queue({200, "OK", chat_reply("{\"translations\":[\"Reglages\",\"Appliquer\"]}")});
        const QVariantMap options{{"context", "Display options"}};
        QVERIFY(llm.translate_batch({"Settings", "Apply"}, TranslationRequest(QString(), "fr", std::nullopt, true, 10, options)).success);
        const auto carried = QJsonDocument::fromJson(server_->exchanges().last().body).object().value("messages").toArray();
        const auto note = carried.first().toObject().value("content").toString();
        QVERIFY2(note.contains("reference only, to show what they continue"), qPrintable(note));
        QVERIFY(note.contains("Display options"));
        QCOMPARE(QJsonDocument::fromJson(carried.last().toObject().value("content").toString().toUtf8()).array().size(), 2);

        // A fenced reply and a leading think block are both still read.
        server_->clear();
        server_->queue({200, "OK", chat_reply("```json\n{\"translations\":[\"Eins\",\"Zwei\"]}\n```")});
        const auto fenced = llm.translate_batch({"One", "Two"}, TranslationRequest(QString(), "de"));
        QVERIFY2(fenced.success, qPrintable(fenced.error_message));
        QCOMPARE(fenced.translations, QStringList({"Eins", "Zwei"}));
        server_->clear();
        server_->queue({200, "OK", chat_reply("<think>hmm</think>\n{\"translations\":[\"Un\",\"Deux\"]}")});
        QCOMPARE(llm.translate_batch({"One", "Two"}, TranslationRequest(QString(), "de")).translations, QStringList({"Un", "Deux"}));

        // A short or reordered array is refused, not paired with the wrong paragraph: the
        // caller then asks text by text.
        server_->clear();
        server_->queue({200, "OK", chat_reply("{\"translations\":[\"Seul\"]}")});
        const auto short_reply = llm.translate_batch({"One", "Two"}, TranslationRequest(QString(), "de"));
        QVERIFY(!short_reply.success);
        QCOMPARE(short_reply.error_message, QString("Custom LLM-1 returned 1 translations for 2 texts"));
        server_->queue({200, "OK", chat_reply("{\"translation\":\"Seul\"}")});
        QVERIFY(!llm.translate_batch({"One", "Two"}, TranslationRequest(QString(), "de")).success);
        // A rejected JSON mode is retried without it, as for single texts.
        server_->clear();
        server_->queue({400, "Bad Request", json(QJsonObject{{"error", "response_format is not supported"}})});
        server_->queue({200, "OK", chat_reply("{\"translations\":[\"Uno\",\"Dos\"]}")});
        QVERIFY(llm.translate_batch({"One", "Two"}, TranslationRequest(QString(), "es")).success);
        const auto attempts = server_->exchanges();
        QCOMPARE(attempts.size(), 2);
        QVERIFY(!QJsonDocument::fromJson(attempts[1].body).object().contains("response_format"));
    }

    // DeepL posts a form with the key in a header and reads the first translation.
    void deeplTranslatesAndNamesHttpErrors() {
        DeepLService deepl("secret:fx");
        QCOMPARE(deepl.api_url, DeepLService::API_URL_FREE);
        QCOMPARE(DeepLService("k", true).api_url, DeepLService::API_URL_PRO);
        deepl.api_url = server_->url("/v2/translate");
        server_->queue({200, "OK", json(QJsonObject{{"translations", QJsonArray{QJsonObject{{"text", "Hallo"}, {"detected_source_language", "EN"}}}}})});
        auto result = deepl.translate("Hello", "de", QString("en"), "nonewlines", false, 5);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.translated_text, QString("Hallo"));
        QCOMPARE(result.detected_source_lang, QString("EN"));
        const auto sent = server_->exchanges().last();
        QCOMPARE(sent.headers.value("authorization"), QByteArray("DeepL-Auth-Key secret:fx"));
        const QUrlQuery form(QString::fromUtf8(sent.body));
        QCOMPARE(form.queryItemValue("target_lang"), QString("DE"));
        QCOMPARE(form.queryItemValue("source_lang"), QString("EN"));
        QCOMPARE(form.queryItemValue("split_sentences"), QString("nonewlines"));
        QCOMPARE(form.queryItemValue("preserve_formatting"), QString("0"));
        const auto failure = [&](const Reply &reply) {
            server_->queue(reply);
            return deepl.translate("Hello", "de").error;
        };
        QCOMPARE(failure({456, "Quota", {}}), QString("Quota exceeded"));
        QCOMPARE(failure({418, "Teapot", {}}), QString("HTTP 418: Teapot"));
        QCOMPARE(failure({200, "OK", "nope"}), QString("Failed to parse API response"));
        QCOMPARE(failure({200, "OK", "{\"translations\":[]}"}), QString("Invalid API response format"));
        QVERIFY(failure({200, "OK", {}, 0, true}).startsWith("Network error"));
        QCOMPARE(deepl.translate("  ").error, QString("Text is empty"));
        QCOMPARE(DeepLService("").translate("Hello").error, QString("API key not configured"));
    }
};

QTEST_GUILESS_MAIN(HttpProviderTests)
#include "test_http_providers.moc"
