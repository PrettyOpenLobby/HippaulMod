// protondxvk.cpp -- keep Proton's Direct3D 8 on DXVK, and SAY which path we are on.
//
// WHY THIS FILE EXISTS, AND WHAT IT COST NOT TO HAVE IT
//
// Every POL title that matters is **Direct3D 8** -- Tetra Master, FFXI, Fantasy Earth
// -- and every stock Proton serves d3d8 through **wined3d (OpenGL)** unless
// `PROTON_DXVK_D3D8=1`. DXVK's d3d8 front-end (d8vk) ships inside that same Proton; it
// is merely opt-in. Front Mission Online is the one d3d9 title, which is why it always
// felt healthiest on a Deck while everything else dragged.
//
// The flag went live on the development Deck 2026-08-20, written by hand into
// `Proton 10.0/user_settings.py`. The title later moved to **Proton 11.0**, whose
// `user_settings.py` EXISTS (written 08-27) but does not carry the key -- so every d3d8
// title silently went back to wined3d for **eleven days**, and nothing anywhere said so.
// It surfaced only because a player asked why Tetra Master's room screen lagged, and it
// cost two published wrong causes on the way, both since retracted.
//
// IMPORTANT: THE KNOB IS PER-PROTON-INSTALL. Any Proton version change reverts it. `install.sh`
// now sets it on every Proton it can find, but an installer only runs when somebody runs
// it -- shim autoupdate ships PolHook.dll and has never touched the Linux side. So an
// already-installed Deck would have kept wined3d for ever. This file closes that gap.
//
// TWO HALVES, DELIBERATELY SEPARATE, BECAUSE THEY CARRY VERY DIFFERENT RISK
//
//   [proton] report=1     READ-ONLY. Log which d3d8 is actually loaded. Costs nothing,
//                         can break nothing, and ONE greppable line would have caught
//                         the eleven days in one. On by default.
//   [proton] dxvk_d3d8=1  WRITES `user_settings.py` in the ACTIVE Proton install. This
//                         is outside the game folder, in a Steam runtime directory, and
//                         it affects EVERY title under that Proton -- a wider blast
//                         radius than anything else this shim does. On by default
//                         because the alternative is players silently running at half
//                         speed, but it is one ini key away from off.
//
// WARNING: THE HEAL CANNOT HELP THE RUN THAT PERFORMS IT. Proton decides which d3d8.dll to
// install into the prefix before our DLL exists, so a repair lands on the NEXT launch.
// That is why the log line says so in those words: a fix that looks like it did nothing
// is how people conclude it does not work.
#include "polshim.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_report    = 1;
static int g_heal      = 1;
static LONG g_reported = 0;

// Wine/Proton only. `wine_get_version` in ntdll is the canonical test and is the same
// one maskguard.cpp uses; the env vars are belt and braces for a Wine that hides it.
static bool under_wine()
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt && GetProcAddress(nt, "wine_get_version")) return true;
    if (GetEnvironmentVariableA("GAMESCOPE_WAYLAND_DISPLAY", NULL, 0) > 0) return true;
    if (GetEnvironmentVariableA("SteamDeck", NULL, 0) > 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// REPORT -- which d3d8 is in this process right now
// ---------------------------------------------------------------------------
//
// KEY: The obvious test is WRONG and was nearly shipped: "is wined3d.dll loaded?" does
// not discriminate. Wine's `ddraw.dll` is built on wined3d, and the POL Viewer shell
// renders through DirectDraw -- so wined3d is loaded on a perfectly healthy DXVK
// session, measured 2026-09-07 with d8vk confirmed active. The only honest question is
// what the loaded **d3d8.dll** itself is, so ask that: DXVK's build carries its own
// config strings ("DXVK Device", "dxvk.*") and wined3d's thin d3d8 shim carries none.
static bool module_contains(HMODULE h, const char* needle)
{
    if (!h || !needle) return false;
    bool found = false;
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)h;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const IMAGE_NT_HEADERS* nt =
            (const IMAGE_NT_HEADERS*)((const BYTE*)h + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        DWORD span = nt->OptionalHeader.SizeOfImage;
        if (span < 0x1000 || span > 0x08000000) return false;   // sanity, not a guess
        size_t n = strlen(needle);
        const BYTE* p   = (const BYTE*)h;
        const BYTE* end = p + span - n;
        for (; p < end; ++p) {
            if (p[0] == (BYTE)needle[0] && memcmp(p, needle, n) == 0) { found = true; break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = false;                       // a torn module is not a verdict
    }
    return found;
}

void protondxvk_report_d3d8(void)
{
    if (!g_report) return;
    if (InterlockedExchange(&g_reported, 1)) return;   // once per process

    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    if (!d3d8) { logf("[proton] d3d8.dll is not loaded -- nothing to report"); return; }

    char path[MAX_PATH]; path[0] = 0;
    GetModuleFileNameA(d3d8, path, sizeof(path));
    bool dxvk    = module_contains(d3d8, "DXVK");
    bool wined3d = GetModuleHandleA("wined3d.dll") != NULL;

    if (dxvk) {
        logf("[proton] d3d8 = DXVK/Vulkan (the fast path) -- %s", path[0] ? path : "?");
    } else {
        logf("[proton] d3d8 = wined3d/OpenGL -- THE SLOW PATH -- %s", path[0] ? path : "?");
        logf("[proton]   every POL title but FMO is d3d8, so this costs framerate "
             "everywhere. Fix: PROTON_DXVK_D3D8=1 in the ACTIVE Proton's "
             "user_settings.py, then relaunch (or re-run install.sh).");
    }
    // Said out loud so nobody re-derives it: wined3d being loaded is NORMAL here.
    if (wined3d)
        logf("[proton]   (wined3d.dll is also loaded -- that is Wine's ddraw.dll, which "
             "the Viewer shell renders through. DXVK does not implement DirectDraw, so "
             "this is expected and is NOT evidence of the slow path.)");
}

// ---------------------------------------------------------------------------
// HEAL -- put PROTON_DXVK_D3D8=1 into the ACTIVE Proton's user_settings.py
// ---------------------------------------------------------------------------

// STEAM_COMPAT_TOOL_PATHS names the active Proton first, then the runtime, ':'-joined:
//   /home/deck/.../common/Proton 11.0:/home/deck/.../common/SteamLinuxRuntime_4
// Verified present in the process environment on a Deck 2026-09-07 -- this is read, not
// assumed. Wine maps the Linux root at Z:, so a Unix path becomes a Windows one by
// prefixing Z: and flipping the slashes.
static bool active_proton_dir(char* out, size_t cch)
{
    char env[2048];
    DWORD n = GetEnvironmentVariableA("STEAM_COMPAT_TOOL_PATHS", env, sizeof(env));
    if (!n || n >= sizeof(env)) return false;
    char* colon = strchr(env, ':');
    if (colon) *colon = 0;                       // first entry = the Proton itself
    if (!env[0] || env[0] != '/') return false;  // not a Unix path: do not guess
    _snprintf(out, cch, "Z:%s", env);
    out[cch - 1] = 0;
    for (char* p = out; *p; ++p) if (*p == '/') *p = '\\';
    return true;
}

static bool file_exists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Read a whole small file. Returns NULL on any failure; caller frees.
static char* slurp(const char* path, DWORD* len_out)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    if (sz == INVALID_FILE_SIZE || sz > 65536) { CloseHandle(h); return NULL; }
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, sz, &got, NULL)) { free(buf); CloseHandle(h); return NULL; }
    CloseHandle(h);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

static bool write_all(const char* path, const char* data, DWORD len)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, data, len, &put, NULL);
    CloseHandle(h);
    return ok && put == len;
}

static const char* K_LINE =
    "    \"PROTON_DXVK_D3D8\": \"1\",   # HippaulMod: d3d8 -> DXVK (Vulkan)\n";

// THE WHOLE FILE-EDITING DECISION, taking the directory as an ARGUMENT so the self-test
// can drive it against a scratch tree. The equivalent logic in install.sh shipped two
// bugs that only running it found -- it dropped the user's other settings, and its
// "is this file mine?" test matched the wrong quote style and would have DELETED a
// hand-written file. Same logic here, so: same scrutiny, and a test that pins both.
int protondxvk_apply_to(const char* dir)
{
    if (!dir || !dir[0]) return PDX_NO_PROTON;

    // Two guards before writing anything into somebody's Steam runtime directory: it
    // must actually BE a Proton, and that Proton must SHIP d8vk. Setting the flag on a
    // Proton without it (anything before 9) buys nothing and muddies the file.
    char probe[MAX_PATH];
    _snprintf(probe, sizeof(probe), "%s\\proton", dir); probe[sizeof(probe)-1] = 0;
    if (!file_exists(probe)) return PDX_NO_PROTON;

    _snprintf(probe, sizeof(probe), "%s\\files\\lib\\wine\\dxvk\\i386-windows\\d3d8.dll", dir);
    probe[sizeof(probe)-1] = 0;
    if (!file_exists(probe)) return PDX_NO_D8VK;

    char us[MAX_PATH];
    _snprintf(us, sizeof(us), "%s\\user_settings.py", dir); us[sizeof(us)-1] = 0;

    DWORD len = 0;
    char* body = slurp(us, &len);

    if (!body) {                                 // no file: write the smallest valid one
        char fresh[512];
        _snprintf(fresh, sizeof(fresh), "user_settings = {\n%s}\n", K_LINE);
        fresh[sizeof(fresh)-1] = 0;
        return write_all(us, fresh, (DWORD)strlen(fresh)) ? PDX_CREATED : PDX_WRITE_FAILED;
    }

    if (strstr(body, "PROTON_DXVK_D3D8")) { free(body); return PDX_ALREADY; }

    // The file is somebody's Python. Anchor on the dict opener and insert one line after
    // it; a file shaped any other way is hand-written and we leave it ALONE rather than
    // guess -- a Proton that cannot parse its own user_settings.py will not launch the
    // game at all, so a bad edit here costs more than the slow renderer it fixes.
    const char* anchor = strstr(body, "user_settings");
    const char* brace  = anchor ? strchr(anchor, '{') : NULL;
    const char* nl     = brace  ? strchr(brace, '\n') : NULL;
    if (!nl) { free(body); return PDX_UNRECOGNISED; }

    // Back the original up ONCE: if a .bak is already there it is the PRISTINE one and
    // must not be overwritten by a later, already-edited copy.
    char bak[MAX_PATH];
    _snprintf(bak, sizeof(bak), "%s.bak-polshim", us); bak[sizeof(bak)-1] = 0;
    if (!file_exists(bak)) CopyFileA(us, bak, TRUE);

    size_t head = (size_t)(nl - body) + 1;
    size_t klen = strlen(K_LINE);
    char*  out  = (char*)malloc(len + klen + 1);
    if (!out) { free(body); return PDX_WRITE_FAILED; }
    memcpy(out, body, head);                     // everything up to and incl. the '{' line
    memcpy(out + head, K_LINE, klen);            // our one line
    memcpy(out + head + klen, body + head, len - head);   // THE USER'S KEYS, untouched
    out[len + klen] = 0;

    int rc = write_all(us, out, (DWORD)(len + klen)) ? PDX_SET : PDX_WRITE_FAILED;
    free(out);
    free(body);
    return rc;
}

// ---------------------------------------------------------------------------
// SWITCH -- put THIS launch on DXVK, before anything has loaded d3d8
// ---------------------------------------------------------------------------
//
// The heal above only helps the NEXT launch, and only when there is a Steam Proton
// folder to write to. That misses three players: the first launch after installing
// the shim, a game started from Lutris / Heroic / Bottles, and a Proton folder the
// shim cannot write. So at shim init -- before d3d8_resolve loads d3d8, and long
// before a title does -- the shim loads DXVK's d3d8 itself, by full path. Every
// later "d3d8.dll" in the process then binds to that module by name.
//
// Measured on the Deck under Proton 11, 2026-09-28:
//   * Wine hands back its OWN builtin d3d8 for any path, DXVK's included, unless
//     d3d8's load order says native. No override at all is not enough.
//   * The per-app key (AppDefaults\pol.exe\DllOverrides) is read once, at process
//     start, so writing it now only helps the next launch.
//   * The GLOBAL HKCU\Software\Wine\DllOverrides values are re-read on every load,
//     so a value written now takes effect for the very next LoadLibrary.
// So d3d8=native is set for the one load and removed straight after. Nothing is
// left in the prefix.
//
// DXVK's d3d8 is built on DXVK's d3d9 (it creates a d3d9 device underneath), so
// the switch only happens when the d3d9 this process loads is DXVK -- and the d3d8
// is taken from beside that d3d9 first, so the two come from the same DXVK. A d3d8
// override the player or their launcher set to builtin is left alone.
static bool g_switched = false;

// The d3d8 load order the environment or the registry already asks for, or "" when
// nothing mentions d3d8. WINEDLLOVERRIDES beats the registry in Wine, so it is
// read first. Entries look like "d3d8=n" or "d3d8,d3d9=n,b", joined by ';'.
static void d3d8_override(char* out, size_t cch)
{
    out[0] = 0;
    char env[2048];
    DWORD n = GetEnvironmentVariableA("WINEDLLOVERRIDES", env, sizeof(env));
    if (n && n < sizeof(env)) {
        _strlwr_s(env, sizeof(env));
        char *c1 = NULL, *c2 = NULL;
        for (char* ent = strtok_s(env, ";", &c1); ent; ent = strtok_s(NULL, ";", &c1)) {
            char* eq = strchr(ent, '=');
            if (!eq) continue;
            *eq = 0;
            for (char* name = strtok_s(ent, ",", &c2); name; name = strtok_s(NULL, ",", &c2)) {
                while (*name == ' ') ++name;
                if (strcmp(name, "d3d8") == 0 || strcmp(name, "*d3d8") == 0) {
                    _snprintf(out, cch, "%s", eq + 1); out[cch - 1] = 0;
                    return;
                }
            }
        }
    }

    char exe[MAX_PATH] = "", key[MAX_PATH + 64];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    const char* leaf = strrchr(exe, '\\');
    leaf = leaf ? leaf + 1 : exe;
    _snprintf(key, sizeof(key), "Software\\Wine\\AppDefaults\\%s\\DllOverrides", leaf);
    key[sizeof(key) - 1] = 0;
    const char* subs[] = { key, "Software\\Wine\\DllOverrides" };
    for (int i = 0; i < 2; i++) {
        DWORD cb = (DWORD)cch;
        if (RegGetValueA(HKEY_CURRENT_USER, subs[i], "d3d8", RRF_RT_REG_SZ, NULL,
                         out, &cb) == ERROR_SUCCESS)
            return;
        out[0] = 0;
    }
}

// Does the file on disk carry DXVK's strings? Read, not loaded: loading it to ask
// would be the very load this is deciding about.
static bool file_is_dxvk(const char* path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(f, NULL), got = 0;
    bool yes = false;
    if (sz != INVALID_FILE_SIZE && sz > 0 && sz < 32u * 1024 * 1024) {
        BYTE* buf = (BYTE*)malloc(sz);
        if (buf && ReadFile(f, buf, sz, &got, NULL))
            for (DWORD i = 0; i + 4 <= got; i++)
                if (buf[i] == 'D' && memcmp(buf + i, "DXVK", 4) == 0) { yes = true; break; }
        free(buf);
    }
    CloseHandle(f);
    return yes;
}

void protondxvk_switch_now(void)
{
    if (!g_heal) return;
    if (!under_wine()) return;
    if (GetModuleHandleA("d3d8.dll")) return;    // too late; the report says which it is

    char ov[64];
    d3d8_override(ov, sizeof(ov));
    char sys[MAX_PATH] = "", sysd3d8[MAX_PATH] = "";
    GetSystemDirectoryA(sys, sizeof(sys));
    _snprintf(sysd3d8, sizeof(sysd3d8), "%s\\d3d8.dll", sys); sysd3d8[sizeof(sysd3d8)-1] = 0;
    const bool sys_is_dxvk = file_is_dxvk(sysd3d8);

    // Proton with PROTON_DXVK_D3D8=1: d3d8=n and the prefix copy IS DXVK. Nothing to do.
    if (ov[0] == 'n' && sys_is_dxvk) return;
    // Builtin-first, or disabled: somebody chose that. Leave it.
    if (ov[0] && ov[0] != 'n') {
        logf("[proton] d3d8 is set to '%s' by the launcher or the player -- leaving "
             "it alone", ov);
        return;
    }

    HMODULE d9 = LoadLibraryA("d3d9.dll");
    if (!d9 || !module_contains(d9, "DXVK")) {
        logf("[proton] d3d8 is on wined3d, and d3d9 here is not DXVK either -- DXVK's "
             "d3d8 needs DXVK's d3d9, so d3d8 stays on wined3d");
        return;
    }

    // Beside the d3d9 first (same DXVK, by construction), then the active Proton.
    char src[MAX_PATH] = "";
    char d9dir[MAX_PATH] = "";
    if (GetModuleFileNameA(d9, d9dir, sizeof(d9dir))) {
        char* slash = strrchr(d9dir, '\\');
        if (slash) {
            *slash = 0;
            _snprintf(src, sizeof(src), "%s\\d3d8.dll", d9dir); src[sizeof(src)-1] = 0;
            if (!file_is_dxvk(src)) src[0] = 0;
        }
    }
    char dir[MAX_PATH];
    if (!src[0] && active_proton_dir(dir, sizeof(dir))) {
        _snprintf(src, sizeof(src), "%s\\files\\lib\\wine\\dxvk\\i386-windows\\d3d8.dll", dir);
        src[sizeof(src)-1] = 0;
        if (!file_is_dxvk(src)) src[0] = 0;
    }
    if (!src[0]) {
        logf("[proton] d3d8 is on wined3d and no DXVK d3d8 was found beside d3d9 or in "
             "the active Proton (DXVK 2.4+ / Proton 9+ ship one) -- d3d8 stays on wined3d");
        return;
    }

    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\DllOverrides", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
        logf("[proton] could not open the Wine DllOverrides key -- d3d8 stays on wined3d");
        return;
    }
    const bool wrote = !ov[0] &&
        RegSetValueExA(k, "d3d8", 0, REG_SZ, (const BYTE*)"native", 7) == ERROR_SUCCESS;
    HMODULE m = LoadLibraryExA(src, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (wrote) RegDeleteValueA(k, "d3d8");       // only ever our own value
    RegCloseKey(k);

    if (m && module_contains(m, "DXVK")) {
        g_switched = true;
        logf("[proton] d3d8 switched to DXVK for this launch -- %s", src);
    } else {
        logf("[proton] loaded %s but Wine gave its own d3d8 -- d3d8 stays on wined3d "
             "this launch", src);
    }
}

void protondxvk_heal(void)
{
    if (!g_heal) return;
    if (!under_wine()) return;                  // native Windows has no Proton to fix

    char dir[MAX_PATH];
    if (!active_proton_dir(dir, sizeof(dir))) {
        logf("[proton] STEAM_COMPAT_TOOL_PATHS is absent or not a Unix path -- "
             "cannot tell which Proton is active; leaving it alone%s",
             g_switched ? " (this launch was switched in-process, and every launch "
                          "will be)" : "");
        return;
    }

    char us[MAX_PATH];
    _snprintf(us, sizeof(us), "%s\\user_settings.py", dir); us[sizeof(us)-1] = 0;

    switch (protondxvk_apply_to(dir)) {
    case PDX_ALREADY:
        break;                                   // the common case; stay quiet
    case PDX_SET:
    case PDX_CREATED:
        // WARNING: SAY THAT IT IS NOT THIS LAUNCH. Proton chose the d3d8.dll before this DLL
        // existed, so a repair cannot help the run that made it -- and a fix that looks
        // like it did nothing is how a working fix gets undone.
        if (g_switched)
            logf("[proton] *** d3d8 -> DXVK ENABLED in %s -- this launch was already "
                 "switched in-process; from the next launch Proton does it itself.", us);
        else
            logf("[proton] *** d3d8 -> DXVK ENABLED in %s -- TAKES EFFECT ON THE NEXT "
                 "LAUNCH, not this one (Proton picks the d3d8 before we exist). "
                 "Restart the game to get it.", us);
        break;
    case PDX_NO_PROTON:
        logf("[proton] '%s' has no proton script -- not writing there", dir);
        break;
    case PDX_NO_D8VK:
        logf("[proton] this Proton ships no DXVK d3d8 (needs Proton 9+) -- d3d8 will "
             "stay on wined3d. Update Proton to get the fast path.");
        break;
    case PDX_UNRECOGNISED:
        logf("[proton] %s is shaped in a way I do not recognise -- leaving it alone. "
             "Add \"PROTON_DXVK_D3D8\": \"1\" by hand, or re-run install.sh.", us);
        break;
    default:
        logf("[proton] could not write %s -- d3d8 stays on wined3d", us);
        break;
    }
}

void protondxvk_configure(const wchar_t* ini)
{
    g_report = GetPrivateProfileIntW(L"proton", L"report",    1, ini);
    g_heal   = GetPrivateProfileIntW(L"proton", L"dxvk_d3d8", 1, ini);
    protondxvk_switch_now();    // this launch -- before anything has loaded d3d8
    protondxvk_heal();          // the next ones, through Proton itself
}
