#include "settings/data_folder.h"
#include "core/constants.h"
#include "core/elevation.h"
#include "core/logger.h"
#include "settings/tool_settings.h"
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>
#include <memory>
#include <utility>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace jietuba::data_folder {
namespace {
const QString REGISTRY_ROOT = "HKEY_CURRENT_USER\\";
const QString LOG_MODULE = "Data";

std::vector<std::pair<int, LogMsg>> &deferred() {
    static std::vector<std::pair<int, LogMsg>> messages;
    return messages;
}

void defer(int level, const LogMsg &message) { deferred().emplace_back(level, message); }

QString shown(const QString &path) { return QDir::toNativeSeparators(path); }

bool same_file(const QString &a, const QString &b) {
    return !a.isEmpty() && QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}

// Reading a registry path through QSettings creates the key when it is missing, so it is looked up first.
bool registry_key_exists(const QString &path) {
#ifdef Q_OS_WIN
    if (!path.startsWith(REGISTRY_ROOT, Qt::CaseInsensitive)) return false;
    const QString subkey = path.mid(REGISTRY_ROOT.size());
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(subkey.utf16()), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
#else
    Q_UNUSED(path);
    return false;
#endif
}

std::unique_ptr<QSettings> open_store(const QString &path) {
    if (path.isEmpty()) return nullptr;
    if (path.startsWith("HKEY_", Qt::CaseInsensitive)) {
        if (!registry_key_exists(path)) return nullptr;
        return std::make_unique<QSettings>(path, QSettings::NativeFormat);
    }
    if (!QFileInfo(path).isFile()) return nullptr;
    return std::make_unique<QSettings>(path, QSettings::IniFormat);
}
}

QList<Source> sources_for(const DataLocation &target) {
    if (target.beside_program) return {};
    return {{LOCAL_SETTINGS_KEY, REGISTRY_ROOT + "Software\\Jietuba\\ClipboardWindow", REGISTRY_ROOT + "Software\\Jietuba\\Migrated"}};
}

QList<Source> default_sources(const DataLocation &target) {
    if (!test_sandbox_dir().isEmpty()) return {};
    return sources_for(target);
}

void migrate(const DataLocation &target, const QList<Source> &sources) {
    QSettings settings(target.settings_file(), QSettings::IniFormat);
    if (!settings.allKeys().isEmpty()) return;
    // Two starts at once would both find the file empty: the second waits here and then finds it filled.
    QDir().mkpath(target.dir);
    QLockFile lock(target.settings_file() + ".migrating");
    lock.tryLock(10'000);
    settings.sync();
    if (!settings.allKeys().isEmpty()) return;
    for (const auto &source : sources) {
        if (same_file(source.settings, target.settings_file())) continue;
        if (registry_key_exists(source.migrated)) {
            defer(LogLevel::Info, T("旧设置已迁移过，不再复制: {source}", {{"source", shown(source.settings)}}));
            continue;
        }
        const auto store = open_store(source.settings);
        // Settings bound to this computer stay in the registry, where SettingsStore reads them.
        QStringList keys;
        if (store)
            for (const auto &key : store->allKeys())
                if (!LOCAL_SETTING_PATHS().contains(key)) keys.append(key);
        if (keys.isEmpty()) continue;
        for (const auto &key : keys) settings.setValue(key, store->value(key));
        if (const auto window = open_store(source.clipboard_window)) {
            for (const auto &key : window->allKeys()) settings.setValue(CLIPBOARD_WINDOW_SETTINGS_GROUP + "/" + key, window->value(key));
        }
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            defer(LogLevel::Warning, T("无法写入设置文件: {path}", {{"path", shown(target.settings_file())}}));
            return;
        }
        if (!source.migrated.isEmpty()) QSettings(source.migrated, QSettings::NativeFormat).setValue("settings_file", shown(target.settings_file()));
        defer(LogLevel::Info, T("已复制设置: {source} → {target}", {{"source", shown(source.settings)}, {"target", shown(target.settings_file())}}));
        return;
    }
}

StartupFix fix_startup(bool autostart, bool run_as_admin, const QString &program, const StartupEntries &entries) {
    StartupFix fix{run_as_admin, std::nullopt};
    const auto &run = entries.run_entry;
    if (!autostart) {
        if (run && same_file(*run, program)) fix.run_entry = false;
        return fix;
    }
    if (run_as_admin) {
        if (entries.admin_task) return fix;
        fix.run_as_admin = false;
    }
    const bool dangling = run && !run->isEmpty() && !same_file(*run, program) && !QFileInfo::exists(*run);
    if (!run || dangling) fix.run_entry = true;
    return fix;
}

StartupEntries read_startup_entries(bool run_as_admin) {
    return {get_autostart_program(), run_as_admin ? elevation::query_task_command() : std::nullopt};
}

void align_startup(ToolSettingsManager &config, const QString &program, const std::function<StartupEntries(bool)> &read_entries,
                   const std::function<void(bool)> &write_run_entry) {
    // Until the welcome wizard or the settings page stores the switch, its default decides nothing.
    if (!config.qsettings.contains(ToolSettingsManager::app_setting_path("autostart_enabled"))) return;
    const bool autostart = config.get_autostart_enabled();
    const bool run_as_admin = config.get_run_as_admin();
    const auto fix = fix_startup(autostart, run_as_admin, program, read_entries(autostart && run_as_admin));
    if (fix.run_as_admin != run_as_admin) {
        config.set_run_as_admin(fix.run_as_admin);
        defer(LogLevel::Info, T("这台电脑上没有管理员启动的计划任务，管理员启动已关闭"));
    }
    if (fix.run_entry) {
        write_run_entry(*fix.run_entry);
        defer(LogLevel::Info, *fix.run_entry ? T("开机自启已改为启动当前位置的程序") : T("开机自启已关闭，已删除启动项"));
    }
}

void write_deferred_log() {
    auto &logger = get_logger();
    for (const auto &[level, message] : deferred()) {
        if (level >= LogLevel::Warning) logger.warning(message, LOG_MODULE);
        else logger.info(message, LOG_MODULE);
    }
    deferred().clear();
}

QStringList deferred_log_text() {
    QStringList lines;
    for (const auto &entry : deferred()) lines.append(entry.second.render());
    return lines;
}
}
