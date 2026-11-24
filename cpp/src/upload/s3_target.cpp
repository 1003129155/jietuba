#include "upload/s3_target.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QUuid>

namespace jietuba::upload {
namespace {
bool http_url_ok(const QString &text) {
    const QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme();
    return url.isValid() && (scheme == "http" || scheme == "https") && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment();
}

bool has_control_or_space(const QString &text) {
    for (const QChar c : text)
        if (c.unicode() <= 0x20 || c.unicode() == 0x7F) return true;
    return false;
}

bool is_ip_literal(const QString &host) {
    static const QRegularExpression ipv4(R"(^\d{1,3}(\.\d{1,3}){3}$)");
    return host.contains(':') || ipv4.match(host).hasMatch();
}
}

QString new_target_id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

std::vector<Target> parse_targets(const QString &json) {
    std::vector<Target> targets;
    QSet<QString> seen;
    for (const auto &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const auto object = value.toObject();
        Target target;
        target.id = object.value("id").toString();
        if (target.id.isEmpty() || seen.contains(target.id)) continue;
        seen.insert(target.id);
        target.name = object.value("name").toString();
        target.endpoint = object.value("endpoint").toString();
        target.region = object.value("region").toString();
        target.bucket = object.value("bucket").toString();
        target.access_key_id = object.value("access_key_id").toString();
        target.secret_access_key = object.value("secret_access_key").toString();
        target.path_prefix = object.value("path_prefix").toString();
        target.public_url = object.value("public_url").toString();
        target.addressing = object.value("addressing").toString() == "virtual" ? Addressing::Virtual : Addressing::Path;
        targets.push_back(std::move(target));
    }
    return targets;
}

QString serialize_targets(const std::vector<Target> &targets) {
    QJsonArray array;
    for (const auto &target : targets) {
        array.append(QJsonObject{{"id", target.id},
                                 {"name", target.name},
                                 {"endpoint", target.endpoint},
                                 {"region", target.region},
                                 {"bucket", target.bucket},
                                 {"access_key_id", target.access_key_id},
                                 {"secret_access_key", target.secret_access_key},
                                 {"path_prefix", target.path_prefix},
                                 {"addressing", target.addressing == Addressing::Virtual ? "virtual" : "path"},
                                 {"public_url", target.public_url}});
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

const Target *find_target(const std::vector<Target> &targets, const QString &id) {
    if (id.isEmpty()) return nullptr;
    for (const auto &target : targets)
        if (target.id == id) return &target;
    return nullptr;
}

LinkFormat parse_link_format(const QString &text) {
    if (text == "markdown") return LinkFormat::Markdown;
    if (text == "html") return LinkFormat::Html;
    return LinkFormat::Url;
}

QString link_format_name(LinkFormat format) {
    switch (format) {
    case LinkFormat::Markdown: return "markdown";
    case LinkFormat::Html: return "html";
    default: return "url";
    }
}

QString format_link(const QString &url, LinkFormat format) {
    switch (format) {
    case LinkFormat::Markdown: return "![](" + url + ")";
    case LinkFormat::Html: return "<img src=\"" + url + "\">";
    default: return url;
    }
}

std::vector<FieldError> validate(const Target &target, const std::vector<Target> &others) {
    std::vector<FieldError> errors;
    if (target.name.isEmpty()) {
        errors.push_back({"name", "Enter a name."});
    } else {
        for (const auto &other : others)
            if (other.id != target.id && other.name.compare(target.name, Qt::CaseInsensitive) == 0) {
                errors.push_back({"name", "This name is already used."});
                break;
            }
    }
    if (target.endpoint.isEmpty()) errors.push_back({"endpoint", "Enter the service endpoint."});
    else if (!http_url_ok(target.endpoint)) errors.push_back({"endpoint", "The endpoint must be an http:// or https:// address without account, query or fragment."});
    if (target.region.isEmpty()) {
        errors.push_back({"region", "Enter the region (Cloudflare R2 uses auto)."});
    } else {
        static const QRegularExpression allowed(R"(^[A-Za-z0-9._-]+$)");
        if (!allowed.match(target.region).hasMatch()) errors.push_back({"region", "The region can only contain letters, digits, dots, hyphens and underscores."});
    }
    if (target.bucket.isEmpty()) errors.push_back({"bucket", "Enter the bucket name."});
    else if (target.bucket.contains('/') || has_control_or_space(target.bucket)) errors.push_back({"bucket", "The bucket name cannot contain '/' or spaces."});
    if (target.access_key_id.isEmpty()) errors.push_back({"access_key_id", "Enter the access key ID."});
    else if (has_control_or_space(target.access_key_id)) errors.push_back({"access_key_id", "The access key ID cannot contain spaces."});
    if (target.secret_access_key.isEmpty()) errors.push_back({"secret_access_key", "Enter the secret access key."});
    else if (has_control_or_space(target.secret_access_key)) errors.push_back({"secret_access_key", "The secret access key cannot contain spaces."});
    if (!target.public_url.isEmpty() && !http_url_ok(target.public_url)) errors.push_back({"public_url", "The public address must be an http:// or https:// address."});
    return errors;
}

bool uses_virtual_host(const Target &target) {
    return target.addressing == Addressing::Virtual && !target.bucket.contains('.') && !is_ip_literal(QUrl(target.endpoint).host());
}

QByteArray uri_encode(const QString &text, bool keep_slash) {
    QByteArray out;
    for (const char raw : text.toUtf8()) {
        const auto c = static_cast<unsigned char>(raw);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || (keep_slash && c == '/')) {
            out.append(raw);
        } else {
            static const char digits[] = "0123456789ABCDEF";
            out.append('%');
            out.append(digits[c >> 4]);
            out.append(digits[c & 15]);
        }
    }
    return out;
}

Location locate(const Target &target, const QString &key) {
    const QUrl endpoint(target.endpoint);
    const bool https = endpoint.scheme() == "https";
    // An IPv6 literal is no domain name to encode, and goes in brackets.
    const QString name = endpoint.host();
    QByteArray host = name.contains(':') ? name.toLatin1() : QUrl::toAce(name);
    if (uses_virtual_host(target)) host = target.bucket.toUtf8() + "." + host;
    else if (host.contains(':')) host = "[" + host + "]";
    const int port = endpoint.port();
    if (port != -1 && port != (https ? 443 : 80)) host += ":" + QByteArray::number(port);
    QString base = endpoint.path();
    while (base.endsWith('/')) base.chop(1);
    QByteArray path = uri_encode(base, true);
    if (!uses_virtual_host(target)) path += "/" + uri_encode(target.bucket, false);
    path += "/" + uri_encode(key, true);
    Location location;
    location.host = host;
    location.path = path;
    location.url = QString(https ? "https://" : "http://") + QString::fromLatin1(host) + QString::fromLatin1(path);
    return location;
}

QString expand_prefix(const QString &prefix, const QDateTime &now) {
    QString text = prefix;
    text.replace("{yyyy}", now.toString("yyyy"));
    text.replace("{MM}", now.toString("MM"));
    text.replace("{dd}", now.toString("dd"));
    text.replace('\\', '/');
    return text.split('/', Qt::SkipEmptyParts).join('/');
}

QString make_object_key(const QString &prefix, const QString &stem, const QString &random_hex, const QString &extension, const QDateTime &now) {
    const QString directory = expand_prefix(prefix, now);
    return (directory.isEmpty() ? QString() : directory + "/") + stem + "-" + random_hex + extension;
}

QString random_suffix() { return QString::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, '0'); }

QString public_link(const Target &target, const QString &key) {
    if (target.public_url.isEmpty()) return locate(target, key).url;
    QString base = target.public_url;
    while (base.endsWith('/')) base.chop(1);
    return base + "/" + QString::fromLatin1(uri_encode(key, true));
}
}
