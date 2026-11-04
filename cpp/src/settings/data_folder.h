#pragma once
#include "core/data_location.h"
#include "core/platform_utils.h"
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

namespace jietuba {
class ToolSettingsManager;
}

// Start-up work on the data folder: taking in the settings older versions kept in the registry, and
// keeping the startup entries in line with the autostart switch.
namespace jietuba::data_folder {
// An older store. settings is a registry path ("HKEY_CURRENT_USER\...") or an INI file;
// clipboard_window a registry path of the clipboard window's geometry, empty when settings holds it;
// migrated a registry key written once it was copied, so a settings file deleted later starts from
// defaults instead.
struct Source {
    QString settings;
    QString clipboard_window;
    QString migrated;
};
// The registry of versions before the data folder. A settings file beside the program is used as
// it is, even empty, so it takes in nothing.
QList<Source> sources_for(const DataLocation &target);
// sources_for; none inside the test sandbox, which must not read the real registry.
QList<Source> default_sources(const DataLocation &target);
// While target's settings file is empty: copies the first source holding settings into it, all but
// the Local ones, which SettingsStore keeps reading from the registry. The sources keep their data,
// so an older version still finds it. Failures are logged and the start goes on.
void migrate(const DataLocation &target, const QList<Source> &sources);

// The programs the startup entries of this computer run; nullopt for a missing entry, an empty
// path for one that could not be read.
struct StartupEntries {
    std::optional<QString> run_entry;
    std::optional<QString> admin_task;
};
struct StartupFix {
    bool run_as_admin;
    // The Run entry to write (true) or delete (false); nullopt leaves it.
    std::optional<bool> run_entry;
};
// Makes the startup entries match the switch, so what the settings page shows is what happens at
// sign-in. An entry that runs another copy still on disk belongs to that copy and
// stays either way. With autostart on, a missing or dangling entry is made to run program; with
// it off, only an entry running program is deleted. A missing admin task needs elevation to
// create, so admin mode goes off until the settings page turns it on again; a dangling task is
// repointed by the elevated start (bootstrap).
StartupFix fix_startup(bool autostart, bool run_as_admin, const QString &program, const StartupEntries &entries);
// The admin task is only queried for run_as_admin.
StartupEntries read_startup_entries(bool run_as_admin);
// Every start: once the switch is stored (the welcome wizard decides it), the startup entries
// follow fix_startup.
void align_startup(ToolSettingsManager &config, const QString &program,
                   const std::function<StartupEntries(bool)> &read_entries = read_startup_entries,
                   const std::function<void(bool)> &write_run_entry = set_autostart);

// Green mode: the settings file beside the program, so copying the program's folder takes the
// settings along; otherwise it is in roaming_dir\Jietuba. Moves the file there, removes the old
// copy and switches config and data_location() to it. The settings this computer keeps in the
// registry (Local) stay where they are. Empty on success, else what went wrong; nothing changes then.
QString move_settings_file(ToolSettingsManager &config, bool beside_program, const QString &program_dir, const QString &roaming_dir);
// Whether a file can be created in the folder.
bool folder_writable(const QString &dir);
// Where green mode keeps the settings: the program's folder; inside the test sandbox a folder of the
// sandbox, so tests never write beside the test program.
QString green_mode_dir();
// move_settings_file for this program, between green_mode_dir() and %APPDATA%.
QString set_green_mode(ToolSettingsManager &config, bool on);

// migrate and align_startup run before the log file opens; their messages wait for this.
void write_deferred_log();
QStringList deferred_log_text();
}
