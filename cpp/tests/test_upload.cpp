#include "core/file_names.h"
#include "core/http_client.h"
#include "settings/tool_settings.h"
#include "ui/fluent_lite/buttons.h"
#include "ui/fluent_lite/inputs.h"
#include "ui/settings_ui/upload_target_dialog.h"
#include "upload/s3_client.h"
#include "upload/s3_signer.h"
#include "upload/s3_target.h"
#include "upload/upload_service.h"
#include <QClipboard>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QLabel>
#include <QMutex>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <deque>
#include <memory>
#include <optional>

using namespace jietuba;
using namespace jietuba::upload;

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
    QByteArray extra_headers;  // whole lines, each ending in CRLF
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
    QString endpoint() const { return QString("http://127.0.0.1:%1").arg(port_); }
    QByteArray host() const { return QByteArray("127.0.0.1:") + QByteArray::number(port_); }
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
                response += reply.extra_headers;
                response += "Content-Type: application/xml\r\nContent-Length: " + QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n";
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

QString header(const HttpHeaders &headers, const QString &name) {
    for (const auto &[key, value] : headers)
        if (key.compare(name, Qt::CaseInsensitive) == 0) return value;
    return {};
}

QDateTime utc(int y, int mo, int d, int h, int mi, int s) { return QDateTime(QDate(y, mo, d), QTime(h, mi, s), QTimeZone::UTC); }

Target target(const QString &endpoint, const QString &bucket, Addressing addressing = Addressing::Path) {
    Target t;
    t.id = "id-1";
    t.name = "Main";
    t.endpoint = endpoint;
    t.region = "us-east-1";
    t.bucket = bucket;
    t.access_key_id = "AKID";
    t.secret_access_key = "secretKEY";
    t.addressing = addressing;
    return t;
}

QByteArray xml_error(const QByteArray &code) { return "<?xml version=\"1.0\"?><Error><Code>" + code + "</Code><Message>secret details</Message></Error>"; }
}

// Cloud upload: the S3 request signature, the addresses and keys, the target list, and the whole exchange against a local server.
class UploadTests : public QObject {
    Q_OBJECT
private:
    std::unique_ptr<FakeServer> server_;
    QTemporaryDir directory_;
    std::unique_ptr<QSettings> settings_;
    std::unique_ptr<ToolSettingsManager> config_;

    void configureService(const QString &format = "url") {
        auto t = target(server_->endpoint(), "bk");
        t.path_prefix = "pre/{yyyy}";
        config_->set_upload_targets(serialize_targets({t}));
        config_->set_upload_default_target(t.id);
        config_->set_upload_link_format(format);
        config_->set_screenshot_format("PNG");
    }
private slots:
    void initTestCase() { server_ = std::make_unique<FakeServer>(); }
    void init() {
        server_->clear();
        settings_ = std::make_unique<QSettings>(directory_.filePath("upload.ini"), QSettings::IniFormat);
        settings_->clear();
        config_ = std::make_unique<ToolSettingsManager>(*settings_);
    }
    void cleanupTestCase() { server_.reset(); }

    // The PUT Object example of the S3 documentation: the same request must give the same signature.
    void theSignatureMatchesTheDocumentedPutObjectExample() {
        Target t;
        t.endpoint = "https://s3.amazonaws.com";
        t.region = "us-east-1";
        t.bucket = "examplebucket";
        t.addressing = Addressing::Virtual;
        t.access_key_id = "AKIAIOSFODNN7EXAMPLE";
        t.secret_access_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
        const auto request = sign_put(t, "test$file.text", "Welcome to Amazon S3.", {}, utc(2013, 5, 24, 0, 0, 0),
                                      {{"date", "Fri, 24 May 2013 00:00:00 GMT"}, {"x-amz-storage-class", "REDUCED_REDUNDANCY"}});
        QCOMPARE(request.url, QString("https://examplebucket.s3.amazonaws.com/test%24file.text"));
        QCOMPARE(header(request.headers, "Authorization"),
                 QString("AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                         "SignedHeaders=date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class, "
                         "Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"));
        QCOMPARE(header(request.headers, "x-amz-content-sha256"), QString("44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"));
        QCOMPARE(header(request.headers, "x-amz-date"), QString("20130524T000000Z"));
        // The HTTP stack writes Host from the URL; it is signed but not sent twice.
        QVERIFY(header(request.headers, "host").isEmpty());
    }

    // Path-style with a port, a content type and a key that needs encoding: the signature was computed apart, from the
    // documented algorithm, for exactly this request.
    void aPathStyleRequestWithAContentTypeIsSignedOverItsEncodedPath() {
        const auto request = sign_put(target("http://localhost:9000", "b"), QString::fromUtf8("a b/中.png"), "Welcome to Amazon S3.", "image/png", utc(2024, 1, 2, 3, 4, 5));
        QCOMPARE(request.url, QString("http://localhost:9000/b/a%20b/%E4%B8%AD.png"));
        QCOMPARE(header(request.headers, "Authorization"),
                 QString("AWS4-HMAC-SHA256 Credential=AKID/20240102/us-east-1/s3/aws4_request, "
                         "SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date, "
                         "Signature=4fa1e5a29b5ec71cab82a809d35372e330d7bde7e9d42e932cac7c7accc49aba"));
        QCOMPARE(header(request.headers, "Content-Type"), QString("image/png"));
    }

    void keysAreEncodedByteByByteAndSlashesStay() {
        QCOMPARE(uri_encode(QString::fromUtf8("a b/中~._-"), true), QByteArray("a%20b/%E4%B8%AD~._-"));
        QCOMPARE(uri_encode("a/b", false), QByteArray("a%2Fb"));
        QCOMPARE(uri_encode("100%+$#?&=", true), QByteArray("100%25%2B%24%23%3F%26%3D"));
        QCOMPARE(uri_encode("", true), QByteArray());
    }

    void addressesFollowTheAddressingStyle() {
        const QString key = "k/x y.png";
        QCOMPARE(locate(target("http://localhost:9000", "b"), key).url, QString("http://localhost:9000/b/k/x%20y.png"));
        const auto virtual_host = locate(target("https://s3.us-east-1.amazonaws.com", "my-bucket", Addressing::Virtual), key);
        QCOMPARE(virtual_host.url, QString("https://my-bucket.s3.us-east-1.amazonaws.com/k/x%20y.png"));
        QCOMPARE(virtual_host.host, QByteArray("my-bucket.s3.us-east-1.amazonaws.com"));
        // A bucket with a dot, or an endpoint that is an address, cannot be a host name: those stay path-style.
        QCOMPARE(locate(target("https://s3.eu.amazonaws.com", "my.bucket", Addressing::Virtual), key).url, QString("https://s3.eu.amazonaws.com/my.bucket/k/x%20y.png"));
        QCOMPARE(locate(target("http://10.0.0.5:9000", "b", Addressing::Virtual), key).url, QString("http://10.0.0.5:9000/b/k/x%20y.png"));
        QCOMPARE(locate(target("http://[::1]:9000", "b", Addressing::Virtual), key).host, QByteArray("[::1]:9000"));
        // A port other than the scheme's own is part of the host, and one that is the default is not.
        QCOMPARE(locate(target("https://host.test:8443", "b", Addressing::Virtual), key).host, QByteArray("b.host.test:8443"));
        QCOMPARE(locate(target("https://host.test:443", "b"), key).host, QByteArray("host.test"));
        QCOMPARE(locate(target("http://host.test:80", "b"), key).host, QByteArray("host.test"));
        // A path in the endpoint is kept, without its trailing slash.
        QCOMPARE(locate(target("https://host.test/proxy/", "b"), key).url, QString("https://host.test/proxy/b/k/x%20y.png"));
        QCOMPARE(locate(target("https://host.test/proxy", "b", Addressing::Virtual), key).url, QString("https://b.host.test/proxy/k/x%20y.png"));
    }

    void theCopiedLinkIsThePublicAddressOrTheObjectAddress() {
        const QString key = "k/x y.png";
        auto t = target("http://localhost:9000", "b");
        QCOMPARE(public_link(t, key), QString("http://localhost:9000/b/k/x%20y.png"));
        t.public_url = "https://cdn.example.com/assets/";
        QCOMPARE(public_link(t, key), QString("https://cdn.example.com/assets/k/x%20y.png"));
        t.public_url = "https://img.example.com";
        t.addressing = Addressing::Virtual;
        QCOMPARE(public_link(t, key), QString("https://img.example.com/k/x%20y.png"));
        QCOMPARE(format_link("https://a/b.png", LinkFormat::Url), QString("https://a/b.png"));
        QCOMPARE(format_link("https://a/b.png", LinkFormat::Markdown), QString("![](https://a/b.png)"));
        QCOMPARE(format_link("https://a/b.png", LinkFormat::Html), QString("<img src=\"https://a/b.png\">"));
        QCOMPARE(parse_link_format("markdown"), LinkFormat::Markdown);
        QCOMPARE(parse_link_format("html"), LinkFormat::Html);
        QCOMPARE(parse_link_format("anything else"), LinkFormat::Url);
        QCOMPARE(link_format_name(LinkFormat::Html), QString("html"));
        QCOMPARE(link_format_name(LinkFormat::Url), QString("url"));
        QCOMPARE(link_format_name(LinkFormat::Markdown), QString("markdown"));
    }

    void objectKeysCarryThePrefixTheNameAndARandomPart() {
        const auto when = QDateTime(QDate(2024, 3, 5), QTime(9, 8, 7));
        QCOMPARE(expand_prefix("shots/{yyyy}/{MM}/{dd}/", when), QString("shots/2024/03/05"));
        QCOMPARE(expand_prefix("//a\\b//{yyyy}", when), QString("a/b/2024"));
        QCOMPARE(expand_prefix("", when), QString());
        QCOMPARE(expand_prefix("/", when), QString());
        QCOMPARE(make_object_key("p/{yyyy}", "Screenshot_1", "0a1b2c3d", ".png", when), QString("p/2024/Screenshot_1-0a1b2c3d.png"));
        QCOMPARE(make_object_key("", "pinned_2", "ffffffff", ".jpg", when), QString("pinned_2-ffffffff.jpg"));
        static const QRegularExpression eight_hex("^[0-9a-f]{8}$");
        QSet<QString> seen;
        for (int i = 0; i < 20; ++i) {
            const auto random = random_suffix();
            QVERIFY(eight_hex.match(random).hasMatch());
            seen.insert(random);
        }
        QVERIFY(seen.size() > 15);
    }

    void targetsRoundTripThroughTheirJson() {
        auto first = target("https://s3.us-east-1.amazonaws.com", "bucket-one", Addressing::Virtual);
        first.id = "11111111-1111-1111-1111-111111111111";
        first.path_prefix = "a/{yyyy}";
        first.public_url = "https://cdn.test";
        auto second = target("http://localhost:9000", "b2");
        second.id = "22222222-2222-2222-2222-222222222222";
        second.name = "Local";
        const auto json = serialize_targets({first, second});
        const auto back = parse_targets(json);
        QCOMPARE(back.size(), size_t(2));
        QCOMPARE(back[0].id, first.id);
        QCOMPARE(back[0].endpoint, first.endpoint);
        QCOMPARE(back[0].addressing, Addressing::Virtual);
        QCOMPARE(back[0].path_prefix, QString("a/{yyyy}"));
        QCOMPARE(back[0].public_url, QString("https://cdn.test"));
        QCOMPARE(back[0].secret_access_key, QString("secretKEY"));
        QCOMPARE(back[1].name, QString("Local"));
        QCOMPARE(back[1].addressing, Addressing::Path);
        QCOMPARE(find_target(back, second.id)->bucket, QString("b2"));
        QVERIFY(!find_target(back, "unknown"));
        QVERIFY(!find_target(back, ""));
        // What is not a usable entry is dropped, and a repeated id keeps the first.
        QVERIFY(parse_targets("").empty());
        QVERIFY(parse_targets("not json").empty());
        QVERIFY(parse_targets("{\"id\":\"x\"}").empty());
        const auto messy = parse_targets("[1, {\"name\":\"no id\"}, {\"id\":\"a\",\"name\":\"first\"}, {\"id\":\"a\",\"name\":\"second\"}, \"text\"]");
        QCOMPARE(messy.size(), size_t(1));
        QCOMPARE(messy[0].name, QString("first"));
        QVERIFY(serialize_targets({}) == "[]");
    }

    void validationNamesTheFieldsThatAreWrong() {
        const auto fields = [](const Target &t, const std::vector<Target> &others = {}) {
            QStringList names;
            for (const auto &error : validate(t, others)) names << error.field;
            return names;
        };
        QVERIFY(fields(target("http://localhost:9000", "b")).isEmpty());
        QVERIFY(fields(target("https://example.com/base/path", "b")).isEmpty());
        QCOMPARE(fields(Target{}), (QStringList{"name", "endpoint", "region", "bucket", "access_key_id", "secret_access_key"}));
        // Endpoint: an http or https address with a host and nothing else.
        for (const QString &bad : {QString("ftp://host"), QString("localhost:9000"), QString("http://"), QString("https://user:pw@host"), QString("https://host?x=1"),
                                   QString("https://host#frag"), QString("https://<ACCOUNT_ID>.r2.cloudflarestorage.com"), QString("https://a b.com")})
            QCOMPARE(fields(target(bad, "b")), (QStringList{"endpoint"}));
        auto t = target("http://localhost:9000", "has/slash");
        QCOMPARE(fields(t), (QStringList{"bucket"}));
        t.bucket = "has space";
        QCOMPARE(fields(t), (QStringList{"bucket"}));
        t = target("http://localhost:9000", "b");
        t.region = "us east";
        QCOMPARE(fields(t), (QStringList{"region"}));
        t.region = "auto";
        QVERIFY(fields(t).isEmpty());
        t.access_key_id = "AK ID";
        t.secret_access_key = "se\ncret";
        QCOMPARE(fields(t), (QStringList{"access_key_id", "secret_access_key"}));
        t = target("http://localhost:9000", "b");
        t.public_url = "cdn.example.com";
        QCOMPARE(fields(t), (QStringList{"public_url"}));
        t.public_url = "https://cdn.example.com";
        QVERIFY(fields(t).isEmpty());
        // A name is unique without regard to case, and a target is not its own duplicate.
        auto other = target("http://localhost:9000", "b");
        other.id = "id-2";
        other.name = "MAIN";
        QCOMPARE(fields(target("http://localhost:9000", "b"), {other}), (QStringList{"name"}));
        auto same = target("http://localhost:9000", "b");
        QVERIFY(fields(same, {same}).isEmpty());
    }

    void aServiceErrorCodeIsKeptOnlyWhenItIsPlainLettersAndDigits() {
        QCOMPARE(parse_error_code(xml_error("SignatureDoesNotMatch")), QString("SignatureDoesNotMatch"));
        QCOMPARE(parse_error_code(xml_error("Bad Code!")), QString());
        QCOMPARE(parse_error_code(xml_error("<script>")), QString());
        QCOMPARE(parse_error_code(xml_error(QByteArray(80, 'a'))), QString(80, 'a'));
        QCOMPARE(parse_error_code(xml_error(QByteArray(81, 'a'))), QString());
        QCOMPARE(parse_error_code(xml_error("")), QString());
        QCOMPARE(parse_error_code("plain text"), QString());
    }

    void imagesAreEncodedInTheAutoSaveFormatWithPdfAsPng() {
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::red);
        const auto png = encode_for_upload(image, {"PNG", -1});
        QCOMPARE(png.extension, QString(".png"));
        QCOMPARE(png.content_type, QString("image/png"));
        QVERIFY(png.bytes.startsWith("\x89PNG"));
        const auto pdf = encode_for_upload(image, {"PDF", -1});
        QCOMPARE(pdf.extension, QString(".png"));
        QVERIFY(pdf.bytes.startsWith("\x89PNG"));
        const auto jpg = encode_for_upload(image, {"jpeg", 70});
        QCOMPARE(jpg.extension, QString(".jpg"));
        QCOMPARE(jpg.content_type, QString("image/jpeg"));
        QVERIFY(jpg.bytes.startsWith("\xFF\xD8"));
        // The quality is the one that was read: a lower one gives fewer bytes.
        QImage noisy(200, 200, QImage::Format_RGB32);
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < 200; ++x) noisy.setPixel(x, y, qRgb((x * 7 + y * 13) % 256, (x * 31) % 256, (y * 17) % 256));
        QVERIFY(encode_for_upload(noisy, {"JPG", 20}).bytes.size() < encode_for_upload(noisy, {"JPG", 95}).bytes.size());
        const auto bmp = encode_for_upload(image, {"BMP", -1});
        QCOMPARE(bmp.extension, QString(".bmp"));
        QCOMPARE(bmp.content_type, QString("image/bmp"));
        QVERIFY(encode_for_upload(QImage(), {"PNG", -1}).bytes.isEmpty());
        // The settings are read as the auto-save ones.
        config_->set_screenshot_format("JPG");
        config_->set_screenshot_quality(33);
        const auto settings = read_encode_settings(*config_);
        QCOMPARE(settings.format, QString("JPG"));
        QCOMPARE(settings.quality, 33);
    }

    // One PUT, signed for exactly what the server received.
    void aPutIsSentSignedAndAnswered() {
        server_->queue({});
        const auto t = target(server_->endpoint(), "bk");
        const QByteArray payload = "payload bytes";
        const auto outcome = put_object(t, "dir/a b.png", payload, "image/png", 5);
        QVERIFY(outcome.ok());
        QCOMPARE(outcome.status, 200);
        const auto sent = server_->exchanges();
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent[0].method, QByteArray("PUT"));
        QCOMPARE(sent[0].path, QByteArray("/bk/dir/a%20b.png"));
        QCOMPARE(sent[0].body, payload);
        QCOMPARE(sent[0].headers.value("content-type"), QByteArray("image/png"));
        QCOMPARE(sent[0].headers.value("x-amz-content-sha256"), QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        QCOMPARE(sent[0].headers.value("host"), server_->host());
        static const QRegularExpression stamp("^\\d{8}T\\d{6}Z$");
        QVERIFY(stamp.match(QString::fromLatin1(sent[0].headers.value("x-amz-date"))).hasMatch());
        const auto authorization = QString::fromLatin1(sent[0].headers.value("authorization"));
        QVERIFY(authorization.startsWith("AWS4-HMAC-SHA256 Credential=AKID/"));
        QVERIFY(authorization.contains("SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date, Signature="));
        // The same request signed again at that moment gives the header that was received, so the host the HTTP stack
        // wrote is the host that was signed.
        const auto stamp_text = QString::fromLatin1(sent[0].headers.value("x-amz-date"));
        const auto when = QDateTime::fromString(stamp_text, "yyyyMMdd'T'HHmmss'Z'");
        const auto again = sign_put(t, "dir/a b.png", payload, "image/png", QDateTime(when.date(), when.time(), QTimeZone::UTC));
        QCOMPARE(header(again.headers, "Authorization"), authorization);
        // No access control, caching or download-name headers are sent.
        for (const char *name : {"x-amz-acl", "cache-control", "content-disposition", "x-amz-storage-class", "x-amz-security-token", "expect"})
            QVERIFY2(!sent[0].headers.contains(name), name);
    }

    void aRejectionCarriesItsStatusAndCodeAndNothingElseIsShown() {
        const auto t = target(server_->endpoint(), "bk");
        server_->queue({403, "Forbidden", xml_error("SignatureDoesNotMatch")});
        auto outcome = put_object(t, "k.png", "x", "image/png", 5);
        QCOMPARE(outcome.kind, PutOutcome::Rejected);
        QCOMPARE(outcome.status, 403);
        QCOMPARE(outcome.code, QString("SignatureDoesNotMatch"));
        const auto summary = outcome_summary(outcome);
        QVERIFY(summary.contains("403") && summary.contains("SignatureDoesNotMatch"));
        QVERIFY(!outcome_hint(outcome).isEmpty());
        // Neither the body's message, the address nor a header comes into what the user reads.
        const auto detail = outcome_detail(outcome);
        for (const char *secret : {"secret details", "127.0.0.1", "AKID", "Authorization", "bk/k.png"}) QVERIFY2(!detail.contains(secret), secret);
        // An answer without a readable code still names the status.
        server_->queue({500, "Internal Server Error", "oops"});
        outcome = put_object(t, "k.png", "x", "image/png", 5);
        QCOMPARE(outcome.kind, PutOutcome::Rejected);
        QCOMPARE(outcome.status, 500);
        QVERIFY(outcome.code.isEmpty());
        QVERIFY(outcome_summary(outcome).contains("500"));
        QVERIFY(outcome_hint(outcome).isEmpty());
        QCOMPARE(outcome_detail(outcome), outcome_summary(outcome));
    }

    void everyCommonCodeHasAHint() {
        for (const char *code : {"SignatureDoesNotMatch", "InvalidAccessKeyId", "AccessDenied", "NoSuchBucket", "RequestTimeTooSkewed", "PermanentRedirect", "AuthorizationHeaderMalformed"}) {
            PutOutcome outcome;
            outcome.kind = PutOutcome::Rejected;
            outcome.status = 400;
            outcome.code = code;
            QVERIFY2(!outcome_hint(outcome).isEmpty(), code);
            QVERIFY2(outcome_detail(outcome).contains('\n'), code);
        }
        PutOutcome other;
        other.kind = PutOutcome::Rejected;
        other.status = 400;
        other.code = "SomethingNew";
        QVERIFY(outcome_hint(other).isEmpty());
        for (auto kind : {PutOutcome::Timeout, PutOutcome::NetworkError, PutOutcome::EncodeFailed}) {
            PutOutcome outcome;
            outcome.kind = kind;
            QVERIFY(!outcome_summary(outcome).isEmpty());
            QVERIFY(outcome_hint(outcome).isEmpty());
        }
        QVERIFY(outcome_summary({PutOutcome::Ok, 200, {}}).isEmpty());
    }

    // A signed request is never sent anywhere else: a redirect is an answer, not a second request.
    void aRedirectIsNotFollowed() {
        const auto t = target(server_->endpoint(), "bk");
        server_->queue({307, "Temporary Redirect", xml_error("TemporaryRedirect"), "Location: " + server_->endpoint().toLatin1() + "/elsewhere\r\n"});
        const auto outcome = put_object(t, "k.png", "x", "image/png", 5);
        QCOMPARE(outcome.kind, PutOutcome::Rejected);
        QCOMPARE(outcome.status, 307);
        QCOMPARE(server_->exchanges().size(), 1);
        server_->clear();
        server_->queue({301, "Moved Permanently", xml_error("PermanentRedirect")});
        const auto moved = put_object(t, "k.png", "x", "image/png", 5);
        QCOMPARE(moved.code, QString("PermanentRedirect"));
        QVERIFY(!outcome_hint(moved).isEmpty());
        // The client's default behaviour, used by every other caller, is unchanged.
        server_->clear();
        server_->queue({307, "Temporary Redirect", {}, "Location: " + server_->endpoint().toLatin1() + "/final\r\n"});
        server_->queue({});
        const auto followed = http_request("GET", server_->endpoint() + "/start", {}, {}, 5);
        QCOMPARE(followed.status, 200);
        QCOMPARE(server_->exchanges().size(), 2);
        QCOMPARE(server_->exchanges().last().path, QByteArray("/final"));
    }

    void silenceIsATimeoutAndAClosedConnectionIsANetworkError() {
        const auto t = target(server_->endpoint(), "bk");
        Reply slow;
        slow.delay_ms = 5000;
        server_->queue(slow);
        QCOMPARE(put_object(t, "k.png", "x", "image/png", 1).kind, PutOutcome::Timeout);
        Reply dropped;
        dropped.drop = true;
        server_->queue(dropped);
        QCOMPARE(put_object(t, "k.png", "x", "image/png", 5).kind, PutOutcome::NetworkError);
        // Nothing listens on a port that was just closed.
        auto closed = target("http://127.0.0.1:1", "bk");
        QCOMPARE(put_object(closed, "k.png", "x", "image/png", 5).kind, PutOutcome::NetworkError);
    }

    void aTestImageIsPutUnderThePrefix() {
        auto t = target(server_->endpoint(), "bk");
        t.path_prefix = "tests/{yyyy}";
        server_->queue({});
        std::optional<Report> report;
        QObject context;
        upload_test_image(t, &context, [&report](const Report &result) { report = result; });
        QTRY_VERIFY_WITH_TIMEOUT(report.has_value(), 10000);
        QVERIFY(report->outcome.ok());
        const auto sent = server_->exchanges();
        QCOMPARE(sent.size(), 1);
        static const QRegularExpression path("^/bk/tests/\\d{4}/jietuba-test-[0-9a-f]{8}\\.png$");
        QVERIFY2(path.match(QString::fromLatin1(sent[0].path)).hasMatch(), sent[0].path.constData());
        QVERIFY(sent[0].body.startsWith("\x89PNG"));
        QCOMPARE(report->link, server_->endpoint() + QString::fromLatin1(sent[0].path));
        // A failure carries the same hint text as everywhere.
        server_->queue({403, "Forbidden", xml_error("AccessDenied")});
        report.reset();
        upload_test_image(t, &context, [&report](const Report &result) { report = result; });
        QTRY_VERIFY_WITH_TIMEOUT(report.has_value(), 10000);
        QVERIFY(!report->outcome.ok());
        QVERIFY(report->message.contains("AccessDenied"));
        QVERIFY(report->link.isEmpty());
    }

    void aTestResultIsNotDeliveredToAContextThatIsGone() {
        auto t = target(server_->endpoint(), "bk");
        Reply slow;
        slow.delay_ms = 300;
        server_->queue(slow);
        bool called = false;
        {
            QObject context;
            upload_test_image(t, &context, [&called](const Report &) { called = true; });
        }
        QTest::qWait(1500);
        QVERIFY(!called);
    }

    // The whole flow: encode, put, copy the link in the chosen form, and say so.
    void anUploadPutsTheImageAndCopiesTheLinkInTheChosenForm() {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("writes the clipboard; run offscreen");
        configureService("markdown");
        QImage image(20, 10, QImage::Format_RGB32);
        image.fill(Qt::blue);
        server_->queue({});
        std::optional<Report> report;
        QVERIFY(start_upload(image, *config_, Source::Screenshot, [&report](const Report &result) { report = result; }));
        QTRY_VERIFY_WITH_TIMEOUT(report.has_value(), 10000);
        QVERIFY(report->outcome.ok());
        const auto sent = server_->exchanges();
        QCOMPARE(sent.size(), 1);
        static const QRegularExpression path("^/bk/pre/\\d{4}/(\\d+[^/]*)-[0-9a-f]{8}\\.png$");
        const auto match = path.match(QString::fromLatin1(sent[0].path));
        QVERIFY2(match.hasMatch(), sent[0].path.constData());
        QVERIFY(sent[0].body.startsWith("\x89PNG"));
        QImage received;
        QVERIFY(received.loadFromData(sent[0].body));
        QCOMPARE(received.size(), image.size());
        QCOMPARE(report->link, QString("![](%1%2)").arg(server_->endpoint(), QString::fromLatin1(sent[0].path)));
        QCOMPARE(QGuiApplication::clipboard()->text(), report->link);
        QVERIFY(report->message.contains("link"));
    }

    // Once encoded the pixels are let go: a slow upload holds only the file.
    void thePixelsAreLetGoWhileTheFileGoesOut() {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("writes the clipboard; run offscreen");
        configureService();
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::red);
        Reply slow;
        slow.delay_ms = 1000;
        server_->queue(slow);
        std::optional<Report> report;
        QVERIFY(start_upload(image, *config_, Source::Screenshot, [&report](const Report &result) { report = result; }));
        QTRY_COMPARE_WITH_TIMEOUT(server_->exchanges().size(), 1, 10000);
        QVERIFY(!report);
        QVERIFY(image.isDetached());
        QTRY_VERIFY_WITH_TIMEOUT(report.has_value(), 10000);
        QVERIFY(report->outcome.ok());
    }

    void aPinIsNamedAsAPinAndTheFormatsFollowTheSettings() {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("writes the clipboard; run offscreen");
        configureService("html");
        config_->set_screenshot_format("JPG");
        config_->set_screenshot_quality(60);
        QImage image(20, 10, QImage::Format_RGB32);
        image.fill(Qt::green);
        server_->queue({});
        std::optional<Report> report;
        QVERIFY(start_upload(image, *config_, Source::Pin, [&report](const Report &result) { report = result; }));
        QTRY_VERIFY_WITH_TIMEOUT(report.has_value(), 10000);
        const auto sent = server_->exchanges();
        QCOMPARE(sent.size(), 1);
        static const QRegularExpression path("^/bk/pre/\\d{4}/pinned_[^/]*-[0-9a-f]{8}\\.jpg$");
        QVERIFY2(path.match(QString::fromLatin1(sent[0].path)).hasMatch(), sent[0].path.constData());
        QCOMPARE(sent[0].headers.value("content-type"), QByteArray("image/jpeg"));
        QVERIFY(sent[0].body.startsWith("\xFF\xD8"));
        QVERIFY(report->link.startsWith("<img src=\"" + server_->endpoint()));
        QVERIFY(report->link.endsWith(".jpg\">"));
    }

    void aFailedUploadReportsItsOutcomeAndLeavesTheClipboardAlone() {
        if (QGuiApplication::platformName() != "offscreen") QSKIP("writes the clipboard; run offscreen");
        configureService();
        QGuiApplication::clipboard()->setText("before");
        QImage image(4, 4, QImage::Format_RGB32);
        image.fill(Qt::white);
        server_->queue({403, "Forbidden", xml_error("NoSuchBucket")});
        std::optional<Report> report;
        QVERIFY(start_upload(image, *config_, Source::Screenshot, [&report](const Report &result) { report = result; }));
        QTRY_VERIFY_WITH_TIMEOUT(report.has_value(), 10000);
        QCOMPARE(report->outcome.kind, PutOutcome::Rejected);
        QCOMPARE(report->outcome.code, QString("NoSuchBucket"));
        QVERIFY(report->link.isEmpty());
        QVERIFY(report->message.contains("NoSuchBucket"));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("before"));
    }

    void withoutADefaultTargetNothingStarts() {
        QImage image(4, 4, QImage::Format_RGB32);
        QVERIFY(!default_target(*config_));
        QVERIFY(!start_upload(image, *config_, Source::Screenshot));
        // A default that is no longer in the list is no default.
        auto t = target(server_->endpoint(), "bk");
        config_->set_upload_targets(serialize_targets({t}));
        config_->set_upload_default_target("gone");
        QVERIFY(!default_target(*config_));
        QVERIFY(!start_upload(image, *config_, Source::Screenshot));
        config_->set_upload_default_target(t.id);
        QCOMPARE(default_target(*config_)->bucket, QString("bk"));
        // Nothing to upload.
        QVERIFY(!start_upload(QImage(), *config_, Source::Screenshot));
        QCOMPARE(server_->exchanges().size(), 0);
    }

    // The dialog's test button puts a 1x1 picture to the typed target, shows the link or the reason, and sends nothing
    // while a field is wrong.
    void theDialogsTestButtonShowsTheLinkOrTheReason() {
        auto t = target(server_->endpoint(), "bk");
        UploadTargetDialog dialog(t, {}, false);
        server_->queue({});
        dialog.test_button->click();
        QVERIFY(!dialog.test_button->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(dialog.test_button->isEnabled(), 10000);
        QCOMPARE(server_->exchanges().size(), 1);
        QVERIFY2(dialog.test_label->text().contains(server_->endpoint() + "/bk/jietuba-test-"), qPrintable(dialog.test_label->text()));
        server_->queue({403, "Forbidden", xml_error("AccessDenied")});
        dialog.test_button->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.test_button->isEnabled() && dialog.test_label->text().contains("AccessDenied"), 10000);
        QCOMPARE(server_->exchanges().size(), 2);
        QVERIFY(dialog.test_label->text().contains(QChar(0x0A)));
        QVERIFY(!dialog.test_label->text().contains(server_->endpoint()));
        dialog.endpoint_edit->setText("ftp://nowhere");
        dialog.test_button->click();
        QVERIFY(dialog.test_button->isEnabled());
        QVERIFY(!dialog.errors.value("endpoint")->isHidden());
        QCOMPARE(server_->exchanges().size(), 2);
    }

    void theUploadSettingsHaveTheirDefaults() {
        QCOMPARE(config_->get_upload_targets(), QString());
        QCOMPARE(config_->get_upload_default_target(), QString());
        QCOMPARE(config_->get_upload_link_format(), QString("url"));
        QCOMPARE(config_->get_inapp_shortcut("inapp_upload"), QString());
        QCOMPARE(ToolSettingsManager::app_setting_path("upload_targets"), QString("upload/targets"));
        QCOMPARE(ToolSettingsManager::app_setting_path("upload_default_target"), QString("upload/default_target"));
        QCOMPARE(ToolSettingsManager::app_setting_path("upload_link_format"), QString("upload/link_format"));
    }
};

QTEST_MAIN(UploadTests)
#include "test_upload.moc"
