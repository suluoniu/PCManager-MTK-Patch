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
//     actually performs the query is not HiConnectivityService at all.
//     HardwareHal.dll owns WlanQueryInterface and is loaded by HwMdcUI.exe,
//     PCManager.exe, HwDistributedMainService.exe, MateBookService.exe,
//     MBAMessageCenter.exe and HwMdcCenter.exe.  HwMdcUI.exe is the one that
//     turns the result into "NO_WIFI" / TOOL_STATE_CLOSED in MdcCore, so it has
//     to be covered too -- in 14.0.7.310 the name list missed it and the fix
//     therefore never reached the screen.
//
// Instead of maintaining a name list that goes stale with every PC Manager
// release, the gate is now "this process was started from the directory the
// shim itself lives in".  Every other application on the machine keeps the
// plain pass-through behaviour.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "log.h"
#include "mtk_hci.h"
#include "patches.h"

// Directory part of a full path, written in place.
static void StripFileName(char* path) {
    char* slash = NULL;
    for (char* q = path; *q; ++q) {
        if (*q == '\\') slash = q;
    }
    if (slash) *slash = 0;
}

static const char* ProcessPath(void) {
    static char path[MAX_PATH] = {0};
    if (!path[0]) GetModuleFileNameA(NULL, path, MAX_PATH);
    return path;
}

static const char* ProcessName(void) {
    const char* path = ProcessPath();
    const char* base = path;
    for (const char* q = path; *q; ++q) {
        if (*q == '\\') base = q + 1;
    }
    return base;
}

static bool IsHiConnectivityService(void) {
    return lstrcmpiA(ProcessName(), "HiConnectivityService.exe") == 0;
}

// True when the running executable sits in the same directory as this DLL.
static bool IsTargetProcess(HMODULE self) {
    char shimDir[MAX_PATH] = {0};
    if (!GetModuleFileNameA(self, shimDir, MAX_PATH)) return false;
    StripFileName(shimDir);

    char exeDir[MAX_PATH] = {0};
    lstrcpynA(exeDir, ProcessPath(), MAX_PATH);
    StripFileName(exeDir);

    return lstrcmpiA(exeDir, shimDir) == 0;
}

// Called once from the worker thread that src/proxy.cpp starts in DllMain.
extern "C" void MtkPatchInstall(HMODULE self) {
    // Log the decision for every process.  When a fix does not fire, the first
    // question is always "did this process get past the gate", and without this
    // line the answer is invisible.  The build tag makes it obvious which DLL a
    // log line came from when several revisions have been installed in a row.
    const bool target = IsTargetProcess(self);
    const bool hi = IsHiConnectivityService();
    shimlog::Line("gate", "%s target=%d hi=%d build=v3", ProcessName(), (int)target, (int)hi);

    if (!mtk::Present()) {
        shimlog::Line("patch", "no \\\\.\\MTKBTFilter: nothing patched");
        return;
    }
    if (!target) {
        shimlog::Line("patch", "not a PC Manager process: nothing patched");
        return;
    }

    if (hi) {
        InstallRealtekHooks();
    }
    InstallWlanFix();
}
