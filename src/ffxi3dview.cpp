// ffxi3dview.cpp -- how FFXI's 3D mode is PRESENTED, once [ffxi] stereo3d has
// switched it on (regredir.cpp's ffxi3d block answers 0030=1).
//
// WHAT FFXI DOES IN 3D MODE (measured 2026-10-03 from FFXiMain.dll unpacked in
// memory; addresses are that build's, the code below finds everything by pattern)
//
//   * Every draw is issued TWICE, once per eye (renderer+0x810 = eye index), into
//     two full render targets. The eye offset is a horizontal shift in the
//     projection matrix, _41 = -/+ a float at 0x1034ffa0 (0.006), set by 0x1000cf20.
//   * 0x1000c0b0 (__thiscall, 4 args) composites the two eye textures half width,
//     side by side, into a buffer the size of the screen, then blits that to the
//     back buffer. Its one caller is at 0x10012745.
//   * 0x1000bb90 (__thiscall, 1 arg) draws a 10px strip along the bottom edge on
//     alternate frames: a sync tag for the 2003 panel the mode was built for. On
//     any modern display it is a flickering bar. Its one caller is at 0x10012aea.
//   * The 1024x2 column masks the renderer also builds are never sampled. Nothing
//     in the game is fixed at 1024x768; that rule lives only in the config app.
//
// WHAT THIS FILE CHANGES -- two CALL SITES, the way fmowanz.cpp does it: no bytes
// stolen from a prologue, no trampoline. The composite still runs in full, so the
// game's own state handling is untouched; for the layouts that need it we then
// draw over the same rectangle from the two eye textures, inside a state block,
// so the device is left exactly as the game expects. The strip is skipped unless
// the layout is "original".
//
// Display-only and opt-in. It changes nothing about how the game plays.
//
// CONFIG ([ffxi], live on Save; the patches go in when FFXiMain.dll loads with
// stereo3d=1, because the game reads 0030 only at startup anyway):
//   stereo3d_layout  sbs (default) | tab | rows | anaglyph | original
//   stereo3d_swap    1 = swap the eyes
//   stereo3d_depth   low | normal | high  (0.5x / 1x / 2x the game's eye shift)

#include "polshim.h"

enum { LAY_ORIGINAL = 0, LAY_SBS, LAY_TAB, LAY_ROWS, LAY_ANAGLYPH };

static volatile LONG g_layout = LAY_SBS;
static volatile LONG g_swap   = 0;
static volatile LONG g_depth  = 100;       // percent of the game's own eye shift

// The eye-shift constant, once found, and the value the game shipped with.
static float* volatile g_sep = NULL;
static float           g_sep_orig = 0.0f;

static const char* lay_name(LONG l)
{
    switch (l) {
    case LAY_ORIGINAL: return "original";
    case LAY_SBS:      return "side by side";
    case LAY_TAB:      return "top and bottom";
    case LAY_ROWS:     return "alternating lines";
    case LAY_ANAGLYPH: return "red/cyan";
    }
    return "?";
}

static LONG parse_layout(const wchar_t* v)
{
    if (!_wcsicmp(v, L"tab"))      return LAY_TAB;
    if (!_wcsicmp(v, L"rows"))     return LAY_ROWS;
    if (!_wcsicmp(v, L"anaglyph")) return LAY_ANAGLYPH;
    if (!_wcsicmp(v, L"original")) return LAY_ORIGINAL;
    return LAY_SBS;
}

static LONG parse_depth(const wchar_t* v)
{
    if (!_wcsicmp(v, L"low"))  return 50;
    if (!_wcsicmp(v, L"high")) return 200;
    return 100;
}

static void apply_depth(void)
{
    float* p = g_sep;
    if (!p) return;
    float want = g_sep_orig * (float)g_depth / 100.0f;
    DWORD old;
    if (!VirtualProtect(p, sizeof(float), PAGE_READWRITE, &old)) return;
    LONG bits; memcpy(&bits, &want, 4);
    InterlockedExchange((volatile LONG*)p, bits);     // 4-aligned global: one store
    VirtualProtect(p, sizeof(float), old, &old);
}

void ffxi3dview_reload(const wchar_t* ini)
{
    wchar_t v[32];
    // ffxi_ini_str: the dialog's FFXI section saves these as [ffxi.FFXiMain.dll]
    // overrides, so they are read there first.
    ffxi_ini_str(L"stereo3d_layout", L"sbs", v, _countof(v), ini);
    LONG lay = parse_layout(v);
    ffxi_ini_str(L"stereo3d_depth", L"normal", v, _countof(v), ini);
    LONG dep = parse_depth(v);
    ffxi_ini_str(L"stereo3d_swap", L"0", v, _countof(v), ini);
    LONG swp = wcstol(v, NULL, 10) ? 1 : 0;

    LONG ol = InterlockedExchange(&g_layout, lay);
    LONG os = InterlockedExchange(&g_swap, swp);
    LONG od = InterlockedExchange(&g_depth, dep);
    if (ol != lay || os != swp || od != dep)
        logf("[ffxi3d] presentation: %s%s, depth %ld%%", lay_name(lay),
             swp ? ", eyes swapped" : "", dep);
    apply_depth();
}

// ---------------------------------------------------------------------------
// the draw. IDirect3DDevice8 by vtable slot; every slot used here is one the
// client itself calls (counted in its code: SetRenderState 50, SetTexture 61,
// SetTextureStageState 63, SetVertexShader 76, DrawPrimitiveUP 72, the state
// block calls 54/56/57, SetPixelShader 88), on the client's own device.
// ---------------------------------------------------------------------------
#define DV_ApplyStateBlock      54
#define DV_DeleteStateBlock     56
#define DV_CreateStateBlock     57
#define DV_SetRenderState       50
#define DV_SetTexture           61
#define DV_SetTextureStageState 63
#define DV_DrawPrimitiveUP      72
#define DV_SetVertexShader      76
#define DV_SetPixelShader       88

typedef HRESULT (__stdcall *PFN_U2)(void*, DWORD, DWORD);
typedef HRESULT (__stdcall *PFN_U3)(void*, DWORD, DWORD, DWORD);
typedef HRESULT (__stdcall *PFN_U1)(void*, DWORD);
typedef HRESULT (__stdcall *PFN_SetTex)(void*, DWORD, void*);
typedef HRESULT (__stdcall *PFN_CreateSB)(void*, DWORD, DWORD*);
typedef HRESULT (__stdcall *PFN_DrawUP)(void*, DWORD, UINT, const void*, UINT);

#define VSLOT(dev, n) ((*(void***)(dev))[n])

struct V3D { float x, y, z, rhw; DWORD c; float u, v; };
#define FVF_XYZRHW_DIFFUSE_TEX1 0x144

static void rs(void* d, DWORD s, DWORD v)  { ((PFN_U2)VSLOT(d, DV_SetRenderState))(d, s, v); }
static void tss(void* d, DWORD st, DWORD s, DWORD v)
{ ((PFN_U3)VSLOT(d, DV_SetTextureStageState))(d, st, s, v); }
static void tex(void* d, DWORD st, void* t) { ((PFN_SetTex)VSLOT(d, DV_SetTexture))(d, st, t); }

static void quad(void* d, void* t, float x0, float y0, float x1, float y1,
                 float v0, float v1)
{
    // -0.5: map texel centres onto pixel centres, as the game's own blit does.
    V3D q[4] = {
        { x0 - 0.5f, y0 - 0.5f, 0, 1, 0xFFFFFFFF, 0, v0 },
        { x1 - 0.5f, y0 - 0.5f, 0, 1, 0xFFFFFFFF, 1, v0 },
        { x0 - 0.5f, y1 - 0.5f, 0, 1, 0xFFFFFFFF, 0, v1 },
        { x1 - 0.5f, y1 - 0.5f, 0, 1, 0xFFFFFFFF, 1, v1 },
    };
    tex(d, 0, t);
    ((PFN_DrawUP)VSLOT(d, DV_DrawPrimitiveUP))(d, 5 /*TRIANGLESTRIP*/, 2, q, sizeof(V3D));
}

// One quad per line of the second eye. Static: up to 4096 lines, half of them.
#define ROWS_MAX 2048
static V3D g_rows[ROWS_MAX * 6];

static void rows(void* d, void* t, float x0, float x1, int y0, int y1, int parity)
{
    int n = 0;
    float h = (float)(y1 - y0);
    for (int y = y0; y < y1 && n < ROWS_MAX; y++) {
        if ((y & 1) != parity) continue;
        float a = (float)y - 0.5f, b = a + 1.0f;
        float va = (float)(y - y0) / h, vb = (float)(y + 1 - y0) / h;
        V3D* q = &g_rows[n * 6];
        V3D p0 = { x0 - 0.5f, a, 0, 1, 0xFFFFFFFF, 0, va };
        V3D p1 = { x1 - 0.5f, a, 0, 1, 0xFFFFFFFF, 1, va };
        V3D p2 = { x0 - 0.5f, b, 0, 1, 0xFFFFFFFF, 0, vb };
        V3D p3 = { x1 - 0.5f, b, 0, 1, 0xFFFFFFFF, 1, vb };
        q[0] = p0; q[1] = p1; q[2] = p2;
        q[3] = p2; q[4] = p1; q[5] = p3;
        n++;
    }
    if (!n) return;
    tex(d, 0, t);
    ((PFN_DrawUP)VSLOT(d, DV_DrawPrimitiveUP))(d, 4 /*TRIANGLELIST*/, (UINT)n * 2,
                                              g_rows, sizeof(V3D));
}

static LONG g_drawlog = 0;
static volatile LONG g_uiblit_seen = 0;    // UI layer pasted per eye this launch
static volatile LONG g_composites  = 0;    // 3D frames this launch

// A plain opaque textured draw, whatever the game had set. Caller holds a state
// block around it.
static void plain_states(void* dev)
{
    ((PFN_U1)VSLOT(dev, DV_SetVertexShader))(dev, FVF_XYZRHW_DIFFUSE_TEX1);
    ((PFN_U1)VSLOT(dev, DV_SetPixelShader))(dev, 0);
    rs(dev, 7, 0);   rs(dev, 14, 0);              // ZENABLE, ZWRITEENABLE
    rs(dev, 15, 0);  rs(dev, 27, 0);              // ALPHATEST, ALPHABLEND
    rs(dev, 22, 1);  rs(dev, 28, 0);              // CULL NONE, FOG off
    rs(dev, 52, 0);  rs(dev, 137, 0);             // STENCIL off, LIGHTING off
    rs(dev, 168, 0xF);                            // COLORWRITEENABLE all
    tss(dev, 0, 1, 2);  tss(dev, 0, 2, 2);        // COLOROP SELECTARG1, ARG1 TEXTURE
    tss(dev, 0, 4, 2);  tss(dev, 0, 5, 2);        // ALPHAOP SELECTARG1, ARG1 TEXTURE
    tss(dev, 0, 11, 0); tss(dev, 0, 24, 0);       // TEXCOORDINDEX 0, no tex transform
    tss(dev, 0, 13, 3); tss(dev, 0, 14, 3);       // ADDRESS U/V CLAMP
    tss(dev, 0, 16, 2); tss(dev, 0, 17, 2); tss(dev, 0, 18, 0);  // LINEAR, no mip
    tss(dev, 1, 1, 1);  tss(dev, 1, 4, 1);        // stage 1 off
    tex(dev, 1, NULL);
}

static void present(void* dev, void* left, void* right, const int* r, LONG lay)
{
    DWORD sb = 0;
    if (FAILED(((PFN_CreateSB)VSLOT(dev, DV_CreateStateBlock))(dev, 1 /*D3DSBT_ALL*/, &sb)))
        return;
    plain_states(dev);

    float x0 = (float)r[0], y0 = (float)r[1], x1 = (float)r[2], y1 = (float)r[3];
    float xm = (x0 + x1) * 0.5f, ym = (float)((r[1] + r[3]) / 2);

    switch (lay) {
    case LAY_SBS:
        quad(dev, left,  x0, y0, xm, y1, 0, 1);
        quad(dev, right, xm, y0, x1, y1, 0, 1);
        break;
    case LAY_TAB:
        quad(dev, left,  x0, y0, x1, ym, 0, 1);
        quad(dev, right, x0, ym, x1, y1, 0, 1);
        break;
    case LAY_ROWS:
        // Even screen lines carry the left eye, odd lines the right; "swap eyes"
        // flips it for panels wired the other way round.
        quad(dev, left, x0, y0, x1, y1, 0, 1);
        rows(dev, right, x0, x1, r[1], r[3], 1);
        break;
    case LAY_ANAGLYPH:
        rs(dev, 168, 0x1);                        // red <- left
        quad(dev, left,  x0, y0, x1, y1, 0, 1);
        rs(dev, 168, 0x6);                        // green+blue <- right
        quad(dev, right, x0, y0, x1, y1, 0, 1);
        break;
    }

    ((PFN_U1)VSLOT(dev, DV_ApplyStateBlock))(dev, sb);
    ((PFN_U1)VSLOT(dev, DV_DeleteStateBlock))(dev, sb);

    if (InterlockedIncrement(&g_drawlog) == 1)
        logf("[ffxi3d] first 3D frame presented as %s, rect %d,%d-%d,%d",
             lay_name(lay), r[0], r[1], r[2], r[3]);
}

// ---------------------------------------------------------------------------
// the two retargeted calls. __thiscall callee-cleans its stack args, and so does
// __fastcall; ecx is `this` in both and edx is ignored.
// ---------------------------------------------------------------------------
typedef void (__fastcall *PFN_COMPOSITE)(void*, void*, void**, DWORD, int*, int*);
typedef void (__fastcall *PFN_TAGSTRIP)(void*, void*, int*);
static PFN_COMPOSITE g_composite = NULL;
static PFN_TAGSTRIP  g_tagstrip  = NULL;

static void __fastcall my_composite(void* self, void* edx, void** texs, DWORD color,
                                    int* rect, int* rect2)
{
    g_composite(self, edx, texs, color, rect, rect2);

    // The UI duplication needs the menu buffer. If it never shows up, say so once
    // rather than leave "the menus are still flat" unexplained.
    if (InterlockedIncrement(&g_composites) == 300 && !g_uiblit_seen &&
        (g_layout == LAY_SBS || g_layout == LAY_TAB))
        logf("[ffxi3d] 300 3D frames and the UI layer was never pasted separately -- "
             "the menu buffer is missing (created before the patch landed?), so the "
             "UI stays a single full-width layer");

    LONG lay = g_layout;
    if (lay == LAY_ORIGINAL || (lay == LAY_SBS && !g_swap)) return;   // game's own output
    if (!self || !texs || !rect) return;
    void* dev = *(void**)((char*)self + 0xC);      // the composite's own [esi+0xC]
    void* l = texs[1];                              // eye 0, drawn left by the game
    void* r = texs[2];
    if (!dev || !l || !r) return;
    if (g_swap) { void* t = l; l = r; r = t; }
    present(dev, l, r, rect, lay);
}

static void __fastcall my_tagstrip(void* self, void* edx, int* rect)
{
    if (g_layout == LAY_ORIGINAL) g_tagstrip(self, edx, rect);
}

// ---------------------------------------------------------------------------
// THE UI. FFXI draws menus, chat and the HUD AFTER the eye composite, so in a
// side-by-side or top-and-bottom picture they straddle the two halves and each
// eye gets half a menu. When the game has a separate menu buffer (renderer+0x1E0,
// made when the menu resolution differs from the window's) the whole UI is first
// drawn into that texture -- over a copy of the left-eye world, with alpha 0
// where no UI was drawn -- and 0x1000abe0 then pastes it onto the screen with an
// alpha test (alpha > 0, no blending). For sbs/tab we paste it twice instead,
// squashed into each half, with the same test. Semi-transparent windows show the
// left eye's world behind them in both halves: a flat pane at screen depth.
//
// The menu buffer is forced to exist (below) so this path is always there.
// ---------------------------------------------------------------------------
typedef void (__fastcall *PFN_UIBLIT)(void*, void*, void*, DWORD);
static PFN_UIBLIT g_uiblit = NULL;

#define DV_GetViewport 41
struct VP8 { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
typedef HRESULT (__stdcall *PFN_GetVP)(void*, VP8*);

static void __fastcall my_uiblit(void* self, void* edx, void* uitex, DWORD alpha)
{
    LONG lay = g_layout;
    void* dev = self ? *(void**)((char*)self + 0xC) : NULL;
    if ((lay != LAY_SBS && lay != LAY_TAB) || !dev || !uitex) {
        g_uiblit(self, edx, uitex, alpha);
        return;
    }
    VP8 vp = {};
    if (FAILED(((PFN_GetVP)VSLOT(dev, DV_GetViewport))(dev, &vp)) || !vp.Width || !vp.Height) {
        g_uiblit(self, edx, uitex, alpha);
        return;
    }
    DWORD sb = 0;
    if (FAILED(((PFN_CreateSB)VSLOT(dev, DV_CreateStateBlock))(dev, 1, &sb))) {
        g_uiblit(self, edx, uitex, alpha);
        return;
    }
    plain_states(dev);
    rs(dev, 15, 1);  rs(dev, 24, 0);  rs(dev, 25, 5);   // ALPHATEST on, REF 0, GREATER

    float x0 = (float)vp.X, y0 = (float)vp.Y;
    float x1 = x0 + vp.Width, y1 = y0 + vp.Height;
    if (lay == LAY_SBS) {
        float xm = (x0 + x1) * 0.5f;
        quad(dev, uitex, x0, y0, xm, y1, 0, 1);
        quad(dev, uitex, xm, y0, x1, y1, 0, 1);
    } else {
        float ym = (y0 + y1) * 0.5f;
        quad(dev, uitex, x0, y0, x1, ym, 0, 1);
        quad(dev, uitex, x0, ym, x1, y1, 0, 1);
    }
    ((PFN_U1)VSLOT(dev, DV_ApplyStateBlock))(dev, sb);
    ((PFN_U1)VSLOT(dev, DV_DeleteStateBlock))(dev, sb);

    if (InterlockedIncrement(&g_uiblit_seen) == 1)
        logf("[ffxi3d] UI layer drawn once per eye (%s, %lux%lu)", lay_name(lay),
             vp.Width, vp.Height);
}

// ---------------------------------------------------------------------------
// finding things. Function bodies by pattern, then the ONE call to each.
// ---------------------------------------------------------------------------
static const char* const PAT_COMPOSITE =   // sub esp,84h; push esi; mov esi,ecx;
    "81EC84000000568BF18B0D????????E8????????3C010F85";   // mov ecx,[stereo]; call IsEnabled; cmp al,1; jne
static const char* const PAT_TAGSTRIP =
    "A0????????56A8018BF10F85????????0C01B980808080";
static const char* const PAT_EYESHIFT =    // eye 1: mov eax,[sep] ... eye 0: fld [sep]; fchs
    "8B861008000083E800740E487517A1????????89442434EB0CD905????????D9E0";
// The UI-layer paste (0x1000abe0). Its opening is shared with a look-alike blit at
// 0x1000aab0, so it is found by what only it does -- ALPHABLENDENABLE 0, then
// ALPHAFUNC GREATER, ALPHAREF 0 -- and the function start is the nearest
// `push esi; mov esi,ecx; call` before that.
static const char* const PAT_UIBLIT_BODY =
    "6A006A1B508B08FF91C80000008B460C6A056A19508B10FF92C80000008B460C6A006A18508B08FF91C8000000";
#define UIBLIT_BACK_MAX 0x140

static const unsigned char* find_uiblit(const unsigned char* base, size_t size)
{
    const unsigned char* body = pol_pattern_find(base, size, PAT_UIBLIT_BODY);
    if (!body || body - base < UIBLIT_BACK_MAX) return NULL;
    for (const unsigned char* p = body - 4; p >= body - UIBLIT_BACK_MAX; p--)
        if (p[0] == 0x56 && p[1] == 0x8B && p[2] == 0xF1 && p[3] == 0xE8) return p;
    return NULL;
}
// Renderer setup's menu-buffer decision (0x10011680): menu w/h zero -> skip;
// menu size == window size -> skip (the final `je`, at +32). That last skip is
// the one removed, so a menu buffer exists even at equal sizes.
static const char* const PAT_MENUCHK =
    "668B4614663BC30F84????????668B4E16663BCB74??663B46107506663B4E1274??";
#define MENUCHK_JE_OFS 32

// .text is EMPTY on disk (the POL1 stub fills it in DllMain), so "is it unpacked
// yet" is one cheap read -- which lets the poller run fast enough to land the
// menu-buffer patch before the renderer is built, without a 12 MB scan per tick.
static bool text_unpacked(const unsigned char* base)
{
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        // MEM_EXECUTE, not CNT_CODE: the packer marks .text as executable
        // UNINITIALIZED data (0xE0000080), with no code flag at all.
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const unsigned char* p = base + s->VirtualAddress;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        for (int k = 0; k < 64; k++) if (p[k]) return true;
        return false;
    }
    return false;
}

static unsigned char* find_one_call(const unsigned char* base, size_t size,
                                    const unsigned char* target, int* count)
{
    unsigned char* hit = NULL;
    *count = 0;
    const unsigned char* p = base;
    const unsigned char* end = base + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        const unsigned char* rs0 = (const unsigned char*)mbi.BaseAddress;
        const unsigned char* re  = rs0 + mbi.RegionSize;
        if (re > end) re = end;
        bool exec = (mbi.State == MEM_COMMIT) &&
                    (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
        if (exec) {
            for (const unsigned char* q = p; q + 5 <= re; q++) {
                if (*q != 0xE8) continue;
                LONG rel; memcpy(&rel, q + 1, 4);
                if (q + 5 + rel == target) { hit = (unsigned char*)q; (*count)++; }
            }
        }
        if (re <= p) break;
        p = re;
    }
    return hit;
}

static bool retarget(unsigned char* site, void* to)
{
    DWORD old;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    LONG rel = (LONG)((unsigned char*)to - (site + 5));
    memcpy(site + 1, &rel, 4);
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

// Returns true when it is DONE (installed, or cannot ever install in this image).
static bool try_install(void)
{
    const unsigned char* base = NULL; size_t size = 0;
    if (!pol_module_range("FFXiMain.dll", &base, &size)) return false;
    if (!text_unpacked(base)) return false;

    // FIRST, while the race is still winnable: keep the menu buffer. Renderer
    // setup runs once per FFXI launch, soon after load.
    const unsigned char* fm = pol_pattern_find(base, size, PAT_MENUCHK);
    if (fm && fm[MENUCHK_JE_OFS] == 0x74) {
        unsigned char* je = (unsigned char*)fm + MENUCHK_JE_OFS;
        DWORD old;
        if (VirtualProtect(je, 2, PAGE_EXECUTE_READWRITE, &old)) {
            je[0] = 0x90; je[1] = 0x90;
            VirtualProtect(je, 2, old, &old);
            FlushInstructionCache(GetCurrentProcess(), je, 2);
            logf("[ffxi3d] menu buffer kept even at window size (FFXiMain.dll+0x%X), so "
                 "the UI can be drawn once per eye", (unsigned)(je - base));
        }
    } else {
        logf("[ffxi3d] menu-buffer check not found -- the UI is only drawn per eye when "
             "FFXI's menu resolution differs from its window resolution");
    }

    const unsigned char* fc = pol_pattern_find(base, size, PAT_COMPOSITE);
    const unsigned char* ft = pol_pattern_find(base, size, PAT_TAGSTRIP);
    if (!fc || !ft) {
        // .text is unpacked, so a miss now is a different client build, not "too
        // early". Decide once instead of re-patching and re-logging every tick.
        logf("[ffxi3d] NOT installed: composite %s, sync strip %s in this FFXiMain.dll "
             "-- 3D stays the game's own output", fc ? "found" : "MISSING",
             ft ? "found" : "MISSING");
        return true;
    }

    int nc = 0, nt = 0;
    unsigned char* sc = find_one_call(base, size, fc, &nc);
    unsigned char* st = find_one_call(base, size, ft, &nt);
    if (nc != 1 || nt != 1) {
        logf("[ffxi3d] NOT installed: expected one call each to the composite "
             "(found %d) and the sync strip (found %d). This FFXiMain.dll is not "
             "the build this was written against -- 3D stays the game's own output.",
             nc, nt);
        return true;
    }
    g_composite = (PFN_COMPOSITE)fc;
    g_tagstrip  = (PFN_TAGSTRIP)ft;
    if (!retarget(sc, (void*)my_composite) || !retarget(st, (void*)my_tagstrip)) {
        logf("[ffxi3d] NOT installed: could not unprotect the call sites");
        return true;
    }
    logf("[ffxi3d] presentation hooks in: composite FFXiMain.dll+0x%X (called at +0x%X), "
         "sync strip +0x%X (called at +0x%X)",
         (unsigned)(fc - base), (unsigned)(sc - base),
         (unsigned)(ft - base), (unsigned)(st - base));

    const unsigned char* fu = find_uiblit(base, size);
    int nu = 0;
    unsigned char* su = fu ? find_one_call(base, size, fu, &nu) : NULL;
    if (su && nu == 1 && retarget(su, (void*)my_uiblit)) {
        g_uiblit = (PFN_UIBLIT)fu;
        logf("[ffxi3d] UI layer paste +0x%X (called at +0x%X) hooked",
             (unsigned)(fu - base), (unsigned)(su - base));
    } else {
        logf("[ffxi3d] UI layer paste not hooked (found=%d calls=%d) -- the UI stays one "
             "full-width layer", fu ? 1 : 0, nu);
    }

    // Depth: the two reads of the eye-shift constant must name the same address.
    const unsigned char* fe = pol_pattern_find(base, size, PAT_EYESHIFT);
    if (fe) {
        float* a; float* b;
        memcpy(&a, fe + 15, 4);
        memcpy(&b, fe + 27, 4);
        if (a == b && ((ULONG_PTR)a & 3) == 0 &&
            (const unsigned char*)a > base && (const unsigned char*)a < base + size) {
            g_sep_orig = *a;
            g_sep = a;
            logf("[ffxi3d] eye shift at FFXiMain.dll+0x%X = %g",
                 (unsigned)((const unsigned char*)a - base), g_sep_orig);
            apply_depth();
        } else {
            logf("[ffxi3d] eye-shift reads disagree (%p vs %p) -- depth setting ignored", a, b);
        }
    } else {
        logf("[ffxi3d] eye-shift pattern not found -- depth setting ignored");
    }
    return true;
}

static volatile LONG g_running = 0;

// ONE install per loaded FFXiMain image, whoever gets there first: the poller, or
// d3d8hook's CreateDevice. The poller alone lost the race in b195 -- FFXI builds
// its render targets (and decides on the menu buffer) right after CreateDevice
// returns, and a 5 ms poll on another thread is not guaranteed to land first.
// CreateDevice is: FFXI is unpacked by then (it is the caller), and nothing that
// matters has been built yet.
static volatile LONG          g_inst_lock = 0;
static const unsigned char* volatile g_inst_base = NULL;

static bool install_once(void)
{
    const unsigned char* base = NULL; size_t size = 0;
    if (!pol_module_range("FFXiMain.dll", &base, &size)) return false;
    if (g_inst_base == base) return true;
    while (InterlockedCompareExchange(&g_inst_lock, 1, 0)) Sleep(0);
    bool done = (g_inst_base == base);
    if (!done && try_install()) { g_inst_base = base; done = true; }
    InterlockedExchange(&g_inst_lock, 0);
    return done;
}

// From d3d8hook's CreateDevice, BEFORE the real call. Cheap when not FFXI.
void ffxi3dview_before_device(void)
{
    if (!ffxi3d_enabled()) return;
    if (install_once()) return;
    logf("[ffxi3d] CreateDevice: FFXiMain.dll not ready for the 3D hooks yet");
}

static DWORD WINAPI install_thread(LPVOID)
{
    // 5 ms ticks: try_install returns at once until .text is unpacked. The
    // CreateDevice path is the guaranteed one; this covers anything that never
    // reaches it. 12000 x 5 ms = 60 s.
    for (int t = 0; t < 12000; t++) {
        if (install_once()) { InterlockedExchange(&g_running, 0); return 0; }
        Sleep(5);
    }
    logf("[ffxi3d] gave up waiting for FFXiMain.dll's renderer code -- 3D stays the "
         "game's own output");
    InterlockedExchange(&g_running, 0);
    return 0;
}

// Beside ffxicfg_on_module: every module load and the startup sweep.
void ffxi3dview_on_module(void* base)
{
    if (!base || !ffxi3d_enabled()) return;
    char path[MAX_PATH] = "";
    if (!GetModuleFileNameA((HMODULE)base, path, MAX_PATH)) return;
    const char* leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : path;
    if (_stricmp(leaf, "FFXiMain.dll") != 0) return;

    // A fresh image: the old patches went with the old one.
    g_sep = NULL;
    InterlockedExchange(&g_drawlog, 0);
    InterlockedExchange(&g_uiblit_seen, 0);
    InterlockedExchange(&g_composites, 0);
    if (InterlockedExchange(&g_running, 1)) return;
    HANDLE h = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    if (!h) { InterlockedExchange(&g_running, 0); return; }
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// selftest: the parsers, and the call finder against a buffer we build.
// ---------------------------------------------------------------------------
int ffxi3dview_selftest(void)
{
    int fail = 0;
    #define CHK(c, m) do { if (!(c)) { logf("[ffxi3d] SELFTEST FAIL: %s", m); fail++; } } while (0)

    CHK(parse_layout(L"tab") == LAY_TAB, "tab");
    CHK(parse_layout(L"ROWS") == LAY_ROWS, "rows, any case");
    CHK(parse_layout(L"anaglyph") == LAY_ANAGLYPH, "anaglyph");
    CHK(parse_layout(L"original") == LAY_ORIGINAL, "original");
    CHK(parse_layout(L"") == LAY_SBS && parse_layout(L"junk") == LAY_SBS,
        "anything unknown is the game's own side by side");
    CHK(parse_depth(L"low") == 50 && parse_depth(L"high") == 200 &&
        parse_depth(L"x") == 100, "depth");

    // find_one_call must find a real rel32 call and count a second one.
    unsigned char* buf = (unsigned char*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_EXECUTE_READWRITE);
    CHK(buf != NULL, "VirtualAlloc");
    if (buf) {
        memset(buf, 0x90, 4096);
        unsigned char* target = buf + 0x800;
        unsigned char* site = buf + 0x100;
        LONG rel = (LONG)(target - (site + 5));
        site[0] = 0xE8; memcpy(site + 1, &rel, 4);
        int n = 0;
        CHK(find_one_call(buf, 4096, target, &n) == site && n == 1, "one call found");
        unsigned char* site2 = buf + 0x200;
        rel = (LONG)(target - (site2 + 5));
        site2[0] = 0xE8; memcpy(site2 + 1, &rel, 4);
        find_one_call(buf, 4096, target, &n);
        CHK(n == 2, "a second call is counted, so the install refuses an ambiguous site");
        CHK(retarget(site, buf + 0x900), "retarget");
        memcpy(&rel, site + 1, 4);
        CHK(site + 5 + rel == buf + 0x900, "retarget writes the right displacement");
        VirtualFree(buf, 0, MEM_RELEASE);
    }

    #undef CHK
    if (!fail) logf("[ffxi3d] view selftest OK");
    return fail;
}
