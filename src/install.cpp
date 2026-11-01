// install.cpp: the startup gate and the single entry point for every patch.
//
// Nothing is touched unless this is a MediaTek MT7922-class machine (the BT
// filter driver's device object exists).  Everywhere else the DLL stays a pure
// winmm pass-through, which is what makes it safe to drop next to PC Manager.
//
// Two different scopes, on purpose:
//
//   * the Realtek advertising hooks are *code* patches aimed at one binary, so
//     they stay restricted to HiConnectivityService.exe.
//
//   * the WLAN switch-state fix is a plain IAT hook, and the process that
//     actually performs the query is not HiConnectivityService at all:
//     HardwareHal.dll owns WlanQueryInterface, and HardwareHal.dll is loaded by
//     PCManager.exe / HwDistributedMainService.exe / MateBookService.exe.  The
//     state reaches the UI over IPC (ModelIPCReader::OnWifiStateChanged), so the
//     hook has to exist in those processes or it never fires.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "log.h"
#include "mtk_hci.h"
#include "patches.h"

static const char* ProcessName(void) {
    static char path[MAX_PATH] = {0};
    static const char* base = path;
    if (!path[0]) {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        base = path;
        for (const char* q = path; *q; ++q) {
            if (*q == '\\') base = q + 1;
        }
    }
    return base;
}

static bool IsHiConnectivityService(void) {
    return lstrcmpiA(ProcessName(), "HiConnectivityService.exe") == 0;
}

// Everything that can call WlanQueryInterface, either directly or through
// HardwareHal.dll.  Keep this list in sync with the modules that reference
// HardwareHal.dll in the PC Manager install directory.
static bool IsTargetProcess(void) {
    static const char* const kAllowed[] = {
        "HiConnectivityService.exe",
        "PCManager.exe",
        "HwDistributedMainService.exe",
        "MateBookService.exe",
        "MBAMessageCenter.exe",
        "HwMdcCenter.exe",
    };
    const char* name = ProcessName();
    for (const char* const candidate : kAllowed) {
        if (lstrcmpiA(name, candidate) == 0) return true;
    }
    return false;
}

// Called once from the worker thread that src/proxy.cpp starts in DllMain.
extern "C" void MtkPatchInstall(void) {
    if (!mtk::Present()) {
        shimlog::Line("patch", "no \\\\.\\MTKBTFilter: nothing patched");
        return;
    }

    const bool hi = IsHiConnectivityService();
    if (!IsTargetProcess()) {
        shimlog::Line("patch", "not a target process: nothing patched");
        return;
    }

    if (hi) {
        InstallRealtekHooks();
    }
    InstallWlanFix();
}
