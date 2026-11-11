/* Resources of sgens.exe (its .rsrc in guest memory): FindResource /
 * LoadResource / LoadString / ..., and the game's mouse cursors turned into
 * host cursors (scaled with the window). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "rt.h"

static u32 rsrc_base(void)
{
    u32 pe = RD32(EXE_BASE + 0x3c);
    u32 rva = RD32(EXE_BASE + pe + 24 + 96 + 2 * 8);
    return rva ? EXE_BASE + rva : 0;
}

/* entry of directory `dir` matching id (numeric) or name (guest string); 0 = first */
static u32 dir_find(u32 dir, u32 id, int first)
{
    u32 root = rsrc_base();
    u32 nn = RD16(dir + 12), ni = RD16(dir + 14);
    for (u32 k = 0; k < nn + ni; k++) {
        u32 e = dir + 16 + 8 * k;
        u32 nm = RD32(e), off = RD32(e + 4);
        int match = first;
        if (!match) {
            if (nm & 0x80000000u) {
                if (id >= 0x10000) {
                    u32 s = root + (nm & 0x7fffffff);
                    u32 len = RD16(s);
                    const char *g = gstr(id);
                    if (strlen(g) == len) {
                        match = 1;
                        for (u32 c = 0; c < len; c++) if (toupper((u8)g[c]) != toupper(RD16(s + 2 + 2 * c))) { match = 0; break; }
                    }
                }
            } else match = id < 0x10000 && nm == id;
        }
        if (match) return off;
    }
    return 0xffffffffu;
}

/* data entry address of resource (type, name), first language */
u32 res_find(u32 type, u32 name)
{
    u32 root = rsrc_base();
    if (!root) return 0;
    u32 o = dir_find(root, type, 0);
    if (o == 0xffffffffu || !(o & 0x80000000u)) return 0;
    u32 d2 = root + (o & 0x7fffffff);
    o = dir_find(d2, name, 0);
    if (o == 0xffffffffu || !(o & 0x80000000u)) return 0;
    u32 d3 = root + (o & 0x7fffffff);
    o = dir_find(d3, 0, 1);
    if (o == 0xffffffffu || (o & 0x80000000u)) return 0;
    return root + o;
}

static u32 res_data(u32 ent) { return ent ? EXE_BASE + RD32(ent) : 0; }
static u32 res_size(u32 ent) { return ent ? RD32(ent + 4) : 0; }

/* handles: HRSRC = data entry address, HGLOBAL = data address */
void api_FindResourceA(void)
{
    u32 e = res_find(ARG(2), ARG(1));
    if (!e) rt_log("FindResourceA type %x name %s: none\n", ARG(2), ARG(1) >= 0x10000 ? gstr(ARG(1)) : "#");
    RET(e, 3);
}
void api_LoadResource(void) { RET(res_data(ARG(1)), 2); }
void api_LockResource(void) { RET(ARG(0), 1); }
void api_SizeofResource(void) { RET(res_size(ARG(1)), 2); }
void api_FreeResource(void) { RET(0, 1); }

/* LoadStringA(inst, id, buf, max) */
void api_LoadStringA(void)
{
    u32 id = ARG(1), buf = ARG(2), max = ARG(3);
    u32 e = res_find(6, (id >> 4) + 1);
    if (!max) RET(0, 4);
    WR8(buf, 0);
    if (!e) RET(0, 4);
    u32 p = res_data(e);
    for (u32 k = 0; k < (id & 15); k++) p += 2 + 2 * RD16(p);
    u32 len = RD16(p), n = 0;
    for (; n < len && n + 1 < max; n++) WR8(buf + n, (u8)RD16(p + 2 + 2 * n));
    WR8(buf + n, 0);
    RET(n, 4);
}

/* LoadMenuA: menus are the windowed debug build's; a dummy handle */
void api_LoadMenuA(void) { RET(0x00080000u | (ARG(1) & 0xffff), 2); }

/* ---------------- cursors ---------------- */
int win_scale(void);
static struct { u32 id; int scale; HCURSOR h; } curs[32];
static int ncurs;

HCURSOR res_cursor(u32 id)
{
    int sc = win_scale();
    if (sc < 1) sc = 1;
    for (int i = 0; i < ncurs; i++) if (curs[i].id == id && curs[i].scale == sc) return curs[i].h;
    u32 g = res_find(12, id);     /* RT_GROUP_CURSOR */
    if (!g) { rt_log("cursor %u: no group\n", id); return LoadCursorA(NULL, IDC_ARROW); }
    u32 gd = res_data(g);
    if (!RD16(gd + 4)) return LoadCursorA(NULL, IDC_ARROW);
    u32 ent = gd + 6;           /* first entry: w, h(x2), planes, bits, bytes, id */
    u32 cid = RD16(ent + 12);
    u32 c = res_find(1, cid);   /* RT_CURSOR */
    if (!c) return LoadCursorA(NULL, IDC_ARROW);
    int w = RD16(ent), h = RD16(ent + 2) / 2;
    if (!w) w = 32;
    if (!h) h = 32;
    HCURSOR hc = (HCURSOR)CreateIconFromResourceEx(MEM + res_data(c), res_size(c), FALSE, 0x00030000, w * sc, h * sc, 0);
    if (!hc) hc = LoadCursorA(NULL, IDC_ARROW);
    if (ncurs < 32) { curs[ncurs].id = id; curs[ncurs].scale = sc; curs[ncurs].h = hc; ncurs++; }
    rt_log("cursor %u -> %dx%d x%d\n", id, w, h, sc);
    return hc;
}
