// IExplorerCommand for the packaged build: "pin to screen" in the first level of the Windows 11
// context menu. Loaded by the package's COM surrogate; see explorer_menu_contract.h for what it
// shares with the application.
#include "explorer_command_core.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <atomic>
#include <new>

namespace {
using namespace jietuba::explorer_command;
using jietuba::explorer_menu::Mode;

HMODULE g_module = nullptr;
std::atomic<ULONG> g_object_count{0};
std::atomic<ULONG> g_lock_count{0};

std::optional<Preference> current_preference() {
    wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1] = {};
    UINT32 length = static_cast<UINT32>(std::size(family));
    if (GetCurrentPackageFamilyName(&length, family) != ERROR_SUCCESS)
        return std::nullopt;
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)))
        return std::nullopt;
    const std::wstring path = preference_path(local, family);
    CoTaskMemFree(local);
    return read_preference_file(path);
}

// The executable sits in the DLL's folder.
std::wstring executable_path(const Preference &preference) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size = GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0)
            return {};
        if (size < path.size()) {
            path.resize(size);
            break;
        }
        if (path.size() >= MAX_COMMAND_LINE)
            return {};
        path.resize(path.size() * 2);
    }
    const std::size_t separator = path.find_last_of(L'\\');
    if (separator == std::wstring::npos)
        return {};
    path.resize(separator + 1);
    path.append(preference.exe_name);
    return path;
}

// The file-system paths of the selected items the mode pins; with `first_only`, stops at the first.
std::vector<std::wstring> wanted_paths(IShellItemArray *items, Mode mode, bool first_only) {
    std::vector<std::wstring> paths;
    DWORD count = 0;
    if (!items || FAILED(items->GetCount(&count)))
        return paths;
    for (DWORD i = 0; i < count; ++i) {
        IShellItem *item = nullptr;
        if (FAILED(items->GetItemAt(i, &item)) || !item)
            continue;
        SFGAOF attributes = 0;
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetAttributes(SFGAO_FILESYSTEM | SFGAO_FOLDER, &attributes)) && (attributes & SFGAO_FILESYSTEM) &&
            !(attributes & SFGAO_FOLDER) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
            if (wants_path(name, mode))
                paths.emplace_back(name);
        }
        CoTaskMemFree(name);
        item->Release();
        if (first_only && !paths.empty())
            break;
    }
    return paths;
}

HRESULT duplicate(const std::wstring &text, PWSTR *out) {
    *out = nullptr;
    return SHStrDupW(text.c_str(), out);
}

class PinCommand final : public IExplorerCommand {
  public:
    explicit PinCommand(const CLSID &clsid) : clsid_(clsid) { ++g_object_count; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IExplorerCommand)
            return E_NOINTERFACE;
        *object = static_cast<IExplorerCommand *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetTitle(IShellItemArray *, PWSTR *title) override {
        if (!title)
            return E_POINTER;
        *title = nullptr;
        const auto preference = current_preference();
        return preference ? duplicate(preference->title, title) : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE GetIcon(IShellItemArray *, PWSTR *icon) override {
        if (!icon)
            return E_POINTER;
        *icon = nullptr;
        const auto preference = current_preference();
        if (!preference)
            return E_FAIL;
        const std::wstring exe = executable_path(*preference);
        if (exe.empty())
            return E_FAIL;
        std::wstring text;
        append_quoted(exe, text);
        text.append(L",0");
        return duplicate(text, icon);
    }

    HRESULT STDMETHODCALLTYPE GetToolTip(IShellItemArray *, PWSTR *tooltip) override {
        if (tooltip)
            *tooltip = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalName(GUID *name) override {
        if (!name)
            return E_POINTER;
        *name = clsid_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetState(IShellItemArray *items, BOOL, EXPCMDSTATE *state) override {
        if (!state)
            return E_POINTER;
        *state = ECS_HIDDEN;
        const auto preference = current_preference();
        if (preference && preference->mode != Mode::Off && !wanted_paths(items, preference->mode, true).empty())
            *state = ECS_ENABLED;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Invoke(IShellItemArray *items, IBindCtx *) override {
        const auto preference = current_preference();
        if (!preference || preference->mode == Mode::Off)
            return S_OK;
        const std::vector<std::wstring> files = wanted_paths(items, preference->mode, false);
        const std::wstring exe = executable_path(*preference);
        if (files.empty() || exe.empty())
            return S_OK;
        CommandLine line = build_command_line(exe, files);
        if (line.text.empty())
            return S_OK;
        if (line.included < files.size())
            OutputDebugStringW(L"jietuba explorer command: selection too long, pinning the first files only\n");
        // The new process shows its pins; let it take the foreground from this surrogate.
        AllowSetForegroundWindow(ASFW_ANY);
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(exe.c_str(), line.text.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFlags(EXPCMDFLAGS *flags) override {
        if (!flags)
            return E_POINTER;
        *flags = ECF_DEFAULT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumSubCommands(IEnumExplorerCommand **commands) override {
        if (commands)
            *commands = nullptr;
        return E_NOTIMPL;
    }

  private:
    ~PinCommand() { --g_object_count; }

    std::atomic<ULONG> references_{1};
    CLSID clsid_;
};

class Factory final : public IClassFactory {
  public:
    explicit Factory(const CLSID &clsid) : clsid_(clsid) { ++g_object_count; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory)
            return E_NOINTERFACE;
        *object = static_cast<IClassFactory *>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (outer)
            return CLASS_E_NOAGGREGATION;
        PinCommand *command = new (std::nothrow) PinCommand(clsid_);
        if (!command)
            return E_OUTOFMEMORY;
        const HRESULT result = command->QueryInterface(iid, object);
        command->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock)
            ++g_lock_count;
        else
            --g_lock_count;
        return S_OK;
    }

  private:
    ~Factory() { --g_object_count; }

    std::atomic<ULONG> references_{1};
    CLSID clsid_;
};
}  // namespace

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    *object = nullptr;
    CLSID lite = {};
    CLSID full = {};
    CLSIDFromString(COMMAND_CLSID_LITE, &lite);
    CLSIDFromString(COMMAND_CLSID_FULL, &full);
    if (clsid != lite && clsid != full)
        return CLASS_E_CLASSNOTAVAILABLE;
    Factory *factory = new (std::nothrow) Factory(clsid);
    if (!factory)
        return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release();
    return result;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return g_object_count == 0 && g_lock_count == 0 ? S_OK : S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
