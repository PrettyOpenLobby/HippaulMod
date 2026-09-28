// cursorlock.cpp -- keep the pointer inside the game window while you are playing,
// and let it go the moment you are not.
//
// WHY
//
// Players asked for Front Mission Online and Fantasy Earth to hold the mouse while
// they play, so a camera drag that runs past the window edge does not click on the
// desktop, while Alt+Tab still gets them out of the game.
//
// Both titles turn the camera on a HELD mouse button. Windowed, a drag that runs
// past the edge of the game carries the pointer onto the desktop, and the next
// click lands on whatever is under it -- the game loses the foreground mid-fight.
// The titles were written for exclusive fullscreen, where the screen edge WAS the
// game edge, so neither ever needed to confine anything on a desktop.
//
// WHY THE TITLES' OWN ClipCursor IS NOT THE ANSWER
//
// Fantasy Earth does call ClipCursor itself, and the
// shim swallows it under [dx] d3d_freecursor, deliberately. A 2003 title clips on
// its own schedule and never releases on focus loss -- that is the "the mouse is
// trapped and alt-tab feels impossible" history this project has already paid for
// (d3d8hook.cpp, release_clip_if_unfocused). So the policy lives HERE, owned by
// the shim, and the titles' requests stay swallowed.
//
// THE RULE
//
// The pointer is confined to the game WINDOW (frame included, so it can still be
// moved and resized) only while ALL of:
//
//   * the game window itself is the foreground window. Not merely "a window of
//     ours": the Friend List, the exit prompt and the settings dialog are all
//     pol.exe windows, and each needs a free pointer to be used at all.
//   * the game is not minimised and the user is not dragging/sizing its frame
//     (the modal loop -- a clip applied mid-drag would pin the frame in place).
//   * the pointer is ALREADY inside the client area and no mouse button is held.
//     This is the rule that keeps the lock from ever being a yank: Alt+Tab back
//     in with the pointer elsewhere on the desktop and nothing happens until you
//     move it over the game. Clicking the title bar to focus the window does not
//     engage it either -- the button is held, and then you are in the modal loop.
//
// and it is released the instant any of them stops being true. That is what makes
// Alt+Tab work: [dx] key_escape already lets Alt+Tab and the Windows key past the
// titles' keyboard hooks (hookspy.cpp), the shell hands the foreground to another
// window, and the next tick here lets the pointer go.
//
// Once engaged the lock HOLDS through a held button -- that is the whole point,
// the camera drag -- and only the foreground/minimise/modal conditions end it.
//
// HOW
//
// A 15 ms watcher thread, not a hook. Foreground changes arrive by several routes
// (Alt+Tab, the Windows key, Ctrl+Alt+Del, a toast, another pol.exe window) and
// none of them passes through code of ours reliably, so it is polled. Each tick
// re-reads GetClipCursor and re-applies when it differs, which also covers
// Windows resetting the clip under us and the window being moved or resized.
// Only a clip THIS module applied is ever released, so no one else's is clobbered.
//
// Stands down entirely when the shim is not the owner of the cursor policy
// ([dx] d3d_freecursor=0 hands the titles their own ClipCursor back, and two
// owners would fight every tick) and under gamescope (Steam Deck Game Mode and
// the like), where the game IS the whole screen and there is no desktop to click
// away to. NOT under Wine/Proton in general: a Linux DESKTOP player has exactly the
// Windows problem, and the lock first shipped standing down for all of Wine, which
// is how a Proton player on a desktop got nothing (reported 2026-09-28). Wine drops
// its own pointer grab and moves the foreground off the game when a native Linux
// window takes focus, so the release below still fires.
//
// CONFIG: [dx] cursor_lock = 1 (default), per title as [dx.<leaf>] cursor_lock.
// [dx] cursor_hide = 0/1 hides the pointer over the picture while locked; absent,
// it is on for FMO only. Both re-read live.

#include "polshim.h"

static int           g_enable  = 1;
static volatile LONG g_stop    = 0;
static HANDLE        g_thread  = NULL;
static volatile LONG g_owned   = 0;     // WE applied the clip that is in force
static RECT          g_want    = { 0, 0, 0, 0 };
static bool          g_wine    = false;   // gamescope: the game is the whole screen

// THE HIDDEN POINTER (2026-09-28, FMO). While locked, the Windows pointer over the
// game's picture is hidden for a title that draws its own or does not use it --
// FMO by default. NOT Fantasy Earth: its in-game pointer IS the Windows cursor, and
// hiding it would leave that game with no pointer at all. Shown again over the
// frame, and whenever the lock is off. [dx] cursor_hide, or [dx.<leaf>]
// cursor_hide, overrides the per-title default; absent (-1) means "the default".
static int  g_hide = -1;

static LONG g_n_engage = 0, g_n_release = 0, g_n_reassert = 0;

#define CURSORLOCK_TICK_MS 15

typedef BOOL (WINAPI *PFN_CLIP)(const RECT*);

static BOOL real_clip(const RECT* r)
{
    PFN_CLIP clip = (PFN_CLIP)d3d8_real_ClipCursor();
    return clip ? clip(r) : ClipCursor(r);
}

// Stand down only where there is nothing to escape to: a gamescope session. Its
// Wayland socket is exported to every child, and the Deck's Game Mode also sets
// XDG_CURRENT_DESKTOP=gamescope. SteamDeck=1 is NOT used: Desktop Mode sets it too,
// and there a desktop does exist.
static bool under_gamescope(void)
{
    if (GetEnvironmentVariableA("GAMESCOPE_WAYLAND_DISPLAY", NULL, 0) > 0) return true;
    char de[64] = "";
    if (GetEnvironmentVariableA("XDG_CURRENT_DESKTOP", de, sizeof(de)) > 0 &&
        _stricmp(de, "gamescope") == 0) return true;
    return false;
}

// ---------------------------------------------------------------------------
// THE DECISION -- pure, so the selftest can prove it without a desktop.
//
//   locked_now   : the lock is currently engaged
//   game_fg      : the game window is the foreground window
//   minimized    : the game is minimised
//   in_modal     : the user is moving/sizing the game's frame
//   inside       : the pointer is inside the game's client area
//   button_held  : a mouse button is physically down
//
// Returns true when the pointer must be confined.
// ---------------------------------------------------------------------------
bool cursorlock_decide(bool locked_now, bool game_fg, bool minimized, bool in_modal,
                       bool inside, bool button_held)
{
    if (!game_fg || minimized || in_modal) return false;   // always let go
    if (locked_now) return true;                            // hold, even mid-drag
    return inside && !button_held;                          // engage without a yank
}

static bool rect_eq(const RECT& a, const RECT& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

static void release(const char* why)
{
    if (!InterlockedExchange(&g_owned, 0)) return;
    real_clip(NULL);
    InterlockedIncrement(&g_n_release);
    logf("[lock] pointer released -- %s", why);
}

static void tick(void)
{
    HWND gw = d3d8_game_window();
    bool active = g_enable && !g_wine && d3d8_freecursor_on() && gw && IsWindow(gw);
    if (!active) {
        release(!g_enable ? "cursor_lock is off" :
                !d3d8_freecursor_on() ? "d3d_freecursor=0 hands the title its own clip" :
                "the game window is gone");
        return;
    }

    bool fg        = GetForegroundWindow() == gw;
    bool minimized = IsIconic(gw) || d3d8_user_minimized() != 0;
    bool modal     = d3d8_in_modal() != 0;

    RECT cr;
    POINT o = { 0, 0 };
    bool have_rect = GetClientRect(gw, &cr) && cr.right > 0 && cr.bottom > 0 &&
                     ClientToScreen(gw, &o);
    RECT client = { 0, 0, 0, 0 };
    if (have_rect) {
        client.left = o.x;              client.top = o.y;
        client.right = o.x + cr.right;  client.bottom = o.y + cr.bottom;
    }
    // THE PEN IS THE WHOLE WINDOW, FRAME INCLUDED (2026-09-28). Penned to the client
    // area the pointer could never reach the caption or the borders, so the window
    // could not be moved or resized while locked. The frame belongs to the game
    // window, so a click there cannot land on another program -- the thing the lock
    // exists to stop -- and grabbing it enters the modal loop, which releases the
    // lock for the drag. GetWindowRect includes Windows' invisible resize borders,
    // which is where the sizing arrows live. Engaging still needs the pointer over
    // the CLIENT area: arriving on the frame is not "playing".
    RECT want = client;
    RECT wr;
    if (have_rect && GetWindowRect(gw, &wr) && wr.right > wr.left && wr.bottom > wr.top)
        want = wr;

    POINT p = { 0, 0 };
    bool inside = have_rect && GetCursorPos(&p) && PtInRect(&client, p);
    // The swapping of primary/secondary buttons does not matter here: any held
    // button means "not now".
    bool held = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) |
                 GetAsyncKeyState(VK_MBUTTON)) & 0x8000;

    bool locked = InterlockedCompareExchange(&g_owned, 0, 0) != 0;
    bool lock = have_rect && cursorlock_decide(locked, fg, minimized, modal, inside, held != 0);

    if (!lock) {
        release(!fg ? "the game is no longer the window in front" :
                minimized ? "the game was minimised" :
                modal ? "you are moving or sizing the game window" :
                "the game window has no client area");
        return;
    }

    RECT cur;
    bool drifted = !GetClipCursor(&cur) || !rect_eq(cur, want);
    if (!locked) {
        g_want = want;
        real_clip(&want);
        InterlockedExchange(&g_owned, 1);
        InterlockedIncrement(&g_n_engage);
        logf("[lock] pointer held inside the game window (%ld,%ld)-(%ld,%ld), frame "
             "included -- Alt+Tab or the Windows key lets it go ([dx] cursor_lock=0 to "
             "stop); pointer %s over the picture",
             want.left, want.top, want.right, want.bottom,
             cursorlock_hide_wanted() ? "HIDDEN" : "shown");
    } else if (drifted) {
        // Moved/resized window, or Windows reset the clip under us. Not logged
        // per event: a resize drag would print hundreds.
        g_want = want;
        real_clip(&want);
        InterlockedIncrement(&g_n_reassert);
    }
}

static DWORD WINAPI watcher(LPVOID)
{
    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        tick();
        Sleep(CURSORLOCK_TICK_MS);
    }
    return 0;
}

// The title's own ClipCursor(NULL) under freecursor would drop OUR clip for up to
// one tick, and FE calls ClipCursor often. d3d8hook asks here first.
bool cursorlock_owns(void) { return InterlockedCompareExchange(&g_owned, 0, 0) != 0; }

void cursorlock_reassert(void)
{
    if (!cursorlock_owns()) return;
    RECT w = g_want;
    if (w.right > w.left && w.bottom > w.top) real_clip(&w);
}

bool cursorlock_hide_wanted(void)
{
    if (g_hide >= 0) return g_hide != 0;
    return _stricmp(title_current(), "FrontMissionOnline.dll") == 0;
}

// Asked by the game window's WM_SETCURSOR and by hook_SetCursor. Only while the
// lock is engaged, so Alt+Tab, a frame drag or a minimise always shows it again.
bool cursorlock_hide_now(void)
{
    return cursorlock_owns() && cursorlock_hide_wanted();
}

void cursorlock_configure(const wchar_t* ini)
{
    g_enable = ini_int_title(L"dx", L"cursor_lock", 1, ini);
    g_hide   = ini_int_title(L"dx", L"cursor_hide", -1, ini);
    g_wine = under_gamescope();
    if (g_wine) {
        logf("[lock] gamescope session -- the pointer lock stands down (the game is the "
             "whole screen; there is no desktop to click away to)");
        return;
    }
    if (!g_thread) {
        g_thread = CreateThread(NULL, 0, watcher, NULL, 0, NULL);
        if (!g_thread)
            logf("[lock] CreateThread failed (%lu) -- the pointer is NOT held", GetLastError());
    }
    logf("[lock] cursor_lock=%d", g_enable);
}

void cursorlock_reload(const wchar_t* ini)
{
    int was = g_enable;
    g_enable = ini_int_title(L"dx", L"cursor_lock", 1, ini);
    g_hide   = ini_int_title(L"dx", L"cursor_hide", -1, ini);
    if (was != g_enable)
        logf("[lock] cursor_lock %d -> %d (live)", was, g_enable);
}

// Signal-only, never joins: called under the loader lock, the same rule as
// maskguard_stop. The clip itself is released here, because a confined pointer
// that outlives the process is the one failure this module must never cause.
void cursorlock_stop(void)
{
    InterlockedExchange(&g_stop, 1);
    if (InterlockedExchange(&g_owned, 0)) real_clip(NULL);
}

void cursorlock_summary(void)
{
    logf("[lock] summary: engaged=%ld released=%ld re-applied=%ld (%s)",
         g_n_engage, g_n_release, g_n_reassert,
         g_wine ? "stood down under gamescope" : g_enable ? "on" : "off");
}

// ---------------------------------------------------------------------------
// selftest -- the decision table
// ---------------------------------------------------------------------------
int cursorlock_selftest(void)
{
    int fail = 0;
    #define CHK(c, m) do { if (!(c)) { logf("[lock] SELFTEST FAIL: %s", m); fail++; } } while (0)

    //                 locked fg     min    modal  inside held
    CHK( cursorlock_decide(false, true,  false, false, true,  false), "in front, pointer over the game: engage");
    CHK(!cursorlock_decide(false, true,  false, false, false, false), "in front, pointer elsewhere: NO yank");
    CHK(!cursorlock_decide(false, true,  false, false, true,  true ), "button held (title-bar click): wait");
    CHK( cursorlock_decide(true,  true,  false, false, false, true ), "engaged mid camera-drag: HOLD");
    CHK(!cursorlock_decide(true,  false, false, false, true,  true ), "Alt+Tab away: release, even mid-drag");
    CHK(!cursorlock_decide(true,  false, false, false, true,  false), "another window in front: release");
    CHK(!cursorlock_decide(true,  true,  true,  false, true,  false), "minimised: release");
    CHK(!cursorlock_decide(true,  true,  false, true,  true,  true ), "dragging the frame: release");
    CHK(!cursorlock_decide(false, false, false, false, true,  false), "not in front: never engage");

    #undef CHK
    if (!fail) logf("[lock] selftest OK");
    return fail;
}
