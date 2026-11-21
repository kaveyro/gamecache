/*
 * gamecache - lets Windows cache game data that a game reads unbuffered.
 *
 * Many games open their data packages with FILE_FLAG_NO_BUFFERING, which bypasses the
 * Windows file cache. If the game then reads the same data again and again, every read
 * hits the drive. On slow or DRAM-less SSDs and on HDDs this shows up as stutter.
 *
 * This DLL is placed in the game folder under the name of a system DLL the game loads
 * (e.g. xinput1_4.dll). It forwards every export to the real DLL in the system directory
 * and, when matching files are opened, removes only FILE_FLAG_NO_BUFFERING. Windows then
 * keeps the data in RAM (standby list) and repeated reads no longer hit the drive.
 *
 * Safety rules:
 *  - only files opened read-only are changed (save games and other writes are untouched)
 *  - device and volume paths (\\.\..., \\?\GLOBALROOT...) are never changed
 *  - only file names matching a pattern from gamecache.ini (default: all files)
 *
 * Configuration: gamecache.ini next to the DLL
 *   [gamecache]
 *   enabled=1
 *   log=1
 *   patterns=*.rmdblob;*.pak
 *
 * Built with build.cmd, which force-includes the generated proxy_exports.h
 * (export table) and links the generated jump stubs (proxy_stubs.asm).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "version.h"

#ifndef GC_EXPORT_COUNT
#error "build with build.cmd: proxy_exports.h must be force-included"
#endif

#define MAX_PATTERNS 32

/* ---------- export forwarding ---------- */

/* Filled at load time; the assembly stubs jump through this table. */
void *gc_real[GC_EXPORT_COUNT];
static int g_resolved;

static DWORD WINAPI gc_missing(void)
{
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void ResolveForwards(WCHAR *realPath, size_t cch)
{
    HMODULE real = NULL;
    UINT n = GetSystemDirectoryW(realPath, (UINT)cch);
    int i;

    if (n && n < cch - 32) {
        wcscat_s(realPath, cch, L"\\");
        wcscat_s(realPath, cch, GC_PROXY_DLL);
        real = LoadLibraryW(realPath);
    }
    for (i = 0; i < GC_EXPORT_COUNT; i++) {
        void *p = real ? (void *)GetProcAddress(real, gc_export_names[i]) : NULL;
        gc_real[i] = p ? p : (void *)gc_missing;
        if (p)
            g_resolved++;
    }
}

/* ---------- state ---------- */

typedef HANDLE(WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *CreateFile2_t)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);

static CreateFileW_t Real_CreateFileW;
static CreateFileA_t Real_CreateFileA;
static CreateFile2_t Real_CreateFile2;

static HMODULE g_self;
static BOOL g_enabled = TRUE;
static BOOL g_log = TRUE;
static WCHAR g_patternBuf[1024];
static WCHAR g_patternStore[1024];
static LPWSTR g_patterns[MAX_PATTERNS];
static int g_patternCount;
static WCHAR g_logPath[MAX_PATH];
static CRITICAL_SECTION g_logLock;
static HANDLE g_logFile = INVALID_HANDLE_VALUE;
static volatile LONG g_changed;
static volatile LONG g_seen;

/* ---------- logging ---------- */

static void LogLine(const WCHAR *fmt, ...)
{
    WCHAR wbuf[1200];
    char buf[3600];
    va_list ap;
    SYSTEMTIME st;
    int n, len;
    DWORD written;

    if (!g_log)
        return;
    GetLocalTime(&st);
    n = _snwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u ", st.wYear,
                     st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    _vsnwprintf_s(wbuf + n, _countof(wbuf) - n, _TRUNCATE, fmt, ap);
    va_end(ap);
    len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, buf, sizeof(buf) - 2, NULL, NULL);
    if (len <= 0)
        return;
    buf[len - 1] = '\r';
    buf[len] = '\n';

    EnterCriticalSection(&g_logLock);
    if (g_logFile == INVALID_HANDLE_VALUE && Real_CreateFileW && g_logPath[0])
        g_logFile = Real_CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_logFile != INVALID_HANDLE_VALUE)
        WriteFile(g_logFile, buf, (DWORD)len + 1, &written, NULL);
    LeaveCriticalSection(&g_logLock);
}

/* ---------- matching ---------- */

static BOOL WildMatch(const WCHAR *pat, const WCHAR *s)
{
    const WCHAR *star = NULL, *mark = NULL;
    while (*s) {
        if (*pat == L'?' || towlower(*pat) == towlower(*s)) {
            pat++;
            s++;
        } else if (*pat == L'*') {
            star = pat++;
            mark = s;
        } else if (star) {
            pat = star + 1;
            s = ++mark;
        } else {
            return FALSE;
        }
    }
    while (*pat == L'*')
        pat++;
    return *pat == 0;
}

static BOOL ShouldBuffer(LPCWSTR name, DWORD access)
{
    const WCHAR *base, *p;
    int i;

    if (!g_enabled || !name)
        return FALSE;
    /* read-only opens only */
    if (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE))
        return FALSE;
    /* no device or volume paths; \\?\C:\... is a normal file path and allowed */
    if (name[0] == L'\\' && name[1] == L'\\' && (name[2] == L'.' || name[2] == L'?')) {
        if (!(name[2] == L'?' && name[3] == L'\\' && name[4] && name[5] == L':'))
            return FALSE;
    }

    base = name;
    for (p = name; *p; p++)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    if (!*base)
        return FALSE;
    if (g_patternCount == 0)
        return TRUE;
    for (i = 0; i < g_patternCount; i++)
        if (WildMatch(g_patterns[i], base))
            return TRUE;
    return FALSE;
}

static void NoteChange(LPCWSTR name)
{
    LONG n = InterlockedIncrement(&g_changed);
    if (n <= 200)
        LogLine(L"buffered open: %s", name);
    else if (n % 1000 == 0)
        LogLine(L"%ld files opened buffered so far", n);
}

/* ---------- hooks ---------- */

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disp, DWORD flags, HANDLE tmpl)
{
    InterlockedIncrement(&g_seen);
    if ((flags & FILE_FLAG_NO_BUFFERING) && ShouldBuffer(name, access)) {
        flags &= ~FILE_FLAG_NO_BUFFERING;
        NoteChange(name);
    }
    return Real_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disp, DWORD flags, HANDLE tmpl)
{
    InterlockedIncrement(&g_seen);
    if ((flags & FILE_FLAG_NO_BUFFERING) && name) {
        WCHAR wname[MAX_PATH * 2];
        if (MultiByteToWideChar(CP_ACP, 0, name, -1, wname, _countof(wname)) && ShouldBuffer(wname, access)) {
            flags &= ~FILE_FLAG_NO_BUFFERING;
            NoteChange(wname);
        }
    }
    return Real_CreateFileA(name, access, share, sa, disp, flags, tmpl);
}

static HANDLE WINAPI Hook_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disp,
                                      LPCREATEFILE2_EXTENDED_PARAMETERS ext)
{
    InterlockedIncrement(&g_seen);
    if (ext && (ext->dwFileFlags & FILE_FLAG_NO_BUFFERING) && ShouldBuffer(name, access)) {
        CREATEFILE2_EXTENDED_PARAMETERS copy = *ext;
        copy.dwFileFlags &= ~FILE_FLAG_NO_BUFFERING;
        NoteChange(name);
        return Real_CreateFile2(name, access, share, disp, &copy);
    }
    return Real_CreateFile2(name, access, share, disp, ext);
}

/* ---------- import table patching ---------- */

typedef struct {
    void *targets[2]; /* address in kernel32 and in kernelbase */
    void *hook;
} HookSpec;

static int PatchModule(HMODULE mod, HookSpec *specs, int nspecs)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0;

    if (!mod || mod == g_self || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        void **iat = (void **)(base + imp->FirstThunk);
        for (; *iat; iat++) {
            int s, t;
            for (s = 0; s < nspecs; s++) {
                for (t = 0; t < 2; t++) {
                    if (specs[s].targets[t] && *iat == specs[s].targets[t]) {
                        DWORD old;
                        if (VirtualProtect(iat, sizeof(void *), PAGE_READWRITE, &old)) {
                            InterlockedExchangePointer(iat, specs[s].hook);
                            VirtualProtect(iat, sizeof(void *), old, &old);
                            patched++;
                        }
                    }
                }
            }
        }
    }
    return patched;
}

static void InstallHooks(const WCHAR *realPath)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    HookSpec specs[3] = {0};
    WCHAR exe[MAX_PATH];
    int total = 0, mods = 0;
    typedef BOOL(WINAPI * EnumMods_t)(HANDLE, HMODULE *, DWORD, LPDWORD);
    EnumMods_t enumMods;
    HMODULE list[1024];
    DWORD needed = 0;

    Real_CreateFileW = (CreateFileW_t)GetProcAddress(kb, "CreateFileW");
    Real_CreateFileA = (CreateFileA_t)GetProcAddress(kb, "CreateFileA");
    Real_CreateFile2 = (CreateFile2_t)GetProcAddress(kb, "CreateFile2");
    GetModuleFileNameW(NULL, exe, _countof(exe));

    if (!Real_CreateFileW || !Real_CreateFileA) {
        LogLine(L"ERROR: CreateFile not found in kernelbase.dll, no hooks installed");
        return;
    }

    specs[0].targets[0] = (void *)GetProcAddress(k32, "CreateFileW");
    specs[0].targets[1] = (void *)Real_CreateFileW;
    specs[0].hook = (void *)Hook_CreateFileW;
    specs[1].targets[0] = (void *)GetProcAddress(k32, "CreateFileA");
    specs[1].targets[1] = (void *)Real_CreateFileA;
    specs[1].hook = (void *)Hook_CreateFileA;
    specs[2].targets[0] = (void *)GetProcAddress(k32, "CreateFile2");
    specs[2].targets[1] = (void *)Real_CreateFile2;
    specs[2].hook = (void *)Hook_CreateFile2;

    /* Patch every module loaded so far. kernel32 forwards its CreateFile functions to
       kernelbase through its own import table, so patching kernel32 also covers modules
       that are loaded later and import from kernel32. */
    enumMods = (EnumMods_t)GetProcAddress(k32, "K32EnumProcessModules");
    if (enumMods && enumMods(GetCurrentProcess(), list, sizeof(list), &needed)) {
        DWORD i, count = needed / sizeof(HMODULE);
        if (count > (DWORD)_countof(list))
            count = (DWORD)_countof(list);
        for (i = 0; i < count; i++) {
            int p = PatchModule(list[i], specs, 3);
            if (p) {
                total += p;
                mods++;
            }
        }
    } else {
        total += PatchModule(GetModuleHandleW(NULL), specs, 3);
        total += PatchModule(k32, specs, 3);
        mods = 2;
    }

    LogLine(L"gamecache %s loaded into %s", GC_VERSION_WSTR, exe);
    LogLine(L"forwarding to %s (%d of %d exports resolved)", realPath, g_resolved, GC_EXPORT_COUNT);
    LogLine(L"%s - patched %d import entries in %d modules, patterns: %s",
            g_enabled ? L"active" : L"DISABLED (enabled=0)", total, mods,
            g_patternCount ? g_patternBuf : L"(all files)");
}

/* ---------- configuration ---------- */

static void LoadConfig(void)
{
    WCHAR ini[MAX_PATH], *p, *slash = NULL, copy[1024], *ctx = NULL, *tok;
    size_t used = 0, dirLen;

    GetModuleFileNameW(g_self, ini, _countof(ini));
    for (p = ini; *p; p++)
        if (*p == L'\\')
            slash = p;
    if (!slash)
        return;
    dirLen = (size_t)(slash + 1 - ini);
    wcsncpy_s(g_logPath, _countof(g_logPath), ini, dirLen);
    wcscat_s(g_logPath, _countof(g_logPath), L"gamecache.log");
    wcscpy_s(slash + 1, _countof(ini) - dirLen, L"gamecache.ini");

    g_enabled = GetPrivateProfileIntW(L"gamecache", L"enabled", 1, ini) != 0;
    g_log = GetPrivateProfileIntW(L"gamecache", L"log", 1, ini) != 0;
    GetPrivateProfileStringW(L"gamecache", L"patterns", L"", g_patternBuf, _countof(g_patternBuf), ini);

    wcscpy_s(copy, _countof(copy), g_patternBuf);
    for (tok = wcstok_s(copy, L";, ", &ctx); tok && g_patternCount < MAX_PATTERNS;
         tok = wcstok_s(NULL, L";, ", &ctx)) {
        size_t len = wcslen(tok) + 1;
        if (used + len > _countof(g_patternStore))
            break;
        wcscpy_s(g_patternStore + used, len, tok);
        g_patterns[g_patternCount++] = g_patternStore + used;
        used += len;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR realPath[MAX_PATH] = L"(system directory not found)";
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logLock);
        ResolveForwards(realPath, _countof(realPath));
        LoadConfig();
        InstallHooks(realPath);
    } else if (reason == DLL_PROCESS_DETACH) {
        /* during process exit another thread may still hold the lock: only try */
        if (g_logFile != INVALID_HANDLE_VALUE && TryEnterCriticalSection(&g_logLock)) {
            LeaveCriticalSection(&g_logLock);
            LogLine(L"exit - %ld file opens seen, %ld switched to buffered", g_seen, g_changed);
        }
    }
    return TRUE;
}
