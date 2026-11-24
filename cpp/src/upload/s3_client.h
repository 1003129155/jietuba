#pragma once
#include "core/save.h"
#include "upload/s3_target.h"
#include <QByteArray>
#include <QImage>
#include <QString>

namespace jietuba {
class ToolSettingsManager;
}

namespace jietuba::upload {
// How an image is turned into the bytes that are uploaded: the auto-save format, quality and PNG level.
struct EncodeSettings {
    QString format = "PNG";
    int quality = -1;
    SaveOptions::PngCompression png = SaveOptions::PngCompression::Medium;
};
// Read on the thread that owns the settings; the encoding itself can then run anywhere.
EncodeSettings read_encode_settings(ToolSettingsManager &config);

struct EncodedImage {
    QByteArray bytes;
    QString extension;     // with its dot, lower case
    QString content_type;
};
// The image in the auto-save format; PDF, and a size the format cannot hold, go as PNG. Empty bytes when encoding failed.
EncodedImage encode_for_upload(const QImage &image, const EncodeSettings &settings);

struct PutOutcome {
    enum Kind { Ok, Rejected, Timeout, NetworkError, EncodeFailed } kind = NetworkError;
    int status = 0;
    // The S3 error code of the answer, only when it is 1 to 80 ASCII letters and digits.
    QString code;
    bool ok() const { return kind == Ok; }
};

// One signed PUT, blocking. Redirects are not followed, so the signed request goes to the endpoint and nowhere else;
// any answer outside 2xx is Rejected.
PutOutcome put_object(const Target &target, const QString &key, const QByteArray &payload, const QString &content_type, double timeout_seconds = 60);

// The S3 error code in an XML answer; empty when absent or not plain letters and digits.
QString parse_error_code(const QByteArray &body);

// What the user is told. The summary names the HTTP status and code and never the address, the headers or the body;
// the hint is one sentence for the common codes and empty otherwise.
QString outcome_summary(const PutOutcome &outcome);
QString outcome_hint(const PutOutcome &outcome);
// Summary and hint on two lines.
QString outcome_detail(const PutOutcome &outcome);
}
