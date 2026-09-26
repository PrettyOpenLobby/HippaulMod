// gamestart.cpp -- hand a title its command-line switch at launch (Fantasy
// Earth's -windowmode).
//
// pol.exe starts a content title by calling ONE method on its content object,
// GameStart, at vtable slot 3:
//
//     0x4142a9  mov  eax, [esi+0x248]
//     0x4142af  lea  ecx, [ebp+0x20]
//     0x4142b2  push ecx              ; 3rd param
//     0x4142b3  mov  ecx, [edx+0x0c]  ; vtable slot 3 == GameStart
//     0x4142b6  push eax              ; 2nd param
//     0x4142ba  push eax              ; this
//     0x4142bb  call ecx
//     0x4142bd  test eax, eax
//     0x4142bf  je   0x41440b         ; ZERO == success
//
// Three pushed dwords and no esp fixup after the call, so it is __stdcall with
// (this, a1, a2), which is what makes a plain C++ wrapper safe here.
//
// A POL content title does NOT read GetCommandLine(). It receives its command
// line from the shell, through the polcore interface it is handed at GameStart:
//
//     GameStart(self=content_obj, a1=pPolCore, a2=pParam)
//         core = a1->QueryInterface(*(IID*)(a2+8))         // IID_IPOLCoreCom
//         core->GetlpCmdLine(&lpCmdLine)                   // vtable SLOT 4
//         WinMain(hInst, 0, lpCmdLine, 0)
//
// Measured in FE_Client.dll: GameStart 0x53EA4B0, the QI at 0x53EA4FE, the
// slot-4 call at 0x53EA5E7, WinMain at 0x53EA616. So whatever GetlpCmdLine
// returns becomes the title's command line, and FE's WinMain accepts
// `-windowmode`: SE's own windowed mode, with FE's own window, frame and pointer
// handling. Without it FE runs its FULLSCREEN input code (a pointer driven from
// DirectInput deltas, SetCursorPos and ClipCursor) inside a window the shim has
// forced, and that pointer does not follow the one on screen.
//
// Only a title whose profile carries a switch is hooked at all. Front Mission
// Online, FFXI and Tetra Master reach GameStart too and are left untouched.
#include "polshim.h"
#include "profiles.h"
#include <objbase.h>          // IUnknown -- the polcore object we patch GetlpCmdLine on
#include <intrin.h>           // _ReturnAddress -- names the title that called GetlpCmdLine

static int g_cmdline = 1;   // [polshim] profile_cmdline -- inject a title's -window switch

typedef HRESULT (__stdcall *PFN_GAMESTART)(void* self, void* a1, void* a2);
static PFN_GAMESTART g_real_gamestart = NULL;
// Non-zero while a watched GameStart is on the stack. Guards the re-arm in
// gamestart_watch(): a second title may be hooked only once the first call has
// returned, since g_real_gamestart is one global trampoline.
static volatile LONG g_in_flight = 0;

void gamestart_set_cmdline(int on) { g_cmdline = on ? 1 : 0; }

typedef HRESULT (__stdcall *PFN_GETCMDLINE)(void* self, LPSTR* out);
static PFN_GETCMDLINE g_real_getcmdline = NULL;
static void** g_cmdline_vtbl = NULL;         // the vtable we patched (to restore it)
static void*  g_cmdline_orig = NULL;         // its original slot-4 pointer
static IUnknown* g_cmdline_ref = NULL;       // the QI'd interface, HELD while patched
                                             // (releasing it early can free the very
                                             //  vtable we patched -- a tear-off with
                                             //  refcount 1 -> use-after-free on disarm)
static char   g_cmdline_buf[1024];           // storage the title reads its line from
static LONG   g_cmdline_said = 0;

// Set once a native-windowed title (Fantasy Earth) has actually been handed its
// -windowmode switch, and WHICH module got it. d3d8hook reads it: such a title
// is left to window itself only if the switch was truly delivered, and otherwise
// the shim forces it windowed rather than leave it in the exclusive-fullscreen
// mode it crashes in. Keyed by module, because a bare flag delivered to FE would
// still read "delivered" while another title ran, and it gates the mouse
// coordinate translation.
static volatile LONG g_nw_delivered = 0;
static char g_nw_leaf[80] = "";

static HRESULT __stdcall hook_GetlpCmdLine(void* self, LPSTR* out)
{
    HRESULT hr = g_real_getcmdline ? g_real_getcmdline(self, out)
                                   : E_UNEXPECTED;
    if (FAILED(hr) || !out) return hr;

    // Who is asking? The title's module names it; anyone else is left untouched.
    const TitleProfile* p = profile_for_addr(_ReturnAddress());
    if (!p || !p->cmdline_append || !p->cmdline_append[0]) return hr;
    // display=fullscreen for a native-windowed title means ITS OWN fullscreen: do
    // not hand it the windowed switch.
    if (display_mode_for(p->module, NULL) == DM_FULLSCREEN) {
        logf("[gs] cmdline for %s: display=fullscreen, so \"%s\" is NOT appended -- the "
             "title runs in its own fullscreen mode", p->title, p->cmdline_append);
        return hr;
    }

    const char* orig = *out ? *out : "";
    // Idempotent: if the switch is already present, do not add a second copy.
    // Case-insensitive substring, written out to avoid a shlwapi dependency.
    bool present = false;
    for (const char* s = orig; *s && !present; s++) {
        const char* a = s; const char* b = p->cmdline_append;
        while (*a && *b && (char)tolower((unsigned char)*a) == (char)tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) present = true;
    }
    if (present) return hr;

    _snprintf_s(g_cmdline_buf, sizeof(g_cmdline_buf), _TRUNCATE,
                "%s%s%s", orig, (*orig ? " " : ""), p->cmdline_append);
    *out = g_cmdline_buf;

    // Only a windowing switch counts as "this title windows itself".
    if (p->window == PW_NATIVE_WINDOWED) {
        InterlockedExchange(&g_nw_delivered, 1);
        strncpy_s(g_nw_leaf, sizeof(g_nw_leaf), p->module, _TRUNCATE);
    }

    if (InterlockedCompareExchange(&g_cmdline_said, 1, 0) == 0)
        logf("[gs] cmdline for %s: appended \"%s\" -> \"%s\" (%s)",
             p->title, p->cmdline_append, g_cmdline_buf, p->note);
    return hr;
}

int gamestart_nw_delivered() { return g_nw_delivered ? 1 : 0; }

// Was the windowing switch delivered TO THIS MODULE? A NULL/empty leaf means "no
// title resolved", which must answer NO: callers use this to STAND OFF, and
// standing off for an unidentified caller is the unsafe default.
int gamestart_nw_delivered_for(const char* leaf)
{
    if (!leaf || !*leaf || !g_nw_leaf[0]) return 0;
    return (_stricmp(leaf, g_nw_leaf) == 0 && g_nw_delivered) ? 1 : 0;
}

// Arm the GetlpCmdLine hook on the polcore object `a1` for the title about to
// launch. `a2` is pParam; the IID the title will QI for is at pParam+8. Returns
// true if it patched a vtable slot that must be restored on GameStart return.
static bool cmdline_arm(void* a1, void* a2, const TitleProfile* launching)
{
    if (!g_cmdline || !a1) return false;
    if (!launching || !launching->cmdline_append || !launching->cmdline_append[0])
        return false;
    if (g_cmdline_vtbl) return false;                 // already armed this call

    // Do exactly what the title does: QI a1 for the IID at pParam+8, and patch the
    // object it hands back, because THAT is the vtable the title's slot-4 call reaches.
    IUnknown* core = (IUnknown*)a1;
    void* target = NULL;
    GUID iid;
    bool have_iid = a2 && !IsBadReadPtr((char*)a2 + 8, sizeof(GUID));
    if (have_iid) {
        memcpy(&iid, (char*)a2 + 8, sizeof(GUID));
        if (IsBadReadPtr(core, sizeof(void*)) ||
            FAILED(core->QueryInterface(iid, &target)) || !target)
            target = NULL;
    }
    // Fallback: patch a1's own vtable (same slot in practice for IPOLCoreCom).
    if (!target) target = a1;

    void** vtbl = *(void***)target;
    if (IsBadReadPtr(vtbl, 5 * sizeof(void*))) {
        if (target != a1) ((IUnknown*)target)->Release();
        return false;
    }
    void* slot4 = vtbl[4];                             // IPOLCoreCom::GetlpCmdLine
    if (!slot4 || IsBadCodePtr((FARPROC)slot4) || slot4 == (void*)hook_GetlpCmdLine) {
        if (target != a1) ((IUnknown*)target)->Release();
        return false;
    }

    DWORD old;
    if (!VirtualProtect(&vtbl[4], sizeof(void*), PAGE_READWRITE, &old)) {
        if (target != a1) ((IUnknown*)target)->Release();
        return false;
    }
    g_real_getcmdline = (PFN_GETCMDLINE)slot4;
    g_cmdline_orig    = slot4;
    g_cmdline_vtbl    = vtbl;
    vtbl[4] = (void*)hook_GetlpCmdLine;
    VirtualProtect(&vtbl[4], sizeof(void*), old, &old);

    // Hold the QI reference while patched; released in cmdline_disarm after the
    // slot is restored. (target == a1 means we never took a QI ref.)
    g_cmdline_ref = (target != a1) ? (IUnknown*)target : NULL;
    logf("[gs] cmdline hook armed on GetlpCmdLine (slot 4) vtable=%p", (void*)vtbl);
    return true;
}

static void cmdline_disarm(void)
{
    if (!g_cmdline_vtbl) return;
    DWORD old;
    if (VirtualProtect(&g_cmdline_vtbl[4], sizeof(void*), PAGE_READWRITE, &old)) {
        g_cmdline_vtbl[4] = g_cmdline_orig;
        VirtualProtect(&g_cmdline_vtbl[4], sizeof(void*), old, &old);
    }
    g_cmdline_vtbl = NULL;
    g_cmdline_orig = NULL;
    g_real_getcmdline = NULL;
    g_cmdline_said = 0;
    if (g_cmdline_ref) { g_cmdline_ref->Release(); g_cmdline_ref = NULL; }
}

static HRESULT __stdcall hook_GameStart(void* self, void* a1, void* a2)
{
    // GetlpCmdLine is called synchronously by the title INSIDE g_real_gamestart,
    // before its WinMain, so arming here and disarming on return scopes the change
    // to exactly this title's launch. The launching title is resolved from its OWN
    // GameStart address.
    const TitleProfile* launching = profile_for_addr((void*)g_real_gamestart);
    bool cmdline_armed = cmdline_arm(a1, a2, launching);

    DWORD t0 = GetTickCount();
    InterlockedIncrement(&g_in_flight);
    // The pad belongs to the TITLE for the length of this call (each title carries
    // its own button config).
    padmap_note_title(1);
    HRESULT hr = g_real_gamestart(self, a1, a2);
    padmap_note_title(0);
    InterlockedDecrement(&g_in_flight);
    if (cmdline_armed) cmdline_disarm();

    logf("[gs] GameStart returned 0x%08lX after %lu ms", (unsigned long)hr,
         (unsigned long)(GetTickCount() - t0));
    log_flush();
    return hr;
}

void gamestart_watch(void* iface, const char* what)
{
    if (!iface || !g_cmdline) return;

    // Only a title with a switch to receive is hooked. Read this content object's
    // GameStart (vtable slot 3) and look up its profile; FMO, FFXI and TM have no
    // switch and their GameStart is never touched.
    void** vtbl = *(void***)iface;
    if (IsBadReadPtr(vtbl, 4 * sizeof(void*)) || !vtbl[3]) return;
    const TitleProfile* prof = profile_for_addr(vtbl[3]);
    if (!prof || !prof->cmdline_append || !prof->cmdline_append[0]) return;

    // Refuse only while a previous hooked call is still on the stack:
    // g_real_gamestart is a single global and that call would return through the
    // wrong pointer. pol.exe calls GameStart exactly once per launch.
    if (g_real_gamestart && InterlockedCompareExchange(&g_in_flight, 0, 0) != 0) {
        logf("[gs] %s: a hooked GameStart is still running -- not re-arming", what);
        return;
    }

    void* slot3 = vtbl[3];
    if (IsBadCodePtr((FARPROC)slot3)) return;
    // ALREADY OURS? Then stop. Arming twice on one vtable would store the hook as
    // the "original" and make it call itself forever. comtrace reaches this from
    // two sites, so a title arriving twice is ordinary.
    if (slot3 == (void*)hook_GameStart) return;

    DWORD old;
    if (!VirtualProtect(&vtbl[3], sizeof(void*), PAGE_READWRITE, &old)) {
        logf("[gs] %s: cannot unprotect vtable slot 3 (err=%lu)", what, GetLastError());
        return;
    }
    g_real_gamestart = (PFN_GAMESTART)slot3;
    vtbl[3] = (void*)hook_GameStart;
    VirtualProtect(&vtbl[3], sizeof(void*), old, &old);

    // A new title is starting: the per-title delivery latch belongs to it now.
    InterlockedExchange(&g_nw_delivered, 0);
    g_nw_leaf[0] = 0;
    d3d8_new_title_launch();

    logf("[gs] %s (%s): GameStart hooked to deliver \"%s\"", prof->title, what,
         prof->cmdline_append);
    log_flush();
}
