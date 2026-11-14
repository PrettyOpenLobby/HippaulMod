// polenv.cpp -- rewrite the Viewer install's data/utf/env.dat so the update
// check reaches OUR patch server instead of the dead SE host.
//
// WHY THIS EXISTS
//
// The Viewer finds its patch server by reading ONE field out of env.dat:
// `PATCH_SERVER_DOMAIN`, stock `pt008.pol.com` (a Square Enix host that no
// longer answers). The version check that produces "a viewer update is needed"
// does NOT run in pol.exe -- pol.exe spawns `util\startpol.exe /INIT`, and the
// check runs down that chain, in a process the in-process winsock redirect never
// reaches. But EVERY one of those processes reads the same env.dat. So moving the
// patch host in env.dat -- a file inside our own install -- redirects the whole
// chain at once, with nothing touched on the player's system (no hosts, no DNS).
// `play.openlobby.fyi` resolves publicly to our patch server, so the redirected
// host reaches us through ordinary name resolution. This is the same lever the
// Xbox 360 brick-fix uses (PATCH_SERVER_DOMAIN -> a public host that points here).
//
// THE FILE FORMAT
//
// env.dat is an 8-byte-block cipher over a Shift-JIS `KEY,VALUE\r\n` body, with a
// `[u32 payload_len][u32 checksum]` trailer; the loader rejects a size that is
// not a multiple of 8 and a trailer checksum that does not match. The codec below
// is a faithful C++ port of pol-server/legacy/tools/polfilecrypt.py (key `env`,
// 8-byte key fd31425364758697), proven byte-exact against that reference. We only
// touch the PATCH_SERVER_DOMAIN value; the rest of the body is re-encrypted
// unchanged. A .orig backup is taken once, and the write is idempotent -- it is a
// no-op when the field already names our host, so re-running every launch is free.
//
// env.dat IS listed in the install's Check-Files manifest, so a full file check
// could restore the stock host. Re-applying at launch (startup(), before startpol
// spawns) keeps the redirect in place for the check that matters; the server side
// serves a matching env.dat so a check that does run is satisfied.

#include "polshim.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ---- the env.dat cipher (port of polfilecrypt.py) --------------------------

static const unsigned char KEY_ENV[8] = { 0xfd,0x31,0x42,0x53,0x64,0x75,0x86,0x97 };
static const uint32_t ENV_MAGIC = 0xA1652347u;

static uint32_t rotl32(uint32_t v, int n) {
    return (uint32_t)((v << n) | (v >> (32 - n)));
}

// -> words[32] (u64), deltas[32] (u8). Mirrors key_schedule().
static void key_schedule(const unsigned char key8[8], uint64_t words[32], unsigned char deltas[32]) {
    uint64_t K = 0;
    for (int i = 0; i < 8; i++) K |= (uint64_t)key8[i] << (8 * i);  // little-endian
    uint32_t hi = (uint32_t)(K >> 32), lo = (uint32_t)(K & 0xFFFFFFFFu);
    uint64_t seed = ((uint64_t)rotl32(lo, 8) << 32) | (uint64_t)rotl32(hi, 16);

    unsigned char t[8];
    for (int i = 0; i < 8; i++) t[i] = (unsigned char)((seed >> (8 * i)) & 0xFF);
    t[0] = (unsigned char)((t[0] + 69) & 0xFF);
    for (int i = 1; i < 8; i++) {
        uint32_t b = t[i - 1];
        uint32_t x = (uint32_t)(b + t[i] + 212);
        x ^= (uint32_t)(b << 2);
        t[i] = (unsigned char)((x ^ 0x45) & 0xFF);
    }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)t[i] << (8 * i);
    for (int r = 0; r < 32; r++) {
        words[r] = v;
        uint32_t s = 0;
        for (int i = 0; i < 8; i++) s += (uint32_t)((v >> (8 * i)) & 0xFF);
        deltas[r] = (unsigned char)((8 * 0x78 - s) & 0xFF);
        v = v * 5;  // mod 2^64 by natural wrap
    }
}

static uint64_t mix64(uint64_t X, uint64_t T, uint32_t pos) {
    uint64_t p = pos;
    uint64_t P = p | (p << 10) | (p << 20) | (p << 30);
    uint64_t Y = (X - T) ^ (P + ENV_MAGIC) ^ T;
    return (Y << 32) | (Y >> 32);
}

static uint64_t unmix64(uint64_t Y, uint64_t T, uint32_t pos) {
    uint64_t p = pos;
    uint64_t P = p | (p << 10) | (p << 20) | (p << 30);
    uint64_t R = (Y << 32) | (Y >> 32);
    return ((R ^ T) ^ (P + ENV_MAGIC)) + T;
}

static uint64_t rd64(const unsigned char* p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}
static void wr64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)((v >> (8 * i)) & 0xFF);
}
static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(unsigned char* p, uint32_t v) {
    p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24);
}

// Decrypt in place-ish: fills `out` (len bytes, caller-sized) and returns the
// payload length via *payload_len; *ok = trailer checksum matched. Returns false
// only on a structurally impossible length.
static bool env_decrypt(const unsigned char* data, size_t len, unsigned char* out,
                        uint32_t* payload_len, bool* ok) {
    if ((len & 7) || len < 16) return false;
    uint64_t words[32]; unsigned char deltas[32];
    key_schedule(KEY_ENV, words, deltas);
    memcpy(out, data, len);
    uint32_t cksum = 0;
    size_t nblk = len / 8;
    for (size_t n = 0; n < nblk; n++) {
        size_t pos = n * 8; int row = (int)(n & 31);
        unsigned char d = deltas[row];
        unsigned char prev = (unsigned char)((n ^ 0x45) & 0xFF);
        unsigned char blk[8];
        for (int j = 0; j < 8; j++) {
            unsigned char c = out[pos + j];
            blk[j] = (unsigned char)(((prev ^ c) + d) & 0xFF);
            prev = c;
        }
        uint64_t o = mix64(rd64(blk), words[row], (uint32_t)pos);
        wr64(out + pos, o);
        if (n < nblk - 1) cksum = (uint32_t)(cksum + out[pos] + out[pos + 4]);
    }
    uint32_t n_ = rd32(out + len - 8);
    uint32_t want = rd32(out + len - 4);
    *payload_len = n_;
    *ok = (cksum == want);
    return true;
}

// Encrypt `payload` (payload_len bytes) into `out`; returns the on-disk length
// (payload padded to a multiple of 8, plus the 8-byte trailer). Caller sizes
// `out` to at least ((payload_len+7)&~7)+8.
static size_t env_encrypt(const unsigned char* payload, uint32_t payload_len, unsigned char* out) {
    uint64_t words[32]; unsigned char deltas[32];
    key_schedule(KEY_ENV, words, deltas);
    size_t body = (payload_len + 7) & ~(size_t)7;
    memcpy(out, payload, payload_len);
    for (size_t i = payload_len; i < body; i++) out[i] = 0;
    uint32_t cksum = 0;
    for (size_t n = 0; n < body / 8; n++)
        cksum = (uint32_t)(cksum + out[n * 8] + out[n * 8 + 4]);
    wr32(out + body, payload_len);
    wr32(out + body + 4, cksum);
    size_t total = body + 8;
    for (size_t n = 0; n < total / 8; n++) {
        size_t pos = n * 8; int row = (int)(n & 31);
        uint64_t X = unmix64(rd64(out + pos), words[row], (uint32_t)pos);
        unsigned char blk[8]; wr64(blk, X);
        unsigned char d = deltas[row];
        unsigned char prev = (unsigned char)((n ^ 0x45) & 0xFF);
        for (int j = 0; j < 8; j++) {
            unsigned char c = (unsigned char)((prev ^ (unsigned char)((blk[j] - d) & 0xFF)) & 0xFF);
            out[pos + j] = c;
            prev = c;
        }
    }
    return total;
}

// ---- the field edit --------------------------------------------------------

// Replace the value of `PATCH_SERVER_DOMAIN,<...>\r\n` in the decrypted body.
// Returns the new payload length, or 0 if the field was absent or already set to
// new_domain (caller treats 0-with-*already as a no-op success).
static uint32_t replace_patch_domain(unsigned char* payload, uint32_t len,
                                     const char* new_domain, size_t cap, bool* already) {
    *already = false;
    static const char* KEY = "PATCH_SERVER_DOMAIN,";
    size_t klen = strlen(KEY);
    // find the KEY at a line start (offset 0 or right after a \n)
    size_t pos = (size_t)-1;
    for (size_t i = 0; i + klen <= len; i++) {
        if ((i == 0 || payload[i - 1] == '\n') && memcmp(payload + i, KEY, klen) == 0) { pos = i; break; }
    }
    if (pos == (size_t)-1) return 0;  // field absent
    size_t vstart = pos + klen;
    size_t vend = vstart;
    while (vend < len && payload[vend] != '\r' && payload[vend] != '\n') vend++;
    size_t oldvlen = vend - vstart;
    size_t ndlen = strlen(new_domain);
    if (oldvlen == ndlen && memcmp(payload + vstart, new_domain, ndlen) == 0) { *already = true; return 0; }
    // splice: [0,vstart) + new_domain + [vend,len)
    long long delta = (long long)ndlen - (long long)oldvlen;
    if ((long long)len + delta > (long long)cap) return 0;  // would overflow buffer
    if (delta != 0)
        memmove(payload + vend + delta, payload + vend, len - vend);
    memcpy(payload + vstart, new_domain, ndlen);
    return (uint32_t)(len + delta);
}

static bool read_file(const wchar_t* path, unsigned char** buf, size_t* len) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz == 0 || sz > (16u << 20)) { CloseHandle(h); return false; }
    unsigned char* b = (unsigned char*)LocalAlloc(LMEM_FIXED, sz);
    if (!b) { CloseHandle(h); return false; }
    DWORD got = 0;
    BOOL ok = ReadFile(h, b, sz, &got, NULL);
    CloseHandle(h);
    if (!ok || got != sz) { LocalFree(b); return false; }
    *buf = b; *len = sz; return true;
}

static bool write_file_atomic(const wchar_t* path, const unsigned char* buf, size_t len) {
    wchar_t tmp[MAX_PATH];
    _snwprintf_s(tmp, _countof(tmp), _TRUNCATE, L"%s.polnew", path);
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, buf, (DWORD)len, &wrote, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || wrote != len) { DeleteFileW(tmp); return false; }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp); return false;
    }
    return true;
}

// Public entry: rewrite <viewer_root>\data\utf\env.dat's PATCH_SERVER_DOMAIN to
// new_domain. Logs one line. Safe to call every launch (idempotent).
void polenv_redirect_patch_domain(const wchar_t* viewer_root, const char* new_domain) {
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\data\\utf\\env.dat", viewer_root);

    unsigned char* raw = NULL; size_t rawlen = 0;
    if (!read_file(path, &raw, &rawlen)) {
        logf("[envredir] env.dat not readable at %ls -- skipped", path);
        return;
    }

    unsigned char* plain = (unsigned char*)LocalAlloc(LMEM_FIXED, rawlen);
    uint32_t plen = 0; bool ok = false;
    if (!plain || !env_decrypt(raw, rawlen, plain, &plen, &ok) || !ok || plen > rawlen) {
        logf("[envredir] env.dat present but did not decrypt/checksum (ok=%d plen=%u raw=%zu) -- left untouched",
             (int)ok, plen, rawlen);
        if (plain) LocalFree(plain); LocalFree(raw);
        return;
    }

    // Work buffer for the (possibly longer) payload.
    size_t cap = plen + 256;
    unsigned char* work = (unsigned char*)LocalAlloc(LMEM_FIXED, cap);
    if (!work) { LocalFree(plain); LocalFree(raw); return; }
    memcpy(work, plain, plen);
    bool already = false;
    uint32_t newlen = replace_patch_domain(work, plen, new_domain, cap, &already);
    LocalFree(plain);

    if (already) {
        logf("[envredir] env.dat PATCH_SERVER_DOMAIN already = %s -- no change", new_domain);
        LocalFree(work); LocalFree(raw);
        return;
    }
    if (newlen == 0) {
        logf("[envredir] PATCH_SERVER_DOMAIN not found in env.dat (or edit would overflow) -- left untouched");
        LocalFree(work); LocalFree(raw);
        return;
    }

    size_t outcap = ((newlen + 7) & ~(size_t)7) + 8;
    unsigned char* out = (unsigned char*)LocalAlloc(LMEM_FIXED, outcap);
    if (!out) { LocalFree(work); LocalFree(raw); return; }
    size_t outlen = env_encrypt(work, newlen, out);
    LocalFree(work);

    // Verify our own round-trip before touching disk: decrypt what we just made
    // and confirm it checksums AND actually carries the new domain.
    {
        unsigned char* chk = (unsigned char*)LocalAlloc(LMEM_FIXED, outlen);
        uint32_t cplen = 0; bool cok = false;
        bool good = chk && env_decrypt(out, outlen, chk, &cplen, &cok) && cok && cplen == newlen;
        if (good) {
            bool found = false;
            size_t klen = strlen("PATCH_SERVER_DOMAIN,");
            for (size_t i = 0; chk && i + klen <= cplen; i++)
                if ((i == 0 || chk[i-1] == '\n') && memcmp(chk + i, "PATCH_SERVER_DOMAIN,", klen) == 0 &&
                    strncmp((char*)chk + i + klen, new_domain, strlen(new_domain)) == 0) { found = true; break; }
            good = found;
        }
        if (chk) LocalFree(chk);
        if (!good) {
            logf("[envredir] self-check FAILED on the re-encrypted env.dat -- NOT writing (install left stock)");
            LocalFree(out); LocalFree(raw);
            return;
        }
    }

    // Back up the stock file once.
    wchar_t orig[MAX_PATH];
    _snwprintf_s(orig, _countof(orig), _TRUNCATE, L"%s.orig", path);
    if (GetFileAttributesW(orig) == INVALID_FILE_ATTRIBUTES)
        CopyFileW(path, orig, TRUE);

    if (write_file_atomic(path, out, outlen))
        logf("[envredir] env.dat PATCH_SERVER_DOMAIN -> %s (%zu B; .orig kept). The update check now reaches us.",
             new_domain, outlen);
    else
        logf("[envredir] could not write env.dat (in use / read-only?) -- install left stock");

    LocalFree(out); LocalFree(raw);
}
