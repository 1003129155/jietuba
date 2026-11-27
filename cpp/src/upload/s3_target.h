#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <vector>

namespace jietuba::upload {
enum class Addressing { Path, Virtual };
enum class LinkFormat { Url, Markdown, Html };

// One S3-compatible storage the user uploads to. Keys are kept as typed apart from trimming.
struct Target {
    QString id, name, endpoint, region, bucket, access_key_id, secret_access_key, path_prefix, public_url;
    Addressing addressing = Addressing::Path;
};

QString new_target_id();

// The list as stored in the settings: a JSON array. Entries that are not objects or have no id are dropped;
// a repeated id keeps its first entry.
std::vector<Target> parse_targets(const QString &json);
QString serialize_targets(const std::vector<Target> &targets);
const Target *find_target(const std::vector<Target> &targets, const QString &id);

LinkFormat parse_link_format(const QString &text);
QString link_format_name(LinkFormat format);
// The link as it is copied: the bare address, a Markdown image or an HTML image.
QString format_link(const QString &url, LinkFormat format);

// A rejected field, named as the target's field (name, endpoint, region, bucket, access_key_id,
// secret_access_key, public_url) with the English message to translate.
struct FieldError {
    QString field;
    const char *message;
};
// others are the targets this one is saved beside, for the name check.
std::vector<FieldError> validate(const Target &target, const std::vector<Target> &others);

// The virtual-hosted form needs a DNS name for the bucket under the endpoint's host; a bucket name with a dot
// or an endpoint given as an IP address cannot have one, so those stay path-style.
bool uses_virtual_host(const Target &target);

// Percent-encoding of the UTF-8 bytes: only A-Za-z0-9-._~ stay, and "/" when keep_slash is set.
QByteArray uri_encode(const QString &text, bool keep_slash);

// The place a signed request goes to: the URL, the Host header value and the encoded path (the signed canonical URI).
struct Location {
    QString url;
    QByteArray host;
    QByteArray path;
};
Location locate(const Target &target, const QString &key);

// {yyyy}, {MM} and {dd} filled from now; empty segments dropped, no slash at either end.
QString expand_prefix(const QString &prefix, const QDateTime &now);
// prefix/stem-random.ext with the prefix expanded; extension includes its dot.
QString make_object_key(const QString &prefix, const QString &stem, const QString &random_hex, const QString &extension, const QDateTime &now);
// Eight random hexadecimal digits.
QString random_suffix();

// What is copied after an upload: the public address when one is set, else the address the object was put to.
QString public_link(const Target &target, const QString &key);
}
