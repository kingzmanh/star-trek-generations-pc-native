/* The rest of KERNEL32 / USER32 / GDI32 / ... that sgens.exe imports.
 *
 * Real replacements where the game depends on the result (memory, strings,
 * files, file mappings, locale, rectangles); the remainder - menus, dialogs,
 * printing, GDI drawing on window DCs that DirectDraw covers - are simple
 * stand-ins with a fixed result, listed in stub_tab (first call logged). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rt.h"

static u32 last_err;
void api_SetLastError(void) { last_err = ARG(0); RET(0, 1); }

/* ---------------- Global* (fixed blocks: handle == pointer) ---------------- */
void api_GlobalAlloc(void) { RET(gmalloc(ARG(1) ? ARG(1) : 1), 2); }
void api_GlobalFree(void) { gfree(ARG(0)); RET(0, 1); }
void api_GlobalLock(void) { RET(ARG(0), 1); }
void api_GlobalUnlock(void) { RET(1, 1); }
void api_GlobalHandle(void) { RET(ARG(0), 1); }
void api_GlobalSize(void) { RET(gsize(ARG(0)), 1); }
void api_GlobalFlags(void) { RET(0, 1); }
void api_GlobalReAlloc(void) { RET(grealloc(ARG(0), ARG(1)), 3); }
void api_GlobalDeleteAtom(void) { RET(0, 1); }
void api_GetProcessHeap(void) { RET(0x00ee0000u, 0); }
void api_HeapDestroy(void) { RET(1, 1); }

/* ---------------- strings ---------------- */
void api_lstrlenA(void) { RET(ARG(0) ? strlen(gstr(ARG(0))) : 0, 1); }
void api_lstrcpyA(void) { strcpy((char *)MEM + ARG(0), gstr(ARG(1))); RET(ARG(0), 2); }
void api_lstrcpynA(void)
{
    u32 n = ARG(2);
    if (n) { strncpy((char *)MEM + ARG(0), gstr(ARG(1)), n - 1); WR8(ARG(0) + n - 1, 0); }
    RET(ARG(0), 3);
}
void api_lstrcatA(void) { strcat((char *)MEM + ARG(0), gstr(ARG(1))); RET(ARG(0), 2); }
void api_lstrcmpA(void) { RET(lstrcmpA(gstr(ARG(0)), gstr(ARG(1))), 2); }
void api_lstrcmpiA(void) { RET(lstrcmpiA(gstr(ARG(0)), gstr(ARG(1))), 2); }
void api_CharUpperA(void)
{
    u32 p = ARG(0);
    if (p < 0x10000) RET(toupper(p & 0xff), 1);
    for (char *s = (char *)MEM + p; *s; s++) *s = (char)toupper((u8)*s);
    RET(p, 1);
}
void api_CharLowerA(void)
{
    u32 p = ARG(0);
    if (p < 0x10000) RET(tolower(p & 0xff), 1);
    for (char *s = (char *)MEM + p; *s; s++) *s = (char)tolower((u8)*s);
    RET(p, 1);
}
void api_CharToOemA(void) { if (ARG(0) != ARG(1)) strcpy((char *)MEM + ARG(1), gstr(ARG(0))); RET(1, 2); }
void api_OemToCharA(void) { if (ARG(0) != ARG(1)) strcpy((char *)MEM + ARG(1), gstr(ARG(0))); RET(1, 2); }
void api_MulDiv(void) { s32 c = (s32)ARG(2); RET(c ? (s32)(((s64)(s32)ARG(0) * (s32)ARG(1) + (c / 2)) / c) : -1, 3); }

/* wvsprintfA(buf, fmt, va): the guest's argument list */
int rt_gsprintf(char *out, size_t n, const char *fmt, u32 va);
void api_wvsprintfA(void)
{
    char tmp[2048];
    int r = rt_gsprintf(tmp, sizeof tmp, gstr(ARG(1)), ARG(2));
    strcpy((char *)MEM + ARG(0), tmp);
    RET(r, 3);
}

/* FormatMessageA: system messages only, as "error N" */
void api_FormatMessageA(void)
{
    char t[64];
    snprintf(t, sizeof t, "error %u", ARG(2));
    u32 buf = ARG(4);
    if (ARG(0) & 0x100) { u32 p = gstrdup(t); WR32(buf, p); }
    else if (ARG(5)) snprintf((char *)MEM + buf, ARG(5), "%s", t);
    RET(strlen(t), 7);
}

/* ---------------- system info ---------------- */
void api_GetVersionExA(void)
{
    u32 p = ARG(0);
    WR32(p + 4, 4); WR32(p + 8, 0); WR32(p + 12, 950); WR32(p + 16, 1);   /* Windows 95 */
    WR8(p + 20, 0);
    RET(1, 1);
}
void api_GetProcessVersion(void) { RET(0x00040000u, 1); }
void api_GetSystemInfo(void)
{
    u32 p = ARG(0);
    memset(MEM + p, 0, 36);
    WR32(p + 4, 0x1000); WR32(p + 8, 0x10000); WR32(p + 12, 0x7ffeffff); WR32(p + 16, 1); WR32(p + 20, 1);
    WR32(p + 24, 586); WR32(p + 28, 0x10000); WR16(p + 32, 5);
    RET(0, 1);
}
void api_GetProcessAffinityMask(void) { WR32(ARG(1), 1); WR32(ARG(2), 1); RET(1, 3); }
void api_SetThreadAffinityMask(void) { RET(1, 2); }
void api_QueryPerformanceFrequency(void) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); WR64(ARG(0), f.QuadPart); RET(1, 1); }
void api_QueryPerformanceCounter(void)
{
    static u32 n;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    WR64(ARG(0), c.QuadPart);
    if ((++n & 255) == 0) thr_yield();
    RET(1, 1);
}
void api_GetLocalTime(void) { GetLocalTime((LPSYSTEMTIME)(MEM + ARG(0))); RET(0, 1); }
void api_LocalFileTimeToFileTime(void) { RET(LocalFileTimeToFileTime((FILETIME *)(MEM + ARG(0)), (FILETIME *)(MEM + ARG(1))), 2); }
void api_GetThreadLocale(void) { RET(0x409, 0); }
void api_GetUserDefaultLCID(void) { RET(0x409, 0); }
void api_IsValidLocale(void) { RET(1, 2); }
void api_IsValidCodePage(void) { RET(ARG(0) == 1252 || ARG(0) == 437, 1); }
void api_GetLocaleInfoA(void)
{
    char t[128];
    int r = GetLocaleInfoA(0x409, ARG(1), t, sizeof t);
    if (ARG(3) && r) snprintf((char *)MEM + ARG(2), ARG(3), "%s", t);
    RET(r, 4);
}
void api_GetLocaleInfoW(void)
{
    WCHAR t[128];
    int r = GetLocaleInfoW(0x409, ARG(1), t, 128);
    if (ARG(3) && r) memcpy(MEM + ARG(2), t, 2 * (r < (int)ARG(3) ? r : (int)ARG(3)));
    RET(r, 4);
}
void api_GetStringTypeExA(void)
{
    RET(GetStringTypeExA(0x409, ARG(1), (LPCSTR)(MEM + ARG(2)), (int)ARG(3), (LPWORD)(MEM + ARG(4))), 5);
}
void api_DebugBreak(void) { rt_log("DebugBreak from %08x\n", RD32(cpu.r[R_ESP])); RET(0, 0); }
void api_RaiseException(void) { rt_fatal("RaiseException %08x", ARG(0)); }
void api_FatalAppExitA(void) { rt_fatal("FatalAppExit: %s", gstr(ARG(1))); }
void api_DebugActiveProcess(void) { RET(0, 1); }
void api_OpenProcess(void) { RET(0x7c00, 3); }
void api_DuplicateHandle(void) { if (ARG(3)) WR32(ARG(3), ARG(1)); RET(1, 7); }

/* ---------------- files ---------------- */
static void hostp(u32 g, char *out, size_t n) { path_to_host(gstr(g), out, n); }

void api_GetFileAttributesA(void)
{
    char p[700];
    hostp(ARG(0), p, sizeof p);
    DWORD a = GetFileAttributesA(p);
    if (a != INVALID_FILE_ATTRIBUTES && !strstr(p, "\\hd\\") ) a |= FILE_ATTRIBUTE_READONLY & 0;
    RET(a, 1);
}
void api_SetFileAttributesA(void) { RET(1, 2); }
void api_CreateDirectoryA(void) { char p[700]; hostp(ARG(0), p, sizeof p); RET(CreateDirectoryA(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS, 2); }
void api_RemoveDirectoryA(void) { char p[700]; hostp(ARG(0), p, sizeof p); RET(RemoveDirectoryA(p), 1); }
void api_DeleteFileA(void) { char p[700]; hostp(ARG(0), p, sizeof p); rt_log("DeleteFileA %s\n", gstr(ARG(0))); RET(DeleteFileA(p), 1); }
void api_MoveFileA(void) { char a[700], b[700]; hostp(ARG(0), a, sizeof a); hostp(ARG(1), b, sizeof b); RET(MoveFileA(a, b), 2); }
void api_CopyFileA(void) { char a[700], b[700]; hostp(ARG(0), a, sizeof a); hostp(ARG(1), b, sizeof b); RET(CopyFileA(a, b, ARG(2)), 3); }
void api_GetTempPathA(void)
{
    const char *t = "C:\\STGENS\\";
    if (ARG(0) > strlen(t)) strcpy((char *)MEM + ARG(1), t);
    RET(strlen(t), 2);
}
void api_GetLogicalDrives(void) { RET((1u << 2) | (1u << 3), 0); }   /* C: D: */
/* GetVolumeInformationA(root, name, cname, serial, maxlen, flags, fs, cfs) */
void api_GetVolumeInformationA(void)
{
    const char *r = gstr(ARG(0));
    int d = r ? toupper((u8)r[0]) : 'C';
    const char *label = d == 'D' ? "GENS-B" : "STGENS";
    if (ARG(1) && ARG(2)) snprintf((char *)MEM + ARG(1), ARG(2), "%s", label);
    if (ARG(3)) WR32(ARG(3), d == 'D' ? 0x12345678u : 0x1997abcdu);
    if (ARG(4)) WR32(ARG(4), 255);
    if (ARG(5)) WR32(ARG(5), 0);
    if (ARG(6) && ARG(7)) snprintf((char *)MEM + ARG(6), ARG(7), "%s", d == 'D' ? "CDFS" : "FAT");
    RET(d == 'C' || d == 'D', 8);
}
/* GetDiskFreeSpaceA(root, spc, bps, free, total) */
void api_GetDiskFreeSpaceA(void)
{
    if (ARG(1)) WR32(ARG(1), 64);
    if (ARG(2)) WR32(ARG(2), 512);
    if (ARG(3)) WR32(ARG(3), 60000);    /* ~1.9 GB, the most Windows 95 reports */
    if (ARG(4)) WR32(ARG(4), 65000);
    RET(1, 5);
}
void api_GetShortPathNameA(void)
{
    const char *s = gstr(ARG(0));
    if (ARG(2) > strlen(s)) strcpy((char *)MEM + ARG(1), s);
    RET(strlen(s), 3);
}

/* INI files: the guest path mapped to the host */
static void ini_path(u32 g, char *out, size_t n)
{
    const char *f = gstr(g);
    if (!f) f = "win.ini";
    if (!strchr(f, '\\') && !strchr(f, '/')) {
        char t[300];
        snprintf(t, sizeof t, "C:\\STGENS\\%s", f);
        path_to_host(t, out, n);
    } else path_to_host(f, out, n);
}
void api_GetPrivateProfileStringA(void)
{
    char p[700];
    ini_path(ARG(5), p, sizeof p);
    DWORD r = GetPrivateProfileStringA(gstr(ARG(0)), gstr(ARG(1)), ARG(2) ? gstr(ARG(2)) : "", (char *)MEM + ARG(3), ARG(4), p);
    RET(r, 6);
}
void api_GetPrivateProfileIntA(void)
{
    char p[700];
    ini_path(ARG(3), p, sizeof p);
    RET(GetPrivateProfileIntA(gstr(ARG(0)), gstr(ARG(1)), (int)ARG(2), p), 4);
}
void api_WritePrivateProfileStringA(void)
{
    char p[700];
    ini_path(ARG(3), p, sizeof p);
    RET(WritePrivateProfileStringA(gstr(ARG(0)), gstr(ARG(1)), gstr(ARG(2)), p), 4);
}

/* file handles (win32.c) */
void api_GetFileSize(void)
{
    GH *g = gh_get(ARG(0));
    if (!g || g->kind != GH_FILE) RET(0xffffffffu, 2);
    DWORD hi = 0, lo = GetFileSize(g->h, &hi);
    if (ARG(1)) WR32(ARG(1), hi);
    RET(lo, 2);
}
void api_SetEndOfFile(void) { GH *g = gh_get(ARG(0)); RET(g && g->kind == GH_FILE ? SetEndOfFile(g->h) : 0, 1); }
void api_LockFile(void) { RET(1, 5); }
void api_UnlockFile(void) { RET(1, 5); }
void api_GetFileTime(void)
{
    GH *g = gh_get(ARG(0));
    if (!g || g->kind != GH_FILE) RET(0, 4);
    FILETIME a, b, c;
    BOOL ok = GetFileTime(g->h, &a, &b, &c);
    if (ARG(1)) memcpy(MEM + ARG(1), &a, 8);
    if (ARG(2)) memcpy(MEM + ARG(2), &b, 8);
    if (ARG(3)) memcpy(MEM + ARG(3), &c, 8);
    RET(ok, 4);
}
void api_SetFileTime(void) { RET(1, 4); }

/* ---------------- file mappings (SmartHeap's shared-memory name space) ---------------- */
/* CreateFileMappingA(file, sec, prot, sizehi, sizelo, name): page-file backed only */
void api_CreateFileMappingA(void)
{
    u32 size = ARG(4);
    if (ARG(0) != 0xffffffffu) rt_log("CreateFileMappingA on a file: not supported\n");
    u32 h = gh_new(GH_MAPPING, NULL);
    GH *g = gh_get(h);
    g->aux = gmalloc(size ? size : 0x1000);
    g->obj = (int)size;
    rt_log("CreateFileMappingA %s size %x -> %08x\n", ARG(5) ? gstr(ARG(5)) : "-", size, g->aux);
    RET(h, 6);
}
void api_OpenFileMappingA(void) { RET(0, 3); }
/* MapViewOfFile(h, access, offhi, offlo, n) / MapViewOfFileEx(..., addr) */
void api_MapViewOfFile(void) { GH *g = gh_get(ARG(0)); RET(g && g->kind == GH_MAPPING ? g->aux + ARG(3) : 0, 5); }
void api_MapViewOfFileEx(void)
{
    GH *g = gh_get(ARG(0));
    if (!g || g->kind != GH_MAPPING) RET(0, 6);
    if (ARG(5) && ARG(5) != g->aux) { rt_log("MapViewOfFileEx at %08x: refused\n", ARG(5)); RET(0, 6); }
    RET(g->aux + ARG(3), 6);
}
void api_UnmapViewOfFile(void) { RET(1, 1); }

/* ---------------- USER32: rectangles, colours, misc ---------------- */
void api_SetRect(void) { WR32(ARG(0), ARG(1)); WR32(ARG(0) + 4, ARG(2)); WR32(ARG(0) + 8, ARG(3)); WR32(ARG(0) + 12, ARG(4)); RET(1, 5); }
void api_CopyRect(void) { memcpy(MEM + ARG(0), MEM + ARG(1), 16); RET(1, 2); }
void api_EqualRect(void) { RET(!memcmp(MEM + ARG(0), MEM + ARG(1), 16), 2); }
void api_OffsetRect(void)
{
    u32 r = ARG(0); s32 dx = (s32)ARG(1), dy = (s32)ARG(2);
    WR32(r, RD32(r) + dx); WR32(r + 4, RD32(r + 4) + dy); WR32(r + 8, RD32(r + 8) + dx); WR32(r + 12, RD32(r + 12) + dy);
    RET(1, 3);
}
void api_InflateRect(void)
{
    u32 r = ARG(0); s32 dx = (s32)ARG(1), dy = (s32)ARG(2);
    WR32(r, RD32(r) - dx); WR32(r + 4, RD32(r + 4) - dy); WR32(r + 8, RD32(r + 8) + dx); WR32(r + 12, RD32(r + 12) + dy);
    RET(1, 3);
}
void api_PtInRect(void)
{
    u32 r = ARG(0); s32 x = (s32)ARG(1), y = (s32)ARG(2);
    RET(x >= (s32)RD32(r) && x < (s32)RD32(r + 8) && y >= (s32)RD32(r + 4) && y < (s32)RD32(r + 12), 3);
}
void api_IntersectRect(void)
{
    u32 d = ARG(0), a = ARG(1), b = ARG(2);
    s32 l = (s32)RD32(a) > (s32)RD32(b) ? (s32)RD32(a) : (s32)RD32(b);
    s32 t = (s32)RD32(a + 4) > (s32)RD32(b + 4) ? (s32)RD32(a + 4) : (s32)RD32(b + 4);
    s32 r = (s32)RD32(a + 8) < (s32)RD32(b + 8) ? (s32)RD32(a + 8) : (s32)RD32(b + 8);
    s32 bo = (s32)RD32(a + 12) < (s32)RD32(b + 12) ? (s32)RD32(a + 12) : (s32)RD32(b + 12);
    if (l >= r || t >= bo) { memset(MEM + d, 0, 16); RET(0, 3); }
    WR32(d, l); WR32(d + 4, t); WR32(d + 8, r); WR32(d + 12, bo);
    RET(1, 3);
}
void api_AdjustWindowRectEx(void) { RET(1, 4); }
void api_GetSysColor(void) { RET(ARG(0) == COLOR_WINDOW ? 0xffffff : ARG(0) == COLOR_BTNFACE ? 0xc0c0c0 : 0, 1); }
void api_GetSysColorBrush(void) { RET(0x00090000u | ARG(0), 1); }
void api_SystemParametersInfoA(void)
{
    u32 a = ARG(0), p = ARG(2);
    if (a == SPI_GETWORKAREA && p) { WR32(p, 0); WR32(p + 4, 0); WR32(p + 8, 640); WR32(p + 12, 480); }
    else if (p && a == SPI_GETSCREENSAVEACTIVE) WR32(p, 0);
    RET(1, 4);
}
void api_MessageBeep(void) { RET(1, 1); }
void api_RegisterWindowMessageA(void) { static u32 next = 0xc200; RET(next++, 1); }
void api_FindWindowA(void) { RET(0, 2); }
void api_GetDialogBaseUnits(void) { RET(0x00100008u, 0); }
/* ToAscii(vk, scan, state, out, flags) */
void api_ToAscii(void)
{
    u32 vk = ARG(0);
    u8 *ks = MEM + ARG(2);
    int shift = (ks[VK_SHIFT] & 0x80) != 0;
    u32 ch = 0;
    if (vk >= 'A' && vk <= 'Z') ch = shift ? vk : vk + 32;
    else if (vk >= '0' && vk <= '9') ch = shift ? ")!@#$%^&*("[vk - '0'] : vk;
    else if (vk == ' ' || vk == 13 || vk == 8 || vk == 27 || vk == 9) ch = vk;
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ch = '0' + vk - VK_NUMPAD0;
    else if (vk == VK_OEM_MINUS) ch = shift ? '_' : '-';
    else if (vk == VK_OEM_PERIOD) ch = shift ? '>' : '.';
    else if (vk == VK_OEM_COMMA) ch = shift ? '<' : ',';
    if (!ch) RET(0, 5);
    WR16(ARG(3), ch);
    RET(1, 5);
}

/* LoadBitmapA / LoadImageA: no GDI bitmaps; a handle the game passes back */
void api_LoadBitmapA(void) { RET(0, 2); }
void api_LoadImageA(void)
{
    u32 type = ARG(2);
    if (type == 2 /* IMAGE_CURSOR */) RET(0x00050000u | (ARG(1) & 0xffff), 6);
    RET(0, 6);
}

/* ---------------- GDI ---------------- */
void api_GetDeviceCaps(void)
{
    u32 i = ARG(1);
    RET(i == BITSPIXEL ? 16 : i == PLANES ? 1 : i == HORZRES ? 640 : i == VERTRES ? 480 : i == LOGPIXELSX || i == LOGPIXELSY ? 96 :
        i == NUMCOLORS ? 0xffffffffu : i == TECHNOLOGY ? DT_RASDISPLAY : i == HORZSIZE ? 320 : i == VERTSIZE ? 240 : 0, 2);
}
/* GetTextMetricsA(dc, tm): an 8x16 system font */
void api_GetClipBox(void) { u32 r = ARG(1); WR32(r, 0); WR32(r + 4, 0); WR32(r + 8, 640); WR32(r + 12, 480); RET(2, 2); }
void api_GetCurrentPositionEx(void) { if (ARG(1)) { WR32(ARG(1), 0); WR32(ARG(1) + 4, 0); } RET(1, 2); }
void api_GetViewportExtEx(void) { if (ARG(1)) { WR32(ARG(1), 1); WR32(ARG(1) + 4, 1); } RET(1, 2); }
void api_GetWindowExtEx(void) { if (ARG(1)) { WR32(ARG(1), 1); WR32(ARG(1) + 4, 1); } RET(1, 2); }
/* the Windows 256-colour system palette: 20 static colours, the rest black */
static const u8 syspal[20][3] = {
    {0,0,0}, {128,0,0}, {0,128,0}, {128,128,0}, {0,0,128}, {128,0,128}, {0,128,128}, {192,192,192},
    {192,220,192}, {166,202,240},
    {255,251,240}, {160,160,164}, {128,128,128}, {255,0,0}, {0,255,0}, {255,255,0}, {0,0,255}, {255,0,255},
    {0,255,255}, {255,255,255} };
static void syspal_entries(u32 start, u32 n, u32 out)
{
    for (u32 k = 0; k < n; k++) {
        u32 i = start + k, c = 0;
        if (i < 10) c = syspal[i][0] | syspal[i][1] << 8 | syspal[i][2] << 16;
        else if (i >= 246 && i < 256) c = syspal[i - 236][0] | syspal[i - 236][1] << 8 | syspal[i - 236][2] << 16;
        WR32(out + 4 * k, c);
    }
}
/* GDI palettes: the entries the game gave CreatePalette, kept here */
#define NGPAL 32
static u32 gpal[NGPAL][256];
static int gpal_used[NGPAL];
void api_CreatePalette(void)
{
    u32 lp = ARG(0);
    for (int i = 1; i < NGPAL; i++)
        if (!gpal_used[i]) {
            gpal_used[i] = 1;
            u32 n = RD16(lp + 2);
            memset(gpal[i], 0, sizeof gpal[i]);
            for (u32 k = 0; k < n && k < 256; k++) gpal[i][k] = RD32(lp + 4 + 4 * k);
            RET(0x000e0000u | i, 1);
        }
    RET(0, 1);
}
static u32 *gpal_of(u32 h) { u32 i = h & 0xffff; return (h >> 16) == 0x0e && i < NGPAL && gpal_used[i] ? gpal[i] : NULL; }
void api_GetPaletteEntries(void)
{
    u32 *p = gpal_of(ARG(0));
    if (!p) { syspal_entries(ARG(1), ARG(2), ARG(3)); RET(ARG(2), 4); }
    for (u32 k = 0; k < ARG(2) && ARG(1) + k < 256; k++) WR32(ARG(3) + 4 * k, p[ARG(1) + k]);
    RET(ARG(2), 4);
}
void api_SetPaletteEntries(void)
{
    u32 *p = gpal_of(ARG(0));
    if (p) for (u32 k = 0; k < ARG(2) && ARG(1) + k < 256; k++) p[ARG(1) + k] = RD32(ARG(3) + 4 * k);
    RET(ARG(2), 4);
}
void api_AnimatePalette(void)
{
    u32 *p = gpal_of(ARG(0));
    if (p) for (u32 k = 0; k < ARG(2) && ARG(1) + k < 256; k++) p[ARG(1) + k] = RD32(ARG(3) + 4 * k);
    RET(1, 4);
}
void api_GetSystemPaletteEntries(void) { syspal_entries(ARG(1), ARG(2), ARG(3)); RET(ARG(2), 4); }
void api_GetNearestPaletteIndex(void) { RET(0, 2); }

/* ---------------- stand-ins: name, argument dwords, result ---------------- */
typedef struct { const char *name; int nargs; u32 ret; } Stub;
static const Stub stub_tab[] = {
    /* USER32 menus, dialogs, carets, scrolling, help */
    {"GetMenu", 1, 0}, {"SetMenu", 2, 1}, {"GetSubMenu", 2, 0}, {"EnableMenuItem", 3, 0}, {"CheckMenuItem", 3, 0},
    {"GetMenuState", 3, 0xffffffffu}, {"GetMenuItemCount", 1, 0}, {"GetMenuItemID", 2, 0xffffffffu}, {"DestroyMenu", 1, 1},
    {"InsertMenuA", 5, 1}, {"DeleteMenu", 3, 1}, {"ModifyMenuA", 5, 1}, {"GetMenuStringA", 5, 0}, {"TrackPopupMenu", 7, 0},
    {"SetMenuItemBitmaps", 5, 1}, {"GetMenuCheckMarkDimensions", 0, 0x000d000du},
    {"IsDialogMessageA", 2, 0}, {"GetNextDlgTabItem", 3, 0}, {"EndDialog", 2, 1}, {"CreateDialogIndirectParamA", 5, 0},
    {"CheckDlgButton", 3, 1}, {"CheckRadioButton", 4, 1}, {"IsDlgButtonChecked", 2, 0}, {"GetDlgItemInt", 4, 0},
    {"GetDlgItemTextA", 4, 0}, {"SetDlgItemInt", 4, 1}, {"SetDlgItemTextA", 3, 1},
    {"CreateCaret", 4, 1}, {"DestroyCaret", 0, 1}, {"ShowCaret", 1, 1}, {"SetCaretPos", 2, 1},
    {"ScrollWindow", 5, 1}, {"ScrollWindowEx", 8, 0}, {"ShowScrollBar", 3, 1}, {"GetScrollRange", 4, 1}, {"SetScrollRange", 5, 1},
    {"GetScrollPos", 2, 0}, {"SetScrollPos", 4, 0}, {"GetScrollInfo", 3, 0}, {"SetScrollInfo", 4, 0},
    {"WinHelpA", 4, 1}, {"ShowOwnedPopups", 2, 1}, {"DrawTextA", 5, 16}, {"TabbedTextOutA", 8, 0}, {"GrayStringA", 9, 1},
    {"FillRect", 3, 1}, {"DragAcceptFiles", 2, 0}, {"SHGetFileInfoA", 5, 0}, {"GetFileTitleA", 3, 0},
    {"ClosePrinter", 1, 1}, {"DocumentPropertiesA", 6, 0xffffffffu}, {"OpenPrinterA", 3, 0},
    {"ImageList_Merge", 6, 0}, {"ImageList_Create", 5, 0}, {"ImageList_LoadImageA", 7, 0}, {"ImageList_Write", 2, 0},
    {"ImageList_Destroy", 1, 1}, {"ImageList_Read", 1, 0}, {"#13", 1, 0}, {"#14", 1, 0}, {"#17", 0, 0},
    /* GDI on window / memory DCs (the game draws with DirectDraw) */
    {"CreateCompatibleDC", 1, 0x000d0001u}, {"DeleteDC", 1, 1}, {"SelectObject", 2, 0x000a0000u}, {"DeleteObject", 1, 1},
    {"SetDIBColorTable", 4, 0}, {"SetWindowOrgEx", 4, 1}, {"SetDIBitsToDevice", 12, 0}, {"SetBkMode", 2, 1},
    {"RealizePalette", 1, 0}, {"SelectPalette", 3, 0}, {"GdiFlush", 0, 1}, {"SetTextColor", 2, 0}, {"UnrealizeObject", 1, 1},
    {"SetSystemPaletteUse", 2, 1},
    {"SetPixel", 4, 0}, {"GetPixel", 3, 0}, {"BitBlt", 9, 1}, {"CreateBitmap", 5, 0x000f0001u}, {"StartDocA", 2, 0},
    {"SetBkColor", 2, 0}, {"RestoreDC", 2, 1}, {"SaveDC", 1, 1}, {"SetROP2", 2, 13}, {"SetStretchBltMode", 2, 1},
    {"SetPolyFillMode", 2, 1}, {"SetViewportOrgEx", 4, 1}, {"OffsetViewportOrgEx", 4, 1}, {"SetMapMode", 2, 1},
    {"ScaleViewportExtEx", 6, 1}, {"OffsetWindowOrgEx", 4, 1}, {"SetViewportExtEx", 4, 1}, {"ScaleWindowExtEx", 6, 1},
    {"SetWindowExtEx", 4, 1}, {"ExcludeClipRect", 5, 2}, {"IntersectClipRect", 5, 2}, {"SelectClipRgn", 2, 2},
    {"MoveToEx", 4, 1}, {"LineTo", 3, 1}, {"OffsetClipRgn", 3, 2}, {"GetDIBits", 7, 0}, {"SetTextJustification", 3, 1},
    {"SetTextAlign", 2, 0}, {"SetMapperFlags", 2, 0}, {"SetTextCharacterExtra", 2, 0}, {"ArcTo", 9, 1},
    {"SetArcDirection", 2, 1}, {"PolyDraw", 4, 1}, {"PolylineTo", 3, 1}, {"SetColorAdjustment", 2, 1}, {"PolyBezierTo", 3, 1},
    {"GetClipRgn", 2, 0}, {"CreateRectRgn", 4, 0x00100001u}, {"SelectClipPath", 2, 1}, {"ExtSelectClipRgn", 3, 2},
    {"PlayMetaFileRecord", 4, 1}, {"EnumMetaFile", 4, 1}, {"PlayMetaFile", 2, 1}, {"CreatePen", 3, 0x00110001u},
    {"ExtCreatePen", 5, 0x00110001u}, {"CreateHatchBrush", 2, 0x000b0001u}, {"CreatePatternBrush", 1, 0x000b0001u},
    {"CreateDIBPatternBrushPt", 2, 0x000b0001u}, {"PtVisible", 3, 1}, {"RectVisible", 2, 1}, {"TextOutA", 5, 1},
    {"ExtTextOutA", 8, 1}, {"Escape", 5, 0}, {"PatBlt", 6, 1}, {"CreateRectRgnIndirect", 1, 0x00100001u},
    {"GetMapMode", 1, 1}, {"CombineRgn", 4, 2}, {"SetRectRgn", 5, 1}, {"DPtoLP", 3, 1},
    {NULL, 0, 0}
};

#define NSTUB 256
static int stub_seen[NSTUB];
static int stub_index;
static void stub_call(int i)
{
    const Stub *s = &stub_tab[i];
    if (!stub_seen[i]) { stub_seen[i] = 1; rt_log("stand-in %s called from %08x\n", s->name, RD32(cpu.r[R_ESP])); }
    RET(s->ret, s->nargs);
}
/* one entry point per stub, generated */
#define S(n) static void stub_##n(void) { stub_call(n); }
S(0) S(1) S(2) S(3) S(4) S(5) S(6) S(7) S(8) S(9) S(10) S(11) S(12) S(13) S(14) S(15) S(16) S(17) S(18) S(19)
S(20) S(21) S(22) S(23) S(24) S(25) S(26) S(27) S(28) S(29) S(30) S(31) S(32) S(33) S(34) S(35) S(36) S(37) S(38) S(39)
S(40) S(41) S(42) S(43) S(44) S(45) S(46) S(47) S(48) S(49) S(50) S(51) S(52) S(53) S(54) S(55) S(56) S(57) S(58) S(59)
S(60) S(61) S(62) S(63) S(64) S(65) S(66) S(67) S(68) S(69) S(70) S(71) S(72) S(73) S(74) S(75) S(76) S(77) S(78) S(79)
S(80) S(81) S(82) S(83) S(84) S(85) S(86) S(87) S(88) S(89) S(90) S(91) S(92) S(93) S(94) S(95) S(96) S(97) S(98) S(99)
S(100) S(101) S(102) S(103) S(104) S(105) S(106) S(107) S(108) S(109) S(110) S(111) S(112) S(113) S(114) S(115) S(116) S(117) S(118) S(119)
S(120) S(121) S(122) S(123) S(124) S(125) S(126) S(127) S(128) S(129) S(130) S(131) S(132) S(133) S(134) S(135) S(136) S(137) S(138) S(139)
S(140) S(141) S(142) S(143) S(144) S(145) S(146) S(147) S(148) S(149)
static NativeFn stubs[] = {
    stub_0, stub_1, stub_2, stub_3, stub_4, stub_5, stub_6, stub_7, stub_8, stub_9, stub_10, stub_11, stub_12, stub_13, stub_14,
    stub_15, stub_16, stub_17, stub_18, stub_19, stub_20, stub_21, stub_22, stub_23, stub_24, stub_25, stub_26, stub_27, stub_28,
    stub_29, stub_30, stub_31, stub_32, stub_33, stub_34, stub_35, stub_36, stub_37, stub_38, stub_39, stub_40, stub_41, stub_42,
    stub_43, stub_44, stub_45, stub_46, stub_47, stub_48, stub_49, stub_50, stub_51, stub_52, stub_53, stub_54, stub_55, stub_56,
    stub_57, stub_58, stub_59, stub_60, stub_61, stub_62, stub_63, stub_64, stub_65, stub_66, stub_67, stub_68, stub_69, stub_70,
    stub_71, stub_72, stub_73, stub_74, stub_75, stub_76, stub_77, stub_78, stub_79, stub_80, stub_81, stub_82, stub_83, stub_84,
    stub_85, stub_86, stub_87, stub_88, stub_89, stub_90, stub_91, stub_92, stub_93, stub_94, stub_95, stub_96, stub_97, stub_98,
    stub_99, stub_100, stub_101, stub_102, stub_103, stub_104, stub_105, stub_106, stub_107, stub_108, stub_109, stub_110,
    stub_111, stub_112, stub_113, stub_114, stub_115, stub_116, stub_117, stub_118, stub_119, stub_120, stub_121, stub_122,
    stub_123, stub_124, stub_125, stub_126, stub_127, stub_128, stub_129, stub_130, stub_131, stub_132, stub_133, stub_134,
    stub_135, stub_136, stub_137, stub_138, stub_139, stub_140, stub_141, stub_142, stub_143, stub_144, stub_145, stub_146,
    stub_147, stub_148, stub_149,
};

NativeFn stub_lookup(const char *name)
{
    (void)stub_index;
    for (int i = 0; stub_tab[i].name; i++)
        if (!strcmp(stub_tab[i].name, name)) {
            if (i >= (int)(sizeof stubs / sizeof stubs[0])) rt_fatal("stub table too long");
            return stubs[i];
        }
    return NULL;
}

/* ---------------- WINMM: timer caps, no joystick (DirectInput / XInput is ours) ---------------- */
void api_timeGetDevCaps(void) { if (ARG(1) >= 8) { WR32(ARG(0), 1); WR32(ARG(0) + 4, 1000000); } RET(0, 2); }
void api_joyGetDevCapsA(void) { RET(167 /* JOYERR_UNPLUGGED */, 3); }
void api_joyGetPosEx(void) { RET(167, 2); }
