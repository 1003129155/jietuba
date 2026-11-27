#include "upload/s3_signer.h"
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

namespace jietuba::upload {
namespace {
QByteArray hmac(const QByteArray &key, const QByteArray &data) { return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256); }
}

SignedPut sign_put(const Target &target, const QString &key, const QByteArray &payload, const QString &content_type, const QDateTime &when,
                   const std::map<QByteArray, QByteArray> &extra_headers) {
    const auto location = locate(target, key);
    const QDateTime utc = when.toUTC();
    const QByteArray date = utc.toString("yyyyMMdd").toLatin1();
    const QByteArray stamp = utc.toString("yyyyMMdd'T'HHmmss'Z'").toLatin1();
    const QByteArray payload_hash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();

    // Sorted by name, as the canonical form requires.
    std::map<QByteArray, QByteArray> signed_headers = extra_headers;
    signed_headers["host"] = location.host;
    signed_headers["x-amz-content-sha256"] = payload_hash;
    signed_headers["x-amz-date"] = stamp;
    if (!content_type.isEmpty()) signed_headers["content-type"] = content_type.toUtf8();

    QByteArray canonical_headers, names;
    for (const auto &[name, value] : signed_headers) {
        canonical_headers += name + ":" + value.simplified() + "\n";
        if (!names.isEmpty()) names += ";";
        names += name;
    }
    // The path is already encoded once, which is the canonical URI for S3.
    const QByteArray canonical_request = "PUT\n" + location.path + "\n\n" + canonical_headers + "\n" + names + "\n" + payload_hash;
    const QByteArray scope = date + "/" + target.region.toUtf8() + "/s3/aws4_request";
    const QByteArray string_to_sign = "AWS4-HMAC-SHA256\n" + stamp + "\n" + scope + "\n" + QCryptographicHash::hash(canonical_request, QCryptographicHash::Sha256).toHex();
    const QByteArray signing_key = hmac(hmac(hmac(hmac("AWS4" + target.secret_access_key.toUtf8(), date), target.region.toUtf8()), "s3"), "aws4_request");
    const QByteArray signature = hmac(signing_key, string_to_sign).toHex();

    SignedPut result;
    result.url = location.url;
    for (const auto &[name, value] : signed_headers)
        if (name != "host") result.headers.emplace_back(QString::fromLatin1(name), QString::fromUtf8(value));
    const QByteArray authorization = "AWS4-HMAC-SHA256 Credential=" + target.access_key_id.toUtf8() + "/" + scope + ", SignedHeaders=" + names + ", Signature=" + signature;
    result.headers.emplace_back("Authorization", QString::fromLatin1(authorization));
    return result;
}
}
