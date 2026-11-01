#pragma once
// Minimal file logger shared by the shim modules.
//
// Most PC Manager processes run elevated (or as SYSTEM) and can write to
// ProgramData.  The UI processes -- HwMdcUI.exe, MessageCenterUI.exe,
// PCManager.exe -- run as the logged-in user and are *denied* writes to a
// ProgramData folder that a SYSTEM service created, which is why the first
// version produced no evidence at all for exactly the processes that decide the
// WLAN state.  Fall back to %LOCALAPPDATA%, then %TEMP%, so the UI side logs too.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

namespace shimlog {

static const wchar_t* kProgDataDir = L"C:\\ProgramData\\MTK-Patch";
static const wchar_t* kFileName    = L"mt7922_hci.log";

static HANDLE OpenLog(const wchar_t* dir) {
    if (!dir || !dir[0]) return INVALID_HANDLE_VALUE;

    wchar_t path[MAX_PATH];
    if (_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, kFileName) <= 0) {
        return INVALID_HANDLE_VALUE;
    }

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        CreateDirectoryW(dir, NULL);
        h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return h;
}

inline HANDLE LogHandle(void) {
    HANDLE h = OpenLog(kProgDataDir);
    if (h != INVALID_HANDLE_VALUE) return h;

    wchar_t dir[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t sub[MAX_PATH];
        if (_snwprintf_s(sub, MAX_PATH, _TRUNCATE, L"%s\\MTK-Patch", dir) > 0) {
            h = OpenLog(sub);
            if (h != INVALID_HANDLE_VALUE) return h;
        }
    }

    n = GetEnvironmentVariableW(L"TEMP", dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t sub[MAX_PATH];
        if (_snwprintf_s(sub, MAX_PATH, _TRUNCATE, L"%s\\MTK-Patch", dir) > 0) {
            return OpenLog(sub);
        }
    }
    return INVALID_HANDLE_VALUE;
}

inline void Line(const char* tag, const char* fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char* base = exe;
    for (const char* q = exe; *q; ++q) {
        if (*q == '\\') base = q + 1;
    }

    char line[1024];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[pid=%lu %s] [%s] %s\r\n",
                GetCurrentProcessId(), base, tag, msg);

    HANDLE h = LogHandle();
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD written = 0;
    WriteFile(h, line, (DWORD)lstrlenA(line), &written, NULL);
    CloseHandle(h);
}

}  // namespace shimlog
