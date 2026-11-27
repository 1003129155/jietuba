#include "core/pin_arguments.h"
#include "core/explorer_menu_contract.h"
#include "core/logger.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace jietuba {
namespace {
// A request longer than this is not one the program wrote.
constexpr int MAX_REQUEST_FILES = 4096;
constexpr int MAX_ID_LENGTH = 64;
constexpr int MAX_PATH_LENGTH = 32767;
}

bool is_pin_path(const QString &path) {
    if (path.size() < 3 || path.size() > MAX_PATH_LENGTH) return false;
    for (const QChar c : path)
        if (c == QLatin1Char('"') || c.unicode() < 0x20 || c.unicode() == 0x7F) return false;
    const bool drive = path[0].unicode() < 128 && path[0].isLetter() && path[1] == QLatin1Char(':') && (path[2] == QLatin1Char('\\') || path[2] == QLatin1Char('/'));
    const bool unc = path.startsWith(QLatin1String("\\\\")) && path[2] != QLatin1Char('\\');
    return drive || unc;
}

PinArguments parse_pin_arguments(const QStringList &args) {
    const QString option = QString::fromWCharArray(explorer_menu::PIN_ARGUMENT);
    PinArguments result;
    bool seen = false;
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] != option) continue;
        seen = true;
        if (i + 1 >= args.size() || !is_pin_path(args[i + 1])) {
            log_warning(T("--pin 后面的路径不合法，整批丢弃"), "SingleInstance");
            return {};
        }
        result.files.append(args[++i]);
    }
    result.requested = seen;
    return result;
}

QString new_pin_request_id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QByteArray pack_pin_request(const PinRequest &request) {
    return QJsonDocument(QJsonObject{{"id", request.id}, {"files", QJsonArray::fromStringList(request.files)}}).toJson(QJsonDocument::Compact);
}

std::optional<PinRequest> parse_pin_request(const QByteArray &data) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    const auto object = document.object();
    const auto id = object.value("id");
    const auto files = object.value("files");
    if (!id.isString() || !files.isArray()) return std::nullopt;
    PinRequest request{id.toString(), {}};
    if (request.id.isEmpty() || request.id.size() > MAX_ID_LENGTH) return std::nullopt;
    const auto array = files.toArray();
    if (array.isEmpty() || array.size() > MAX_REQUEST_FILES) return std::nullopt;
    for (const auto &value : array) {
        if (!value.isString() || !is_pin_path(value.toString())) return std::nullopt;
        request.files.append(value.toString());
    }
    return request;
}

bool RecentRequestIds::remember(const QString &id) {
    if (std::find(ids_.begin(), ids_.end(), id) != ids_.end()) return false;
    ids_.push_back(id);
    if (static_cast<int>(ids_.size()) > CAPACITY) ids_.pop_front();
    return true;
}

QStringList unique_pin_paths(const QStringList &files) {
    QStringList unique;
    QSet<QString> seen;
    for (const auto &file : files) {
        const QString key = QDir::fromNativeSeparators(file).toLower();
        if (seen.contains(key)) continue;
        seen.insert(key);
        unique.append(file);
    }
    return unique;
}
}
