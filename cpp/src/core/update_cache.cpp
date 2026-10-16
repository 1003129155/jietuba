#include "core/update_cache.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace jietuba::update_cache {
namespace {
constexpr qint64 MAX_AGE_SECONDS = 7 * 24 * 60 * 60;
// Session leases live as long as the process, like the original's module-level dictionary.
std::map<QString, std::unique_ptr<QLockFile>> &sessions() {
    static std::map<QString, std::unique_ptr<QLockFile>> leases;
    return leases;
}

bool is_id(const QString &name) {
    static const QRegularExpression id("\\A[0-9a-f]{32}\\z");
    return id.match(name).hasMatch();
}

// A regular file or directory that is not a link, junction or any other reparse point.
bool plain(const QString &path, bool directory = false) {
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("No such file or directory: " + path.toStdString());
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    return directory == ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

QString child(const QString &parent, const QString &name) { return QDir(parent).filePath(name); }

QFileInfoList entries(const QString &directory) {
    return QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

qint64 mtime(const QString &path) { return QFileInfo(path).lastModified().toSecsSinceEpoch(); }

void sweep(const QString &root) {
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - MAX_AGE_SECONDS;
    QSet<QString> protected_ids;
    // Incomplete handoffs may still need their staged EXE. Keep those records.
    for (const auto &entry : entries(root)) {
        const QString directory = entry.absoluteFilePath();
        if (!is_id(entry.fileName()) || !plain(directory, true)) continue;
        const QString request = child(directory, "request.json");
        if (QFileInfo::exists(request) && !QFileInfo::exists(child(directory, "result.json"))) {
            protected_ids.insert(entry.fileName());
            // An unreadable handoff cannot safely identify its stage.
            if (!plain(request)) return;
            QFile file(request);
            if (!file.open(QIODevice::ReadOnly)) return;
            const auto document = QJsonDocument::fromJson(file.readAll());
            const auto staged = document.object().value("staged");
            if (!document.isObject() || !staged.isString()) return;
            protected_ids.insert(QFileInfo(QFileInfo(staged.toString()).absolutePath()).fileName());
        }
    }
    for (const QString &parent : {root, child(root, "helpers"), child(root, "requests")}) {
        if (!QFileInfo::exists(parent) || !plain(parent, true)) continue;
        const bool requests = QFileInfo(parent).fileName() == "requests" && parent != root;
        for (const auto &entry : entries(parent)) {
            const QString path = entry.absoluteFilePath();
            if (requests) {
                if (plain(path) && mtime(path) < cutoff) remove_request(path);
            } else if (is_id(entry.fileName()) && !protected_ids.contains(entry.fileName()) && plain(path, true)) {
                const auto files = entries(path);
                if (!std::all_of(files.begin(), files.end(), [](const QFileInfo &file) { return plain(file.absoluteFilePath()); })) continue;
                qint64 newest = mtime(path);
                for (const auto &file : files) newest = std::max(newest, mtime(file.absoluteFilePath()));
                if (newest < cutoff) remove_directory(path);
            }
        }
    }
}
}

QString cache_root() {
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (local.isEmpty()) throw std::runtime_error("LOCALAPPDATA is not set");
    const QString parent = QDir::cleanPath(child(local, "jietuba"));
    const QString root = child(parent, "updater");
    if (QFileInfo::exists(parent) && !plain(parent, true)) throw std::runtime_error("Updater cache must not be a link or junction");
    if (!QDir().mkpath(root)) throw std::runtime_error("Cannot create the updater cache");
    // Refuse cache trees redirected outside the application's directory.
    for (const QString &path : {parent, root})
        if (!plain(path, true)) throw std::runtime_error("Updater cache must not be a link or junction");
    return root;
}

QString cache_subdirectory(const QString &name) {
    if (name != "helpers" && name != "requests") throw std::invalid_argument("Unknown updater cache directory");
    const QString path = child(begin_session(), name);
    QDir().mkdir(path);
    if (!plain(path, true)) throw std::runtime_error("Updater cache must not be a link or junction");
    return path;
}

void remove_directory(const QString &directory) {
    try {
        const QString root = cache_root();
        const QFileInfo info(directory);
        const QString parent = QDir::cleanPath(info.absolutePath());
        if ((parent != QDir::cleanPath(root) && parent != QDir::cleanPath(child(root, "helpers"))) || !is_id(info.fileName())) return;
        if (!plain(parent, true) || !plain(directory, true)) return;
        auto files = entries(directory);
        if (!std::all_of(files.begin(), files.end(), [](const QFileInfo &file) { return plain(file.absoluteFilePath()); })) return;
        // A running Windows helper cannot be deleted. Try it before other files.
        std::stable_sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) {
            return (a.fileName() != "jietuba_updater.exe") < (b.fileName() != "jietuba_updater.exe");
        });
        for (const auto &file : files)
            if (!QFile::remove(file.absoluteFilePath())) return;
        QDir().rmdir(directory);
    } catch (const std::exception &) {
    }
}

void remove_request(const QString &path) {
    try {
        const QFileInfo info(path);
        const QString root = child(cache_root(), "requests");
        if (QDir::cleanPath(info.absolutePath()) == QDir::cleanPath(root) && info.suffix() == "json" && is_id(info.completeBaseName()))
            if (plain(root, true) && plain(path)) QFile::remove(path);
    } catch (const std::exception &) {
    }
}

void remove_helper(const QString &path) {
    if (!path.isEmpty()) remove_directory(QFileInfo(path).absolutePath());
}

QString begin_session() {
    const QString root = cache_root();
    if (sessions().count(root)) return root;
    const QString lock_path = child(root, ".maintenance.lock");
    if (QFileInfo::exists(lock_path) && !plain(lock_path)) throw std::runtime_error("Updater cache lock must be a regular file");
    QLockFile maintenance(lock_path);
    maintenance.setStaleLockTime(0);
    if (!maintenance.tryLock(1000)) throw std::runtime_error("Updater cache is busy; try again");
    bool active = false;
    for (const auto &entry : QDir(root).entryInfoList({"*.session.lock"}, QDir::Files | QDir::Hidden | QDir::System)) {
        const QString name = entry.fileName();
        if (!is_id(name.chopped(QString(".session.lock").size())) || !plain(entry.absoluteFilePath())) {
            active = true;
            continue;
        }
        QLockFile probe(entry.absoluteFilePath());
        probe.setStaleLockTime(0);
        if (probe.tryLock(0)) {
            probe.unlock();
        } else {
            active = true;
        }
    }
    if (!active) {
        try {
            sweep(root);
        } catch (const std::exception &) {
        }
    }
    auto lease = std::make_unique<QLockFile>(child(root, QUuid::createUuid().toString(QUuid::Id128) + ".session.lock"));
    lease->setStaleLockTime(0);
    if (!lease->tryLock(0)) throw std::runtime_error("Cannot reserve updater cache");
    sessions()[root] = std::move(lease);
    return root;
}

void reset_sessions_for_test() { sessions().clear(); }
}
