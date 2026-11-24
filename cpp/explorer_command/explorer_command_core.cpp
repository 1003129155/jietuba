#include "explorer_command_core.h"
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace jietuba::explorer_command {
namespace {
using explorer_menu::Mode;

wchar_t ascii_lower(wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + (L'a' - L'A')) : c; }
bool is_separator(wchar_t c) { return c == L'\\' || c == L'/'; }

// `lower` is already lower case.
bool equals_ascii_nocase(std::wstring_view text, std::wstring_view lower) {
    if (text.size() != lower.size())
        return false;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (ascii_lower(text[i]) != lower[i])
            return false;
    return true;
}

bool in_list(std::wstring_view extension, const wchar_t *const *list, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (equals_ascii_nocase(extension, list[i]))
            return true;
    return false;
}

bool is_plain_exe_name(std::wstring_view name) {
    constexpr std::wstring_view suffix = L".exe";
    if (name.size() <= suffix.size() || !equals_ascii_nocase(name.substr(name.size() - suffix.size()), suffix))
        return false;
    for (wchar_t c : name)
        if (c < L' ' || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos)
            return false;
    return true;
}

std::wstring to_wide(std::string_view utf8, bool &ok) {
    ok = true;
    if (utf8.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        ok = false;
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}
}  // namespace

std::optional<Preference> parse_preference(std::string_view text) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
        text.remove_prefix(3);
    std::string_view lines[3];
    for (auto &line : lines) {
        if (text.empty())
            return std::nullopt;
        const std::size_t end = text.find('\n');
        line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
    }
    Preference preference;
    bool known = false;
    for (std::size_t i = 0; i < std::size(explorer_menu::MODE_WORDS); ++i) {
        if (lines[0] == explorer_menu::MODE_WORDS[i]) {
            preference.mode = static_cast<Mode>(i);
            known = true;
        }
    }
    bool title_ok = false;
    bool name_ok = false;
    preference.title = to_wide(lines[1], title_ok);
    preference.exe_name = to_wide(lines[2], name_ok);
    if (!known || !title_ok || !name_ok || preference.title.empty() || !is_plain_exe_name(preference.exe_name))
        return std::nullopt;
    return preference;
}

std::optional<Preference> read_preference_file(const std::wstring &path) {
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return std::nullopt;
    // The three short lines never come near this; a larger file is not ours.
    constexpr std::size_t LIMIT = 64 * 1024;
    std::string text(LIMIT + 1, '\0');
    text.resize(fread(text.data(), 1, text.size(), file));
    fclose(file);
    if (text.size() > LIMIT)
        return std::nullopt;
    return parse_preference(text);
}

std::wstring preference_path(std::wstring_view local_app_data, std::wstring_view family_name) {
    std::wstring path(local_app_data);
    while (!path.empty() && is_separator(path.back()))
        path.pop_back();
    path.append(L"\\Packages\\").append(family_name).append(L"\\LocalState\\").append(explorer_menu::PREFERENCE_FILE);
    return path;
}

bool is_pin_path(std::wstring_view path) {
    for (wchar_t c : path)
        if (c == L'"' || c < L' ' || c == 0x7F)
            return false;
    if (path.size() >= 4) {
        const wchar_t drive = ascii_lower(path[0]);
        if (drive >= L'a' && drive <= L'z' && path[1] == L':' && is_separator(path[2]))
            return !is_separator(path[3]);
    }
    // \\server\share\... with a non-empty server and share.
    if (path.size() >= 5 && is_separator(path[0]) && is_separator(path[1])) {
        const std::size_t server_end = path.find_first_of(L"\\/", 2);
        return server_end != std::wstring_view::npos && server_end > 2 && server_end + 1 < path.size() &&
               !is_separator(path[server_end + 1]);
    }
    return false;
}

bool extension_matches(std::wstring_view path, Mode mode) {
    if (mode == Mode::Off)
        return false;
    const std::size_t separator = path.find_last_of(L"\\/");
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring_view::npos || (separator != std::wstring_view::npos && dot < separator))
        return false;
    const std::wstring_view extension = path.substr(dot + 1);
    if (in_list(extension, explorer_menu::IMAGE_EXTENSIONS, std::size(explorer_menu::IMAGE_EXTENSIONS)))
        return true;
    return mode == Mode::ImagesAndText &&
           in_list(extension, explorer_menu::TEXT_EXTENSIONS, std::size(explorer_menu::TEXT_EXTENSIONS));
}

bool wants_path(std::wstring_view path, Mode mode) { return extension_matches(path, mode) && is_pin_path(path); }

void append_quoted(std::wstring_view argument, std::wstring &command_line) {
    command_line.push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        // Backslashes only matter in front of a quote; those get doubled, and the quote escaped.
        command_line.append(backslashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"')
            command_line.push_back(L'\\');
        backslashes = 0;
        command_line.push_back(c);
    }
    command_line.append(backslashes * 2, L'\\');
    command_line.push_back(L'"');
}

CommandLine build_command_line(std::wstring_view exe_path, const std::vector<std::wstring> &files) {
    CommandLine result;
    std::wstring line;
    append_quoted(exe_path, line);
    for (const std::wstring &file : files) {
        std::wstring next = line;
        next.append(L" ").append(explorer_menu::PIN_ARGUMENT).append(L" ");
        append_quoted(file, next);
        if (next.size() >= MAX_COMMAND_LINE)
            break;
        line = std::move(next);
        ++result.included;
    }
    if (result.included > 0)
        result.text = std::move(line);
    return result;
}

}  // namespace jietuba::explorer_command
