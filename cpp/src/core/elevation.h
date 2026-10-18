#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
#include <utility>

// Run as administrator: the elevation state and the highest-privilege scheduled task used for
// sign-in. A task is the only way to start an elevated process without a UAC prompt: only an
// elevated process can create it, then a normal process of the same user can run it.
namespace jietuba::elevation {
inline const QString ELEVATED = "elevated";
inline const QString CAN_ELEVATE = "can_elevate";
inline const QString STANDARD = "standard";

QString elevation_state();
QString task_name();
// The task definition handed to schtasks /XML; the tests compare it with a fixed sample.
QString build_task_xml(const QString &command, const QString &user);

using SchtasksRunner = std::function<std::pair<int, QByteArray>(const QStringList &)>;
// Replace the schtasks calls (plain and through UAC); empty restores them. Inside the test
// sandbox the real calls refuse to run.
void set_schtasks_runner_for_test(SchtasksRunner runner);
void set_elevated_schtasks_runner_for_test(SchtasksRunner runner);
// Replaces this executable's path, the state and the user name; empty restores them.
void set_environment_for_test(std::optional<QString> exe, std::optional<QString> state, std::optional<QString> user);
// Replaces starting this program again through UAC; empty restores it.
void set_self_launcher_for_test(std::function<bool()> launcher);

// nullopt when the task is missing, else the program it runs (empty when unreadable).
std::optional<QString> query_task_command();
bool points_to_self(const std::optional<QString> &command);
// The task's program is another one and gone from disk (its folder was moved or deleted), so nothing
// starts at sign-in. A missing or unreadable task is not stale: the user may have removed it on purpose.
bool task_is_stale(const std::optional<QString> &command);
// Both go through UAC without elevation. A missing task counts as removed.
bool install_task();
bool remove_task();
bool run_task();
// Puts autostart in place and returns whether admin mode is in effect (the task exists). Admin
// mode uses the task, plain autostart the Run entry, never both. Without elevation only an
// interactive caller (the settings page) changes the task, through UAC; the welcome wizard runs
// in the background and leaves it. A task that already runs this program stays as it is, so
// applying other settings never asks for UAC. A task that could not be created falls back to the
// Run entry; one that could not be removed still starts at sign-in, so no Run entry is written then.
bool apply_startup(bool autostart, bool run_as_admin, bool was_run_as_admin, bool interactive = false);
// Starts this program again through UAC; false when that was declined or could not start.
bool run_self_elevated();
// An elevated instance can write the task without UAC: points a stale task at this program.
// True when it rewrote the task. A task for another copy that still exists is left alone.
bool repair_task();
}
