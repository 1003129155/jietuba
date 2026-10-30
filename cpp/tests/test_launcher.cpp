// ctest starts every test through this. Windows' critical-error dialogs are switched off first, so a
// DLL missing from the test or from anything it starts ends that process with 0xC0000135 instead of
// a system dialog that stays on the user's screen after the test is gone. Children inherit the mode.
// No Qt: it must start even when the Qt DLLs are what is missing.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
// The command line after this program's own path, with the test's quoting intact.
wchar_t *test_command_line() {
    wchar_t *line = GetCommandLineW();
    if (*line == L'"') {
        ++line;
        while (*line && *line != L'"') ++line;
        if (*line) ++line;
    } else {
        while (*line && *line != L' ' && *line != L'\t') ++line;
    }
    while (*line == L' ' || *line == L'\t') ++line;
    return line;
}
}

int wmain() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    wchar_t *command = test_command_line();
    if (!*command) return 2;
    // ctest ends a timed-out test by killing this process; the job takes the test down with it.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command, nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) return 3;
    if (job) AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
