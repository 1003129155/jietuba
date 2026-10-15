#pragma once
#include <QString>

// Best-effort cleanup of updater-owned cache files, never installation backups. The cache is
// %LOCALAPPDATA%\jietuba\updater, shared with the Python version; failures throw
// std::runtime_error where the original raises OSError.
namespace jietuba::update_cache {
QString cache_root();
// "helpers" or "requests" under the root, after this process has started its session.
QString cache_subdirectory(const QString &name);
// Deletes only a flat UUID cache directory; refuses links and subdirectories.
void remove_directory(const QString &directory);
void remove_request(const QString &path);
void remove_helper(const QString &path);
// Sweeps old orphans once, with live application sessions excluded by locks.
QString begin_session();
// Releases this process's session leases so a test can start over with another cache.
void reset_sessions_for_test();
}
