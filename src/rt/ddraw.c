/* DirectDraw replacement: IDirectDraw / IDirectDraw2, IDirectDrawSurface(2),
 * IDirectDrawPalette.
 *
 * The game runs 640x480 exclusive mode, 8-bit (logos) or 16-bit (the game
 * and movies), a primary surface with a back buffer and offscreen surfaces
 * it Blts / BltFasts (colour keyed) or Locks and draws into itself.  Here
 * every surface is plain guest memory; Flip / drawing into the primary
 * hands the front buffer to the presenter (window.c).  16-bit surfaces are
 * RGB565.  COM objects live in guest memory: [vtable][object id]; the
 * vtables hold native thunks.  Presenting a frame is also where the other
 * guest threads (the sound manager) get to run. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_NOTFOUND 0x887600ffu
#define DDERR_NOCOLORKEY 0x887600e2u
#define DDERR_NOPALETTEATTACHED 0x887600e9u

typedef struct Surf {
    u32 obj;               /* guest COM object */
    int w, h, pitch, bpp;
    u32 mem;               /* guest pixels */
    u32 caps;
    int ck_on;
    u32 ck_lo, ck_hi;
    struct Surf *back;     /* primary: attached back buffer */
    struct Pal *pal;
    int primary, refs;
    u32 dc;                /* GetDC: guest DIB the game drew into */
} Surf;

typedef struct Pal { u32 obj; u32 rgb[256]; int refs; } Pal;

#define MAXS 1024
static Surf *surfs[MAXS];
static Pal *pals[64];
static u32 vt_dd, vt_dd2, vt_surf, vt_pal, vt_clip;
static Surf *primary;
static u32 obj_dd, obj_dd2;
static int mode_bpp = 8;
static u32 cur_pal[256];

static Surf *S(u32 obj) { if (!obj) return NULL; u32 i = RD32(obj + 4); return (i < MAXS && surfs[i] && surfs[i]->obj == obj) ? surfs[i] : NULL; }
static Pal *P(u32 obj) { if (!obj) return NULL; u32 i = RD32(obj + 4); return (i < 64 && pals[i] && pals[i]->obj == obj) ? pals[i] : NULL; }

static void present(void)
{
    if (!primary) return;
    if (primary->bpp == 16) present_frame16(MEM + primary->mem, primary->pitch, primary->w, primary->h);
    else {
        if (primary->pal) memcpy(cur_pal, primary->pal->rgb, sizeof cur_pal);
        present_frame(MEM + primary->mem, primary->pitch, cur_pal);
    }
    thr_yield();
}

/* frame pacing: a Flip with DDFLIP_WAIT waited for the 60 Hz vertical blank */
static void vsync_wait(void)
{
    static LARGE_INTEGER freq, last;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    s64 period = freq.QuadPart / 60;
    if (last.QuadPart && now.QuadPart - last.QuadPart < period) {
        s64 left = period - (now.QuadPart - last.QuadPart);
        DWORD ms = (DWORD)(left * 1000 / freq.QuadPart);
        if (ms > 1) Sleep(ms - 1);
        do QueryPerformanceCounter(&now); while (now.QuadPart - last.QuadPart < period);
        last.QuadPart += period;
        if (now.QuadPart - last.QuadPart > period) last = now;
    } else last = now;
}

static Surf *surf_new(int w, int h, int bpp, u32 caps)
{
    int i = 1;
    while (i < MAXS && surfs[i]) i++;
    if (i == MAXS) rt_fatal("out of DirectDraw surfaces");
    Surf *s = calloc(1, sizeof *s);
    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->pitch = (w * (bpp / 8) + 3) & ~3;
    s->mem = gmalloc(s->pitch * h);
    s->caps = caps;
    s->refs = 1;
    s->obj = gmalloc(8);
    WR32(s->obj, vt_surf);
    WR32(s->obj + 4, i);
    surfs[i] = s;
    return s;
}

/* ---------------- IDirectDraw / IDirectDraw2 ---------------- */
static void dd_QueryInterface(void)
{
    u32 iid = ARG(1);
    /* IID_IDirectDraw2 = B3A6F3E0-2B43-11CF-A2DE-00AA00B93356 */
    if (RD32(iid) == 0xb3a6f3e0u) { WR32(ARG(2), obj_dd2); RET(DD_OK, 3); }
    WR32(ARG(2), obj_dd);
    RET(DD_OK, 3);
}
static void dd_AddRef(void) { RET(2, 1); }
static void dd_Release(void) { RET(0, 1); }
static void dd_SetCooperativeLevel(void) { rt_log("ddraw: SetCooperativeLevel %x\n", ARG(2)); RET(DD_OK, 3); }
static void set_mode(u32 w, u32 h, u32 bpp)
{
    rt_log("ddraw: SetDisplayMode %ux%ux%u\n", w, h, bpp);
    mode_bpp = bpp == 16 ? 16 : 8;
}
static void dd_SetDisplayMode(void) { set_mode(ARG(1), ARG(2), ARG(3)); RET(DD_OK, 4); }
static void dd2_SetDisplayMode(void) { set_mode(ARG(1), ARG(2), ARG(3)); RET(DD_OK, 6); }
static void dd_RestoreDisplayMode(void) { RET(DD_OK, 1); }
static void dd_WaitForVerticalBlank(void) { vsync_wait(); RET(DD_OK, 3); }
static void dd_FlipToGDISurface(void) { RET(DD_OK, 1); }
static void dd_GetAvailableVidMem(void) { if (ARG(2)) WR32(ARG(2), 8 << 20); if (ARG(3)) WR32(ARG(3), 8 << 20); RET(DD_OK, 4); }
static void dd_GetVerticalBlankStatus(void) { WR32(ARG(1), 0); RET(DD_OK, 2); }
static void dd_GetMonitorFrequency(void) { WR32(ARG(1), 60); RET(DD_OK, 2); }
static void dd_Compact(void) { RET(DD_OK, 1); }

static void dd_GetCaps(void)
{
    for (int k = 1; k <= 2; k++) {
        u32 c = ARG(k);
        if (!c) continue;
        u32 n = RD32(c);
        if (n > 4 && n < 0x200) memset(MEM + c + 4, 0, n - 4);
        WR32(c + 4, 0x00000040u | 0x00000400u | 0x00400000u | 0x00000200u);   /* BLT, BLTCOLORFILL, COLORKEY, BLTSTRETCH */
        WR32(c + 8, 0);
        WR32(c + 12, 0x00000200u);    /* dwCKeyCaps: SRCBLT */
        WR32(c + 0x3c, 8 << 20);      /* dwVidMemTotal */
        WR32(c + 0x40, 8 << 20);      /* dwVidMemFree */
        WR32(c + 0x6c, 0x00004000u | 0x00000200u | 0x00000040u | 0x00000004u);   /* ddsCaps: video, primary, offscreen, backbuffer */
    }
    RET(DD_OK, 3);
}

static void pixfmt(u32 p, int bpp)
{
    memset(MEM + p, 0, 32);
    WR32(p, 32);
    if (bpp == 16) {
        WR32(p + 4, 0x40);        /* DDPF_RGB */
        WR32(p + 12, 16);
        WR32(p + 16, 0xf800); WR32(p + 20, 0x07e0); WR32(p + 24, 0x001f);
    } else {
        WR32(p + 4, 0x60);        /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
        WR32(p + 12, 8);
    }
}

static void dd_GetDisplayMode(void)
{
    u32 d = ARG(1);
    memset(MEM + d + 4, 0, 0x68);
    WR32(d, 0x6c);
    WR32(d + 4, 0x100f);
    WR32(d + 8, 480); WR32(d + 12, 640); WR32(d + 16, 640 * mode_bpp / 8);
    pixfmt(d + 0x48, mode_bpp);
    RET(DD_OK, 2);
}

/* EnumDisplayModes(this, flags, desc, ctx, cb): 640x480 at 8 and 16 bits */
static void dd_EnumDisplayModes(void)
{
    u32 d = gcalloc(0x6c);
    int bpps[2] = { 8, 16 };
    for (int i = 0; i < 2; i++) {
        memset(MEM + d, 0, 0x6c);
        WR32(d, 0x6c); WR32(d + 4, 0x100f); WR32(d + 8, 480); WR32(d + 12, 640); WR32(d + 16, 640 * bpps[i] / 8);
        WR32(d + 0x1c, 60);
        pixfmt(d + 0x48, bpps[i]);
        if (!guest_call(ARG(4), 2, d, ARG(3))) break;
    }
    gfree(d);
    RET(DD_OK, 5);
}

/* CreateSurface(this, desc, &surface, outer) */
static void dd_CreateSurface(void)
{
    u32 d = ARG(1);
    u32 flags = RD32(d + 4), caps = RD32(d + 0x68);
    Surf *s;
    int bpp = mode_bpp;
    if ((flags & 0x1000) && RD32(d + 0x48 + 12)) bpp = RD32(d + 0x48 + 12) == 16 ? 16 : 8;   /* DDSD_PIXELFORMAT */
    if (caps & 0x200) {   /* DDSCAPS_PRIMARYSURFACE */
        s = surf_new(640, 480, mode_bpp, caps | 0x2000 | 0x4000 | 0x8000);
        s->primary = 1;
        primary = s;
        if ((flags & 0x20) && RD32(d + 0x14)) s->back = surf_new(640, 480, mode_bpp, 0x4 | 0x4000);
        rt_log("ddraw: primary surface %d-bit%s\n", mode_bpp, s->back ? " + back buffer" : "");
    } else {
        int w = (flags & 4) ? (int)RD32(d + 12) : 640, h = (flags & 2) ? (int)RD32(d + 8) : 480;
        if (!(caps & 0x800)) caps |= 0x4000;   /* in "video memory" unless asked for system memory */
        s = surf_new(w, h, bpp, caps | 0x40);
    }
    if (flags & 0x10000) {   /* DDSD_CKSRCBLT */
        s->ck_on = 1; s->ck_lo = RD32(d + 0x40); s->ck_hi = RD32(d + 0x44);
        if (s->ck_hi < s->ck_lo) s->ck_hi = s->ck_lo;
    }
    WR32(ARG(2), s->obj);
    RET(DD_OK, 4);
}

/* CreatePalette(this, flags, entries, &pal, outer) */
static void dd_CreatePalette(void)
{
    int i = 1;
    while (i < 64 && pals[i]) i++;
    if (i == 64) RET(DDERR_GENERIC, 5);
    Pal *p = calloc(1, sizeof *p);
    p->refs = 1;
    p->obj = gmalloc(8);
    WR32(p->obj, vt_pal);
    WR32(p->obj + 4, i);
    pals[i] = p;
    u32 e = ARG(2);
    if (e) for (int k = 0; k < 256; k++) p->rgb[k] = RD32(e + 4 * k) & 0xffffff;
    WR32(ARG(3), p->obj);
    RET(DD_OK, 5);
}

/* CreateClipper(this, flags, &clipper, outer): a clipper that does nothing */
static void dd_CreateClipper(void)
{
    u32 c = gcalloc(8);
    WR32(c, vt_clip);
    WR32(ARG(2), c);
    RET(DD_OK, 4);
}

static void dd_unsup1(void) { RET(DDERR_UNSUPPORTED, 1); }
static void dd_unsup2(void) { RET(DDERR_UNSUPPORTED, 2); }
static void dd_unsup3(void) { RET(DDERR_UNSUPPORTED, 3); }
static void dd_unsup4(void) { RET(DDERR_UNSUPPORTED, 4); }
static void dd_unsup5(void) { RET(DDERR_UNSUPPORTED, 5); }
static void dd_unsup6(void) { RET(DDERR_UNSUPPORTED, 6); }
static void dd_ok1(void) { RET(DD_OK, 1); }
static void dd_ok2(void) { RET(DD_OK, 2); }
static void dd_ok3(void) { RET(DD_OK, 3); }
static void dd_ok4(void) { RET(DD_OK, 4); }

/* ---------------- IDirectDrawSurface ---------------- */
static void s_QueryInterface(void) { Surf *s = S(ARG(0)); if (s) s->refs++; WR32(ARG(2), ARG(0)); RET(DD_OK, 3); }
static void s_AddRef(void) { Surf *s = S(ARG(0)); if (s) s->refs++; RET(s ? s->refs : 0, 1); }
static void surf_free(Surf *s)
{
    surfs[RD32(s->obj + 4)] = NULL;
    gfree(s->mem);
    gfree(s->obj);
    free(s);
}
static void s_Release(void)
{
    Surf *s = S(ARG(0));
    if (!s) RET(0, 1);
    if (--s->refs > 0) RET(s->refs, 1);
    if (s == primary) primary = NULL;
    if (s->back) surf_free(s->back);
    surf_free(s);
    RET(0, 1);
}

static inline u32 px(Surf *s, const u8 *p) { return s->bpp == 16 ? *(const u16 *)p : *p; }
static int key_match(Surf *s, u32 c) { return c >= s->ck_lo && c <= s->ck_hi; }

static void blit(Surf *d, int dx, int dy, Surf *s, int sx, int sy, int w, int h, int keyed)
{
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (sx + w > s->w) w = s->w - sx;
    if (sy + h > s->h) h = s->h - sy;
    if (dx + w > d->w) w = d->w - dx;
    if (dy + h > d->h) h = d->h - dy;
    if (w <= 0 || h <= 0) return;
    int b = s->bpp / 8;
    if (d->bpp != s->bpp) { rt_log("ddraw: blit between %d and %d bit surfaces\n", s->bpp, d->bpp); return; }
    for (int y = 0; y < h; y++) {
        u8 *dp = MEM + d->mem + (dy + y) * d->pitch + dx * b;
        const u8 *sp = MEM + s->mem + (sy + y) * s->pitch + sx * b;
        if (!keyed) memmove(dp, sp, w * b);
        else if (b == 2) {
            u16 *d16 = (u16 *)dp; const u16 *s16 = (const u16 *)sp;
            for (int x = 0; x < w; x++) if (!key_match(s, s16[x])) d16[x] = s16[x];
        } else for (int x = 0; x < w; x++) if (!key_match(s, sp[x])) dp[x] = sp[x];
    }
}

static void read_rect(u32 r, int *x, int *y, int *w, int *h, Surf *s)
{
    if (r) { *x = (s32)RD32(r); *y = (s32)RD32(r + 4); *w = (s32)RD32(r + 8) - *x; *h = (s32)RD32(r + 12) - *y; }
    else { *x = 0; *y = 0; *w = s->w; *h = s->h; }
}

/* BltFast(this, x, y, src, srcrect, trans) */
static void s_BltFast(void)
{
    Surf *d = S(ARG(0)), *s = S(ARG(3));
    if (!d || !s) RET(DDERR_INVALIDPARAMS, 6);
    int sx, sy, w, h;
    read_rect(ARG(4), &sx, &sy, &w, &h, s);
    blit(d, (s32)ARG(1), (s32)ARG(2), s, sx, sy, w, h, (ARG(5) & 1) && s->ck_on);
    if (d == primary) present();
    RET(DD_OK, 6);
}

static void fill(Surf *d, int dx, int dy, int dw, int dh, u32 c)
{
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > d->w) dw = d->w - dx;
    if (dy + dh > d->h) dh = d->h - dy;
    if (dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        u8 *p = MEM + d->mem + (dy + y) * d->pitch;
        if (d->bpp == 16) { u16 *q = (u16 *)p + dx; for (int x = 0; x < dw; x++) q[x] = (u16)c; }
        else memset(p + dx, (u8)c, dw);
    }
}

/* Blt(this, dstrect, src, srcrect, flags, fx) */
static void s_Blt(void)
{
    Surf *d = S(ARG(0));
    if (!d) RET(DDERR_INVALIDPARAMS, 6);
    u32 flags = ARG(4);
    int dx, dy, dw, dh;
    read_rect(ARG(1), &dx, &dy, &dw, &dh, d);
    if (flags & 0x400) fill(d, dx, dy, dw, dh, RD32(ARG(5) + 0x50));   /* DDBLT_COLORFILL */
    else {
        Surf *s = S(ARG(2));
        if (!s) RET(DDERR_INVALIDPARAMS, 6);
        int sx, sy, sw, sh;
        read_rect(ARG(3), &sx, &sy, &sw, &sh, s);
        int keyed = (flags & 0x8000) && s->ck_on;
        if (flags & 0x10000) {   /* DDBLT_KEYSRCOVERRIDE */
            keyed = 1; s->ck_lo = RD32(ARG(5) + 0x4c); s->ck_hi = RD32(ARG(5) + 0x50);
        }
        if (sw == dw && sh == dh) blit(d, dx, dy, s, sx, sy, sw, sh, keyed);
        else if (d->bpp == s->bpp && dw > 0 && dh > 0) {   /* stretch, nearest */
            int b = s->bpp / 8;
            for (int y = 0; y < dh; y++) {
                int ty = dy + y, fy = sy + y * sh / dh;
                if (ty < 0 || ty >= d->h || fy < 0 || fy >= s->h) continue;
                for (int x = 0; x < dw; x++) {
                    int tx = dx + x, fx = sx + x * sw / dw;
                    if (tx < 0 || tx >= d->w || fx < 0 || fx >= s->w) continue;
                    const u8 *sp = MEM + s->mem + fy * s->pitch + fx * b;
                    u32 c = px(s, sp);
                    if (keyed && key_match(s, c)) continue;
                    u8 *dp = MEM + d->mem + ty * d->pitch + tx * b;
                    if (b == 2) *(u16 *)dp = (u16)c; else *dp = (u8)c;
                }
            }
        }
    }
    if (d == primary) present();
    RET(DD_OK, 6);
}

static void s_Flip(void)
{
    Surf *s = S(ARG(0));
    if (s && s->back) {
        u32 m = s->mem;
        s->mem = s->back->mem;
        s->back->mem = m;
        if (!(ARG(2) & 0x8)) vsync_wait();   /* not DDFLIP_NOVSYNC */
        present();
    }
    RET(DD_OK, 3);
}

static void s_GetAttachedSurface(void)
{
    Surf *s = S(ARG(0));
    if (!s || !s->back) RET(DDERR_NOTFOUND, 3);
    s->back->refs++;
    WR32(ARG(2), s->back->obj);
    RET(DD_OK, 3);
}

static void s_GetCaps(void) { Surf *s = S(ARG(0)); WR32(ARG(1), s ? s->caps : 0); RET(DD_OK, 2); }

static void fill_desc(Surf *s, u32 d, u32 ptr)
{
    memset(MEM + d + 4, 0, 0x68);
    WR32(d, 0x6c);
    WR32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (ptr ? 0x800 : 0));
    WR32(d + 8, s->h);
    WR32(d + 12, s->w);
    WR32(d + 16, s->pitch);
    if (s->back) { WR32(d + 0x14, 1); WR32(d + 4, RD32(d + 4) | 0x20); }
    WR32(d + 0x24, ptr);
    if (s->ck_on) { WR32(d + 0x40, s->ck_lo); WR32(d + 0x44, s->ck_hi); WR32(d + 4, RD32(d + 4) | 0x10000); }
    pixfmt(d + 0x48, s->bpp);
    WR32(d + 0x68, s->caps);
}

static void s_GetSurfaceDesc(void) { Surf *s = S(ARG(0)); if (!s) RET(DDERR_INVALIDPARAMS, 2); fill_desc(s, ARG(1), 0); RET(DD_OK, 2); }
static void s_GetPixelFormat(void) { Surf *s = S(ARG(0)); pixfmt(ARG(1), s ? s->bpp : mode_bpp); RET(DD_OK, 2); }

/* Lock(this, rect, desc, flags, event) */
static void s_Lock(void)
{
    Surf *s = S(ARG(0));
    if (!s) RET(DDERR_INVALIDPARAMS, 5);
    u32 p = s->mem;
    if (ARG(1)) p += (s32)RD32(ARG(1) + 4) * s->pitch + (s32)RD32(ARG(1)) * (s->bpp / 8);
    fill_desc(s, ARG(2), p);
    RET(DD_OK, 5);
}

static void s_Unlock(void)
{
    Surf *s = S(ARG(0));
    if (s && s == primary) present();
    RET(DD_OK, 2);
}

/* SetColorKey(this, flags, key) */
static void s_SetColorKey(void)
{
    Surf *s = S(ARG(0));
    if (s && (ARG(1) & 8)) {   /* DDCKEY_SRCBLT */
        if (ARG(2)) { s->ck_on = 1; s->ck_lo = RD32(ARG(2)); s->ck_hi = RD32(ARG(2) + 4); if (s->ck_hi < s->ck_lo) s->ck_hi = s->ck_lo; }
        else s->ck_on = 0;
    }
    RET(DD_OK, 3);
}

static void s_GetColorKey(void)
{
    Surf *s = S(ARG(0));
    if (!s || !s->ck_on) RET(DDERR_NOCOLORKEY, 3);
    WR32(ARG(2), s->ck_lo);
    WR32(ARG(2) + 4, s->ck_hi);
    RET(DD_OK, 3);
}

static void s_SetPalette(void)
{
    Surf *s = S(ARG(0));
    Pal *p = ARG(1) ? P(ARG(1)) : NULL;
    if (s) {
        s->pal = p;
        if (p) p->refs++;
        if (s == primary && p) memcpy(cur_pal, p->rgb, sizeof cur_pal);
    }
    RET(DD_OK, 2);
}

static void s_GetPalette(void)
{
    Surf *s = S(ARG(0));
    if (!s || !s->pal) RET(DDERR_NOPALETTEATTACHED, 2);
    s->pal->refs++;
    WR32(ARG(1), s->pal->obj);
    RET(DD_OK, 2);
}

static void s_IsLost(void) { RET(DD_OK, 1); }
static void s_Restore(void) { RET(DD_OK, 1); }
static void s_GetBltStatus(void) { RET(DD_OK, 2); }
static void s_GetFlipStatus(void) { RET(DD_OK, 2); }
static void s_GetDC(void) { rt_log("ddraw: Surface::GetDC (unsupported)\n"); RET(DDERR_UNSUPPORTED, 2); }
static void s_ReleaseDC(void) { RET(DD_OK, 2); }
static void s_SetClipper(void) { RET(DD_OK, 2); }
static void s_GetClipper(void) { RET(DDERR_NOTFOUND, 2); }
static void s_PageLock(void) { RET(DD_OK, 2); }
static void s_GetDDInterface(void) { WR32(ARG(1), obj_dd2); RET(DD_OK, 2); }
static void s_AddAttachedSurface(void) { RET(DD_OK, 2); }

/* ---------------- IDirectDrawPalette ---------------- */
static void p_AddRef(void) { Pal *p = P(ARG(0)); if (p) p->refs++; RET(p ? p->refs : 0, 1); }
static void p_Release(void) { Pal *p = P(ARG(0)); if (p && p->refs > 0) p->refs--; RET(p ? p->refs : 0, 1); }
static void p_GetCaps(void) { WR32(ARG(1), 0x4 | 0x40); RET(DD_OK, 2); }   /* 8BIT | ALLOW256 */

/* SetEntries(this, flags, start, count, entries) */
static void p_SetEntries(void)
{
    Pal *p = P(ARG(0));
    if (!p) RET(DDERR_INVALIDPARAMS, 5);
    u32 st = ARG(2), n = ARG(3), e = ARG(4);
    for (u32 k = 0; k < n && st + k < 256; k++) p->rgb[st + k] = RD32(e + 4 * k) & 0xffffff;
    if (primary && primary->pal == p && primary->bpp == 8) present();   /* palette fades show without a flip */
    RET(DD_OK, 5);
}

static void p_GetEntries(void)
{
    Pal *p = P(ARG(0));
    if (!p) RET(DDERR_INVALIDPARAMS, 5);
    u32 st = ARG(2), n = ARG(3), e = ARG(4);
    for (u32 k = 0; k < n && st + k < 256; k++) WR32(e + 4 * k, p->rgb[st + k]);
    RET(DD_OK, 5);
}

/* ---------------- vtables ---------------- */
static u32 make_vt(const NativeFn *fns, const char **names, int n)
{
    u32 vt = gmalloc(4 * n);
    for (int i = 0; i < n; i++) WR32(vt + 4 * i, thunk_new(fns[i], names[i]));
    return vt;
}

static void init_vtables(void)
{
    static NativeFn dd[] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette,
        dd_CreateSurface, dd_unsup3, dd_EnumDisplayModes, dd_unsup5, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode,
        dd_unsup3, dd_unsup2, dd_GetMonitorFrequency, dd_unsup2, dd_GetVerticalBlankStatus, dd_unsup2, dd_RestoreDisplayMode,
        dd_SetCooperativeLevel, dd_SetDisplayMode, dd_WaitForVerticalBlank, dd_GetAvailableVidMem };
    static const char *ddn[] = { "IDirectDraw::QueryInterface", "IDirectDraw::AddRef", "IDirectDraw::Release",
        "IDirectDraw::Compact", "IDirectDraw::CreateClipper", "IDirectDraw::CreatePalette", "IDirectDraw::CreateSurface",
        "IDirectDraw::DuplicateSurface", "IDirectDraw::EnumDisplayModes", "IDirectDraw::EnumSurfaces",
        "IDirectDraw::FlipToGDISurface", "IDirectDraw::GetCaps", "IDirectDraw::GetDisplayMode",
        "IDirectDraw::GetFourCCCodes", "IDirectDraw::GetGDISurface", "IDirectDraw::GetMonitorFrequency",
        "IDirectDraw::GetScanLine", "IDirectDraw::GetVerticalBlankStatus", "IDirectDraw::Initialize",
        "IDirectDraw::RestoreDisplayMode", "IDirectDraw::SetCooperativeLevel", "IDirectDraw::SetDisplayMode",
        "IDirectDraw::WaitForVerticalBlank", "IDirectDraw2::GetAvailableVidMem" };
    vt_dd = make_vt(dd, ddn, 23);
    dd[21] = dd2_SetDisplayMode;
    vt_dd2 = make_vt(dd, ddn, 24);
    static const NativeFn sf[] = { s_QueryInterface, s_AddRef, s_Release, s_AddAttachedSurface, dd_unsup2, s_Blt, dd_unsup4,
        s_BltFast, dd_unsup3, dd_unsup3, dd_unsup4, s_Flip, s_GetAttachedSurface, s_GetBltStatus, s_GetCaps,
        s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus, dd_unsup3, s_GetPalette, s_GetPixelFormat,
        s_GetSurfaceDesc, dd_unsup3, s_IsLost, s_Lock, s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey,
        dd_unsup3, s_SetPalette, s_Unlock, dd_unsup6, dd_unsup2, dd_unsup3,
        s_GetDDInterface, s_PageLock, s_PageLock, dd_unsup3 };
    static const char *sfn[] = { "Surface::QueryInterface", "Surface::AddRef", "Surface::Release",
        "Surface::AddAttachedSurface", "Surface::AddOverlayDirtyRect", "Surface::Blt", "Surface::BltBatch",
        "Surface::BltFast", "Surface::DeleteAttachedSurface", "Surface::EnumAttachedSurfaces",
        "Surface::EnumOverlayZOrders", "Surface::Flip", "Surface::GetAttachedSurface", "Surface::GetBltStatus",
        "Surface::GetCaps", "Surface::GetClipper", "Surface::GetColorKey", "Surface::GetDC", "Surface::GetFlipStatus",
        "Surface::GetOverlayPosition", "Surface::GetPalette", "Surface::GetPixelFormat", "Surface::GetSurfaceDesc",
        "Surface::Initialize", "Surface::IsLost", "Surface::Lock", "Surface::ReleaseDC", "Surface::Restore",
        "Surface::SetClipper", "Surface::SetColorKey", "Surface::SetOverlayPosition", "Surface::SetPalette",
        "Surface::Unlock", "Surface::UpdateOverlay", "Surface::UpdateOverlayDisplay", "Surface::UpdateOverlayZOrder",
        "Surface2::GetDDInterface", "Surface2::PageLock", "Surface2::PageUnlock", "Surface3::SetSurfaceDesc" };
    vt_surf = make_vt(sf, sfn, 40);
    static const NativeFn pf[] = { dd_QueryInterface, p_AddRef, p_Release, p_GetCaps, p_GetEntries, dd_unsup4, p_SetEntries };
    static const char *pfn[] = { "Palette::QueryInterface", "Palette::AddRef", "Palette::Release", "Palette::GetCaps",
        "Palette::GetEntries", "Palette::Initialize", "Palette::SetEntries" };
    vt_pal = make_vt(pf, pfn, 7);
    static const NativeFn cf[] = { dd_ok3, dd_AddRef, dd_Release, dd_unsup4, dd_unsup3, dd_ok2, dd_ok3, dd_ok4, dd_ok2 };
    static const char *cfn[] = { "Clipper::QueryInterface", "Clipper::AddRef", "Clipper::Release", "Clipper::GetClipList",
        "Clipper::GetHWnd", "Clipper::Initialize", "Clipper::IsClipListChanged", "Clipper::SetClipList", "Clipper::SetHWnd" };
    vt_clip = make_vt(cf, cfn, 9);
    (void)dd_ok1;
}

/* DirectDrawCreate(guid, lplpDD, outer) */
void api_DirectDrawCreate(void)
{
    if (!vt_dd) init_vtables();
    if (!obj_dd) {
        obj_dd = gmalloc(8);
        WR32(obj_dd, vt_dd);
        obj_dd2 = gmalloc(8);
        WR32(obj_dd2, vt_dd2);
    }
    WR32(ARG(1), obj_dd);
    rt_log("DirectDrawCreate\n");
    RET(DD_OK, 3);
}
