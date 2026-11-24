#pragma once
#include "core/http_client.h"
#include "upload/s3_target.h"
#include <QByteArray>
#include <QDateTime>
#include <map>

namespace jietuba::upload {
struct SignedPut {
    QString url;
    // Everything to send besides Host, which the HTTP stack writes itself from the URL.
    HttpHeaders headers;
};

// A single-request PUT signed with AWS Signature Version 4 (service s3, the payload's real SHA-256). The signed
// headers are host, x-amz-content-sha256, x-amz-date, content-type when given, and any extra_headers (names lower case).
SignedPut sign_put(const Target &target, const QString &key, const QByteArray &payload, const QString &content_type, const QDateTime &when,
                   const std::map<QByteArray, QByteArray> &extra_headers = {});
}
