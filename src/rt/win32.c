/* Native replacements for the Windows functions sgens.exe
 * imports (KERNEL32, USER32, GDI32, WINMM; ADVAPI32 is registry.c, DDRAW
 * ddraw.c, DSOUND dsound.c, windows and messages window.c).
 *
 * Every native reads its arguments from the guest stack (ARG) and returns
 * like the stdcall original (RET pops them).  The game sees a Windows 95
 * machine: C:\STGENS is the installed game (rt_root\hd), D:\ the CD
 * (rt_root\cd). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rt.h"

void thunk_set(u32 a, NativeFn fn, const char *name);
static u32 last_error;
static u32 str_cmdline, str_env, str_envw;

const char *gstr(u32 a) { return a ? (const char *)MEM + a : NULL; }

u32 now_ms(void) { return timeGetTime(); }

/* ---------------- paths ---------------- */
/* The game sees C:\STGENS (the install folder, rt_root\hd) and the CD in
   D:\ (rt_root\cd, both discs merged).  Relative paths use the guest's
   current directory (SetCurrentDirectoryA). */
static char guest_cwd[260] = "C:\\STGENS";

void path_to_host(const char *g, char *out, size_t n)
{
    char tmp[700];
    const char *p = g;
    if (isalpha((u8)p[0]) && p[1] == ':') {
        if (p[2] == '\\' || p[2] == '/') snprintf(tmp, sizeof tmp, "%s", p);
        else snprintf(tmp, sizeof tmp, "%c:\\%s", p[0], p + 2);
    } else if (*p == '\\' || *p == '/') {
        snprintf(tmp, sizeof tmp, "%c:%s", guest_cwd[0], p);
    } else {
        snprintf(tmp, sizeof tmp, "%s\\%s", guest_cwd, p);
    }
    for (char *c = tmp; *c; c++) if (*c == '/') *c = '\\';
    int d = toupper((u8)tmp[0]);
    const char *base, *rest = tmp + 2;
    if (d == 'D') base = "cd";
    else {
        base = "hd";
        if (!_strnicmp(rest, "\\STGENS", 7) && (rest[7] == 0 || rest[7] == '\\')) rest += 7;
    }
    while (*rest == '\\') rest++;
    snprintf(out, n, "%s\\%s\\%s", rt_root, base, rest);
    size_t l = strlen(out);
    while (l && out[l - 1] == '\\') out[--l] = 0;
}

static void api_SetCurrentDirectoryA(void)
{
    char h[700];
    const char *p = gstr(ARG(0));
    path_to_host(p, h, sizeof h);
    DWORD a = GetFileAttributesA(h);
    rt_log("SetCurrentDirectoryA %s -> %s\n", p, (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? "ok" : "missing");
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) RET(0, 1);
    char t[700];
    if (isalpha((u8)p[0]) && p[1] == ':') snprintf(t, sizeof t, "%s", p);
    else if (*p == '\\') snprintf(t, sizeof t, "%c:%s", guest_cwd[0], p);
    else snprintf(t, sizeof t, "%s\\%s", guest_cwd, p);
    size_t l = strlen(t);
    while (l > 3 && (t[l - 1] == '\\' || t[l - 1] == '/')) t[--l] = 0;
    snprintf(guest_cwd, sizeof guest_cwd, "%s", t);
    RET(1, 1);
}

/* ---------------- handles (files, find, mutex...) ---------------- */
#define MAXF 256
static GH ghandles[MAXF];

u32 gh_new(int kind, HANDLE h)
{
    for (int i = 4; i < MAXF; i++)
        if (!ghandles[i].used) { memset(&ghandles[i], 0, sizeof ghandles[i]); ghandles[i].used = 1; ghandles[i].kind = kind; ghandles[i].h = h; return 0x100 + i * 4; }
    rt_fatal("out of handles");
    return 0;
}

GH *gh_get(u32 h)
{
    u32 i = (h - 0x100) / 4;
    if (h < 0x100 || i >= MAXF || !ghandles[i].used) return NULL;
    return &ghandles[i];
}

/* ---------------- KERNEL32 ---------------- */
static void api_GetVersion(void) { RET(0xc0000004u, 0); }   /* Windows 95 */
static void api_GetCommandLineA(void) { RET(str_cmdline, 0); }
static void api_GetEnvironmentStrings(void) { RET(str_env, 0); }
static void api_GetEnvironmentStringsW(void) { RET(str_envw, 0); }
static void api_FreeEnvironmentStringsA(void) { RET(1, 1); }
static void api_FreeEnvironmentStringsW(void) { RET(1, 1); }
static void api_SetEnvironmentVariableA(void) { RET(1, 2); }

static void api_GetStartupInfoA(void)
{
    memset(MEM + ARG(0), 0, 68);
    WR32(ARG(0), 68);
    RET(0, 1);
}

static void api_GetModuleHandleA(void)
{
    const char *n = gstr(ARG(0));
    if (!n) RET(EXE_BASE, 1);
    if (!_stricmp(n, "sgens.exe") || !_stricmp(n, "sgens")) RET(EXE_BASE, 1);
    RET(0x7e000000u, 1);
}

static void api_GetModuleFileNameA(void)
{
    const char *s = "C:\\STGENS\\SGENS.EXE";
    u32 n = ARG(2);
    if (n) {
        strncpy((char *)MEM + ARG(1), s, n);
        WR8(ARG(1) + n - 1, 0);
    }
    RET(strlen(s) < n ? strlen(s) : n, 3);
}

static void api_GetCurrentDirectoryA(void)
{
    const char *s = guest_cwd;
    if (ARG(0) > strlen(s)) strcpy((char *)MEM + ARG(1), s);
    RET(strlen(s), 2);
}

static void api_GetFullPathNameA(void)
{
    char out[600];
    const char *p = gstr(ARG(0));
    if (isalpha((u8)p[0]) && p[1] == ':') snprintf(out, sizeof out, "%s", p);
    else if (p[0] == '\\') snprintf(out, sizeof out, "%c:%s", guest_cwd[0], p);
    else snprintf(out, sizeof out, "%s\\%s", guest_cwd, p);
    u32 n = (u32)strlen(out);
    if (ARG(1) > n) {
        strcpy((char *)MEM + ARG(2), out);
        if (ARG(3)) { char *b = strrchr((char *)MEM + ARG(2), '\\'); WR32(ARG(3), b ? ARG(2) + (u32)(b + 1 - ((char *)MEM + ARG(2))) : 0); }
        RET(n, 4);
    }
    RET(n + 1, 4);
}

static void api_GetDriveTypeA(void)
{
    const char *p = gstr(ARG(0));
    int d = p ? toupper((u8)p[0]) : 'C';
    RET(d == 'C' ? DRIVE_FIXED : d == 'D' ? DRIVE_CDROM : DRIVE_NO_ROOT_DIR, 1);
}

static void api_HeapCreate(void) { RET(0x00ee0000u, 3); }
static void api_HeapAlloc(void) { RET(gmalloc(ARG(2)), 3); }
static void api_HeapFree(void) { gfree(ARG(2)); RET(1, 3); }
static void api_HeapSize(void) { RET(gsize(ARG(2)), 3); }
static void api_HeapReAlloc(void)
{
    u32 p = ARG(2), n = ARG(3);
    if (ARG(1) & 0x10) {   /* HEAP_REALLOC_IN_PLACE_ONLY */
        if (n + 16 <= (1u << RD32(p - 16 + 4))) { WR32(p - 16, n); RET(p, 4); }
        RET(0, 4);
    }
    RET(grealloc(p, n), 4);
}

static void api_LocalAlloc(void) { RET(gmalloc(ARG(1)), 2); }
static void api_LocalFree(void) { gfree(ARG(0)); RET(0, 1); }
static void api_LocalLock(void) { RET(ARG(0), 1); }
static void api_LocalUnlock(void) { RET(0, 1); }
static void api_LocalHandle(void) { RET(ARG(0), 1); }
static void api_LocalSize(void) { RET(gsize(ARG(0)), 1); }
static void api_LocalReAlloc(void) { RET(grealloc(ARG(0), ARG(1)), 3); }

static void api_GlobalMemoryStatus(void)
{
    u32 m = ARG(0);
    WR32(m, 32);
    WR32(m + 4, 20);
    WR32(m + 8, 256u << 20); WR32(m + 12, 200u << 20);
    WR32(m + 16, 512u << 20); WR32(m + 20, 400u << 20);
    WR32(m + 24, 0x7ffe0000u); WR32(m + 28, 0x70000000u);
    RET(0, 1);
}

static void api_GetACP(void) { RET(1252, 0); }
static void api_GetOEMCP(void) { RET(437, 0); }
static void api_GetCPInfo(void)
{
    u32 p = ARG(1);
    memset(MEM + p, 0, 20);
    WR32(p, 1);
    WR8(p + 4, '?');
    RET(1, 2);
}

/* string conversions: the guest's buffers are host memory (MEM + addr) */
static void api_MultiByteToWideChar(void)
{
    int r = MultiByteToWideChar(ARG(0), ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3),
                                ARG(4) ? (LPWSTR)(MEM + ARG(4)) : NULL, (int)ARG(5));
    RET(r, 6);
}
static void api_WideCharToMultiByte(void)
{
    int r = WideCharToMultiByte(ARG(0), ARG(1), (LPCWSTR)(MEM + ARG(2)), (int)ARG(3),
                                ARG(4) ? (LPSTR)(MEM + ARG(4)) : NULL, (int)ARG(5), NULL, NULL);
    RET(r, 8);
}
static void api_LCMapStringA(void)
{
    int r = LCMapStringA(ARG(0), ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3), ARG(4) ? (LPSTR)(MEM + ARG(4)) : NULL, (int)ARG(5));
    RET(r, 6);
}
static void api_LCMapStringW(void)
{
    int r = LCMapStringW(ARG(0), ARG(1), (LPCWSTR)(MEM + ARG(2)), (int)ARG(3), ARG(4) ? (LPWSTR)(MEM + ARG(4)) : NULL, (int)ARG(5));
    RET(r, 6);
}
static void api_CompareStringA(void)
{
    RET(CompareStringA(ARG(0), ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3), (LPCSTR)(MEM + ARG(4)), (int)ARG(5)), 6);
}
static void api_CompareStringW(void)
{
    RET(CompareStringW(ARG(0), ARG(1), (LPCWSTR)(MEM + ARG(2)), (int)ARG(3), (LPCWSTR)(MEM + ARG(4)), (int)ARG(5)), 6);
}
static void api_GetStringTypeA(void)
{
    RET(GetStringTypeA(ARG(0), ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3), (LPWORD)(MEM + ARG(4))), 5);
}
static void api_GetStringTypeW(void)
{
    RET(GetStringTypeW(ARG(0), (LPCWSTR)(MEM + ARG(1)), (int)ARG(2), (LPWORD)(MEM + ARG(3))), 4);
}

static void api_GetTimeZoneInformation(void) { RET(GetTimeZoneInformation((LPTIME_ZONE_INFORMATION)(MEM + ARG(0))), 1); }
static void api_GetSystemTime(void) { GetSystemTime((LPSYSTEMTIME)(MEM + ARG(0))); RET(0, 1); }
static void api_SystemTimeToFileTime(void) { RET(SystemTimeToFileTime((SYSTEMTIME *)(MEM + ARG(0)), (FILETIME *)(MEM + ARG(1))), 2); }
static void api_FileTimeToSystemTime(void) { RET(FileTimeToSystemTime((FILETIME *)(MEM + ARG(0)), (SYSTEMTIME *)(MEM + ARG(1))), 2); }
static void api_FileTimeToLocalFileTime(void) { RET(FileTimeToLocalFileTime((FILETIME *)(MEM + ARG(0)), (FILETIME *)(MEM + ARG(1))), 2); }

static void api_IsBadPtr(void) { RET(0, 2); }
static void api_IsBadCodePtr(void) { RET(0, 1); }
static void api_SetUnhandledExceptionFilter(void) { RET(0, 1); }
static void api_UnhandledExceptionFilter(void) { RET(0, 1); }
static void api_RtlUnwind(void)
{
    /* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): no
       exceptions are ever raised here, so only drop the SEH frames above
       the target */
    u32 target = ARG(0);
    rt_log("RtlUnwind to frame %08x\n", target);
    if (target) WR32(TEB_ADDR, target);
    RET(0, 4);
}

static void api_OutputDebugStringA(void) { rt_log("debug: %s\n", gstr(ARG(0))); RET(0, 1); }
static void api_GetLastError(void) { RET(last_error, 0); }

static void api_ExitProcess(void)
{
    rt_log("ExitProcess(%d)\n", ARG(0));
    reg_save();
    exit((int)ARG(0));
}
static void api_TerminateProcess(void) { rt_log("TerminateProcess\n"); reg_save(); exit((int)ARG(1)); }
static void api_GetCurrentProcess(void) { RET(0xffffffffu, 0); }
static void api_GetCurrentProcessId(void) { RET(0x40, 0); }
static void api_CloseHandle(void)
{
    GH *g = gh_get(ARG(0));
    if (g) {
        if (g->kind == 1 && g->h) CloseHandle(g->h);
        if (g->kind == 2 && g->h) FindClose(g->h);
        g->used = 0;
    }
    RET(1, 1);
}

static void api_GetStdHandle(void) { RET(0x10 + (ARG(0) & 3), 1); }
static void api_SetStdHandle(void) { RET(1, 2); }
static void api_SetHandleCount(void) { RET(ARG(0), 1); }
static void api_GetFileType(void)
{
    GH *g = gh_get(ARG(0));
    RET(g && g->kind == 1 ? FILE_TYPE_DISK : ARG(0) >= 0x10 && ARG(0) < 0x14 ? FILE_TYPE_CHAR : FILE_TYPE_UNKNOWN, 1);
}

/* CreateFileA(name, access, share, sec, disp, flags, template) */
static void api_CreateFileA(void)
{
    char p[700];
    path_to_host(gstr(ARG(0)), p, sizeof p);
    HANDLE h = CreateFileA(p, ARG(1), ARG(2) | FILE_SHARE_READ, NULL, ARG(4), FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        last_error = GetLastError();
        rt_log("CreateFileA %s -> %s failed (%u)\n", gstr(ARG(0)), p, last_error);
        RET(0xffffffffu, 7);
    }
    rt_log("CreateFileA %s\n", gstr(ARG(0)));
    RET(gh_new(1, h), 7);
}

static void api_ReadFile(void)
{
    GH *g = gh_get(ARG(0));
    DWORD got = 0;
    if (!g || g->kind != 1) { last_error = ERROR_INVALID_HANDLE; RET(0, 5); }
    BOOL ok = ReadFile(g->h, MEM + ARG(1), ARG(2), &got, NULL);
    if (ARG(3)) WR32(ARG(3), got);
    if (!ok) last_error = GetLastError();
    RET(ok, 5);
}

static void api_WriteFile(void)
{
    DWORD put = 0;
    u32 h = ARG(0);
    if (h >= 0x10 && h < 0x14) {   /* std handles: to the log */
        rt_log("%.*s", (int)ARG(2), (char *)MEM + ARG(1));
        if (ARG(3)) WR32(ARG(3), ARG(2));
        RET(1, 5);
    }
    GH *g = gh_get(h);
    if (!g || g->kind != 1) { last_error = ERROR_INVALID_HANDLE; RET(0, 5); }
    BOOL ok = WriteFile(g->h, MEM + ARG(1), ARG(2), &put, NULL);
    if (ARG(3)) WR32(ARG(3), put);
    RET(ok, 5);
}

static void api_SetFilePointer(void)
{
    GH *g = gh_get(ARG(0));
    if (!g || g->kind != 1) { last_error = ERROR_INVALID_HANDLE; RET(0xffffffffu, 4); }
    LONG hi = ARG(2) ? (LONG)RD32(ARG(2)) : 0;
    DWORD r = SetFilePointer(g->h, (LONG)ARG(1), ARG(2) ? &hi : NULL, ARG(3));
    if (ARG(2)) WR32(ARG(2), hi);
    if (r == INVALID_SET_FILE_POINTER) last_error = GetLastError();
    RET(r, 4);
}

static void api_FlushFileBuffers(void) { RET(1, 1); }

static void find_to_guest(WIN32_FIND_DATAA *fd, u32 g)
{
    memcpy(MEM + g, fd, sizeof *fd);   /* same layout (320 bytes) */
}

static void api_FindFirstFileA(void)
{
    char p[700];
    path_to_host(gstr(ARG(0)), p, sizeof p);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        last_error = GetLastError();
        rt_log("FindFirstFileA %s -> %s: none\n", gstr(ARG(0)), p);
        RET(0xffffffffu, 2);
    }
    find_to_guest(&fd, ARG(1));
    RET(gh_new(2, h), 2);
}

static void api_FindNextFileA(void)
{
    GH *g = gh_get(ARG(0));
    WIN32_FIND_DATAA fd;
    if (!g || g->kind != 2 || !FindNextFileA(g->h, &fd)) { last_error = ERROR_NO_MORE_FILES; RET(0, 2); }
    find_to_guest(&fd, ARG(1));
    RET(1, 2);
}

static void api_FindClose(void)
{
    GH *g = gh_get(ARG(0));
    if (g && g->kind == 2) { FindClose(g->h); g->used = 0; }
    RET(1, 1);
}

static void api_LoadLibraryA(void)
{
    const char *n = gstr(ARG(0));
    rt_log("LoadLibraryA %s\n", n);
    /* a pseudo module handle per DLL name, resolved by GetProcAddress */
    u32 h = gstrdup(n ? n : "");
    RET(h | 0x80000000u, 1);
}
static void api_LoadModule(void) { RET(2, 2); }
static void api_FreeLibrary(void) { RET(1, 1); }

static void api_GetProcAddress(void)
{
    u32 h = ARG(0);
    const char *nm = gstr(ARG(1));
    if (ARG(1) < 0x10000) RET(0, 2);
    const char *dll = (h & 0x80000000u) ? (const char *)MEM + (h & 0x7fffffffu) : "KERNEL32.DLL";
    u32 a = api_resolve(dll, nm);
    rt_log("GetProcAddress %s!%s -> %08x\n", dll, nm, a);
    RET(a, 2);
}

static void api_SetErrorMode(void) { RET(0, 1); }
static void api_SetConsoleCtrlHandler(void) { RET(1, 2); }

/* ---------------- WINMM ---------------- */
static void api_timeGetTime(void) { timers_poll(); RET(now_ms(), 0); }
static void api_timeBeginPeriod(void) { RET(0, 1); }
static void api_timeEndPeriod(void) { RET(0, 1); }

/* timeSetEvent(delay, res, callback, user, flags): callbacks run at API calls */
typedef struct { int used; u32 delay, cb, user, flags, next; } Timer;
static Timer timers[8];
static int in_timer;
static void api_timeSetEvent(void)
{
    for (int i = 0; i < 8; i++)
        if (!timers[i].used) {
            timers[i] = (Timer){ 1, ARG(0) ? ARG(0) : 1, ARG(2), ARG(3), ARG(4), now_ms() + ARG(0) };
            RET(i + 1, 5);
        }
    RET(0, 5);
}
static void api_timeKillEvent(void) { if (ARG(0) - 1 < 8) timers[ARG(0) - 1].used = 0; RET(0, 1); }

void timers_poll(void)
{
    if (in_timer) return;
    in_timer = 1;
    u32 t = now_ms();
    for (int i = 0; i < 8; i++) {
        Timer *tm = &timers[i];
        if (!tm->used || (s32)(t - tm->next) < 0) continue;
        tm->next = t + tm->delay;
        if (!(tm->flags & 1)) tm->used = 0;   /* TIME_ONESHOT */
        guest_call(tm->cb, 5, i + 1, 0, tm->user, 0, 0);
    }
    in_timer = 0;
}

/* waveOut: no device, Smacker uses DirectSound */
static void api_waveOutOpen(void) { RET(MMSYSERR_NODRIVER, 6); }
static void api_waveOut2(void) { RET(MMSYSERR_INVALHANDLE, 2); }
static void api_waveOut1(void) { RET(MMSYSERR_INVALHANDLE, 1); }
static void api_waveOut3(void) { RET(MMSYSERR_INVALHANDLE, 3); }

/* mmio on host files */
#define MAXMM 16
static HMMIO mm[MAXMM];
static void api_mmioOpenA(void)
{
    char p[700];
    path_to_host(gstr(ARG(0)), p, sizeof p);
    HMMIO h = mmioOpenA(p, NULL, ARG(2));
    if (!h) { rt_log("mmioOpenA %s failed\n", p); RET(0, 3); }
    for (int i = 0; i < MAXMM; i++) if (!mm[i]) { mm[i] = h; RET(0x200 + i, 3); }
    RET(0, 3);
}
static HMMIO mmh(u32 h) { return h >= 0x200 && h < 0x200 + MAXMM ? mm[h - 0x200] : NULL; }
static void api_mmioClose(void) { HMMIO h = mmh(ARG(0)); if (h) { mmioClose(h, 0); mm[ARG(0) - 0x200] = NULL; } RET(0, 2); }
static void api_mmioRead(void) { HMMIO h = mmh(ARG(0)); RET(h ? mmioRead(h, (HPSTR)(MEM + ARG(1)), (LONG)ARG(2)) : -1, 3); }
static void api_mmioWrite(void) { HMMIO h = mmh(ARG(0)); RET(h ? mmioWrite(h, (const char *)(MEM + ARG(1)), (LONG)ARG(2)) : -1, 3); }
static void api_mmioSeek(void) { HMMIO h = mmh(ARG(0)); RET(h ? mmioSeek(h, (LONG)ARG(1), (int)ARG(2)) : -1, 3); }

/* ---------------- USER32 (window side in window.c) ---------------- */
void api_window_table(void);
static void api_MessageBoxA(void)
{
    rt_log("MessageBoxA: [%s] %s\n", gstr(ARG(2)) ? gstr(ARG(2)) : "", gstr(ARG(1)) ? gstr(ARG(1)) : "");
    if (getenv(ENV("HIDDEN"))) RET(IDOK, 4);
    win_cursor(1);
    RET(MessageBoxA(NULL, gstr(ARG(1)), gstr(ARG(2)), ARG(3) & ~0x3000u), 4);
}

/* wsprintfA(buf, fmt, ...): cdecl */
/* printf with the guest's arguments (dwords from guest address va) */
int rt_gsprintf(char *out, size_t n, const char *f, u32 va)
{
    size_t o = 0;
    while (*f && o + 1 < n) {
        if (*f != '%') { out[o++] = *f++; continue; }
        char spec[32];
        int s = 0;
        spec[s++] = *f++;
        while (*f && strchr("-+ #0123456789.lh", *f) && s < 28) spec[s++] = *f++;
        char c = *f ? *f++ : 0;
        spec[s++] = c;
        spec[s] = 0;
        if (c == 's') {
            const char *a = gstr(RD32(va));
            o += snprintf(out + o, n - o, spec, a ? a : "(null)");
            va += 4;
        } else if (c == 'c') {
            out[o++] = (char)RD32(va);
            va += 4;
        } else if (c == '%') out[o++] = '%';
        else if (c) {
            /* ints are 32 bits: drop size modifiers */
            char sp2[32]; int k = 0;
            for (int i = 0; spec[i]; i++) if (spec[i] != 'l' && spec[i] != 'h') sp2[k++] = spec[i];
            sp2[k] = 0;
            o += snprintf(out + o, n - o, sp2, RD32(va));
            va += 4;
        }
        if (o >= n) o = n - 1;
    }
    out[o] = 0;
    return (int)o;
}

static void api_wsprintfA(void)
{
    char out[1100];
    int o = rt_gsprintf(out, 1025, gstr(ARG(1)), cpu.r[R_ESP] + 12);
    memcpy(MEM + ARG(0), out, o + 1);
    RETC(o);
}

/* ---------------- table ---------------- */
typedef struct { const char *name; NativeFn fn; } ApiEnt;
extern const ApiEnt api_tab_threads[], api_tab_rt[];
static const ApiEnt api_tab[] = {
    {"SetCurrentDirectoryA", api_SetCurrentDirectoryA},
    {"GetVersion", api_GetVersion}, {"GetCommandLineA", api_GetCommandLineA},
    {"GetEnvironmentStrings", api_GetEnvironmentStrings}, {"GetEnvironmentStringsA", api_GetEnvironmentStrings},
    {"GetEnvironmentStringsW", api_GetEnvironmentStringsW}, {"FreeEnvironmentStringsA", api_FreeEnvironmentStringsA},
    {"FreeEnvironmentStringsW", api_FreeEnvironmentStringsW}, {"SetEnvironmentVariableA", api_SetEnvironmentVariableA},
    {"GetStartupInfoA", api_GetStartupInfoA}, {"GetModuleHandleA", api_GetModuleHandleA},
    {"GetModuleFileNameA", api_GetModuleFileNameA}, {"GetCurrentDirectoryA", api_GetCurrentDirectoryA},
    {"GetFullPathNameA", api_GetFullPathNameA}, {"GetDriveTypeA", api_GetDriveTypeA},
    {"HeapCreate", api_HeapCreate}, {"HeapAlloc", api_HeapAlloc}, {"HeapFree", api_HeapFree},
    {"HeapSize", api_HeapSize}, {"HeapReAlloc", api_HeapReAlloc},
    {"LocalAlloc", api_LocalAlloc}, {"LocalFree", api_LocalFree}, {"LocalLock", api_LocalLock},
    {"LocalUnlock", api_LocalUnlock}, {"LocalHandle", api_LocalHandle}, {"LocalSize", api_LocalSize},
    {"LocalReAlloc", api_LocalReAlloc}, 
    {"GlobalMemoryStatus", api_GlobalMemoryStatus}, {"GetACP", api_GetACP}, {"GetOEMCP", api_GetOEMCP},
    {"GetCPInfo", api_GetCPInfo}, {"MultiByteToWideChar", api_MultiByteToWideChar},
    {"WideCharToMultiByte", api_WideCharToMultiByte}, {"LCMapStringA", api_LCMapStringA},
    {"LCMapStringW", api_LCMapStringW}, {"CompareStringA", api_CompareStringA}, {"CompareStringW", api_CompareStringW},
    {"GetStringTypeA", api_GetStringTypeA}, {"GetStringTypeW", api_GetStringTypeW},
    {"GetTimeZoneInformation", api_GetTimeZoneInformation}, {"GetSystemTime", api_GetSystemTime},
    {"SystemTimeToFileTime", api_SystemTimeToFileTime}, {"FileTimeToSystemTime", api_FileTimeToSystemTime},
    {"FileTimeToLocalFileTime", api_FileTimeToLocalFileTime},
    {"IsBadReadPtr", api_IsBadPtr}, {"IsBadWritePtr", api_IsBadPtr}, {"IsBadCodePtr", api_IsBadCodePtr},
    {"SetUnhandledExceptionFilter", api_SetUnhandledExceptionFilter}, {"UnhandledExceptionFilter", api_UnhandledExceptionFilter},
    {"RtlUnwind", api_RtlUnwind}, {"OutputDebugStringA", api_OutputDebugStringA}, {"GetLastError", api_GetLastError},
    {"ExitProcess", api_ExitProcess}, {"TerminateProcess", api_TerminateProcess},
    {"GetCurrentProcess", api_GetCurrentProcess}, {"GetCurrentProcessId", api_GetCurrentProcessId},
                        {"CloseHandle", api_CloseHandle},
    {"GetStdHandle", api_GetStdHandle}, {"SetStdHandle", api_SetStdHandle}, {"SetHandleCount", api_SetHandleCount},
    {"GetFileType", api_GetFileType}, {"CreateFileA", api_CreateFileA}, {"ReadFile", api_ReadFile},
    {"WriteFile", api_WriteFile}, {"SetFilePointer", api_SetFilePointer}, {"FlushFileBuffers", api_FlushFileBuffers},
    {"FindFirstFileA", api_FindFirstFileA}, {"FindNextFileA", api_FindNextFileA}, {"FindClose", api_FindClose},
    {"LoadLibraryA", api_LoadLibraryA}, {"LoadModule", api_LoadModule}, {"FreeLibrary", api_FreeLibrary},
    {"GetProcAddress", api_GetProcAddress}, {"SetErrorMode", api_SetErrorMode},
    {"SetConsoleCtrlHandler", api_SetConsoleCtrlHandler},
        {"timeGetTime", api_timeGetTime}, {"timeBeginPeriod", api_timeBeginPeriod}, {"timeEndPeriod", api_timeEndPeriod},
    {"timeSetEvent", api_timeSetEvent}, {"timeKillEvent", api_timeKillEvent},
    {"waveOutOpen", api_waveOutOpen}, {"waveOutClose", api_waveOut1}, {"waveOutGetID", api_waveOut2},
    {"waveOutPrepareHeader", api_waveOut3}, {"waveOutUnprepareHeader", api_waveOut3}, {"waveOutReset", api_waveOut1},
    {"waveOutSetVolume", api_waveOut2}, {"waveOutWrite", api_waveOut3},
    {"mmioOpenA", api_mmioOpenA}, {"mmioClose", api_mmioClose}, {"mmioRead", api_mmioRead},
    {"mmioWrite", api_mmioWrite}, {"mmioSeek", api_mmioSeek},
    {"MessageBoxA", api_MessageBoxA}, {"wsprintfA", api_wsprintfA},
                                                            {NULL, NULL}
};

NativeFn api_lookup(const char *dll, const char *name)
{
    (void)dll;
    for (const ApiEnt *e = api_tab; e->name; e++)
        if (!strcmp(e->name, name)) return e->fn;
    for (const ApiEnt *e = api_tab_threads; e->name; e++)
        if (!strcmp(e->name, name)) return e->fn;
    for (const ApiEnt *e = api_tab_rt; e->name; e++)
        if (!strcmp(e->name, name)) return e->fn;
    NativeFn stub_lookup(const char *name);
    return stub_lookup(name);
    return NULL;
}

u32 api_resolve(const char *dll, const char *name)
{
    NativeFn fn = api_lookup(dll, name);
    if (!fn) return 0;
    static struct { NativeFn fn; u32 a; } cache[64];
    static int nc;
    for (int i = 0; i < nc; i++) if (cache[i].fn == fn) return cache[i].a;
    u32 a = thunk_new(fn, _strdup(name));
    if (nc < 64) { cache[nc].fn = fn; cache[nc].a = a; nc++; }
    return a;
}

void api_init(void)
{
    const char *cl = "C:\\STGENS\\SGENS.EXE";
    str_cmdline = STRINGS_ADDR;
    strcpy((char *)MEM + str_cmdline, cl);
    str_env = STRINGS_ADDR + 0x400;
    const char *env = "PATH=C:\\STGENS";
    strcpy((char *)MEM + str_env, env);
    WR16(str_env + strlen(env) + 1, 0);
    str_envw = STRINGS_ADDR + 0x800;
    for (size_t i = 0; env[i]; i++) WR16(str_envw + 2 * i, (u8)env[i]);
    WR32(str_envw + 2 * strlen(env), 0);
    WR32(str_envw + 2 * strlen(env) + 4, 0);
}
