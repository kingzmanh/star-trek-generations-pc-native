/* The install itself: copies the user's two game CDs (they are then always
 * "in the drive"), lays out what the original installer put on the hard
 * disk, and puts the PC executable in place with the game's own icon. */
#include "setup.h"
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include "install.h"

static int fail(const char *msg) { setup_log("ERROR: %s", msg); return 0; }

/* ---------- game icon: a Windows .ico on the CD -> the exe's icon resource 1 ---------- */
int inject_icon_ico(const char *exe, const u8 *d, size_t n)
{
    if (n < 6 || d[0] || d[1] || d[2] != 1) return fail("icon file is not an .ico");
    int cnt = d[4] | d[5] << 8;
    if (6 + 16 * (size_t)cnt > n) return fail("icon file is truncated");
    HANDLE u = BeginUpdateResourceA(exe, FALSE);
    if (!u) return fail("cannot update the game executable's icon");
    u8 *grp = calloc(1, 6 + 14 * cnt);
    grp[2] = 1;
    grp[4] = (u8)cnt;
    for (int i = 0; i < cnt; i++) {
        const u8 *e = d + 6 + 16 * i;
        u32 size = e[8] | e[9] << 8 | e[10] << 16 | (u32)e[11] << 24;
        u32 off = e[12] | e[13] << 8 | e[14] << 16 | (u32)e[15] << 24;
        if (off + size > n) { free(grp); EndUpdateResourceA(u, TRUE); return fail("icon image out of range"); }
        UpdateResourceA(u, (LPCSTR)RT_ICON, MAKEINTRESOURCEA(i + 1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), (void *)(d + off), size);
        u8 *g = grp + 6 + 14 * i;
        memcpy(g, e, 12);            /* w, h, colours, reserved, planes, bit count, bytes */
        g[12] = (u8)(i + 1);
        g[13] = 0;
    }
    UpdateResourceA(u, (LPCSTR)RT_GROUP_ICON, MAKEINTRESOURCEA(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), grp, 6 + 14 * cnt);
    free(grp);
    if (!EndUpdateResourceA(u, FALSE)) return fail("cannot write the game executable's icon");
    setup_log("icon: %d images from the CD", cnt);
    return 1;
}

/* ---------- game icon: the icon group of another exe (the game's own) ---------- */
int inject_icon_from_exe(const char *exe, const char *src)
{
    HMODULE m = LoadLibraryExA(src, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return fail("cannot open the game program for its icon");
    /* first icon group */
    struct { LPSTR name; int found; } first = {0};
    BOOL CALLBACK pick(HMODULE mm, LPCSTR type, LPSTR name, LONG_PTR p);
    EnumResourceNamesA(m, (LPCSTR)RT_GROUP_ICON, pick, (LONG_PTR)&first);
    int ok = 0;
    HRSRC r = first.found ? FindResourceA(m, first.name, (LPCSTR)RT_GROUP_ICON) : NULL;
    if (r) {
        const u8 *g = LockResource(LoadResource(m, r));
        DWORD gn = SizeofResource(m, r);
        int cnt = gn >= 6 ? g[4] | g[5] << 8 : 0;
        if (cnt && 6 + 14u * cnt <= gn) {
            /* rebuild an .ico file: header, 16-byte entries, images */
            size_t total = 6 + 16 * (size_t)cnt;
            for (int i = 0; i < cnt; i++) {
                const u8 *e = g + 6 + 14 * i;
                HRSRC ri = FindResourceA(m, MAKEINTRESOURCEA(e[12] | e[13] << 8), (LPCSTR)RT_ICON);
                if (ri) total += SizeofResource(m, ri);
            }
            u8 *ico = calloc(1, total);
            ico[2] = 1;
            ico[4] = (u8)cnt;
            size_t off = 6 + 16 * (size_t)cnt;
            for (int i = 0; i < cnt; i++) {
                const u8 *e = g + 6 + 14 * i;
                HRSRC ri = FindResourceA(m, MAKEINTRESOURCEA(e[12] | e[13] << 8), (LPCSTR)RT_ICON);
                DWORD sz = ri ? SizeofResource(m, ri) : 0;
                u8 *o = ico + 6 + 16 * i;
                memcpy(o, e, 8);
                memcpy(o + 8, &sz, 4);
                u32 of32 = (u32)off;
                memcpy(o + 12, &of32, 4);
                if (sz) memcpy(ico + off, LockResource(LoadResource(m, ri)), sz);
                off += sz;
            }
            ok = inject_icon_ico(exe, ico, total);
            free(ico);
        }
    }
    if (first.found && !IS_INTRESOURCE(first.name)) free(first.name);
    FreeLibrary(m);
    return ok;
}

BOOL CALLBACK pick(HMODULE mm, LPCSTR type, LPSTR name, LONG_PTR p)
{
    (void)mm; (void)type;
    struct { LPSTR name; int found; } *f = (void *)p;
    f->name = IS_INTRESOURCE(name) ? name : _strdup(name);
    f->found = 1;
    return FALSE;
}

/* ---------- game icon: from an LBM picture on the CD ---------- */
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* Deluxe Paint PBM (chunky) -> RGBA; black is transparent */
static u32 *decode_lbm(const u8 *d, size_t n, int *w, int *h)
{
    if (n < 12 || memcmp(d, "FORM", 4) || memcmp(d + 8, "PBM ", 4)) return NULL;
    size_t p = 12, end = 8 + be32(d + 4);
    if (end > n) end = n;
    const u8 *pal = NULL, *body = NULL;
    size_t blen = 0;
    int comp = 0;
    *w = *h = 0;
    while (p + 8 <= end) {
        u32 len = be32(d + p + 4);
        const u8 *c = d + p + 8;
        if (!memcmp(d + p, "BMHD", 4)) { *w = c[0] << 8 | c[1]; *h = c[2] << 8 | c[3]; comp = c[10]; }
        else if (!memcmp(d + p, "CMAP", 4)) pal = c;
        else if (!memcmp(d + p, "BODY", 4)) { body = c; blen = len; }
        p += 8 + len + (len & 1);
    }
    if (!*w || !*h || !pal || !body) return NULL;
    int row = *w + (*w & 1);
    u8 *pix = calloc(row, *h);
    if (comp == 1) {
        size_t i = 0, o = 0, tot = (size_t)row * *h;
        while (o < tot && i < blen) {
            u8 b = body[i++];
            if (b < 128) { for (int k = 0; k <= b && o < tot && i < blen; k++) pix[o++] = body[i++]; }
            else if (b > 128) { u8 v = body[i++]; for (int k = 0; k < 257 - b && o < tot; k++) pix[o++] = v; }
        }
    } else memcpy(pix, body, blen < (size_t)row * *h ? blen : (size_t)row * *h);
    u32 *rgba = calloc((size_t)*w * *h, 4);
    for (int y = 0; y < *h; y++)
        for (int x = 0; x < *w; x++) {
            const u8 *c = pal + pix[y * row + x] * 3;
            int lum = c[0] + c[1] + c[2];
            rgba[y * *w + x] = lum < 60 ? 0 : (0xffu << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
        }
    free(pix);
    return rgba;
}

static u8 *icon_image(const u32 *sq, int side, int size, u32 *len)
{
    u32 mask_row = ((size + 31) / 32) * 4;
    *len = 40 + size * size * 4 + mask_row * size;
    u8 *b = calloc(1, *len);
    u32 *px = (u32 *)(b + 40);
    b[0] = 40;
    *(int *)(b + 4) = size;
    *(int *)(b + 8) = size * 2;
    b[12] = 1;
    b[14] = 32;
    /* box filter down from the large source */
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int x0 = x * side / size, x1 = (x + 1) * side / size, y0 = y * side / size, y1 = (y + 1) * side / size;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            u32 a = 0, r = 0, g = 0, bl = 0, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    u32 v = sq[yy * side + xx], al = v >> 24;
                    a += al; r += ((v >> 16) & 255) * al; g += ((v >> 8) & 255) * al; bl += (v & 255) * al; n++;
                }
            u32 out = 0;
            if (a) out = ((a / n) << 24) | ((r / a) << 16) | ((g / a) << 8) | (bl / a);
            px[(size - 1 - y) * size + x] = out;
        }
    return b;
}

int inject_icon_lbm(const char *exe, const u8 *lbm, size_t n)
{
    int w, h;
    u32 *src = decode_lbm(lbm, n, &w, &h);
    if (!src) return fail("game emblem picture not understood");
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (src[y * w + x] >> 24) { if (x < x0) x0 = x; if (y < y0) y0 = y; if (x > x1) x1 = x; if (y > y1) y1 = y; }
    if (x1 < 0) { free(src); return fail("game emblem picture is empty"); }
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1, side = cw > ch ? cw : ch;
    side += side / 16;
    u32 *sq = calloc((size_t)side * side, 4);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            sq[(y + (side - ch) / 2) * side + x + (side - cw) / 2] = src[(y0 + y) * w + x0 + x];
    free(src);
    static const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    const int ns = 7;
    HANDLE u = BeginUpdateResourceA(exe, FALSE);
    if (!u) { free(sq); return fail("cannot update the game executable's icon"); }
    u8 grp[6 + 7 * 14];
    memset(grp, 0, sizeof grp);
    grp[2] = 1;
    grp[4] = (u8)ns;
    for (int i = 0; i < ns; i++) {
        u32 len;
        u8 *img = icon_image(sq, side, sizes[i], &len);
        UpdateResourceA(u, (LPCSTR)RT_ICON, MAKEINTRESOURCEA(1 + i), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), img, len);
        free(img);
        u8 *e = grp + 6 + i * 14;
        e[0] = sizes[i] == 256 ? 0 : (u8)sizes[i];
        e[1] = e[0];
        e[4] = 1;
        e[6] = 32;
        memcpy(e + 8, &len, 4);
        u16 id = (u16)(1 + i);
        memcpy(e + 12, &id, 2);
    }
    UpdateResourceA(u, (LPCSTR)RT_GROUP_ICON, MAKEINTRESOURCEA(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), grp, sizeof grp);
    free(sq);
    if (!EndUpdateResourceA(u, FALSE)) return fail("cannot write the game executable's icon");
    return 1;
}

/* ---------- shortcuts / uninstall entry ---------- */
static int make_link(const char *lnk, const char *target, const char *dir, const char *desc)
{
    IShellLinkA *sl = NULL;
    IPersistFile *pf = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&sl))) return 0;
    sl->lpVtbl->SetPath(sl, target);
    sl->lpVtbl->SetWorkingDirectory(sl, dir);
    sl->lpVtbl->SetDescription(sl, desc);
    sl->lpVtbl->SetIconLocation(sl, target, 0);
    int ok = 0;
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        WCHAR w[MAX_PATH * 2];
        MultiByteToWideChar(CP_ACP, 0, lnk, -1, w, MAX_PATH * 2);
        ok = SUCCEEDED(pf->lpVtbl->Save(pf, w, TRUE));
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
    return ok;
}

void shortcut_paths(char *startmenu, char *desktop)
{
    char p[MAX_PATH];
    SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, 0, p);
    snprintf(startmenu, MAX_PATH * 2, "%s\\%s.lnk", p, GAME_TITLE);
    SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, p);
    snprintf(desktop, MAX_PATH * 2, "%s\\%s.lnk", p, GAME_TITLE);
}

static void register_uninstall(const char *dir, u64 bytes)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, UNINSTALL_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return;
    char s[MAX_PATH * 3];
    RegSetValueExA(k, "DisplayName", 0, REG_SZ, (const BYTE *)GAME_TITLE " (PC Native)", (DWORD)strlen(GAME_TITLE " (PC Native)") + 1);
    snprintf(s, sizeof s, "\"%s\\uninstall.exe\" /uninstall", dir);
    RegSetValueExA(k, "UninstallString", 0, REG_SZ, (const BYTE *)s, (DWORD)strlen(s) + 1);
    snprintf(s, sizeof s, "%s\\" GAME_SHORT ".exe,0", dir);
    RegSetValueExA(k, "DisplayIcon", 0, REG_SZ, (const BYTE *)s, (DWORD)strlen(s) + 1);
    RegSetValueExA(k, "InstallLocation", 0, REG_SZ, (const BYTE *)dir, (DWORD)strlen(dir) + 1);
    const char *pub = "PC Native port (fan project)";
    RegSetValueExA(k, "Publisher", 0, REG_SZ, (const BYTE *)pub, (DWORD)strlen(pub) + 1);
    DWORD kb = (DWORD)(bytes / 1024), one = 1;
    RegSetValueExA(k, "EstimatedSize", 0, REG_DWORD, (const BYTE *)&kb, 4);
    RegSetValueExA(k, "NoModify", 0, REG_DWORD, (const BYTE *)&one, 4);
    RegSetValueExA(k, "NoRepair", 0, REG_DWORD, (const BYTE *)&one, 4);
    RegCloseKey(k);
}

/* ---------- prebuilt game ---------- */
static int extract_prebuilt(const char *dst)
{
    HRSRC r = FindResourceA(NULL, "GAMEEXE", (LPCSTR)RT_RCDATA);
    if (!r) return fail("this setup does not contain the game executable");
    HGLOBAL g = LoadResource(NULL, r);
    DWORD n = SizeofResource(NULL, r);
    return write_file(dst, LockResource(g), n);
}

int prebuilt_available(void) { return FindResourceA(NULL, "GAMEEXE", (LPCSTR)RT_RCDATA) != NULL; }

/* ---------- the whole install ---------- */
int do_install(const InstallOpts *o, void (*cb)(u64, u64, void *), void *user)
{
    char p[MAX_PATH * 2], game[MAX_PATH * 2], hd[MAX_PATH * 2], cd[MAX_PATH * 2];
    setup_log("Install to %s", o->dir);
    if (!prebuilt_available()) return fail("this setup does not contain the game executable");
    CdSrc *c = cd_open(o->cd);
    if (!c) return fail("disc 1 is not a readable disc, image or folder");
    if (!game_is_cd(c)) { cd_close(c); return fail("this is not disc 1 of " GAME_TITLE); }
    CdSrc *c2 = cd_open(o->cd2);
    if (!c2) { cd_close(c); return fail("disc 2 is not a readable disc, image or folder"); }
    if (!game_is_cd2(c2)) { cd_close(c); cd_close(c2); return fail("this is not disc 2 of " GAME_TITLE); }

    Progress pr = {0};
    pr.cb = cb;
    pr.user = user;
    pr.total = cd_total_bytes(c) + cd_total_bytes(c2) + (8ull << 20);
    u64 bytes = pr.total + (12ull << 20);

    snprintf(game, sizeof game, "%s\\game", o->dir);
    snprintf(hd, sizeof hd, "%s\\hd", game);
    snprintf(cd, sizeof cd, "%s\\cd", game);
    if (!make_dirs(hd)) { cd_close(c); cd_close(c2); return fail("cannot create the install folder"); }

    /* 1. the discs: drive D: of the game, both always in the drive (disc 2
       holds only GENS, merged into disc 1's) */
    setup_log("Copying disc 1: %s ...", cd_describe(c));
    if (!cd_copy_all(c, cd, &pr)) { cd_close(c); cd_close(c2); return fail("copying disc 1 failed"); }
    cd_close(c);
    setup_log("Copying disc 2: %s ...", cd_describe(c2));
    if (!cd_copy_all(c2, cd, &pr)) { cd_close(c2); return fail("copying disc 2 failed"); }
    cd_close(c2);

    /* 2. what the DOS installer put on the hard disk (game_install.c) */
    setup_log("Setting up the game files ...");
    if (!game_install_files(cd, hd)) return 0;
    progress_add(&pr, 6ull << 20);

    /* 3. the game executable and its icon */
    char exe[MAX_PATH * 2];
    snprintf(exe, sizeof exe, "%s\\" GAME_SHORT ".exe", o->dir);
    if (!extract_prebuilt(exe)) return 0;
    if (game_icon_exe()) {
        snprintf(p, sizeof p, "%s\\%s", hd, game_icon_exe());
        inject_icon_from_exe(exe, p);
    }
    if (game_icon_lbm()) {
        snprintf(p, sizeof p, "%s\\%s", cd, game_icon_lbm());
        size_t ln;
        u8 *lbm = read_file(p, &ln);
        if (lbm) { inject_icon_lbm(exe, lbm, ln); free(lbm); }
    }
    if (game_icon_ico()) {
        snprintf(p, sizeof p, "%s\\%s", cd, game_icon_ico());
        size_t ln;
        u8 *ico = read_file(p, &ln);
        if (ico) { inject_icon_ico(exe, ico, ln); free(ico); }
    }

    /* 4. uninstaller, shortcuts */
    char me[MAX_PATH * 2];
    GetModuleFileNameA(NULL, me, sizeof me);
    snprintf(p, sizeof p, "%s\\uninstall.exe", o->dir);
    if (_stricmp(me, p)) CopyFileA(me, p, FALSE);
    register_uninstall(o->dir, bytes);
    char sm[MAX_PATH * 2], dt[MAX_PATH * 2];
    shortcut_paths(sm, dt);
    CoInitialize(NULL);
    if (o->startmenu) make_link(sm, exe, o->dir, GAME_TITLE);
    if (o->desktop) make_link(dt, exe, o->dir, GAME_TITLE);
    CoUninitialize();
    progress_add(&pr, 2ull << 20);
    setup_log("Done.");
    return 1;
}

int do_uninstall(const char *dir)
{
    char sm[MAX_PATH * 2], dt[MAX_PATH * 2], p[MAX_PATH * 2];
    shortcut_paths(sm, dt);
    DeleteFileA(sm);
    DeleteFileA(dt);
    RegDeleteKeyA(HKEY_CURRENT_USER, UNINSTALL_KEY);
    snprintf(p, sizeof p, "%s\\game", dir);
    delete_tree(p);
    static const char *files[] = {GAME_SHORT ".exe", "uninstall.exe", GAME_SHORT ".ini", GAME_SHORT "_registry.txt", GAME_SHORT ".log", GAME_SHORT "_setup.log"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(p, sizeof p, "%s\\%s", dir, files[i]);
        DeleteFileA(p);
    }
    RemoveDirectoryA(dir);
    return 1;
}
