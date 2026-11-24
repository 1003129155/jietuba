#include "core/explorer_menu.h"
#include "core/i18n.h"
#include "core/logger.h"
#include "core/package_identity.h"
#include "settings/tool_settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>

namespace jietuba::explorer_menu {
namespace {
// Records the executable that wrote a verb, whatever its command line says.
constexpr wchar_t OWNER_VALUE[] = L"JietubaExe";

std::wstring wide(const QString &text) { return text.toStdWString(); }

std::optional<std::wstring> read_string(HKEY key, const wchar_t *name) {
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return std::nullopt;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) return std::nullopt;
    // The size counts the terminating zero.
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

bool write_string(HKEY key, const wchar_t *name, const std::wstring &value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

// A key that closes itself.
struct Key {
    HKEY handle = nullptr;
    Key() = default;
    Key(Key &&other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
    Key(const Key &) = delete;
    ~Key() {
        if (handle) RegCloseKey(handle);
    }
    static Key open(const std::wstring &path, REGSAM access = KEY_READ) {
        Key key;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, access, &key.handle) != ERROR_SUCCESS) key.handle = nullptr;
        return key;
    }
    static Key create(const std::wstring &path) {
        Key key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key.handle, nullptr) != ERROR_SUCCESS) key.handle = nullptr;
        return key;
    }
    explicit operator bool() const { return handle != nullptr; }
};

std::wstring icon_value(const QString &exe_path) { return L"\"" + wide(exe_path) + L"\",0"; }
std::wstring command_value(const QString &exe_path) { return L"\"" + wide(exe_path) + L"\" " + PIN_ARGUMENT + L" \"%1\""; }

// Whether the verb already holds exactly what menu asks for.
bool verb_is_current(const std::wstring &path, const Menu &menu) {
    const Key verb = Key::open(path);
    if (!verb) return false;
    const auto equals = [&verb](const wchar_t *name, const std::wstring &expected) {
        const auto value = read_string(verb.handle, name);
        return value && *value == expected;
    };
    if (!equals(nullptr, wide(menu.title)) || !equals(L"Icon", icon_value(menu.exe_path)) || !equals(L"MultiSelectModel", L"Document") ||
        !equals(OWNER_VALUE, wide(menu.exe_path)))
        return false;
    const Key command = Key::open(path + L"\\command");
    if (!command) return false;
    const auto value = read_string(command.handle, nullptr);
    return value && *value == command_value(menu.exe_path);
}

bool write_verb(const std::wstring &path, const Menu &menu) {
    const Key verb = Key::create(path);
    if (!verb) return false;
    const bool written = write_string(verb.handle, nullptr, wide(menu.title)) && write_string(verb.handle, L"Icon", icon_value(menu.exe_path)) &&
                         write_string(verb.handle, L"MultiSelectModel", L"Document") && write_string(verb.handle, OWNER_VALUE, wide(menu.exe_path));
    const Key command = Key::create(path + L"\\command");
    return written && command && write_string(command.handle, nullptr, command_value(menu.exe_path));
}

// A key this program made above its verb goes with the verb, unless something else lives there now.
void delete_if_empty(const std::wstring &path) {
    DWORD subkeys = 0, values = 0;
    {
        const Key key = Key::open(path);
        if (!key || RegQueryInfoKeyW(key.handle, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) return;
    }
    if (subkeys == 0 && values == 0) RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
}

bool remove_verb(const std::wstring &extension_key, const std::wstring &verb_path) {
    if (!Key::open(verb_path)) return false;
    RegDeleteTreeW(HKEY_CURRENT_USER, verb_path.c_str());
    delete_if_empty(extension_key + L"\\shell");
    delete_if_empty(extension_key);
    return true;
}
}

Mode mode_from_word(const QString &word) {
    for (int i = 0; i < static_cast<int>(std::size(MODE_WORDS)); ++i)
        if (word == QLatin1String(MODE_WORDS[i])) return static_cast<Mode>(i);
    return Mode::Off;
}

namespace {
template <size_t N>
bool listed(const wchar_t *const (&extensions)[N], const QString &extension) {
    const QString lower = extension.toLower();
    for (const wchar_t *candidate : extensions)
        if (lower == QString::fromWCharArray(candidate)) return true;
    return false;
}
}

bool is_image_extension(const QString &extension) { return listed(IMAGE_EXTENSIONS, extension); }
bool is_text_extension(const QString &extension) { return listed(TEXT_EXTENSIONS, extension); }

QString word_of(Mode mode) { return QString::fromLatin1(MODE_WORDS[static_cast<int>(mode)]); }

QString menu_title() { return i18n_tr("Pin to Screen with jietuba", "ExplorerMenu"); }

QString verb_name(const QString &exe_path) { return QStringLiteral("jietuba.%1.Pin").arg(QFileInfo(exe_path).completeBaseName().toLower()); }

QString verb_key(const QString &extension, const QString &exe_path) {
    return QStringLiteral("SystemFileAssociations\\.%1\\shell\\%2").arg(extension, verb_name(exe_path));
}

bool reconcile_registry(const Menu &menu, const Environment &environment) {
    const std::wstring root = wide(environment.classes_root);
    bool changed = false;
    const auto reconcile = [&](const wchar_t *extension, bool wanted) {
        const QString ext = QString::fromWCharArray(extension);
        const std::wstring verb_path = root + L"\\" + wide(verb_key(ext, menu.exe_path));
        if (wanted) {
            if (verb_is_current(verb_path, menu)) return;
            if (write_verb(verb_path, menu)) changed = true;
            else log_warning(T("无法写入资源管理器右键菜单项: .{ext}", {{"ext", ext}}), "ExplorerMenu");
        } else if (remove_verb(root + L"\\SystemFileAssociations\\." + extension, verb_path)) {
            changed = true;
        }
    };
    for (const wchar_t *extension : IMAGE_EXTENSIONS) reconcile(extension, menu.mode != Mode::Off);
    for (const wchar_t *extension : TEXT_EXTENSIONS) reconcile(extension, menu.mode == Mode::ImagesAndText);
    if (!changed) return false;
    log_info(T("资源管理器右键菜单已更新 (模式 {mode})", {{"mode", word_of(menu.mode)}}), "ExplorerMenu");
    if (environment.notify) environment.notify();
    else SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool write_preference_file(const Menu &menu, const QString &path) {
    // One line per field, so none may hold a line break.
    const QString title = QString(menu.title).replace(QLatin1Char('\r'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
    const QByteArray content = (word_of(menu.mode) + QLatin1Char('\n') + title + QLatin1Char('\n') + QFileInfo(menu.exe_path).fileName() + QLatin1Char('\n')).toUtf8();
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) return true;
    existing.close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size() && file.commit();
}

QString preference_file_path() {
    if (!is_packaged()) return {};
    UINT32 length = PACKAGE_FAMILY_NAME_MAX_LENGTH + 1;
    std::wstring family(length, L'\0');
    if (GetCurrentPackageFamilyName(&length, family.data()) != ERROR_SUCCESS) return {};
    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) return {};
    return QDir(base).filePath(QStringLiteral("Packages/%1/LocalState/%2").arg(QString::fromWCharArray(family.c_str()), QString::fromWCharArray(PREFERENCE_FILE)));
}

void apply(const Menu &menu, const Environment &environment) {
    if (!is_packaged()) {
        reconcile_registry(menu, environment);
        return;
    }
    const QString path = environment.preference_path.isEmpty() ? preference_file_path() : environment.preference_path;
    if (path.isEmpty() || !write_preference_file(menu, path)) log_warning(T("无法写入资源管理器菜单的偏好文件"), "ExplorerMenu");
}

void sync(const ToolSettingsManager &settings, const Environment &environment) {
    apply({mode_from_word(settings.get_explorer_context_menu()), menu_title(), QDir::toNativeSeparators(QCoreApplication::applicationFilePath())}, environment);
}
}
