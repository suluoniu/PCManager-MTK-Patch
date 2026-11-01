// wlan_fix.cpp: repair the WLAN switch-state query.
//
// Measured problem: the process that decides "WLAN is on/off" calls
// WlanQueryInterface and gets ERROR_NOT_FOUND (1168), so PC Manager concludes the
// WLAN is off: the super-device panel shows "click to enable WLAN" while the WLAN
// is connected and the radio is on.  The identical query from another process
// succeeds, so the API is fine -- the caller is handing WlanQueryInterface a wrong
// interface GUID (or a stale client handle).
//
// Measured detail that matters: the caller is *not* the host executable.  In
// PC Manager 14.0.7.310 the query comes from HardwareHal.dll, and HardwareHal.dll
// is loaded by PCManager.exe / HwDistributedMainService.exe / MateBookService.exe,
// never by HiConnectivityService.exe.  Hooking only the EXE's import table -- the
// first version of this file -- therefore installs a hook that is never called.
// The hook is now applied to every loaded module that imports the function, in
// every process src/install.cpp lets through, plus a watcher for modules that are
// loaded later on demand.
//
// The hook calls the real API first and only on ERROR_NOT_FOUND retries with:
//   1. the caller's handle and a GUID taken from WlanEnumInterfaces
//   2. a freshly opened client handle and a freshly enumerated GUID
// The caller then gets the data it asked for, or the original error.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wlanapi.h>

#include "log.h"
#include "patch_util.h"
#include "patches.h"

typedef DWORD (WINAPI* FnQuery)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD,
                                PVOID*, PWLAN_OPCODE_VALUE_TYPE);
typedef DWORD (WINAPI* FnEnum)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
typedef VOID  (WINAPI* FnFree)(PVOID);
typedef DWORD (WINAPI* FnOpen)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD (WINAPI* FnClose)(HANDLE, PVOID);
typedef BOOL  (WINAPI* FnEnumMods)(HANDLE, HMODULE*, DWORD, LPDWORD);
typedef DWORD (WINAPI* FnModName)(HANDLE, HMODULE, LPWSTR, DWORD);

static FnQuery g_query = NULL;
static FnEnum  g_enum  = NULL;
static FnFree  g_free  = NULL;
static FnOpen  g_open  = NULL;
static FnClose g_close = NULL;
static FnEnumMods g_enumMods = NULL;   // resolved from kernel32, no new import
static FnModName  g_modName  = NULL;

static GUID g_guid = {};
static bool g_haveGuid = false;
static volatile LONG g_repairs = 0;
static volatile LONG g_loggedFailure = 0;
static volatile LONG g_calls = 0;

// The first few calls are logged unconditionally.  Without this, "no log line"
// means either "the hook succeeded" or "the hook was never reached", and the two
// need opposite fixes.
static const LONG kLoggedCalls = 6;

// Modules already handled in this process.  PatchImport on an already patched
// slot would hand back our own thunk as "previous", so keep a list.
static const int kMaxModules = 512;
static HMODULE g_patched[kMaxModules] = {};
static volatile LONG g_patchedCount = 0;

static bool AlreadyPatched(HMODULE module) {
    const LONG count = g_patchedCount;
    for (LONG i = 0; i < count && i < kMaxModules; ++i) {
        if (g_patched[i] == module) return true;
    }
    return false;
}

static void RememberPatched(HMODULE module) {
    const LONG index = InterlockedIncrement(&g_patchedCount) - 1;
    if (index < kMaxModules) g_patched[index] = module;
}

static bool EnumerateGuid(HANDLE client) {
    if (g_haveGuid) return true;
    if (!g_enum || !client) return false;

    PWLAN_INTERFACE_INFO_LIST list = NULL;
    if (g_enum(client, NULL, &list) != ERROR_SUCCESS || !list) return false;
    if (list->dwNumberOfItems > 0) {
        g_guid = list->InterfaceInfo[0].InterfaceGuid;
        g_haveGuid = true;
    }
    if (g_free) g_free(list);
    return g_haveGuid;
}

static DWORD WINAPI HookQuery(HANDLE client, const GUID* guid, WLAN_INTF_OPCODE opcode,
                              PVOID reserved, PDWORD size, PVOID* data,
                              PWLAN_OPCODE_VALUE_TYPE type) {
    DWORD rc = g_query ? g_query(client, guid, opcode, reserved, size, data, type)
                       : ERROR_PROC_NOT_FOUND;

    const LONG call = InterlockedIncrement(&g_calls);
    if (call <= kLoggedCalls) {
        shimlog::Line("wlan", "query call=%ld opcode=%d rc=%lu", call, (int)opcode, rc);
    }

    if (rc == ERROR_SUCCESS) return rc;

    if (rc != ERROR_NOT_FOUND) {
        // The first version returned here silently, which made "wrong error code"
        // indistinguishable from "never called".  Log it once.
        if (InterlockedCompareExchange(&g_loggedFailure, 1, 0) == 0) {
            shimlog::Line("wlan", "switch-state query failed err=%lu (opcode=%d)",
                          rc, (int)opcode);
        }
        return rc;
    }

    // repair 1: the caller's handle, GUID from the interface list
    if (EnumerateGuid(client) && (guid == NULL || !IsEqualGUID(*guid, g_guid))) {
        DWORD retry = g_query(client, &g_guid, opcode, reserved, size, data, type);
        if (retry == ERROR_SUCCESS) {
            if (InterlockedIncrement(&g_repairs) == 1) {
                shimlog::Line("wlan", "repaired the switch-state query with the enumerated GUID");
            }
            return retry;
        }
        rc = retry;
    }

    // repair 2: our own client handle and a freshly enumerated GUID
    if (g_open && g_enum) {
        DWORD negotiated = 0;
        HANDLE fresh = NULL;
        if (g_open(2, NULL, &negotiated, &fresh) == ERROR_SUCCESS && fresh) {
            PWLAN_INTERFACE_INFO_LIST list = NULL;
            if (g_enum(fresh, NULL, &list) == ERROR_SUCCESS && list &&
                list->dwNumberOfItems > 0) {
                GUID freshGuid = list->InterfaceInfo[0].InterfaceGuid;
                DWORD retry = g_query(fresh, &freshGuid, opcode, reserved, size, data, type);
                if (retry == ERROR_SUCCESS) {
                    g_guid = freshGuid;
                    g_haveGuid = true;
                    if (InterlockedIncrement(&g_repairs) == 1) {
                        shimlog::Line("wlan", "repaired the switch-state query with a fresh handle");
                    }
                    if (g_free) g_free(list);
                    if (g_close) g_close(fresh, NULL);
                    return retry;
                }
                rc = retry;
            }
            if (list && g_free) g_free(list);
            if (g_close) g_close(fresh, NULL);
        }
    }

    if (InterlockedCompareExchange(&g_loggedFailure, 1, 0) == 0) {
        shimlog::Line("wlan", "switch-state query still fails (opcode=%d err=%lu)",
                      (int)opcode, rc);
    }
    return rc;
}

// Patch every module that imports wlanapi!WlanQueryInterface.  PatchImport scans
// the module's own import table, so modules that never mention wlanapi cost one
// NULL return and nothing else.
static int PatchEveryModule(void) {
    if (!g_enumMods) return 0;

    HMODULE stackBuffer[128] = {};
    DWORD needed = 0;
    if (!g_enumMods(GetCurrentProcess(), stackBuffer, (DWORD)sizeof(stackBuffer), &needed)) {
        return 0;
    }

    HMODULE* modules = stackBuffer;
    DWORD capacity = (DWORD)sizeof(stackBuffer);
    HMODULE* heapBuffer = NULL;

    if (needed > capacity) {
        capacity = needed + 128 * (DWORD)sizeof(HMODULE);
        heapBuffer = (HMODULE*)HeapAlloc(GetProcessHeap(), 0, capacity);
        if (!heapBuffer) return 0;
        DWORD needed2 = 0;
        if (!g_enumMods(GetCurrentProcess(), heapBuffer, capacity, &needed2)) {
            HeapFree(GetProcessHeap(), 0, heapBuffer);
            return 0;
        }
        modules = heapBuffer;
        needed = needed2;
    }

    const DWORD total = needed / (DWORD)sizeof(HMODULE);
    int hits = 0;

    for (DWORD i = 0; i < total; ++i) {
        HMODULE module = modules[i];
        if (!module || AlreadyPatched(module)) continue;

        void* previous = PatchImport(module, "wlanapi.dll", "WlanQueryInterface",
                                     (void*)HookQuery);
        if (!previous) continue;             // does not import it

        RememberPatched(module);
        if (previous == (void*)HookQuery) continue;   // already ours

        wchar_t name[MAX_PATH] = {};
        if (g_modName) g_modName(GetCurrentProcess(), module, name, MAX_PATH);
        shimlog::Line("wlan", "patched WlanQueryInterface in %ls", name);
        ++hits;
    }

    if (heapBuffer) HeapFree(GetProcessHeap(), 0, heapBuffer);
    return hits;
}

// HardwareHal.dll and friends can be pulled in by LoadLibrary well after start-up,
// so keep sweeping for a few minutes.  Costs one module enumeration every 3 s.
static DWORD WINAPI LateLoadWatch(LPVOID) {
    for (int i = 0; i < 120; ++i) {
        PatchEveryModule();
        Sleep(3000);
    }
    return 0;
}

void InstallWlanFix(void) {
    HMODULE api = GetModuleHandleW(L"wlanapi.dll");
    if (!api) api = LoadLibraryW(L"wlanapi.dll");
    if (!api) {
        shimlog::Line("wlan", "wlanapi not loadable: nothing patched");
        return;
    }

    g_query = (FnQuery)GetProcAddress(api, "WlanQueryInterface");
    g_enum  = (FnEnum) GetProcAddress(api, "WlanEnumInterfaces");
    g_free  = (FnFree) GetProcAddress(api, "WlanFreeMemory");
    g_open  = (FnOpen) GetProcAddress(api, "WlanOpenHandle");
    g_close = (FnClose)GetProcAddress(api, "WlanCloseHandle");
    if (!g_query) {
        shimlog::Line("wlan", "WlanQueryInterface not found: nothing patched");
        return;
    }

    // kernel32 exports the K32* variants since Windows 7; resolving them this way
    // keeps psapi out of the import table, the same trick used for wlanapi.
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (kernel32) {
        g_enumMods = (FnEnumMods)GetProcAddress(kernel32, "K32EnumProcessModules");
        g_modName  = (FnModName) GetProcAddress(kernel32, "K32GetModuleFileNameExW");
    }

    int hits = PatchEveryModule();
    if (hits == 0) {
        // Fallback for hosts where the module walk is unavailable: at least keep
        // the original EXE-only behaviour.
        HMODULE exe = GetModuleHandleA(NULL);
        if (PatchImport(exe, "wlanapi.dll", "WlanQueryInterface", (void*)HookQuery)) {
            shimlog::Line("wlan", "WlanQueryInterface hooked in the host EXE only");
            hits = 1;
        }
    }

    shimlog::Line("wlan", "WlanQueryInterface hooked in %d module(s) (real=%p)",
                  hits, (void*)g_query);

    HANDLE watcher = CreateThread(NULL, 0, LateLoadWatch, NULL, 0, NULL);
    if (watcher) CloseHandle(watcher);
}
