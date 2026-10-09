#!/usr/bin/env python3
"""Check that binaries from `build.bat xp` can load on Windows XP.

pol.exe imports PolHook.dll statically, so a single import that XP does not
export stops the Viewer from starting, with nothing in any log. This reads each
file's PE header and import table and fails when the image is linked for a
newer Windows than XP (5.01), is not 32-bit x86, or imports a function from the
list below of functions that first shipped in Vista or later.

    python xpcheck.py build-xp\\PolHook.dll build-xp\\PolShimSetup.exe
    python xpcheck.py --expect-fail build\\PolHook.dll

--expect-fail inverts the result. CI runs it on the normal build, which needs
Windows Vista or later, to prove the check can still fail.
"""
import re
import struct
import sys

# Functions XP does not have, as prefixes or patterns matched against each
# imported name. Not every post-XP function in Windows, but every one the shim,
# its C runtime or BearSSL has ever pulled in, and their close relatives.
DENY = [
    r"\w*SRWLock", r"\w*ConditionVariable", r"InitOnce", r"Fls(Alloc|Free|GetValue|SetValue)",
    r"InitializeCriticalSectionEx", r"GetTickCount64", r"Create(Event|Mutex|Semaphore|WaitableTimer)Ex",
    r"LocaleNameToLCID", r"LCIDToLocaleName", r"GetLocaleInfoEx", r"LCMapStringEx",
    r"CompareStringEx", r"GetUserDefaultLocaleName", r"Get(Date|Time)FormatEx",
    r"EnumSystemLocalesEx", r"IsValidLocaleName", r"K32", r"GetFileInformationByHandleEx",
    r"SetFileInformationByHandle", r"CancelIoEx", r"CreateSymbolicLink", r"GetFinalPathNameByHandle",
    r"QueryFullProcessImageName", r"GetThreadId$", r"RegGetValue", r"RegDeleteTree", r"RegSetKeyValue",
    r"RegCopyTree", r"RegLoadMUIString", r"Interlocked\w*64", r"inet_pton", r"inet_ntop", r"InetPton",
    r"InetNtop", r"WSAPoll", r"SetThreadDescription", r"GetSystemTimePreciseAsFileTime",
    r"CreateRemoteThreadEx", r"GetCurrentProcessorNumber", r"SetDllDirectory", r"AddDllDirectory",
    r"SetDefaultDllDirectories", r"GetErrorMode", r"GetSystemDefaultLocaleName", r"\w*Threadpool",
    r"QueryThreadCycleTime", r"Wow64", r"Dwm", r"SHGetKnownFolder", r"SHCreateItem", r"TaskDialog",
    r"ChangeWindowMessageFilter", r"SetProcessDPIAware", r"GetDpiFor", r"EnableNonClientDpiScaling",
    r"IsWow64Process2", r"WinHttp", r"BCrypt", r"NCrypt",
]
DENY_RE = re.compile("^(?:%s)" % "|".join(DENY))
# Whole DLLs XP does not ship.
DENY_DLL = re.compile(r"^(bcrypt|ncrypt|dwmapi|winhttp|api-ms-win-.*|ext-ms-win-.*)\.dll$", re.I)


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def cstr(b, o):
    end = b.index(b"\0", o)
    return b[o:end].decode("ascii", "replace")


def check(path):
    """Problems found in one file, as a list of strings (empty = XP-ready)."""
    b = open(path, "rb").read()
    if b[:2] != b"MZ":
        return ["not a PE file"]
    pe = u32(b, 0x3C)
    if b[pe:pe + 4] != b"PE\0\0":
        return ["not a PE file"]
    machine = u16(b, pe + 4)
    nsec = u16(b, pe + 6)
    optsz = u16(b, pe + 20)
    opt = pe + 24
    probs = []
    if machine != 0x14C or u16(b, opt) != 0x10B:
        return ["not a 32-bit x86 image"]
    osv = (u16(b, opt + 40), u16(b, opt + 42))
    subv = (u16(b, opt + 48), u16(b, opt + 50))
    for what, v in (("operating system", osv), ("subsystem", subv)):
        if v > (5, 1):
            probs.append("%s version %d.%02d (XP is 5.01)" % (what, v[0], v[1]))

    secs = []
    for i in range(nsec):
        s = opt + optsz + i * 40
        secs.append((u32(b, s + 12), max(u32(b, s + 8), u32(b, s + 16)), u32(b, s + 20)))

    def off(rva):
        for va, size, raw in secs:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError("RVA %#x outside every section" % rva)

    imp_rva = u32(b, opt + 96 + 8)          # DataDirectory[1], the import table
    if imp_rva:
        d = off(imp_rva)
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from("<5I", b, d)
            if not name_rva:
                break
            dll = cstr(b, off(name_rva))
            if DENY_DLL.match(dll):
                probs.append("imports %s" % dll)
            t = off(oft or ft)
            while True:
                thunk = u32(b, t)
                if not thunk:
                    break
                if not thunk & 0x80000000:
                    fn = cstr(b, off(thunk) + 2)
                    if DENY_RE.match(fn):
                        probs.append("imports %s!%s" % (dll, fn))
                t += 4
            d += 20
    return probs


def main(argv):
    expect_fail = "--expect-fail" in argv
    files = [a for a in argv if a != "--expect-fail"]
    if not files:
        print(__doc__)
        return 2
    rc = 0
    for f in files:
        probs = check(f)
        if expect_fail:
            if probs:
                print("%s: not XP-ready, as expected (%d findings, e.g. %s)" % (f, len(probs), probs[0]))
            else:
                print("%s: passed, but it was expected to FAIL; the check has gone blind" % f)
                rc = 1
        elif probs:
            rc = 1
            print("%s: NOT XP-READY" % f)
            for p in probs:
                print("  " + p)
        else:
            print("%s: XP-ready" % f)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
