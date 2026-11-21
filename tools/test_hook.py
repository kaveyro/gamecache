"""Self-test for a built gamecache proxy DLL, run in a throwaway process.

Usage: python -I test_hook.py <path\\to\\xinput1_4.dll> <empty test directory>
Exit code 0 = all checks passed.
"""
import ctypes
import os
import shutil
import sys
from ctypes import wintypes as wt

src, testdir = sys.argv[1], sys.argv[2]
proxy_name = os.path.basename(src)
os.makedirs(testdir, exist_ok=True)
dll_path = os.path.join(testdir, proxy_name)
shutil.copy(src, dll_path)
with open(os.path.join(testdir, "gamecache.ini"), "w") as fh:
    fh.write("[gamecache]\nenabled=1\nlog=1\npatterns=*.bin\n")
for name in ("data.bin", "other.txt"):
    with open(os.path.join(testdir, name), "wb") as fh:
        fh.write(b"x" * 65536)
log = os.path.join(testdir, "gamecache.log")
if os.path.exists(log):
    os.remove(log)

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
proxy = ctypes.WinDLL(dll_path)  # DllMain installs the hooks

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
OPEN_EXISTING = 3
NO_BUF = 0x20000000
INVALID = wt.HANDLE(-1).value

k32.CreateFileW.restype = wt.HANDLE
k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
k32.CreateFileA.restype = wt.HANDLE
k32.CreateFileA.argtypes = [wt.LPCSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]


class CF2(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("dwFileAttributes", wt.DWORD), ("dwFileFlags", wt.DWORD),
                ("dwSecurityQosFlags", wt.DWORD), ("lpSecurityAttributes", wt.LPVOID), ("hTemplateFile", wt.HANDLE)]


k32.CreateFile2.restype = wt.HANDLE
k32.CreateFile2.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.POINTER(CF2)]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [wt.HMODULE, ctypes.c_void_p]


def unbuffered(h):
    iosb = (ctypes.c_ulonglong * 2)()
    mode = wt.ULONG()
    st = ntdll.NtQueryInformationFile(wt.HANDLE(h), iosb, ctypes.byref(mode), 4, 16)  # FileModeInformation
    assert st == 0, hex(st & 0xFFFFFFFF)
    return bool(mode.value & 0x8)  # FILE_NO_INTERMEDIATE_BUFFERING


results = []


def report(ok, label):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {label}")


def check(label, h, expect_unbuffered):
    if h == INVALID:
        report(False, f"{label}: open failed ({ctypes.get_last_error()})")
        return
    got = unbuffered(h)
    k32.CloseHandle(wt.HANDLE(h))
    report(got == expect_unbuffered, f"{label}: unbuffered={got} (expected {expect_unbuffered})")


b = os.path.join(testdir, "data.bin")
t = os.path.join(testdir, "other.txt")
check("CreateFileW, matching file, read-only", k32.CreateFileW(b, GENERIC_READ, 1, None, OPEN_EXISTING, NO_BUF, None), False)
check("CreateFileW, non-matching file", k32.CreateFileW(t, GENERIC_READ, 1, None, OPEN_EXISTING, NO_BUF, None), True)
check("CreateFileW, matching file, opened for writing",
      k32.CreateFileW(b, GENERIC_READ | GENERIC_WRITE, 1, None, OPEN_EXISTING, NO_BUF, None), True)
check("CreateFileA, matching file, read-only",
      k32.CreateFileA(b.encode("mbcs"), GENERIC_READ, 1, None, OPEN_EXISTING, NO_BUF, None), False)
p = CF2(ctypes.sizeof(CF2), 0, NO_BUF, 0, None, None)
check("CreateFile2, matching file, read-only", k32.CreateFile2(b, GENERIC_READ, 1, OPEN_EXISTING, ctypes.byref(p)), False)

# Forwarding: every export of the proxy must resolve to the real system DLL
sysdll = os.path.join(os.environ["SystemRoot"], "System32", proxy_name)
exports_file = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "proxies",
                            os.path.splitext(proxy_name)[0] + ".exports")
real = ctypes.WinDLL(sysdll) if os.path.exists(sysdll) else None
if real is None:
    print(f"SKIP  forwarding checks: {sysdll} does not exist on this machine")
else:
    for line in open(exports_file):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        ordinal, nm = line.split()
        if nm == "DllMain":
            continue
        key = ctypes.c_char_p(nm.encode()) if nm != "-" else ctypes.c_void_p(int(ordinal))
        stub = k32.GetProcAddress(proxy._handle, key)
        target = k32.GetProcAddress(real._handle, key)
        # each stub is "jmp qword ptr [rip+disp32]" (FF 25 disp32) into the pointer table
        ok = False
        if stub and target:
            code = (ctypes.c_ubyte * 6).from_address(stub)
            if code[0] == 0xFF and code[1] == 0x25:
                disp = int.from_bytes(bytes(code[2:6]), "little", signed=True)
                ok = ctypes.c_void_p.from_address(stub + 6 + disp).value == target
        report(ok, f"export {nm if nm != '-' else '#' + ordinal} forwards to {proxy_name} in System32")

log_text = open(log, encoding="utf-8").read() if os.path.exists(log) else ""
report("loaded into" in log_text and "buffered open:" in log_text, "log file written")
print("\n--- gamecache.log ---\n" + (log_text or "(no log)"))
sys.exit(0 if all(results) else 1)
