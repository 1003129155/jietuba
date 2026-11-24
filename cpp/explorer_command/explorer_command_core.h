#pragma once
// Logic of the packaged Explorer command that needs neither COM nor Qt, so it can be tested apart
// from the DLL. The DLL runs inside Explorer's COM surrogate and must stay small.
#include "core/explorer_menu_contract.h"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jietuba::explorer_command {

// One CLSID per package variant: two installed packages must not claim the same class. The
// manifest takes them from here (scripts/explorer_menu_manifest.ps1).
inline constexpr const wchar_t *COMMAND_CLSID_LITE = L"{C7EF3C85-6DFF-49F0-A22F-2E2E790D627D}";
inline constexpr const wchar_t *COMMAND_CLSID_FULL = L"{DF1BC5D3-9DE7-4233-A59A-3303B0939E38}";

// CreateProcess accepts a command line of at most this many characters including the terminator.
inline constexpr std::size_t MAX_COMMAND_LINE = 32767;

struct Preference {
    explorer_menu::Mode mode = explorer_menu::Mode::Off;
    std::wstring title;     // menu text in the interface language
    std::wstring exe_name;  // file name only; the executable sits beside the DLL
};

// Missing lines, an unknown mode word, an empty title or an exe name that is not a plain *.exe
// file name give no preference. A UTF-8 byte order mark and CRLF line ends are accepted.
std::optional<Preference> parse_preference(std::string_view text);
std::optional<Preference> read_preference_file(const std::wstring &path);

// <local app data>\Packages\<family name>\LocalState\explorer_menu.txt
std::wstring preference_path(std::wstring_view local_app_data, std::wstring_view family_name);

// Drive-letter or UNC absolute path without quotes or control characters: what the application's
// --pin parsing accepts.
bool is_pin_path(std::wstring_view path);
// The extension (case-insensitive) is one the mode pins. Always false for Off.
bool extension_matches(std::wstring_view path, explorer_menu::Mode mode);
// is_pin_path and extension_matches together.
bool wants_path(std::wstring_view path, explorer_menu::Mode mode);

// Appends the argument quoted so that CommandLineToArgvW returns it unchanged.
void append_quoted(std::wstring_view argument, std::wstring &command_line);

struct CommandLine {
    std::wstring text;
    std::size_t included = 0;  // leading files that fit; fewer than given when truncated
};
// "<exe>" --pin "<a>" --pin "<b>" ... Files that would push the line past MAX_COMMAND_LINE are
// left out; text is empty when not even the first fits.
CommandLine build_command_line(std::wstring_view exe_path, const std::vector<std::wstring> &files);

}  // namespace jietuba::explorer_command
