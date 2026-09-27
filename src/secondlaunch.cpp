// secondlaunch.cpp -- clicking PlayOnline while it is already running shows you where
// it is, instead of quitting silently.
//
// pol.exe allows one instance. The second one finds the first and exits, which is fine
// while the Viewer is on screen, but while a game runs the Viewer's own window is hidden,
// so the second launch vanishes with no feedback and reads like a stuck process.

#include "polshim.h"
#include <tlhelp32.h>

// --- second-instance feedback ------------------------------------------------
//
// WHY THIS EXISTS (measured 2026-08-19)
//
// The guard described above is CreateMutexW(L"PlayOnlineUS") + ERROR_ALREADY_EXISTS,
// and on that branch pol.exe is supposed to FindWindow the prior instance, activate
// it, and exit. That works while the VIEWER is the thing on screen. It does nothing
// visible once a TITLE is running: the Viewer's own PlayOnlineUS window is HIDDEN for
// the duration of a game, so the window pol.exe finds and "activates" is invisible and
// the launch dies with no feedback whatsoever.
//
// Measured: with Tetra Master up in a 640x480 window at (0,0), three launches (16:52,
// 16:53, 16:56) each initialised the shim fully and then quit silently. From the user's
// seat that is indistinguishable from a ghost process holding the mutex -- click
// PlayOnline, nothing happens -- which is exactly why the two were conflated for so
// long. The ghost was a real and separate bug (see log_begin_teardown in log.cpp); this
// is the OTHER half of the same symptom.
//
// WHAT THIS DOES
//
// Find the other instance's real on-screen window -- the GAME window when a title is
// up, the Viewer window otherwise -- restore it if minimised and bring it to the front.
// pol.exe still exits exactly as it did before; single-instance behaviour is unchanged.
// The only difference is that the user is shown where their session actually went.
//
// [multi] focus_existing=0 turns it off.

static int     g_focus_existing = 1;
static HWND    g_focus_found    = NULL;
static wchar_t g_self_exe[MAX_PATH];

// Sibling pids, collected ONCE before the window walk. Doing this per-window (an
// OpenProcess for every top-level window on the desktop) lost the race with pol.exe's
// exit -- the scan simply never finished. One snapshot is a few milliseconds.
static DWORD g_sibs[64];
static int   g_nsibs = 0;

static void focus_collect_siblings()
{
    g_nsibs = 0;
    const wchar_t* self_leaf = wcsrchr(g_self_exe, L'\\');
    self_leaf = self_leaf ? self_leaf + 1 : g_self_exe;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    DWORD self = GetCurrentProcessId();
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) continue;
            if (lstrcmpiW(pe.szExeFile, self_leaf) != 0) continue;
            if (g_nsibs < (int)(sizeof(g_sibs) / sizeof(g_sibs[0])))
                g_sibs[g_nsibs++] = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static bool focus_is_sibling(DWORD pid)
{
    for (int i = 0; i < g_nsibs; i++) if (g_sibs[i] == pid) return true;
    return false;
}

static BOOL CALLBACK focus_enum(HWND h, LPARAM)
{
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;                              // owned popups, IME
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;   // PlayOnlineMask*
    // The mask is TOOLWINDOW only once the other instance's mask guard has armed; before
    // that it is a plain full-screen topmost popup, and raising it would cover the desktop.
    // The shim's own backdrop is never the window a player means either.
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (!_wcsnicmp(cls, L"PlayOnlineMask", 14) || !lstrcmpiW(cls, L"PolShimBackdrop")) return TRUE;
    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    if (r.right - r.left < 64 || r.bottom - r.top < 64) return TRUE;      // 0x0 stubs
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!focus_is_sibling(pid)) return TRUE;
    g_focus_found = h;
    return FALSE;
}

// Named _thread for historical reasons only -- it is called INLINE now; see the
// note in multi_focus_existing_start about the 0.15 s a second instance gets.
static DWORD WINAPI focus_thread(void*)
{
    logf("[multi] focus: checking for an existing instance"); log_flush();
    focus_collect_siblings();
    logf("[multi] focus: %d other instance(s) of this exe", g_nsibs); log_flush();
    if (g_nsibs == 0) {
        logf("[multi] second-instance check: no other %S process -- first instance.",
             wcsrchr(g_self_exe, L'\\') ? wcsrchr(g_self_exe, L'\\') + 1 : g_self_exe);
        log_flush();
        return 0;
    }
    g_focus_found = NULL;
    EnumWindows(focus_enum, 0);
    logf("[multi] focus: window scan done, target hwnd=%p", g_focus_found); log_flush();
    if (!g_focus_found) {
        // Siblings exist but not one of them has a visible window. That is precisely
        // the ghost signature, and it is worth saying so loudly: this launch is about
        // to die on a mutex held by a process the user cannot see or click.
        logf("[multi] %d other instance(s) alive but NONE has a visible window -- this "
             "is the ghost case. This launch will be refused by a process you cannot "
             "see. Check whether that pid's log reached '==== polshim summary ===='.",
             g_nsibs);
        log_flush();
        return 0;
    }

    wchar_t title[256] = L"";
    GetWindowTextW(g_focus_found, title, 256);
    char t8[512] = "";
    WideCharToMultiByte(CP_UTF8, 0, title, -1, t8, sizeof(t8), NULL, NULL);
    char cls[128] = "";
    GetClassNameA(g_focus_found, cls, sizeof(cls));

    // ASYNC on purpose. This runs inline under the loader lock (see the note in
    // multi_focus_existing_start), so every touch of the OTHER process's window must
    // be non-blocking: a synchronous ShowWindow/BringWindowToTop can send an
    // inter-thread message to the existing Viewer's UI thread and block there, and
    // blocking under the loader lock is the deadlock this whole module exists to cure.
    // ShowWindowAsync posts, and SWP_ASYNCWINDOWPOS posts instead of sending -- the
    // same cross-thread pattern maskguard uses. SetForegroundWindow is internally
    // bounded by the foreground-lock timeout (it cannot wedge), and FlashWindow is
    // non-blocking; both stay.
    if (IsIconic(g_focus_found)) ShowWindowAsync(g_focus_found, SW_RESTORE);
    SetWindowPos(g_focus_found, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    BOOL fg = SetForegroundWindow(g_focus_found);
    // Windows refuses a foreground steal unless the caller owns it. A freshly launched
    // process usually does, but when it does not, flashing the taskbar button is still
    // vastly better than the silent death this replaces.
    if (!fg) FlashWindow(g_focus_found, TRUE);

    logf("[multi] PlayOnline is ALREADY RUNNING -- brought its window to the front "
         "instead of quitting silently. hwnd=%p class='%s' title='%s' foreground=%s. "
         "This launch still exits: that is pol.exe's own single-instance rule, not ours.",
         g_focus_found, cls, t8, fg ? "yes" : "no (flashed instead)");
    log_flush();
    return 0;
}

void multi_focus_existing_start(const wchar_t* ini)
{
    g_focus_existing = GetPrivateProfileIntW(L"multi", L"focus_existing", 1, ini);
    logf("[multi] focus_existing=%d -- second-instance feedback %s",
         g_focus_existing, g_focus_existing ? "ARMED" : "off");
    log_flush();
    if (!g_focus_existing) return;
    GetModuleFileNameW(NULL, g_self_exe, MAX_PATH);
    // SYNCHRONOUS, and that is the whole point. This first ran on a thread, which lost
    // the race every time: a second instance lives **0.15 s** (measured) -- pol.exe
    // hits its single-instance mutex check and calls ExitProcess before a freshly
    // created thread can even finish a process snapshot. The thread reached "entered"
    // and was killed mid-scan on every run.
    //
    // DllMain runs BEFORE pol.exe's WinMain, so doing it inline here completes before
    // the mutex is ever checked. The calls used are deliberately ones that do not send
    // messages into another process while we hold the loader lock -- a ToolHelp
    // snapshot, then EnumWindows with IsWindowVisible/GetWindowLong/
    // GetWindowThreadProcessId. Only the final activate touches another process, and by
    // then we are simply asking the shell to raise a window that already exists.
    focus_thread(NULL);
}
