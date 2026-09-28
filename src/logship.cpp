// logship.cpp -- read this client's own shim log safely, for the bug report.
//
// SCOPE IN HIPPAULMOD. The development shim this came from can also POST a log
// on its own after a crash; that is not here. What IS here: the LIVE log at the
// bottom of this file, which sends only while the player has ticked it and
// switches itself off after a set time. Everything else exists so the report
// key (polreport.cpp) and the live log share ONE place for the three things
// that are expensive to get wrong when a log leaves the machine:
//
//   - the SNAPSHOT: `polshim.<pid>.log` is truncated when Windows recycles the
//     pid, so the report reads a copy of the tail, never streams the live file;
//   - the REDACTION: passwords, login tokens and the Blowfish session key are
//     blanked before the bytes leave the machine;
//   - the ENDPOINT: the same server the player connects to, never a default
//     host this project does not control.
//
//   [logship]
//   url=          ; blank = the server the Viewer is redirected to, port 51300
//   redact=1      ; leave on -- this is what keeps credentials off the wire
//   live=0        ; 1 = stream this session's log (see LIVE below)
//   live_minutes=30
#include "polshim.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int      g_redact = 1;
static DWORD    g_max_bytes = 6 * 1024 * 1024;
static char     g_url[512];
static wchar_t  g_logpath[MAX_PATH];
static wchar_t  g_ini_path[MAX_PATH];

// --- redaction ---------------------------------------------------------------
//
// Each entry: find `lead`, then blank everything up to the first `stop` char
// (or `run` bytes if stop is 0). A plain substring scan on purpose -- a log is a
// stream of unknown shapes and a regex engine is not worth linking for this.
//
// LEADS ARE DELIBERATELY LONG. A bare "K=" would also hit `Direct3DCreate8(SDK=%u)`
// and blank the D3D version out of every report; over-redaction is not the safe
// direction, it is another way to lose the log.
struct Redaction { const char* lead; char stop; int run; };
static const Redaction kRedactions[] = {
    { "cred='",          '\'', 0 },   // login credential field
    { "PASS ",           ' ',  0 },   // IRC PASS
    { "password=",       ' ',  0 },
    { "password: ",      ' ',  0 },
    { "polcryptInit K=", ' ',  0 },   // the Blowfish session key...
    { " IV=",            ' ',  0 },   // ...its IV...
    { " sbox=",          ' ',  0 },   // ...and the S-box derived from it
    // fmokey.cpp's hexline(), which is off by default but prints FMO's world key
    // and its 52-byte credential blob in full when someone turns it on.
    { "[fmokey] key16 = ",  ' ', 0 },
    { "[fmokey] auth52 = ", ' ', 0 },
};

// The NICK token is positional rather than led by a keyword:
//     NICK U<id>:<32 hex>:<token>
// so it gets its own pass -- blank the hex run after the first ':'.
static void redact_nick_tokens(char* buf, DWORD len)
{
    for (DWORD i = 0; i + 5 < len; ++i) {
        if (memcmp(buf + i, "NICK U", 6) != 0) continue;
        DWORD j = i + 6;
        while (j < len && buf[j] != ':' && buf[j] != '\n') ++j;
        if (j >= len || buf[j] != ':') continue;
        ++j;
        DWORD start = j;
        while (j < len && buf[j] != ':' && buf[j] != '\n' && buf[j] != ' ') ++j;
        if (j - start >= 8) memset(buf + start, '#', j - start);
    }
}

static int redact_buffer(char* buf, DWORD len)
{
    int n = 0;
    for (size_t r = 0; r < sizeof(kRedactions) / sizeof(kRedactions[0]); ++r) {
        const Redaction& rd = kRedactions[r];
        size_t llen = strlen(rd.lead);
        for (DWORD i = 0; i + llen < len; ++i) {
            if (memcmp(buf + i, rd.lead, llen) != 0) continue;
            DWORD j = i + (DWORD)llen;
            DWORD start = j;
            while (j < len && buf[j] != '\n' && buf[j] != '\r' &&
                   (rd.stop ? buf[j] != rd.stop : (j - start) < (DWORD)rd.run))
                ++j;
            if (j > start) { memset(buf + start, '#', j - start); ++n; }
            i = j;
        }
    }
    redact_nick_tokens(buf, len);
    return n;
}

// --- the previous session's log ------------------------------------------------
//
// Same pattern rule as log_prune, so the two can never disagree about which
// files are ours: `polshim.log` matches `polshim.*.log`. Newest wins, and OUR
// OWN pid is skipped.
static bool newest_other_log(const wchar_t* mine, wchar_t* out, size_t cch)
{
    wchar_t dir[MAX_PATH]; wcscpy_s(dir, mine);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return false;
    slash[1] = 0;

    const wchar_t* base = wcsrchr(mine, L'\\');
    base = base ? base + 1 : mine;
    wchar_t stem[MAX_PATH]; wcscpy_s(stem, base);
    wchar_t* dot = wcschr(stem, L'.');
    if (dot) *dot = 0;
    wchar_t pat[MAX_PATH];
    swprintf_s(pat, L"%s%s.*.log", dir, stem);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    ULONGLONG best = 0; bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t full[MAX_PATH]; swprintf_s(full, L"%s%s", dir, fd.cFileName);
        if (_wcsicmp(full, mine) == 0) continue;          // never our own
        ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32)
                    |  fd.ftLastWriteTime.dwLowDateTime;
        if (t >= best) { best = t; wcsncpy_s(out, cch, full, _TRUNCATE); found = true; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

// --- snapshot ------------------------------------------------------------------
//
// The TAIL of the log, into memory. Opened with full share flags: our own
// logger holds this file open for writing.
static char* snapshot(const wchar_t* path, DWORD* out_len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) { CloseHandle(f); return NULL; }

    DWORD want = (sz.QuadPart > (LONGLONG)g_max_bytes) ? g_max_bytes
                                                       : (DWORD)sz.QuadPart;
    LARGE_INTEGER at;
    at.QuadPart = sz.QuadPart - (LONGLONG)want;
    SetFilePointerEx(f, at, NULL, FILE_BEGIN);

    char* buf = (char*)malloc(want + 1);
    if (!buf) { CloseHandle(f); return NULL; }
    DWORD got = 0;
    BOOL ok = ReadFile(f, buf, want, &got, NULL);
    CloseHandle(f);
    if (!ok || got == 0) { free(buf); return NULL; }
    buf[got] = 0;

    // Drop the partial first line when we began mid-file. Redaction is anchored
    // on a lead token; a tail cut that lands PAST a line's lead would leave the
    // rest of that credential with nothing to recognise it by, and it would ship
    // un-blanked. memmove keeps buf as the malloc base so the caller's free() holds.
    if (at.QuadPart > 0) {
        char* nl = (char*)memchr(buf, '\n', got);
        if (nl) {
            DWORD skip = (DWORD)(nl + 1 - buf);
            memmove(buf, buf + skip, got - skip);
            got -= skip;
        } else {
            got = 0;    // one giant partial line: send nothing rather than a cut credential
        }
        buf[got] = 0;
    }

    *out_len = got;
    return buf;
}

// --- the endpoint ----------------------------------------------------------------
//
// An explicit [logship] url wins (a bare host or host:port is expanded to the
// standard path), then POLSHIM_SERVER. Otherwise the address the Viewer itself is
// being redirected to -- netredir already folded --polserver, POLSHIM_SERVER and
// [redirect] server into that, so asking it cannot disagree with where the player
// actually connects.
//
// NEVER SQUARE ENIX. With [redirect] enable=1 the Viewer is bypassing to SE's own
// servers and netredir's table holds SE's addresses; a report must not be posted
// there. And there is NO fallback host: an install with no server configured has
// nowhere it should be sending a report, so the player is told that instead.
static const char* resolve_url()
{
    if (g_url[0]) {
        if (!strncmp(g_url, "http://", 7) || !strncmp(g_url, "https://", 8))
            return g_url;
        char host[256];
        _snprintf_s(host, sizeof(host), _TRUNCATE, "%s", g_url);
        _snprintf_s(g_url, sizeof(g_url), _TRUNCATE,
                    strchr(host, ':') ? "http://%s/_shim/log"
                                      : "http://%s:51300/_shim/log", host);
        return g_url;
    }
    char srv[128] = "";
    char v[128];
    if (GetEnvironmentVariableA("POLSHIM_SERVER", v, sizeof(v)) && v[0]) {
        _snprintf_s(srv, sizeof(srv), _TRUNCATE, "%s", v);
    } else if (!netredir_se_bypass()) {
        unsigned long a = netredir_effective_addr("wh000.pol.com");
        if (a && a != 0xFFFFFFFFUL) {
            const unsigned char* b = (const unsigned char*)&a;   // network order
            _snprintf_s(srv, sizeof(srv), _TRUNCATE, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        }
    }
    if (!srv[0]) return NULL;
    static char url[512];
    _snprintf_s(url, sizeof(url), _TRUNCATE, "http://%s:51300/_shim/log", srv);
    logf("[logship] report endpoint from the game server -> %s", url);
    return url;
}

// --- what polreport.cpp borrows ----------------------------------------------------

// Tail of `path`, redacted, caller free()s. `max_bytes` of 0 means the default cap.
char* logship_snapshot_redacted(const wchar_t* path, DWORD max_bytes, DWORD* out_len)
{
    DWORD saved = g_max_bytes;
    if (max_bytes) g_max_bytes = max_bytes;
    DWORD len = 0;
    char* buf = snapshot(path ? path : g_logpath, &len);
    g_max_bytes = saved;
    if (!buf) return NULL;
    if (g_redact) redact_buffer(buf, len);
    if (out_len) *out_len = len;
    return buf;
}

// Redact a buffer we already hold -- the ini, which carries `password=` lines.
int logship_redact_inplace(char* buf, DWORD len) { return redact_buffer(buf, len); }

// The endpoint for `leaf` (e.g. "/_shim/report") on the resolved server.
bool logship_endpoint(const char* leaf, char* out, size_t cch)
{
    if (!leaf || !out || cch < 16) return false;
    const char* base = resolve_url();
    if (!base || !base[0]) return false;
    const char* p = strstr(base, "://");
    if (!p) return false;
    const char* slash = strchr(p + 3, '/');
    size_t hostpart = slash ? (size_t)(slash - base) : strlen(base);
    if (hostpart + strlen(leaf) + 1 > cch) return false;
    memcpy(out, base, hostpart);
    strcpy_s(out + hostpart, cch - hostpart, leaf);
    return true;
}

const wchar_t* logship_logpath() { return g_logpath; }

bool logship_prev_logpath(wchar_t* out, size_t cch)
{
    if (!out || !g_logpath[0]) return false;
    return newest_other_log(g_logpath, out, cch);
}

static void live_read(const wchar_t* ini);   // the LIVE section, below

void logship_configure(const wchar_t* ini, const wchar_t* logpath)
{
    wcsncpy_s(g_ini_path, ini, _TRUNCATE);
    g_redact = GetPrivateProfileIntW(L"logship", L"redact", 1, ini);
    wchar_t w[512];
    ini_str(L"logship", L"url", L"", w, _countof(w), ini);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, g_url, sizeof(g_url) - 1, NULL, NULL);
    if (logpath) wcsncpy_s(g_logpath, logpath, _TRUNCATE);
    if (!g_redact)
        logf("[logship] WARNING: [logship] redact=0 -- a report would carry passwords "
             "and login tokens from the log unblanked");
    live_read(ini);
}

// ---------------------------------------------------------------------------
// LIVE -- this session's log, to the server, as it is written, for a while
// ---------------------------------------------------------------------------
//
//   [logship]
//   live=0            ; the settings row "Send my log to the server as I play"
//   live_minutes=30   ; then it turns itself off
//   live_until=0      ; written by the shim: when it turns off (Unix time)
//
// For the problem one report cannot catch: "it goes wrong after twenty minutes".
// Ported from the development shim with one change a PUBLIC build needs: it
// stops on its own. A tick nobody remembers would otherwise send a player's log
// every few seconds for as long as they play. The deadline is written to the ini
// the moment streaming starts, so restarting the game does not buy more time;
// when it passes, live goes back to 0 in the ini too, and the settings window
// shows it off. Unticking clears the deadline, so ticking again starts afresh.
//
// The loop is the development shim's, for the reasons given there: each POST
// carries only NEW lines, ends on a line boundary (redaction works line by
// line, and half a credential line would slip past it), and an increment only
// counts as sent on a 200, so the server's copy has no holes. The server
// appends them to logs/shim-<host>.log.
static const DWORD     LIVE_INTERVAL_MS = 5000;
static const DWORD     LIVE_CHUNK       = 256 * 1024;
static const ULONGLONG LIVE_BACKFILL    = 2 * 1024 * 1024;   // most history on the first tick

static int               g_live = 0;
static int               g_live_minutes = 30;
static volatile LONGLONG g_live_until = 0;        // Unix seconds; 0 = not started
static volatile LONG     g_live_on = 0;
static LONG              g_live_thread_up = 0;
static ULONGLONG         g_live_offset = 0;
static char              g_live_session[48];

static LONGLONG unix_now() { return (LONGLONG)_time64(NULL); }

static void live_read(const wchar_t* ini)
{
    g_live         = GetPrivateProfileIntW(L"logship", L"live", 0, ini);
    g_live_minutes = GetPrivateProfileIntW(L"logship", L"live_minutes", 30, ini);
    if (g_live_minutes < 1)   g_live_minutes = 1;
    if (g_live_minutes > 240) g_live_minutes = 240;   // a typo must not mean a day
    wchar_t w[32] = L"";
    GetPrivateProfileStringW(L"logship", L"live_until", L"0", w, _countof(w), ini);
    g_live_until = _wtoi64(w);
}

static void live_write_ini(int live, LONGLONG until)
{
    if (!g_ini_path[0]) return;
    wchar_t v[32];
    _snwprintf_s(v, _countof(v), _TRUNCATE, L"%lld", until);
    WritePrivateProfileStringW(L"logship", L"live", live ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"logship", L"live_until", v, g_ini_path);
}

static void live_stop(const char* why)
{
    if (!InterlockedExchange(&g_live_on, 0)) return;
    logf("[logship] live log OFF -- %s", why);
}

static bool live_post(const char* body, DWORD len)
{
    char url[512];
    if (!logship_endpoint("/_shim/log", url, sizeof(url))) return false;
    wchar_t wname[64] = L"unknown";
    DWORD wn = _countof(wname);
    GetComputerNameW(wname, &wn);
    char name[128] = "";
    WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name) - 1, NULL, NULL);
    // The server names the stored file from THESE and sanitises them.
    char hdrs[320];
    _snprintf_s(hdrs, sizeof(hdrs), _TRUNCATE,
                "X-Shim-Host: %s\r\nX-Shim-Pid: %lu\r\nX-Shim-Reason: live\r\n"
                "X-Shim-Mode: live\r\nX-Shim-Session: %s\r\n",
                name, (unsigned long)GetCurrentProcessId(), g_live_session);
    DWORD status = 0;
    bool sent = shim_http_post(url, body, len, "text/plain", hdrs, NULL, 0, &status);
    return sent && status == 200;
}

static DWORD WINAPI live_thread(LPVOID)
{
    _snprintf_s(g_live_session, sizeof(g_live_session), _TRUNCATE, "%lu-%lu",
                (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    bool server_ok = true;     // log changes of state only
    bool caught_up = false;
    bool skip_partial = false;
    for (;;) {
        Sleep(caught_up ? LIVE_INTERVAL_MS : 250);    // drain a backlog quickly
        if (!g_live_on) { caught_up = true; continue; }
        if (unix_now() >= g_live_until) {
            live_write_ini(0, 0);
            live_stop("the time limit passed; tick it again in the settings window "
                      "if someone still needs it");
            continue;
        }
        log_flush();

        HANDLE f = CreateFileW(g_logpath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) { caught_up = true; continue; }
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) { CloseHandle(f); caught_up = true; continue; }

        // Turned on in a long session: do not replay hours of it, and drop the
        // partial first line so redaction sees whole lines.
        if (g_live_offset == 0 && (ULONGLONG)sz.QuadPart > LIVE_BACKFILL) {
            g_live_offset = (ULONGLONG)sz.QuadPart - LIVE_BACKFILL;
            skip_partial = true;
        }
        if ((ULONGLONG)sz.QuadPart <= g_live_offset) { CloseHandle(f); caught_up = true; continue; }

        ULONGLONG avail = (ULONGLONG)sz.QuadPart - g_live_offset;
        DWORD want = avail > LIVE_CHUNK ? LIVE_CHUNK : (DWORD)avail;
        LARGE_INTEGER at; at.QuadPart = (LONGLONG)g_live_offset;
        SetFilePointerEx(f, at, NULL, FILE_BEGIN);
        char* buf = (char*)malloc(want + 1);
        DWORD got = 0;
        BOOL rd = buf && ReadFile(f, buf, want, &got, NULL);
        CloseHandle(f);
        if (!rd || !got) { free(buf); caught_up = true; continue; }
        buf[got] = 0;

        DWORD start = 0;
        if (skip_partial) {
            while (start < got && buf[start] != '\n') ++start;
            if (start < got) ++start;
            skip_partial = false;
        }
        if (start >= got) { g_live_offset += got; free(buf); continue; }
        // End on a line boundary; the partial last line goes next time. Only a
        // chunk that is one giant line is sent split.
        DWORD end = got;
        while (end > start && buf[end - 1] != '\n') --end;
        if (end == start) {
            if (got < LIVE_CHUNK) { free(buf); caught_up = true; continue; }
            end = got;
        }

        if (g_redact) redact_buffer(buf + start, end - start);
        bool ok = live_post(buf + start, end - start);
        free(buf);
        if (ok) {
            g_live_offset += end;
            caught_up = (g_live_offset >= (ULONGLONG)sz.QuadPart);
            if (!server_ok) { server_ok = true; logf("[logship] live log: server reachable again"); }
        } else {
            caught_up = true;
            if (server_ok) { server_ok = false;
                logf("[logship] live log: sending failed -- will keep trying quietly"); }
        }
    }
}

// Start if the ini says so and the time has not run out. Called at startup and
// on every Save.
void logship_live_start(void)
{
    if (!g_live) return;
    LONGLONG now = unix_now();
    if (g_live_until == 0) {
        g_live_until = now + (LONGLONG)g_live_minutes * 60;
        live_write_ini(1, g_live_until);
    } else if (now >= g_live_until) {
        live_write_ini(0, 0);
        g_live = 0;
        logf("[logship] live log was ticked but its time ran out in an earlier session "
             "-- left off");
        return;
    }
    if (InterlockedExchange(&g_live_on, 1)) return;       // already streaming
    if (!InterlockedExchange(&g_live_thread_up, 1)) {
        HANDLE t = CreateThread(NULL, 0, live_thread, NULL, 0, NULL);
        if (t) CloseHandle(t); else InterlockedExchange(&g_live_thread_up, 0);
    }
    char url[512] = "(no server set)";
    logship_endpoint("/_shim/log", url, sizeof(url));
    logf("[logship] live log ON: every %lus to %s until it turns itself off in %d "
         "minute(s) (redact=%d)", (unsigned long)(LIVE_INTERVAL_MS / 1000), url,
         (int)((g_live_until - now + 59) / 60), g_redact);
}

void logship_reload(const wchar_t* ini)
{
    int was = g_live;
    g_redact = GetPrivateProfileIntW(L"logship", L"redact", 1, ini);
    live_read(ini);
    if (!g_live && (was || g_live_on)) {
        live_write_ini(0, 0);          // unticked: the next tick starts a fresh window
        g_live_until = 0;
        live_stop("unticked in the settings window");
    } else if (g_live) {
        logship_live_start();
    }
}

int logship_live_minutes_left(void)
{
    if (!g_live_on) return 0;
    LONGLONG left = g_live_until - unix_now();
    return left > 0 ? (int)((left + 59) / 60) : 0;
}
