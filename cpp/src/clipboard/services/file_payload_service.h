#pragma once
#include <QString>
#include <optional>

// File items between forms and storage: normalized paths and the {"files": [...]} payload.
namespace jietuba::clipboard {
QString normalize_file_path(const QString &path);
QString build_file_payload(const QString &path);
// The first path of a file item; old items stored the raw path text.
QString extract_first_file_path_from_content(const std::optional<QString> &content);
}
