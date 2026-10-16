#include "core/http_client.h"
#include "translation/deepl_service.h"
#include "translation/providers/azure.h"
#include "translation/providers/custom_llm.h"
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
