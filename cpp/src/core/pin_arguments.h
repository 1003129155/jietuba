#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <deque>
#include <optional>

// What an Explorer menu entry asks of the program: files to pin, on the command line of a new
// process or in a message to the running one.
namespace jietuba {
// A path the pin command accepts: absolute with a drive letter or as a UNC path, free of quotes
// and control characters.
bool is_pin_path(const QString &path);

struct PinArguments {
    // At least one --pin is on the command line and every path after it is valid.
    bool requested = false;
    // In command line order, duplicates kept.
    QStringList files;
};
// args are the arguments without the program name. One invalid path discards the whole batch,
// and the process then starts as it would without --pin.
PinArguments parse_pin_arguments(const QStringList &args);

struct PinRequest {
    // Tells a resent message from a new one.
    QString id;
    QStringList files;
};
QString new_pin_request_id();
// UTF-8 JSON: {"id": ..., "files": [...]}.
QByteArray pack_pin_request(const PinRequest &request);
// Nothing for malformed data, a missing id or any invalid path.
std::optional<PinRequest> parse_pin_request(const QByteArray &data);

// The last few request ids seen.
class RecentRequestIds {
public:
    static constexpr int CAPACITY = 64;
    // False when id was already among them; otherwise remembers it.
    bool remember(const QString &id);
private:
    std::deque<QString> ids_;
};

// Order kept, the first spelling of each path kept; paths differing only in case or in the kind
// of slash are one file.
QStringList unique_pin_paths(const QStringList &files);
}
