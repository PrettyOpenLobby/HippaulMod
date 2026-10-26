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
//
// ============================================================================
// mic_boost -- FMO PLAYS THE RAW MICROPHONE WITH NO GAIN
// ============================================================================
//
// sqVOICE's capture read (0x612439A0) copies the DirectSoundCapture block into
// its queue and then scales it in place, sample / 100 * mic level (0x61243AFE):
// the slider at 10 is x1, it can only cut. Nothing after it adds gain either
// (mix 0x61244A00 and pump 0x61244870 copy, the output slider at 10 is 0 dB).
// 2005 headset mics were loud at the source; a modern mic without the AGC every
// voice app applies peaks at 2,000-9,000 of 32,767 for normal speech, and the
// loopback test is barely audible (live 2026-10-26, Deck and Windows, all
// sliders at 10). So the block is multiplied by [fmovoice] mic_boost (default 4,
// +12 dB, clamped to 16-bit) just before that loop: the 8 bytes at 0x61243AED,
//   8B 7C 24 24   mov edi,[esp+0x24]   (bytes in the block)
//   8B 4C 24 28   mov ecx,[esp+0x28]   (the block)
// become a jmp to a stub that runs them, boosts, and jumps back to 0x61243AF5.
// No code jumps into those 8 bytes. What others hear is louder too, because the
// boost lands before the encoder. mic_boost=1 (or 0) leaves the client alone.
//
// ============================================================================
// vol_curve -- THE VOICE OUTPUT SLIDER IS -10 dB PER STEP
// ============================================================================
//
// sqVOICE SetVolume (0x61244170) turns the 0..100 slider value into DirectSound
// volume as (v-100)*100 hundredths of a dB, so 9 is -10 dB, 7 (the default) is
// -30 dB and only 10 is usable. With [fmovoice] vol_curve=1 (default) the 15 bytes
// at 0x612441A8,
//   8D 8C B6 0C FE FF FF   lea ecx,[esi+esi*4-0x1F4]
//   8B 10                  mov edx,[eax]
//   8D 0C 89 / C1 E1 02    lea ecx,[ecx+ecx*4] / shl ecx,2
// become a load from a 101-entry table, mov ecx,[esi*4+table] / mov edx,[eax]
// plus nops, holding the usual audio taper 40*log10(v/100) dB: 9 -1.8 dB,
// 7 -6 dB, 5 -12 dB, 1 -40 dB, 0 silent. esi is already 0..100 there.
#include <math.h>
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

#define VA_BOOST        0x61243AEDu
#define VA_BOOST_BACK   0x61243AF5u
static const BYTE BOOST_ORIG[8] = { 0x8B, 0x7C, 0x24, 0x24, 0x8B, 0x4C, 0x24, 0x28 };

#define VA_VOLCURVE     0x612441A8u
static const BYTE VOLCURVE_ORIG[15] = { 0x8D, 0x8C, 0xB6, 0x0C, 0xFE, 0xFF, 0xFF, 0x8B, 0x10,
                                        0x8D, 0x0C, 0x89, 0xC1, 0xE1, 0x02 };
static int   g_volcurve = 1;
static LONG* g_voltab = NULL;
static BYTE* g_vol_base = NULL;
static int   g_vol_said = 0;

static int   g_playdev = 1;
static BYTE* g_base = NULL;
static int   g_done = 0;
static int   g_said = 0;

static int   g_boost_x100 = 400;        // mic_boost * 100
static BYTE* g_boost_base = NULL;
static BYTE* g_stub = NULL;
static int   g_boost_said = 0;

int fmofix_enabled() { return g_playdev || g_boost_x100 > 100 || g_volcurve; }

void fmofix_init(const wchar_t* ini)
{
    g_playdev = GetPrivateProfileIntW(L"fmovoice", L"fix_playdev", 1, ini);
    g_volcurve = GetPrivateProfileIntW(L"fmovoice", L"vol_curve", 1, ini);
    wchar_t v[32];
    GetPrivateProfileStringW(L"fmovoice", L"mic_boost", L"4", v, 32, ini);
    double b = _wtof(v);
    if (b > 16.0) b = 16.0;
    g_boost_x100 = (int)(b * 100.0 + 0.5);
}

// cdecl, called from the stub with the block and its sample count
static void __cdecl mic_boost_block(short* pcm, unsigned n)
{
    int k = g_boost_x100;
    for (unsigned i = 0; i < n; i++) {
        int v = pcm[i] * k / 100;
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        pcm[i] = (short)v;
    }
}

static void micboost_apply(BYTE* base)
{
    if (g_boost_x100 <= 100) return;
    BYTE* p = base + (VA_BOOST - FMO_IMAGE_BASE);
    if (base != g_boost_base) { g_boost_base = base; g_boost_said = 0; }
    if (IsBadReadPtr(p, sizeof(BOOST_ORIG))) return;
    if (p[0] == 0xE9) return;                           // ours already
    if (memcmp(p, BOOST_ORIG, sizeof(BOOST_ORIG)) != 0) {
        int blank = 1;
        for (size_t k = 0; k < sizeof(BOOST_ORIG); k++) if (p[k]) { blank = 0; break; }
        if (!blank && !g_boost_said) {
            g_boost_said = 1;
            logf("[fmofix] mic_boost: %s+0x%X holds other bytes -- NOT patched (different build?)",
                 FMO_MODULE, VA_BOOST - FMO_IMAGE_BASE);
        }
        return;
    }
    if (!g_stub) {
        g_stub = (BYTE*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_stub) return;
    }
    BYTE* s = g_stub;
    memcpy(s, BOOST_ORIG, 8); s += 8;                   // the two loads
    *s++ = 0x60;                                        // pushad
    *s++ = 0x9C;                                        // pushfd
    *s++ = 0x8B; *s++ = 0xC7;                           // mov eax,edi
    *s++ = 0xD1; *s++ = 0xE8;                           // shr eax,1   (samples)
    *s++ = 0x50;                                        // push eax
    *s++ = 0x51;                                        // push ecx
    *s++ = 0xB8; *(DWORD*)s = (DWORD)(UINT_PTR)&mic_boost_block; s += 4;
    *s++ = 0xFF; *s++ = 0xD0;                           // call eax
    *s++ = 0x83; *s++ = 0xC4; *s++ = 0x08;              // add esp,8
    *s++ = 0x9D;                                        // popfd
    *s++ = 0x61;                                        // popad
    BYTE* back = base + (VA_BOOST_BACK - FMO_IMAGE_BASE);
    *s++ = 0xE9; *(LONG*)s = (LONG)(back - (s + 4)); s += 4;
    FlushInstructionCache(GetCurrentProcess(), g_stub, s - g_stub);

    BYTE jmp[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    *(LONG*)(jmp + 1) = (LONG)(g_stub - (p + 5));
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(p, jmp, 8);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    logf("[fmofix] mic_boost: voice capture x%d.%02d before FMO's own mic level (%s+0x%X -> stub %p)",
         g_boost_x100 / 100, g_boost_x100 % 100, FMO_MODULE, VA_BOOST - FMO_IMAGE_BASE, g_stub);
    log_flush();
}

static void volcurve_apply(BYTE* base)
{
    if (!g_volcurve) return;
    BYTE* p = base + (VA_VOLCURVE - FMO_IMAGE_BASE);
    if (base != g_vol_base) { g_vol_base = base; g_vol_said = 0; }
    if (IsBadReadPtr(p, sizeof(VOLCURVE_ORIG))) return;
    if (p[0] == 0x8B && p[1] == 0x0C && p[2] == 0xB5) return;     // ours already
    if (memcmp(p, VOLCURVE_ORIG, sizeof(VOLCURVE_ORIG)) != 0) {
        int blank = 1;
        for (size_t k = 0; k < sizeof(VOLCURVE_ORIG); k++) if (p[k]) { blank = 0; break; }
        if (!blank && !g_vol_said) {
            g_vol_said = 1;
            logf("[fmofix] vol_curve: %s+0x%X holds other bytes -- NOT patched (different build?)",
                 FMO_MODULE, VA_VOLCURVE - FMO_IMAGE_BASE);
        }
        return;
    }
    if (!g_voltab) {
        g_voltab = (LONG*)VirtualAlloc(NULL, 101 * sizeof(LONG), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_voltab) return;
        g_voltab[0] = -10000;
        for (int v = 1; v <= 100; v++) {
            double db100 = 4000.0 * log10(v / 100.0);
            g_voltab[v] = db100 < -10000.0 ? -10000 : (LONG)floor(db100 + 0.5);
        }
    }
    BYTE code[15] = { 0x8B, 0x0C, 0xB5, 0, 0, 0, 0, 0x8B, 0x10,
                      0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    *(DWORD*)(code + 3) = (DWORD)(UINT_PTR)g_voltab;
    DWORD old;
    if (!VirtualProtect(p, sizeof(code), PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(p, code, sizeof(code));
    VirtualProtect(p, sizeof(code), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof(code));
    logf("[fmofix] vol_curve: voice output slider now 40*log10 (9 %ld, 7 %ld, 5 %ld, 1 %ld hundredths dB) "
         "instead of -10 dB per step (%s+0x%X)", g_voltab[90], g_voltab[70], g_voltab[50], g_voltab[10],
         FMO_MODULE, VA_VOLCURVE - FMO_IMAGE_BASE);
    log_flush();
}

static void playdev_apply(void* module_base);

void fmofix_apply_module(void* module_base, const char* module)
{
    if (!module_base || !module) return;
    if (_stricmp(module, FMO_MODULE) != 0) return;
    micboost_apply((BYTE*)module_base);
    volcurve_apply((BYTE*)module_base);
    if (g_playdev) playdev_apply(module_base);
}

static void playdev_apply(void* module_base)
{
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
