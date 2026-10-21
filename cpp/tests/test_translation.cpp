#include "core/html_unescape.h"
#include "core/http_client.h"
#include "translation/providers/amazon.h"
#include "translation/providers/baidu.h"
#include "translation/providers/custom_llm.h"
#include "translation/providers/deepseek.h"
#include "translation/providers/tencent.h"
#include "translation/providers/volcengine.h"
#include "translation/providers/youdao.h"
#include "settings/tool_settings.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QTest>
#include <memory>

using namespace jietuba;

namespace {
const QVariantMap VOLCENGINE_CONFIG{{"access_key_id", "AKIDvolc"}, {"secret_access_key", "volc-secret"}};
const QVariantMap TENCENT_CONFIG{{"secret_id", "AKIDtencent"}, {"secret_key", "tencent-secret"}};
const QVariantMap YOUDAO_CONFIG{{"app_key", "test-app-key"}, {"app_secret", "test-app-secret"}};

QString error_value(const TranslationResult &result) { return result.error_code ? translation_error_code_value(*result.error_code) : QString(); }

// Empty when result equals the reference result, else what differs.
QString result_mismatch(const TranslationResult &result, const QJsonObject &expected) {
    QStringList differences;
    if (result.success != expected["success"].toBool()) differences << QString("success %1").arg(result.success);
    if (result.translated_text != expected["translated_text"].toString()) differences << "translated_text " + result.translated_text;
    if (result.detected_source_lang != expected["detected_source_lang"].toString()) differences << "detected_source_lang " + result.detected_source_lang;
    if (error_value(result) != expected["error_code"].toString()) differences << "error_code " + error_value(result);
    if (result.error_message != expected["error_message"].toString()) differences << "error_message " + result.error_message;
    return differences.join("; ");
}

QString field_kind(const ProviderField &field) {
    switch (field.kind) {
    case ProviderField::Toggle: return "toggle";
    case ProviderField::Model: return "model";
    default: return "text";
    }
}

QJsonArray fields_json(const std::vector<ProviderField> &fields) {
    QJsonArray result;
    for (const auto &field : fields)
        result.append(QJsonObject{{"kind", field_kind(field)}, {"config_key", field.config_key}, {"label", field.label},
                                  {"placeholder", field.placeholder}, {"secret", field.secret}, {"description", field.description}});
    return result;
}

// A reference urlopen outcome as the HTTP stack reports it.
HttpResponse http_response(const QJsonObject &scenario) {
    const auto kind = scenario["kind"].toString();
    HttpResponse response;
    response.body = QByteArray::fromHex(scenario["body_hex"].toString().toLatin1());
    if (kind == "ok") {
        response.kind = HttpResponse::Ok;
        response.status = 200;
        response.reason = "OK";
    } else if (kind == "http_error") {
        response.kind = HttpResponse::HttpError;
        response.status = scenario["status"].toInt();
        response.reason = scenario["reason"].toString();
    } else if (kind == "timeout") {
        response.kind = HttpResponse::Timeout;
    } else {
        response.kind = HttpResponse::NetworkError;
        response.reason = scenario["reason"].toString();
    }
    return response;
}

HttpResponse json_answer(const QJsonObject &body) {
    HttpResponse response;
    response.kind = HttpResponse::Ok;
    response.status = 200;
    response.reason = "OK";
    response.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    return response;
}

// A form body parsed back into its fields.
QHash<QString, QString> form_fields(const QByteArray &form) {
    QHash<QString, QString> fields;
    for (const auto &pair : form.split('&')) {
        const auto at = pair.indexOf('=');
        const auto decode = [](QByteArray part) { return QUrl::fromPercentEncoding(part.replace('+', ' ')); };
        fields.insert(decode(pair.left(at)), decode(pair.mid(at + 1)));
    }
    return fields;
}

std::optional<QString> optional_text(const QJsonValue &value) { return value.isNull() ? std::nullopt : std::optional<QString>(value.toString()); }

std::unique_ptr<TranslationProvider> machine_translation(const QString &provider_id, const QVariantMap &config) {
    if (provider_id == "volcengine") return std::make_unique<VolcengineTranslateProvider>(config);
    return std::make_unique<TencentTranslateProvider>(config);
}
}

// Expected values come from the Python reference (scripts/generate_translation_fixtures.py).
class TranslationTests : public QObject {
    Q_OBJECT
    QJsonObject fixtures_;
    QJsonArray cases(const char *name) const { return fixtures_[name].toArray(); }
private slots:
    void initTestCase() {
        QFile file(JIETUBA_TRANSLATION_FIXTURE_PATH);
        QVERIFY(file.open(QIODevice::ReadOnly));
        fixtures_ = QJsonDocument::fromJson(file.readAll()).object();
        QVERIFY(!fixtures_.isEmpty());
    }
    void htmlUnescape() {
        for (const auto &value : cases("html_unescape")) {
            const auto item = value.toObject();
            QCOMPARE(html_unescape(item["input"].toString()), item["expected"].toString());
        }
    }
    void urlencode() {
        for (const auto &value : cases("urlencode")) {
            const auto item = value.toObject();
            std::vector<std::pair<QString, QString>> query;
            for (const auto &pair : item["query"].toArray()) query.emplace_back(pair.toArray()[0].toString(), pair.toArray()[1].toString());
            QCOMPARE(python_urlencode(query), item["expected"].toString());
        }
    }
    void extractReply() {
        for (const auto &value : cases("extract")) {
            const auto item = value.toObject();
            const auto [translated, detected] = OpenAICompatibleProvider::extract(item["input"].toString());
            QCOMPARE(translated, item["expected"].toArray()[0].toString());
            QCOMPARE(detected, item["expected"].toArray()[1].toString());
        }
    }
    void normalizeBaseUrl() {
        for (const auto &value : cases("normalize_base_url")) {
            const auto item = value.toObject();
            QCOMPARE(OpenAICompatibleProvider::normalize_base_url(item["input"].toString()), item["expected"].toString());
        }
    }
    void parseResponse() {
        DeepSeekProvider deepseek({{"api_key", "k"}});
        CustomLLMProvider custom({{"base_url", "http://x"}, {"model", "m"}});
        for (const auto &value : cases("parse_response")) {
            const auto item = value.toObject();
            const OpenAICompatibleProvider &provider = item["provider"].toString() == "deepseek" ? static_cast<OpenAICompatibleProvider &>(deepseek) : custom;
            const auto result = provider.parse_response(item["raw"]);
            QCOMPARE(result.success, item["success"].toBool());
            QCOMPARE(result.translated_text, item["translated_text"].toString());
            QCOMPARE(result.detected_source_lang, item["detected_source_lang"].toString());
            QCOMPARE(result.error_code ? translation_error_code_value(*result.error_code) : QString(), item["error_code"].toString());
            QCOMPARE(result.error_message, item["error_message"].toString());
        }
    }
    void customLlmConfig() {
        for (const auto &value : cases("custom_llm")) {
            const auto item = value.toObject();
            const auto config = item["config"].toObject().toVariantMap();
            std::unique_ptr<CustomLLMProvider> provider;
            switch (item["slot"].toInt()) {
            case 1: provider = std::make_unique<CustomLLMProvider>(config); break;
            case 2: provider = std::make_unique<CustomLLM2Provider>(config); break;
            default: provider = std::make_unique<CustomLLM3Provider>(config); break;
            }
            QCOMPARE(provider->provider_id(), item["provider_id"].toString());
            QCOMPARE(provider->is_configured(), item["is_configured"].toBool());
            QCOMPARE(provider->config_error(), item["config_error"].toString());
            QCOMPARE(provider->min_timeout, item["min_timeout"].toInt());
            QCOMPARE(provider->temperature().has_value(), !item["temperature"].isNull());
            if (provider->temperature()) QCOMPARE(*provider->temperature(), item["temperature"].toDouble());
            QCOMPARE(provider->json_mode(), item["json_mode"].toBool());
            QCOMPARE(provider->extra_body(), item["extra_body"].toObject());
            QCOMPARE(provider->display_label("Custom LLM"), item["display_label"].toString());
            const TranslationRequest request("hello", "zh-Hans", QString("en"));
            QCOMPARE(provider->request_body(request, provider->json_mode()), item["body"].toObject());
        }
    }
    void amazonSignature() {
        for (const auto &value : cases("amazon_signature")) {
            const auto item = value.toObject();
            AmazonTranslateProvider provider(item["config"].toObject().toVariantMap());
            const auto now = QDateTime::fromString(item["now"].toString(), Qt::ISODate);
            QVERIFY(now.isValid());
            const auto [url, headers] = provider.signed_request(item["payload"].toString().toUtf8(), now);
            QCOMPARE(url, item["url"].toString());
            const auto expected = item["headers"].toObject();
            QCOMPARE(static_cast<qsizetype>(headers.size()), expected.size());
            for (const auto &[name, header_value] : headers) QCOMPARE(header_value, expected[name].toString());
        }
    }
    void machineTranslationMetadata() {
        for (const auto &value : cases("mt_metadata")) {
            const auto item = value.toObject();
            const auto provider_id = item["provider_id"].toString();
            const auto metadata = provider_id == "volcengine" ? VolcengineTranslateProvider::class_metadata() : TencentTranslateProvider::class_metadata();
            QCOMPARE(metadata.provider_id, provider_id);
            QCOMPARE(metadata.display_name, item["display_name"].toString());
            QCOMPARE(fields_json(metadata.credentials), item["credentials"].toArray());
            QCOMPARE(fields_json(metadata.options), item["options"].toArray());
            QVERIFY(metadata.supported_request_options.isEmpty() && item["supported_request_options"].toArray().isEmpty());
            QCOMPARE(metadata.notice, item["notice"].toString());
            QCOMPARE(metadata.help_label, item["help_label"].toString());
            QCOMPARE(metadata.help_url, item["help_url"].toString());
            const auto provider = machine_translation(provider_id, {});
            QCOMPARE(provider->provider_id(), provider_id);
            QCOMPARE(provider->display_name(), item["display_name"].toString());
            QCOMPARE(provider->min_timeout, item["min_timeout"].toInt());
            QCOMPARE(provider->quota_limit_chars, item["quota_limit_chars"].toInteger());
        }
    }
    void volcengineSignature() {
        for (const auto &value : cases("volcengine_signature")) {
            const auto item = value.toObject();
            VolcengineTranslateProvider provider(item["config"].toObject().toVariantMap());
            const auto headers = provider.signed_headers(item["payload"].toString().toUtf8(), item["x_date"].toString());
            const auto expected = item["headers"].toArray();
            QCOMPARE(static_cast<qsizetype>(headers.size()), expected.size());
            for (qsizetype i = 0; i < expected.size(); ++i) {
                QCOMPARE(headers[i].first, expected[i].toArray()[0].toString());
                QCOMPARE(headers[i].second, expected[i].toArray()[1].toString());
            }
        }
    }
    void tencentSignature() {
        for (const auto &value : cases("tencent_signature")) {
            const auto item = value.toObject();
            TencentTranslateProvider provider(item["config"].toObject().toVariantMap());
            const auto headers = provider.signed_headers(item["payload"].toString().toUtf8(), item["timestamp"].toInteger());
            const auto expected = item["headers"].toArray();
            QCOMPARE(static_cast<qsizetype>(headers.size()), expected.size());
            for (qsizetype i = 0; i < expected.size(); ++i) {
                QCOMPARE(headers[i].first, expected[i].toArray()[0].toString());
                QCOMPARE(headers[i].second, expected[i].toArray()[1].toString());
            }
        }
    }
    void machineTranslationRequests() {
        for (const auto &value : cases("mt_requests")) {
            const auto item = value.toObject();
            const auto source = item["source"].isNull() ? std::nullopt : std::optional<QString>(item["source"].toString());
            const TranslationRequest request(item["text"].toString(), item["target"].toString(), source, true, 7);
            QByteArray payload;
            QString url;
            if (item["provider"].toString() == "volcengine") {
                payload = VolcengineTranslateProvider(VOLCENGINE_CONFIG).request_payload(request);
                url = VolcengineTranslateProvider::request_url();
            } else {
                payload = TencentTranslateProvider(TENCENT_CONFIG).request_payload(request);
                url = TencentTranslateProvider::request_url();
            }
            QCOMPARE(QJsonDocument::fromJson(payload).object(), item["body"].toObject());
            QCOMPARE(url, item["url"].toString());
        }
    }
    void machineTranslationResponses() {
        for (const auto &value : cases("mt_responses")) {
            const auto item = value.toObject();
            const auto scenario = item["scenario"].toObject();
            const auto response = http_response(scenario);
            const auto provider_id = item["provider"].toString();
            const auto result = provider_id == "volcengine" ? VolcengineTranslateProvider(VOLCENGINE_CONFIG).handle_response(response, 7)
                                                            : TencentTranslateProvider(TENCENT_CONFIG).handle_response(response, 7);
            const auto mismatch = result_mismatch(result, item["result"].toObject());
            QVERIFY2(mismatch.isEmpty(), qPrintable(provider_id + " " + QJsonDocument(scenario).toJson(QJsonDocument::Compact) + ": " + mismatch));
        }
    }
    void machineTranslationChecksBeforeAnyRequest() {
        for (const auto &value : cases("mt_prechecks")) {
            const auto item = value.toObject();
            const auto provider = machine_translation(item["provider"].toString(), item["config"].toObject().toVariantMap());
            QCOMPARE(provider->is_configured(), item["is_configured"].toBool());
            const auto result = provider->translate(TranslationRequest(item["text"].toString(), item["target"].toString()));
            const auto mismatch = result_mismatch(result, item["result"].toObject());
            QVERIFY2(mismatch.isEmpty(), qPrintable(item["provider"].toString() + " " + item["target"].toString() + ": " + mismatch));
        }
    }
    void machineTranslationErrorCodes() {
        for (const auto &value : cases("mt_error_codes")) {
            const auto item = value.toObject();
            const auto code = item["code"].toString();
            const auto mapped = item["provider"].toString() == "volcengine" ? VolcengineTranslateProvider::map_api_error_code(code)
                                                                            : TencentTranslateProvider::map_api_error_code(code);
            QVERIFY2(translation_error_code_value(mapped) == item["expected"].toString(), qPrintable(code));
        }
    }
    void machineTranslationLanguageCodes() {
        for (const auto &value : cases("mt_language_codes")) {
            const auto item = value.toObject();
            const auto code = item["code"].toString();
            if (item["provider"].toString() == "volcengine") {
                QCOMPARE(VolcengineTranslateProvider::to_code(code), item["to"].toString());
                QCOMPARE(VolcengineTranslateProvider::from_code(code), item["from"].toString());
            } else {
                QCOMPARE(TencentTranslateProvider::to_code(code), item["to"].toString());
                QCOMPARE(TencentTranslateProvider::from_code(code), item["from"].toString());
            }
        }
    }
    // The steps run in order on one store, as in the reference.
    void quotaSettings() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings settings(dir.filePath("quota.ini"), QSettings::IniFormat);
        ToolSettingsManager manager(settings);
        for (const auto &value : cases("quota_settings")) {
            const auto item = value.toObject();
            const auto op = item["op"].toString();
            const auto provider_id = item["provider"].toString();
            const bool volcengine = provider_id == "volcengine";
            if (op == "limit") {
                const auto input = item["input"].toString();
                if (volcengine) manager.set_volcengine_translate_quota_limit(input);
                else manager.set_tencent_translate_quota_limit(input);
                QCOMPARE(volcengine ? manager.get_volcengine_translate_quota_limit() : manager.get_tencent_translate_quota_limit(), item["stored"].toString());
                QVERIFY2(manager.get_translation_quota_limit(provider_id) == item["limit"].toInteger(), qPrintable(input));
            } else if (op == "enabled") {
                if (volcengine) manager.set_volcengine_translate_quota_enabled(item["input"].toBool());
                else manager.set_tencent_translate_quota_enabled(item["input"].toBool());
                QCOMPARE(manager.get_translation_quota_enabled(provider_id), item["enabled"].toBool());
            } else {
                QCOMPARE(manager.get_translation_quota_enabled(provider_id), item["enabled"].toBool());
                QCOMPARE(manager.get_translation_quota_limit(provider_id), item["limit"].toInteger());
            }
        }
    }
    // Hand-written: the limit is a plain integer with optional sign; full-width digits from an IME
    // count, anything the reference's int() accepted beyond that does not.
    void quotaLimitParsesPlainIntegers() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings settings(dir.filePath("quota.ini"), QSettings::IniFormat);
        ToolSettingsManager manager(settings);
        const QList<std::pair<QString, qint64>> cases{
            {" 1234 ", 1234}, {"+7", 7}, {"-5", -5}, {QString::fromUtf8("\uFF11\uFF12\uFF13"), 123}, {QString::fromUtf8("\uFF0B\uFF17"), 7},
            {"1_000", 0}, {QString::fromUtf8("\u0663\u0664"), 0}, {"1.5", 0}, {"abc", 0},
            {"9223372036854775807", 9223372036854775807LL}, {"9223372036854775808", 0},
        };
        for (const auto &[input, expected] : cases) {
            manager.set_tencent_translate_quota_limit(input);
            QVERIFY2(manager.get_translation_quota_limit("tencent") == expected, qPrintable(input));
        }
    }
    void youdaoMetadata() {
        const auto item = fixtures_["youdao_metadata"].toObject();
        const auto metadata = YoudaoTranslateProvider::class_metadata();
        QCOMPARE(metadata.provider_id, item["provider_id"].toString());
        QCOMPARE(metadata.display_name, item["display_name"].toString());
        QCOMPARE(fields_json(metadata.credentials), item["credentials"].toArray());
        QCOMPARE(fields_json(metadata.options), item["options"].toArray());
        QVERIFY(metadata.supported_request_options.isEmpty() && item["supported_request_options"].toArray().isEmpty());
        QCOMPARE(metadata.notice, item["notice"].toString());
        QCOMPARE(metadata.help_label, item["help_label"].toString());
        QCOMPARE(metadata.help_url, item["help_url"].toString());
        const YoudaoTranslateProvider provider({});
        QCOMPARE(provider.provider_id(), item["provider_id"].toString());
        QCOMPARE(provider.display_name(), item["display_name"].toString());
        QCOMPARE(provider.min_timeout, item["min_timeout"].toInt());
        QCOMPARE(provider.quota_limit_chars, qint64(0));
    }
    void youdaoSign() {
        for (const auto &value : cases("youdao_sign")) {
            const auto item = value.toObject();
            const YoudaoTranslateProvider provider(item["config"].toObject().toVariantMap());
            const auto sign = provider.sign(item["text"].toString(), item["salt"].toString(), item["curtime"].toString(), item["utf16"].toBool());
            QVERIFY2(sign == item["expected"].toString(), qPrintable(item["text"].toString() + (item["utf16"].toBool() ? " utf16" : " codepoint")));
        }
    }
    void youdaoRequests() {
        for (const auto &value : cases("youdao_requests")) {
            const auto item = value.toObject();
            const TranslationRequest request(item["text"].toString(), item["target"].toString(), optional_text(item["source"]), true, 7);
            const YoudaoTranslateProvider provider(YOUDAO_CONFIG);
            QCOMPARE(QString::fromUtf8(provider.request_form(request, item["salt"].toString(), item["curtime"].toString(), true)), item["body"].toString());
            QCOMPARE(YoudaoTranslateProvider::API_URL, item["url"].toString());
            QCOMPARE(YoudaoTranslateProvider::CONTENT_TYPE, item["content_type"].toString());
            const auto result = provider.translate_with(request, [](const QByteArray &) {
                return json_answer(QJsonObject{{"errorCode", "0"}, {"translation", QJsonArray{"ok"}}, {"l", "en2zh-CHS"}});
            });
            const auto mismatch = result_mismatch(result, item["result"].toObject());
            QVERIFY2(mismatch.isEmpty(), qPrintable(item["text"].toString() + ": " + mismatch));
        }
    }
    void youdaoResponses() {
        for (const auto &value : cases("youdao_responses")) {
            const auto item = value.toObject();
            const auto scenario = item["scenario"].toObject();
            const auto [result, api_code] = YoudaoTranslateProvider(YOUDAO_CONFIG).handle_response(http_response(scenario), 7);
            const auto mismatch = result_mismatch(result, item["result"].toObject());
            QVERIFY2(mismatch.isEmpty(), qPrintable(QJsonDocument(scenario).toJson(QJsonDocument::Compact) + ": " + mismatch));
            QCOMPARE(api_code, item["api_code"].toString());
        }
    }
    void youdaoRetriesARejectedSignatureOutsideTheBmp() {
        for (const auto &value : cases("youdao_retries")) {
            const auto item = value.toObject();
            const YoudaoTranslateProvider provider(YOUDAO_CONFIG);
            QJsonArray modes;
            const auto result = provider.translate_with(TranslationRequest(item["text"].toString(), "ja", std::nullopt, true, 7), [&](const QByteArray &form) {
                const auto fields = form_fields(form);
                QJsonArray matching;
                for (const bool utf16 : {true, false})
                    if (fields["sign"] == provider.sign(fields["q"], fields["salt"], fields["curtime"], utf16)) matching.append(utf16 ? "utf16" : "codepoint");
                modes.append(matching);
                return json_answer(modes.size() == 1 ? item["first"].toObject()
                                                     : QJsonObject{{"errorCode", "0"}, {"translation", QJsonArray{"second"}}, {"l", "en2ja"}});
            });
            QCOMPARE(modes, item["modes"].toArray());
            const auto mismatch = result_mismatch(result, item["result"].toObject());
            QVERIFY2(mismatch.isEmpty(), qPrintable(item["text"].toString() + ": " + mismatch));
        }
    }
    void youdaoChecksBeforeAnyRequest() {
        for (const auto &value : cases("youdao_prechecks")) {
            const auto item = value.toObject();
            const YoudaoTranslateProvider provider(item["config"].toObject().toVariantMap());
            QCOMPARE(provider.is_configured(), item["is_configured"].toBool());
            bool sent = false;
            const auto result = provider.translate_with(TranslationRequest(item["text"].toString(), "ja"), [&sent](const QByteArray &) {
                sent = true;
                return HttpResponse{};
            });
            QVERIFY(!sent);
            const auto mismatch = result_mismatch(result, item["result"].toObject());
            QVERIFY2(mismatch.isEmpty(), qPrintable(item["text"].toString() + ": " + mismatch));
        }
    }
    void youdaoErrorCodesAndLanguages() {
        for (const auto &value : cases("youdao_error_codes")) {
            const auto item = value.toObject();
            QVERIFY2(translation_error_code_value(YoudaoTranslateProvider::map_api_error_code(item["code"].toString())) == item["expected"].toString(),
                     qPrintable(item["code"].toString()));
        }
        for (const auto &value : cases("youdao_language_codes")) {
            const auto item = value.toObject();
            QCOMPARE(YoudaoTranslateProvider::to_code(item["code"].toString()), item["to"].toString());
            QCOMPARE(YoudaoTranslateProvider::from_code(item["code"].toString()), item["from"].toString());
        }
    }
    void baiduSign() {
        for (const auto &value : cases("baidu_sign")) {
            const auto item = value.toObject();
            BaiduTranslateProvider provider({{"appid", item["appid"].toString()}, {"secret_key", item["secret_key"].toString()}});
            QCOMPARE(provider.sign(item["text"].toString(), item["salt"].toInt()), item["expected"].toString());
        }
    }
};

QTEST_MAIN(TranslationTests)
#include "test_translation.moc"
