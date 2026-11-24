#pragma once
#include "core/explorer_menu_contract.h"
#include <QString>
#include <functional>

// The "pin to screen" entry in Explorer's right-click menu. The portable build writes a verb for
// each extension under HKCU\Software\Classes\SystemFileAssociations (Windows 11 shows it under
// "Show more options"); the packaged build cannot, and leaves its choice in a file for the
// command DLL that its manifest registers.
namespace jietuba {
class ToolSettingsManager;
}

namespace jietuba::explorer_menu {
Mode mode_from_word(const QString &word);
QString word_of(Mode mode);

// By extension, without the dot, in any case: the lists of the shared contract.
bool is_image_extension(const QString &extension);
bool is_text_extension(const QString &extension);

struct Menu {
    Mode mode = Mode::Off;
    // In the interface language.
    QString title;
    // Native separators.
    QString exe_path;
};

// What a run touches, replaceable by tests.
struct Environment {
    // Under HKEY_CURRENT_USER.
    QString classes_root = QStringLiteral("Software\\Classes");
    // Tells the shell the associations changed; the real call by default.
    std::function<void()> notify;
    // The packaged build's preference file; its place in the package's LocalState by default.
    QString preference_path;
};

// The menu text in the current interface language.
QString menu_title();
// The verb's key name, per executable so the lightweight and the full build keep their own.
QString verb_name(const QString &exe_path);
// The registry key of one extension's verb, below the classes root.
QString verb_key(const QString &extension, const QString &exe_path);

// Makes the registry match menu: the verb exists for the extensions its mode asks for, with the
// current title and executable, and nowhere else. Entries that are right are not written again.
// Notifies the shell once when anything changed. Returns whether anything did.
bool reconcile_registry(const Menu &menu, const Environment &environment = {});
// Writes the packaged build's preference file; false when it cannot be written.
bool write_preference_file(const Menu &menu, const QString &path);
// Where the packaged build keeps the file; empty when this process has no package.
QString preference_file_path();

// The registry for the portable build, the preference file for the packaged one.
void apply(const Menu &menu, const Environment &environment = {});
// apply for the saved choice, the current interface language and this executable's place.
void sync(const ToolSettingsManager &settings, const Environment &environment = {});
}
