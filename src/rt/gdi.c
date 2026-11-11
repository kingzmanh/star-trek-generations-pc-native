/* GDI on DirectDraw surfaces and the screen, done with the host's GDI.
 *
 * Some of the game's text widgets draw with Windows fonts: Surface::GetDC,
 * CreateFontIndirect, SelectObject, SetTextColor, DrawText / TextOut,
 * FillRect, Surface::ReleaseDC.  A surface DC here is a host memory DC with
 * a 32-bit DIB section; GetDC copies the surface's pixels in, ReleaseDC
 * copies them back (16-bit RGB565 or 8-bit through the surface palette).
 * Fonts, brushes and pens are host objects behind guest handles.  Window
 * DCs (GetDC(hwnd)) get a 640x480 scratch DC: measuring text works there,
 * drawing goes nowhere (the game shows everything through DirectDraw). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

/* ---------------- objects: guest handle 0x00Cxxxxx -> host object ---------------- */
#define NOBJ 512
typedef struct { int used, kind; HGDIOBJ h; u32 color; } GObj;   /* kind 1 font, 2 brush, 3 pen, 4 stock */
static GObj objs[NOBJ];
#define OBJ_BASE 0x00c00000u

static u32 obj_new(int kind, HGDIOBJ h, u32 color)
{
    for (int i = 1; i < NOBJ; i++)
        if (!objs[i].used) { objs[i] = (GObj){ 1, kind, h, color }; return OBJ_BASE | i; }
    return 0;
}
static GObj *obj_get(u32 g) { u32 i = g - OBJ_BASE; return i < NOBJ && objs[i].used ? &objs[i] : NULL; }

/* ---------------- DCs: guest handle 0x00Dxxxxx ---------------- */
#define NDC 32
typedef struct {
    int used;
    HDC dc;
    HBITMAP bmp, oldbmp;
    u32 *bits;
    int w, h;
    u32 surf_mem, surf_pitch;   /* surface DC: where the pixels go back */
    int surf_bpp;
    const u32 *pal;             /* 8-bit surface: its palette (0x00BBGGRR) */
    u32 sel_font, sel_brush, sel_pen;
} GDC;
static GDC dcs[NDC];
#define DC_BASE 0x00d00000u

static GDC *dc_get(u32 g)
{
    u32 i = g - DC_BASE;
    if (i < NDC && dcs[i].used) return &dcs[i];
    return NULL;
}

static GDC *dc_alloc(int w, int h)
{
    for (int i = 1; i < NDC; i++)
        if (!dcs[i].used) {
            GDC *d = &dcs[i];
            memset(d, 0, sizeof *d);
            d->used = 1;
            d->w = w; d->h = h;
            BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
            d->dc = CreateCompatibleDC(NULL);
            d->bmp = CreateDIBSection(d->dc, &bi, DIB_RGB_COLORS, (void **)&d->bits, NULL, 0);
            d->oldbmp = SelectObject(d->dc, d->bmp);
            SetBkMode(d->dc, OPAQUE);
            return d;
        }
    return NULL;
}

static void dc_free(GDC *d)
{
    SelectObject(d->dc, d->oldbmp);
    DeleteObject(d->bmp);
    DeleteDC(d->dc);
    d->used = 0;
}

/* screen / window DC: one shared scratch DC */
static u32 screen_dc(void)
{
    static u32 g;
    if (!g) { GDC *d = dc_alloc(640, 480); g = DC_BASE | (u32)(d - dcs); }
    return g;
}

/* ---------------- surface DCs (ddraw.c) ---------------- */
u32 gdi_surface_dc(u32 mem, int w, int h, int pitch, int bpp, const u32 *pal)
{
    GDC *d = dc_alloc(w, h);
    if (!d) return 0;
    d->surf_mem = mem; d->surf_pitch = pitch; d->surf_bpp = bpp; d->pal = pal;
    for (int y = 0; y < h; y++) {
        u32 *o = d->bits + y * w;
        const u8 *s = MEM + mem + y * pitch;
        if (bpp == 16) for (int x = 0; x < w; x++) {
            u32 c = ((const u16 *)s)[x], r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            o[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
        } else for (int x = 0; x < w; x++) {
            u32 c = pal ? pal[s[x]] : s[x] * 0x010101u;
            o[x] = (c & 0xff) << 16 | (c & 0xff00) | ((c >> 16) & 0xff);
        }
    }
    return DC_BASE | (u32)(d - dcs);
}

static u8 nearest(const u32 *pal, u32 rgb)
{
    int best = 0, bd = 1 << 30;
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    for (int i = 0; i < 256; i++) {
        int pr = pal[i] & 255, pg = (pal[i] >> 8) & 255, pb = (pal[i] >> 16) & 255;
        int dd = (pr - r) * (pr - r) + (pg - g) * (pg - g) + (pb - b) * (pb - b);
        if (dd < bd) { bd = dd; best = i; if (!dd) break; }
    }
    return (u8)best;
}

void gdi_surface_release(u32 g)
{
    GDC *d = dc_get(g);
    if (!d || !d->surf_mem) return;
    GdiFlush();
    for (int y = 0; y < d->h; y++) {
        const u32 *s = d->bits + y * d->w;
        u8 *o = MEM + d->surf_mem + y * d->surf_pitch;
        if (d->surf_bpp == 16) for (int x = 0; x < d->w; x++) {
            u32 p = s[x];
            ((u16 *)o)[x] = (u16)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x1f));
        } else if (d->pal) {
            /* only pixels GDI changed: compare with the original colour */
            for (int x = 0; x < d->w; x++) {
                u32 c = d->pal[o[x]], cur = (c & 0xff) << 16 | (c & 0xff00) | ((c >> 16) & 0xff);
                if ((s[x] & 0xffffff) != cur) o[x] = nearest(d->pal, s[x]);
            }
        }
    }
    dc_free(d);
}

static HDC host_dc(u32 g)
{
    GDC *d = dc_get(g);
    if (d) return d->dc;
    return dcs[screen_dc() - DC_BASE].dc;
}

/* colours: PALETTEINDEX / PALETTERGB forms reduced to RGB */
static COLORREF col(u32 c) { return c & 0x00ffffff; }

/* ---------------- object creation ---------------- */
void api_CreateFontIndirectA(void)
{
    LOGFONTA lf;
    memcpy(&lf, MEM + ARG(0), sizeof lf);
    lf.lfQuality = ANTIALIASED_QUALITY;
    HFONT f = CreateFontIndirectA(&lf);
    rt_log("gdi: font %s %d\n", lf.lfFaceName, (int)lf.lfHeight);
    RET(obj_new(1, f, 0), 1);
}
void api_CreateSolidBrush(void) { RET(obj_new(2, CreateSolidBrush(col(ARG(0))), ARG(0)), 1); }
void api_CreatePen(void) { RET(obj_new(3, CreatePen(ARG(0), ARG(1), col(ARG(2))), ARG(2)), 3); }
void api_GetStockObject(void)
{
    static u32 stock[32];
    u32 i = ARG(0);
    if (i >= 32) RET(0, 1);
    if (!stock[i]) stock[i] = obj_new(4, GetStockObject(i), 0);
    RET(stock[i], 1);
}
void api_DeleteObject(void)
{
    GObj *o = obj_get(ARG(0));
    if (o && o->kind != 4) { DeleteObject(o->h); o->used = 0; }
    RET(1, 1);
}
void api_GetObjectType(void) { GObj *o = obj_get(ARG(0)); RET(o ? (o->kind == 1 ? OBJ_FONT : o->kind == 2 ? OBJ_BRUSH : OBJ_PEN) : 0, 1); }
void api_GetObjectA(void)
{
    GObj *o = obj_get(ARG(0));
    if (!o || !ARG(2)) RET(0, 3);
    u8 tmp[256];
    int n = GetObjectA(o->h, sizeof tmp, tmp);
    if (n > (int)ARG(1)) n = ARG(1);
    memcpy(MEM + ARG(2), tmp, n);
    RET(n, 3);
}

/* SelectObject(dc, obj): returns the previous guest handle of that kind */
void api_SelectObject(void)
{
    GObj *o = obj_get(ARG(1));
    GDC *d = dc_get(ARG(0));
    HDC h = host_dc(ARG(0));
    if (!o) RET(0, 2);
    u32 prev = 0;
    SelectObject(h, o->h);
    if (d) {
        u32 *slot = o->kind == 1 ? &d->sel_font : o->kind == 3 ? &d->sel_pen : &d->sel_brush;
        if (o->kind == 4) {   /* stock: font / brush / pen by its type */
            DWORD t = GetObjectType(o->h);
            slot = t == OBJ_FONT ? &d->sel_font : t == OBJ_PEN ? &d->sel_pen : &d->sel_brush;
        }
        prev = *slot;
        *slot = ARG(1);
    }
    if (!prev) { static u32 sysfont; if (!sysfont) sysfont = obj_new(4, GetStockObject(SYSTEM_FONT), 0); prev = sysfont; }
    RET(prev, 2);
}

/* ---------------- DCs ---------------- */
void api_CreateCompatibleDC(void) { GDC *d = dc_alloc(1, 1); RET(d ? DC_BASE | (u32)(d - dcs) : 0, 1); }
void api_DeleteDC(void) { GDC *d = dc_get(ARG(0)); if (d && !d->surf_mem && ARG(0) != screen_dc()) dc_free(d); RET(1, 1); }
void api_GetDC(void) { RET(screen_dc(), 1); }
void api_GetWindowDC(void) { RET(screen_dc(), 1); }
void api_ReleaseDC(void) { RET(1, 2); }
void api_SaveDC(void) { RET(SaveDC(host_dc(ARG(0))), 1); }
void api_RestoreDC(void) { RET(RestoreDC(host_dc(ARG(0)), (int)ARG(1)), 2); }

/* ---------------- state ---------------- */
void api_SetBkMode(void) { RET(SetBkMode(host_dc(ARG(0)), ARG(1)), 2); }
void api_SetTextColor(void) { RET(SetTextColor(host_dc(ARG(0)), col(ARG(1))), 2); }
void api_SetBkColor(void) { RET(SetBkColor(host_dc(ARG(0)), col(ARG(1))), 2); }
void api_SetTextAlign(void) { RET(SetTextAlign(host_dc(ARG(0)), ARG(1)), 2); }
void api_SetTextCharacterExtra(void) { RET(SetTextCharacterExtra(host_dc(ARG(0)), ARG(1)), 2); }
void api_SetROP2(void) { RET(SetROP2(host_dc(ARG(0)), ARG(1)), 2); }

/* ---------------- text ---------------- */
void api_TextOutA(void) { RET(TextOutA(host_dc(ARG(0)), (s32)ARG(1), (s32)ARG(2), (LPCSTR)(MEM + ARG(3)), ARG(4)), 5); }
/* ExtTextOutA(dc, x, y, opts, rect, str, n, dx) */
void api_ExtTextOutA(void)
{
    RECT r, *pr = NULL;
    if (ARG(4)) { memcpy(&r, MEM + ARG(4), 16); pr = &r; }
    INT *dx = NULL;
    if (ARG(7)) { dx = malloc(4 * (ARG(6) + 1)); memcpy(dx, MEM + ARG(7), 4 * ARG(6)); }
    BOOL ok = ExtTextOutA(host_dc(ARG(0)), (s32)ARG(1), (s32)ARG(2), ARG(3), pr, (LPCSTR)(MEM + ARG(5)), ARG(6), dx);
    free(dx);
    RET(ok, 8);
}
/* DrawTextA(dc, str, n, rect, fmt) */
void api_DrawTextA(void)
{
    RECT r;
    memcpy(&r, MEM + ARG(3), 16);
    int n = (int)ARG(2);
    int h = DrawTextA(host_dc(ARG(0)), (LPCSTR)(MEM + ARG(1)), n, &r, ARG(4));
    memcpy(MEM + ARG(3), &r, 16);
    RET(h, 5);
}
void api_GetTextExtentPointA(void)
{
    SIZE s = { 0, 0 };
    BOOL ok = GetTextExtentPoint32A(host_dc(ARG(0)), (LPCSTR)(MEM + ARG(1)), ARG(2), &s);
    WR32(ARG(3), s.cx); WR32(ARG(3) + 4, s.cy);
    RET(ok, 4);
}
void api_GetTextExtentPoint32A(void)
{
    SIZE s = { 0, 0 };
    BOOL ok = GetTextExtentPoint32A(host_dc(ARG(0)), (LPCSTR)(MEM + ARG(1)), ARG(2), &s);
    WR32(ARG(3), s.cx); WR32(ARG(3) + 4, s.cy);
    RET(ok, 4);
}
void api_GetTextMetricsA(void)
{
    TEXTMETRICA tm;
    BOOL ok = GetTextMetricsA(host_dc(ARG(0)), &tm);
    memcpy(MEM + ARG(1), &tm, sizeof tm);
    RET(ok, 2);
}

/* ---------------- drawing ---------------- */
static HBRUSH brush_of(u32 g)
{
    GObj *o = obj_get(g);
    if (o) return (HBRUSH)o->h;
    if ((g >> 16) == 0x0009) return GetSysColorBrush(g & 0xffff);   /* GetSysColorBrush */
    if (g && g <= 31) return GetSysColorBrush(g - 1);               /* COLOR_x + 1 */
    return NULL;
}
void api_FillRect(void)
{
    RECT r;
    memcpy(&r, MEM + ARG(1), 16);
    HBRUSH b = brush_of(ARG(2));
    RET(b ? FillRect(host_dc(ARG(0)), &r, b) : 0, 3);
}
void api_PatBlt(void) { RET(PatBlt(host_dc(ARG(0)), (s32)ARG(1), (s32)ARG(2), (s32)ARG(3), (s32)ARG(4), ARG(5)), 6); }
void api_MoveToEx(void)
{
    POINT p;
    BOOL ok = MoveToEx(host_dc(ARG(0)), (s32)ARG(1), (s32)ARG(2), &p);
    if (ARG(3)) { WR32(ARG(3), p.x); WR32(ARG(3) + 4, p.y); }
    RET(ok, 4);
}
void api_LineTo(void) { RET(LineTo(host_dc(ARG(0)), (s32)ARG(1), (s32)ARG(2)), 3); }
void api_Rectangle(void) { RET(Rectangle(host_dc(ARG(0)), (s32)ARG(1), (s32)ARG(2), (s32)ARG(3), (s32)ARG(4)), 5); }
void api_SetPixel(void) { RET(SetPixel(host_dc(ARG(0)), (s32)ARG(1), (s32)ARG(2), col(ARG(3))), 4); }
void api_GetPixel(void) { RET(GetPixel(host_dc(ARG(0)), (s32)ARG(1), (s32)ARG(2)), 3); }
