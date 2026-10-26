// fmofix.cpp -- one-instruction fixes to bugs in SE's own Front Mission Online
// client that no server could have avoided (no-client-behavior-patches: a code
// change to the client is justified only for a proven client bug the real server
// would have hit too).
//
// ============================================================================
// fix_playdev -- THE VOICE PLAYBACK DEVICE IS CHECKED AGAINST THE RECORDING LIST
// ============================================================================
//
// FMO's voice config (ctor 0x610807E0) builds two device lists: [cfg+4] from
// DirectSoundEnumerateA (playback) and [cfg+8] from DirectSoundCaptureEnumerateA
// (recording). Each saved device is read back by a reader that parses the saved
// index and accepts it only if the list entry at that index has the saved name:
//   recording  0x6107FA50: VoiceChatRecDevNo  vs [cfg+8] at 0x6107FBBC -- right
//   playback   0x6107F770: VoiceChatPlayDevNo vs [cfg+8] at 0x6107F8DD -- WRONG
// The playback reader compares against the RECORDING list. With 2005 sound cards,
// which named both directions the same, the wrong list held the right name. On
// any machine since ("Speakers (X)" / "Microphone (X)", Proton's "PulseAudio
// Output" / "PulseAudio Input") it never matches: the playback setting is always
// rejected, the settings box reads back EMPTY, and the main-menu mic test prints
// "TkVoiceConfig[ 0 , -1 ]" and never starts (live 2026-10-26, Windows and Deck).
//
// The fix is the displacement of that one load: [ecx+8] -> [ecx+4]. The whole
// 13-byte sequence around it is verified before the byte is written, so another
// build is left alone. [fmovoice] fix_playdev=0 turns it off.
#include "polshim.h"

#define FMO_MODULE      "FrontMissionOnline.dll"
#define FMO_IMAGE_BASE  0x61000000u

// 8B 0D C0 78 3B 61   mov ecx,[0x613B78C0]   (the config object)
// 8B 41 08            mov eax,[ecx+8]        <- the recording list; want +4
// 8B 74 D0 04         mov esi,[eax+edx*8+4]
#define VA_PLAYDEV      0x6107F8D7u
static const BYTE PLAYDEV_ORIG[13] = { 0x8B, 0x0D, 0xC0, 0x78, 0x3B, 0x61,
                                       0x8B, 0x41, 0x08, 0x8B, 0x74, 0xD0, 0x04 };
#define PLAYDEV_FIX_OFF 8               // the disp8 of `mov eax,[ecx+8]`
#define PLAYDEV_FIX_VAL 0x04

static int   g_playdev = 1;
static BYTE* g_base = NULL;
static int   g_done = 0;
static int   g_said = 0;

int fmofix_enabled() { return g_playdev; }

void fmofix_init(const wchar_t* ini)
{
    g_playdev = GetPrivateProfileIntW(L"fmovoice", L"fix_playdev", 1, ini);
}

void fmofix_apply_module(void* module_base, const char* module)
{
    if (!g_playdev || !module_base || !module) return;
    if (_stricmp(module, FMO_MODULE) != 0) return;
    BYTE* p = (BYTE*)module_base + (VA_PLAYDEV - FMO_IMAGE_BASE);
    if ((BYTE*)module_base != g_base) { g_base = (BYTE*)module_base; g_done = 0; g_said = 0; }
    if (IsBadReadPtr(p, sizeof(PLAYDEV_ORIG))) return;
    if (g_done && p[PLAYDEV_FIX_OFF] == PLAYDEV_FIX_VAL) return;
    g_done = 0;                         // a reload at the same base: patch again
    BYTE want[sizeof(PLAYDEV_ORIG)];
    memcpy(want, PLAYDEV_ORIG, sizeof(want));
    want[PLAYDEV_FIX_OFF] = PLAYDEV_FIX_VAL;
    if (memcmp(p, want, sizeof(want)) == 0) { g_done = 1; return; }
    if (memcmp(p, PLAYDEV_ORIG, sizeof(PLAYDEV_ORIG)) != 0) {
        int blank = 1;
        for (size_t k = 0; k < sizeof(PLAYDEV_ORIG); k++) if (p[k]) { blank = 0; break; }
        if (!blank && !g_said) {        // not unpacked yet is normal; anything else is a build we did not measure
            g_said = 1;
            logf("[fmofix] fix_playdev: %s+0x%X holds other bytes -- NOT patched (different build?)",
                 FMO_MODULE, VA_PLAYDEV - FMO_IMAGE_BASE);
        }
        return;
    }
    DWORD old;
    if (!VirtualProtect(p + PLAYDEV_FIX_OFF, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    p[PLAYDEV_FIX_OFF] = PLAYDEV_FIX_VAL;
    VirtualProtect(p + PLAYDEV_FIX_OFF, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p + PLAYDEV_FIX_OFF, 1);
    g_done = p[PLAYDEV_FIX_OFF] == PLAYDEV_FIX_VAL;
    logf("[fmofix] fix_playdev: the voice playback device is now checked against the "
         "playback list (%s+0x%X: [ecx+8] -> [ecx+4]) -- %s",
         FMO_MODULE, VA_PLAYDEV + PLAYDEV_FIX_OFF - FMO_IMAGE_BASE,
         g_done ? "patched" : "WRITE DID NOT STICK");
    log_flush();
}
