// xpcompat.h -- the Vista-and-later calls the shim makes, rebuilt from what Windows
// XP has. Included by polshim.h in the legacy XP build only (build.bat xp, which
// defines POLSHIM_XP); the normal build never sees this file.
//
// WHY A SEPARATE BUILD NEEDS THIS AT ALL
//
// pol.exe imports PolHook.dll statically, so the loader resolves every function
// the shim imports before the Viewer's own entry point runs. One import XP lacks
// and the Viewer does not start, with no window and nothing in our log. Every
// name below is mapped onto an xp_ function, so the call sites read the same in
// both builds and the XP DLL's import table holds only functions XP exports.
// After a build, check it with: dumpbin /imports build-xp\PolHook.dll
#pragma once
#ifdef POLSHIM_XP

#include <windows.h>
#include <intrin.h>
#include <psapi.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <bcrypt.h>

// --- 64-bit interlocked ops -----------------------------------------------------
// kernel32 exports these from Vista on. The compiler intrinsic is cmpxchg8b,
// present on every Pentium and later, so it needs nothing from the OS.
#pragma intrinsic(_InterlockedCompareExchange64)
inline LONG64 xp_InterlockedCompareExchange64(volatile LONG64* p, LONG64 x, LONG64 cmp)
{
    return _InterlockedCompareExchange64(p, x, cmp);
}
inline LONG64 xp_InterlockedExchange64(volatile LONG64* p, LONG64 v)
{
    LONG64 old;
    do { old = *p; } while (_InterlockedCompareExchange64(p, v, old) != old);
    return old;
}
#undef InterlockedCompareExchange64
#undef InterlockedExchange64
#define InterlockedCompareExchange64 xp_InterlockedCompareExchange64
#define InterlockedExchange64        xp_InterlockedExchange64

// --- one-time init (INIT_ONCE) ----------------------------------------------------
// Only the synchronous form with no context is used (poltoken.cpp).
// 0 = not started, 1 = running, 2 = done. A thread arriving while another runs
// the callback yields until it finishes, which is what InitOnceExecuteOnce does.
typedef struct { volatile LONG state; } xp_INIT_ONCE, *xp_PINIT_ONCE;
typedef BOOL (CALLBACK* xp_PINIT_ONCE_FN)(xp_PINIT_ONCE, PVOID, PVOID*);
inline BOOL xp_InitOnceExecuteOnce(xp_PINIT_ONCE once, xp_PINIT_ONCE_FN fn, PVOID param, PVOID* ctx)
{
    for (;;) {
        LONG s = InterlockedCompareExchange(&once->state, 1, 0);
        if (s == 2) return TRUE;
        if (s == 0) {
            BOOL ok = fn(once, param, ctx);
            InterlockedExchange(&once->state, ok ? 2 : 0);
            return ok;
        }
        Sleep(0);
    }
}
#undef INIT_ONCE_STATIC_INIT
#define INIT_ONCE            xp_INIT_ONCE
#define PINIT_ONCE           xp_PINIT_ONCE
#define INIT_ONCE_STATIC_INIT {0}
#define InitOnceExecuteOnce  xp_InitOnceExecuteOnce

// --- slim reader/writer lock ------------------------------------------------------
// count > 0: that many readers; -1: one writer; 0: free. Like the real SRW lock it
// is not recursive, and it keeps SRWLOCK_INIT a static initializer, which a
// CRITICAL_SECTION cannot be. The one user (d3d8hook's texture-format table) holds
// it for a few hundred instructions, so yielding is enough.
typedef struct { volatile LONG count; } xp_SRWLOCK;
inline void xp_AcquireSRWLockShared(xp_SRWLOCK* l)
{
    for (;;) {
        LONG c = l->count;
        if (c >= 0 && InterlockedCompareExchange(&l->count, c + 1, c) == c) return;
        Sleep(0);
    }
}
inline void xp_ReleaseSRWLockShared(xp_SRWLOCK* l) { InterlockedDecrement(&l->count); }
inline void xp_AcquireSRWLockExclusive(xp_SRWLOCK* l)
{
    while (InterlockedCompareExchange(&l->count, -1, 0) != 0) Sleep(0);
}
inline void xp_ReleaseSRWLockExclusive(xp_SRWLOCK* l) { InterlockedExchange(&l->count, 0); }
#undef SRWLOCK_INIT
#define SRWLOCK                  xp_SRWLOCK
#define SRWLOCK_INIT             {0}
#define AcquireSRWLockShared     xp_AcquireSRWLockShared
#define ReleaseSRWLockShared     xp_ReleaseSRWLockShared
#define AcquireSRWLockExclusive  xp_AcquireSRWLockExclusive
#define ReleaseSRWLockExclusive  xp_ReleaseSRWLockExclusive

// --- threads and stacks -------------------------------------------------------------
// GetThreadId is Vista+. ntdll's NtQueryInformationThread(ThreadBasicInformation)
// gives the same id on XP, and fails for a handle that is not a thread, so the
// caller's "0 = not a thread" test still holds.
inline DWORD xp_GetThreadId(HANDLE h)
{
    struct TBI { LONG exit; PVOID teb; HANDLE pid; HANDLE tid; ULONG_PTR aff; LONG pri, basepri; };
    typedef LONG (WINAPI* NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
    static NtQIT q = (NtQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    TBI t;
    if (!q || q(h, 0, &t, sizeof(t), NULL) < 0) return 0;
    return (DWORD)(ULONG_PTR)t.tid;
}
#define GetThreadId xp_GetThreadId

// XP has RtlCaptureStackBackTrace in ntdll, but the 7.1A headers only declare it
// for later targets. Resolved at run time so it never becomes a load-time import.
inline USHORT xp_RtlCaptureStackBackTrace(ULONG skip, ULONG n, PVOID* frames, PULONG hash)
{
    typedef USHORT (WINAPI* Fn)(ULONG, ULONG, PVOID*, PULONG);
    static Fn f = (Fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlCaptureStackBackTrace");
    // +1 so the frames start at our caller, as they would without this wrapper.
    return f ? f(skip + 1, n, frames, hash) : 0;
}
#define RtlCaptureStackBackTrace xp_RtlCaptureStackBackTrace

// --- processes ----------------------------------------------------------------------
// XP rejects PROCESS_QUERY_LIMITED_INFORMATION outright, and GetModuleFileNameEx
// needs VM_READ as well, so every OpenProcess that asks for it gets these instead.
#undef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION (PROCESS_QUERY_INFORMATION | PROCESS_VM_READ)
inline BOOL xp_QueryFullProcessImageNameA(HANDLE ph, DWORD, LPSTR out, PDWORD n)
{
    DWORD got = GetModuleFileNameExA(ph, NULL, out, *n);
    if (!got) return FALSE;
    *n = got;
    return TRUE;
}
inline BOOL xp_QueryFullProcessImageNameW(HANDLE ph, DWORD, LPWSTR out, PDWORD n)
{
    DWORD got = GetModuleFileNameExW(ph, NULL, out, *n);
    if (!got) return FALSE;
    *n = got;
    return TRUE;
}
#define QueryFullProcessImageNameA xp_QueryFullProcessImageNameA
#define QueryFullProcessImageNameW xp_QueryFullProcessImageNameW
// psapi.dll's own export. K32EnumProcessModules is the Windows 7 kernel32 copy of it.
#define K32EnumProcessModules EnumProcessModules

// --- registry -----------------------------------------------------------------------
#ifndef RRF_RT_REG_SZ
#define RRF_RT_REG_SZ 0x00000002
#endif
// RegGetValue first shipped in XP x64 / Server 2003 SP1, not 32-bit XP. The shim only
// reads strings through it (RRF_RT_REG_SZ), so this checks the type and
// guarantees the terminator the way RegGetValue does, and nothing more.
template <typename C, typename OpenFn, typename QueryFn>
inline LSTATUS xp_reg_get_sz(HKEY root, const C* sub, const C* name, DWORD flags,
                             LPDWORD type, PVOID data, LPDWORD cb, OpenFn open_, QueryFn query_)
{
    HKEY k = root;
    if (sub && *sub) {
        LSTATUS r = open_(root, sub, 0, KEY_QUERY_VALUE, &k);
        if (r != ERROR_SUCCESS) return r;
    }
    DWORD t = 0, have = cb ? *cb : 0;
    LSTATUS r = query_(k, name, NULL, &t, (LPBYTE)data, cb);
    if (k != root) RegCloseKey(k);
    if (type) *type = t;
    if (r != ERROR_SUCCESS) return r;
    if ((flags & RRF_RT_REG_SZ) && t != REG_SZ) return ERROR_UNSUPPORTED_TYPE;
    if (data && cb) {
        DWORD nch = *cb / sizeof(C);
        C* s = (C*)data;
        if (nch == 0 || s[nch - 1] != 0) {
            if ((nch + 1) * sizeof(C) > have) return ERROR_MORE_DATA;
            s[nch] = 0;
            *cb = (nch + 1) * (DWORD)sizeof(C);
        }
    }
    return ERROR_SUCCESS;
}
inline LSTATUS xp_RegGetValueA(HKEY root, LPCSTR sub, LPCSTR name, DWORD flags,
                               LPDWORD type, PVOID data, LPDWORD cb)
{
    return xp_reg_get_sz<char>(root, sub, name, flags, type, data, cb, RegOpenKeyExA, RegQueryValueExA);
}
inline LSTATUS xp_RegGetValueW(HKEY root, LPCWSTR sub, LPCWSTR name, DWORD flags,
                               LPDWORD type, PVOID data, LPDWORD cb)
{
    return xp_reg_get_sz<wchar_t>(root, sub, name, flags, type, data, cb, RegOpenKeyExW, RegQueryValueExW);
}
#define RegGetValueA xp_RegGetValueA
#define RegGetValueW xp_RegGetValueW
// shlwapi's SHDeleteKey removes a key and everything under it, as RegDeleteTree does.
#define RegDeleteTreeW(root, sub) ((LSTATUS)SHDeleteKeyW((root), (sub)))

// --- hashing (bcrypt.dll is Vista+) ---------------------------------------------------
// The shim hashes with SHA-256 (update checks) and MD5 (file.txt names). CryptoAPI's
// AES provider has both on XP SP3. The handles carry the provider and algorithm, so
// the BCrypt call sequence in the callers is unchanged.
#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c     // wincrypt.h hides it below an SP3 target
#endif
struct xp_bcrypt_alg { HCRYPTPROV prov; ALG_ID id; };
inline NTSTATUS xp_BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE* out, LPCWSTR alg, LPCWSTR, ULONG)
{
    ALG_ID id = 0;
    if (!lstrcmpW(alg, BCRYPT_SHA256_ALGORITHM)) id = CALG_SHA_256;
    else if (!lstrcmpW(alg, BCRYPT_MD5_ALGORITHM)) id = CALG_MD5;
    else return (NTSTATUS)0xC00000BBL;                          // STATUS_NOT_SUPPORTED
    HCRYPTPROV p = 0;
    if (!CryptAcquireContextW(&p, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return (NTSTATUS)0xC0000001L;                           // STATUS_UNSUCCESSFUL
    xp_bcrypt_alg* a = (xp_bcrypt_alg*)HeapAlloc(GetProcessHeap(), 0, sizeof(*a));
    if (!a) { CryptReleaseContext(p, 0); return (NTSTATUS)0xC0000017L; }  // NO_MEMORY
    a->prov = p; a->id = id;
    *out = a;
    return 0;
}
inline NTSTATUS xp_BCryptCreateHash(BCRYPT_ALG_HANDLE alg, BCRYPT_HASH_HANDLE* out,
                                    PUCHAR, ULONG, PUCHAR, ULONG, ULONG)
{
    xp_bcrypt_alg* a = (xp_bcrypt_alg*)alg;
    HCRYPTHASH h = 0;
    if (!CryptCreateHash(a->prov, a->id, 0, 0, &h)) return (NTSTATUS)0xC0000001L;
    *out = (BCRYPT_HASH_HANDLE)h;
    return 0;
}
inline NTSTATUS xp_BCryptHashData(BCRYPT_HASH_HANDLE h, PUCHAR p, ULONG n, ULONG)
{
    return CryptHashData((HCRYPTHASH)h, p, n, 0) ? 0 : (NTSTATUS)0xC0000001L;
}
inline NTSTATUS xp_BCryptFinishHash(BCRYPT_HASH_HANDLE h, PUCHAR out, ULONG n, ULONG)
{
    DWORD cb = n;
    return CryptGetHashParam((HCRYPTHASH)h, HP_HASHVAL, out, &cb, 0) && cb == n
           ? 0 : (NTSTATUS)0xC0000001L;
}
inline NTSTATUS xp_BCryptDestroyHash(BCRYPT_HASH_HANDLE h)
{
    CryptDestroyHash((HCRYPTHASH)h);
    return 0;
}
inline NTSTATUS xp_BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE alg, ULONG)
{
    xp_bcrypt_alg* a = (xp_bcrypt_alg*)alg;
    CryptReleaseContext(a->prov, 0);
    HeapFree(GetProcessHeap(), 0, a);
    return 0;
}
#define BCryptOpenAlgorithmProvider  xp_BCryptOpenAlgorithmProvider
#define BCryptCreateHash             xp_BCryptCreateHash
#define BCryptHashData               xp_BCryptHashData
#define BCryptFinishHash             xp_BCryptFinishHash
#define BCryptDestroyHash            xp_BCryptDestroyHash
#define BCryptCloseAlgorithmProvider xp_BCryptCloseAlgorithmProvider

#endif  // POLSHIM_XP
