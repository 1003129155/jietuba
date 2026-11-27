#include "upload/s3_client.h"
#include "core/http_client.h"
#include "core/i18n.h"
#include "core/save.h"
#include "settings/tool_settings.h"
#include "upload/s3_signer.h"
#include <QRegularExpression>

namespace jietuba::upload {
namespace {
QString upload_tr(const char *text) { return i18n_tr(text, "CloudUpload"); }

QString content_type_of(const QString &format) {
    if (format == "JPG") return "image/jpeg";
    if (format == "WEBP") return "image/webp";
    if (format == "BMP") return "image/bmp";
    return "image/png";
}
}

EncodeSettings read_encode_settings(ToolSettingsManager &config) {
    const auto options = SaveOptions::from_settings(config);
    return {config.get_screenshot_format(), options.quality, options.png};
}

EncodedImage encode_for_upload(const QImage &image, const EncodeSettings &settings) {
    SaveOptions options;
    options.quality = settings.quality;
    options.png = settings.png;
    QString format = SaveService::normalize_format(settings.format);
    if (format == "PDF") format = "PNG";
    format = SaveService::format_for_size(format, image.width(), image.height(), options);
    auto bytes = SaveService::encode_image(image, format, options);
    if (bytes.isEmpty() && format != "PNG") {
        format = "PNG";
        bytes = SaveService::encode_image(image, format, options);
    }
    return {bytes, "." + format.toLower(), content_type_of(format)};
}

QString parse_error_code(const QByteArray &body) {
    static const QRegularExpression code(R"(<Code>([A-Za-z0-9]{1,80})</Code>)");
    return code.match(QString::fromUtf8(body)).captured(1);
}

PutOutcome put_object(const Target &target, const QString &key, const QByteArray &payload, const QString &content_type, double timeout_seconds) {
    const auto signed_request = sign_put(target, key, payload, content_type, QDateTime::currentDateTimeUtc());
    HttpOptions options;
    options.follow_redirects = false;
    const auto response = http_request("PUT", signed_request.url, signed_request.headers, payload, timeout_seconds, options);
    PutOutcome outcome;
    outcome.status = response.status;
    switch (response.kind) {
    case HttpResponse::Timeout: outcome.kind = PutOutcome::Timeout; break;
    case HttpResponse::NetworkError: outcome.kind = PutOutcome::NetworkError; break;
    default:
        if (response.status >= 200 && response.status < 300) {
            outcome.kind = PutOutcome::Ok;
        } else {
            outcome.kind = PutOutcome::Rejected;
            outcome.code = parse_error_code(response.body);
        }
    }
    return outcome;
}

QString outcome_summary(const PutOutcome &outcome) {
    switch (outcome.kind) {
    case PutOutcome::Ok: return {};
    case PutOutcome::Timeout: return upload_tr("Upload timed out");
    case PutOutcome::NetworkError: return upload_tr("Upload failed: network error");
    case PutOutcome::EncodeFailed: return upload_tr("Upload failed: the image could not be encoded");
    case PutOutcome::Rejected: break;
    }
    if (outcome.code.isEmpty()) return upload_tr("Upload failed: HTTP %1").arg(outcome.status);
    return upload_tr("Upload failed: HTTP %1, %2").arg(outcome.status).arg(outcome.code);
}

QString outcome_hint(const PutOutcome &outcome) {
    if (outcome.kind != PutOutcome::Rejected) return {};
    if (outcome.code == "SignatureDoesNotMatch") return upload_tr("Check the access key, the secret key and the region.");
    if (outcome.code == "InvalidAccessKeyId") return upload_tr("The access key ID is not known to this service.");
    if (outcome.code == "AccessDenied") return upload_tr("Check that this key may write to the bucket.");
    if (outcome.code == "NoSuchBucket") return upload_tr("The bucket does not exist.");
    if (outcome.code == "RequestTimeTooSkewed") return upload_tr("Check the date and time of this computer.");
    if (outcome.code == "PermanentRedirect" || outcome.code == "AuthorizationHeaderMalformed") return upload_tr("The region or the endpoint is wrong.");
    return {};
}

QString outcome_detail(const PutOutcome &outcome) {
    const auto hint = outcome_hint(outcome);
    return hint.isEmpty() ? outcome_summary(outcome) : outcome_summary(outcome) + "\n" + hint;
}
}
