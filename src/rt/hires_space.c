/* Real higher internal resolution for space combat's viewscreen.
 *
 * The space engine draws the viewscreen 8-bit into its own buffer
 * (54f66c, 532x231, pitch 570238), then 494b64 converts it through a
 * 256-entry RGB565 table (56fb98) into the game's 16-bit screen at (52,10).
 * The 3D library (4e0000-4fd000) is parametric: its size 570230/570234,
 * pitch 570238, buffer 54f66c, z-buffer 5c9d24, and the window / centre it
 * derives in 4e7eb0 from fractions of that size (block 5a5140-5a5260).
 *
 * Per frame (494d18): the game clears the buffer and draws the star field
 * at 1x, then 4e2d70 starts the 3D.  There the library is switched to an
 * N x scratch buffer (seeded with the 1x picture upscaled) and an N x
 * z-buffer, and its window is re-derived from the fractions the game last
 * gave 4e7eb0 for this view.  At the conversion (494b64) everything is put
 * back, the game's buffer gets the centre sample of each N x block, and the
 * N x picture becomes the presenter's layer over the viewscreen (shown where
 * the screen still has the converted pixel - HUD drawn later wins). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define HR_BUF  0x3a000000u      /* guest scratch: N x 8-bit picture */
#define HR_Z    0x3c000000u      /* guest scratch: N x z-buffer */
#define G_W     0x570230u
#define G_H     0x570234u
#define G_PITCH 0x570238u
#define G_BUF   0x54f66cu
#define G_ZBUF  0x5c9d24u
#define G_MODE  0x54a0dcu        /* 2: 8-bit buffer + conversion */
#define G_TAB   0x56fb98u        /* 8-bit -> RGB565 */
#define G_SCR   0x5700c4u        /* the game's 16-bit screen */
#define G_SCRPITCH 0x54a0d8u
#define BLK     0x5a5140u        /* the library's derived window block */
#define BLK_N   0xd8u        /* 5a5140-5a5217 (not the per-frame counters after it) */
#define MAIN_W  0x214
#define MAIN_H  0xe7

static u32 vp_args[8];
static int have_args, active, pass_n;
static u8 save_blk[BLK_N];
static int sp_debug = -1;
/* block offsets of the window/projection fields derived from the size */
static const u8 VP_FIELDS[] = {
    0x00, 0x04, 0x08, 0x0c, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x3c, 0x40,
    0x80, 0x84, 0x88, 0x8c, 0x9c, 0xa0, 0xa4, 0xa8, 0xac, 0xb0, 0xb4, 0xb8, 0xbc, 0xc0, 0xc4,
    0xc8, 0xcc, 0xd0,
};
static u32 save_w, save_h, save_pitch, save_buf, save_z;
static u8 *lo_copy;
static u16 tab[256];
static int frame_n, out_x, out_y;
static u32 frame_ms, out_scr;

static int want_n(void)
{
    extern int opt_render;
    return opt_render < 1 ? 1 : opt_render > 4 ? 4 : opt_render;
}

/* 4e7eb0(left, top, right, bottom, cx, cy, ., .): the window as fractions of
   the size - remember the main view's, to derive the N x window from */
void N_sp_window(void)
{
    if (!active && RD32(G_W) == MAIN_W && RD32(G_H) == MAIN_H) {
        for (int i = 0; i < 8; i++) vp_args[i] = ARG(i);
        have_args = 1;
    }
    rt_wrap_orig("N_sp_window", NULL)();
}

/* The space frame (494d18) steps the simulation by the game clock
   (565940, ~291 ticks/s from timeGetTime) since the last frame, but forces a
   step of 0 up to 1 tick.  Built for 15-30 fps; unpaced it ran ~700 fps, so
   most frames got a tick that had not passed: combat ran ~2.4x too fast and
   the AI's steering wobbled.  Frames are paced to the display refresh
   (60-120 fps: always at least 2 ticks per frame).  STG_FPSCAP=n overrides
   (0 = unpaced), STG_CAMLOG=1 logs every ship's position/orientation per
   frame to camlog.txt. Paced in the combat loop (47b83e) before it reads the clock. */
static void pace_frame(void)
{
    static int cap = -1;
    static LARGE_INTEGER fq;
    static long long next;
    if (cap < 0) {
        QueryPerformanceFrequency(&fq);
        DEVMODEA dm = { .dmSize = sizeof dm };
        int hz = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 60;
        cap = hz < 60 ? 60 : hz > 120 ? 120 : hz;
        const char *c = getenv(ENV("FPSCAP"));
        if (c) cap = atoi(c);
        rt_log("space: frames paced to %d fps\n", cap);
    }
    if (cap <= 0) return;
    /* spaced from when the last frame really started: the wait lets the
       other guest threads run and can overshoot, and a deadline chain would
       then let the next frame through at once (a step of 0 again) */
    if (next) thr_wait_qpc(next);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    next = t.QuadPart + fq.QuadPart / cap;
}

/* 47b83e: one turn of the space combat loop (idle handler) - reads the
   clock, then runs the frame: pace before the clock is read */
void N_combat_idle(void)
{
    pace_frame();
    rt_wrap_orig("N_combat_idle", NULL)();
}

static void camlog_frame(void)
{
    static int on = -1;
    static FILE *f;
    static LARGE_INTEGER fq;
    if (on < 0) {
        on = getenv(ENV("CAMLOG")) != NULL;
        QueryPerformanceFrequency(&fq);
        if (on) { char p[700]; snprintf(p, sizeof p, "%s/camlog.txt", rt_exe_dir); f = fopen(p, "w"); }
    }
    if (!on || !f) return;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    double ms = (double)t.QuadPart * 1000.0 / (double)fq.QuadPart;
    u32 list = RD32(0x5700d0u);
    float dt; u32 dtr = RD32(0x570130u); memcpy(&dt, &dtr, 4);
    float div; u32 dvr = RD32(0x5370dcu); memcpy(&div, &dvr, 4);
    fprintf(f, "F %.3f tick %d d %d dt %.5f div %.3f", ms, (int)RD32(0x565940u), (int)RD32(0x57012cu), dt, div);
    if (list) {
        int cnt = (int)RD32(list);
        fprintf(f, " n %d", cnt);
        for (int k = 0; k < cnt && k < 16; k++) {
            u32 o = RD32(list + (u32)(k + 5) * 4);
            if (!o) continue;
            float v[6];
            for (int i = 0; i < 3; i++) { u32 r = RD32(o + 0xf0 + i * 4); memcpy(&v[i], &r, 4); v[3 + i] = (float)(s32)RD32(o + 0x114 + i * 4); }
            fprintf(f, " | %x %.2f %.2f %.2f %.0f %.0f %.0f", o, v[0], v[1], v[2], v[3], v[4], v[5]);
        }
        fprintf(f, " | pl %x en %x", RD32(list + 16), RD32(list + 12));
    }
    fputc('\n', f);
    fflush(f);
}

/* 4e2d70: the 3D part of a frame begins */
void N_sp_begin(void)
{
    void (*orig)(void) = rt_wrap_orig("N_sp_begin", NULL);
    camlog_frame();
    int n = want_n();
    if (active || n < 2 || !have_args || RD32(G_MODE) != 2 || RD32(G_W) != MAIN_W || RD32(G_H) != MAIN_H ||
        RD32(G_PITCH) != MAIN_W) { orig(); return; }
    int w = MAIN_W, h = MAIN_H, W = w * n, H = h * n;
    VirtualAlloc(MEM + HR_BUF, (size_t)W * H + 0x10000, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(MEM + HR_Z, (size_t)W * H * 4 + 0x10000, MEM_COMMIT, PAGE_READWRITE);
    save_w = RD32(G_W); save_h = RD32(G_H); save_pitch = RD32(G_PITCH); save_buf = RD32(G_BUF); save_z = RD32(G_ZBUF);
    memcpy(save_blk, MEM + BLK, BLK_N);
    /* seed: the 1x picture so far (cleared background, stars) */
    for (int y = 0; y < h; y++) {
        const u8 *s = MEM + save_buf + (u32)y * save_pitch;
        for (int yy = 0; yy < n; yy++) {
            u8 *d = MEM + HR_BUF + (u32)((y * n + yy) * W);
            for (int x = 0; x < w; x++) memset(d + x * n, s[x], n);
        }
    }
    WR32(G_W, (u32)W); WR32(G_H, (u32)H); WR32(G_PITCH, (u32)W); WR32(G_BUF, HR_BUF); WR32(G_ZBUF, HR_Z);
    guest_call(0x4e7eb0, 8, vp_args[0], vp_args[1], vp_args[2], vp_args[3], vp_args[4], vp_args[5], vp_args[6], vp_args[7]);
    active = 1;
    pass_n = n;
    orig();
}

static void finish(void)
{
    if (!active) return;
    active = 0;
    int n = pass_n, w = MAIN_W, h = MAIN_H, W = w * n;
    WR32(G_W, save_w); WR32(G_H, save_h); WR32(G_PITCH, save_pitch); WR32(G_BUF, save_buf); WR32(G_ZBUF, save_z);
    /* only the window fields 4e7eb0/4e7c80/4e8130 derive from the size: the
       same block holds the camera (angles 5150/5178/5184, matrix 5190-51b0,
       position 51d0-51d8) the pass sets, which the fire code between frames
       reads - that must stay as the pass left it */
    if (sp_debug < 0) sp_debug = getenv(ENV("SPDEBUG")) != NULL;
    for (unsigned i = 0; i < sizeof VP_FIELDS / sizeof VP_FIELDS[0]; i++) {
        u32 o = VP_FIELDS[i], v;
        memcpy(&v, save_blk + o, 4);
        if (sp_debug && RD32(BLK + o) == v) {
            static int nlog;
            if (nlog++ < 5) rt_log("spdebug: window field %x unchanged at %dx\n", BLK + o, n);
        }
        WR32(BLK + o, v);
    }
    lo_copy = realloc(lo_copy, (size_t)w * h);
    for (int y = 0; y < h; y++) {
        const u8 *s = MEM + HR_BUF + (u32)((y * n + n / 2) * W) + n / 2;
        u8 *d = MEM + save_buf + (u32)y * save_pitch;
        for (int x = 0; x < w; x++) d[x] = s[x * n];
        memcpy(lo_copy + y * w, d, w);
    }
    memcpy(tab, MEM + G_TAB, sizeof tab);
    frame_n = n;
    frame_ms = now_ms();
}

/* 494b64(x, y): the 8-bit picture converted into the 16-bit screen */
void N_sp_convert(void)
{
    finish();
    if (frame_n > 1 && now_ms() - frame_ms < 100) {
        out_x = (s32)ARG(0); out_y = (s32)ARG(1);
        out_scr = RD32(G_SCR);
    }
    rt_wrap_orig("N_sp_convert", NULL)();
}

/* 494d18: the viewscreen frame.  When the battle ends it returns between the
   3D begin and the conversion, so the library would keep pointing at the
   N x scratch buffer - and the game then frees "its" buffer (SmartHeap
   MEM_BAD_POINTER, then a crash).  Put everything back whenever it returns. */
void N_sp_frame(void)
{
    rt_wrap_orig("N_sp_frame", NULL)();
    if (active) {
        u32 eax = cpu.r[R_EAX];
        finish();
        cpu.r[R_EAX] = eax;
    }
}

/* ddraw.c, presenting a 16-bit frame: the N x layer over the viewscreen */
static u32 *layer;
void space_compose(u32 mem, int pitch)
{
    int n = frame_n;
    if (n < 2 || !lo_copy || now_ms() - frame_ms > 250 || want_n() != n) return;
    int w = MAIN_W, h = MAIN_H, W = w * n, H = h * n;
    layer = realloc(layer, (size_t)W * H * 4);
    u32 argb[256];
    for (int i = 0; i < 256; i++) {
        u32 c = tab[i], r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        argb[i] = 0xff000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    int any = 0;
    for (int y = 0; y < h; y++) {
        int sy = out_y + y;
        const u16 *scr = sy >= 0 && sy < 480 ? (const u16 *)(MEM + mem + (u32)sy * pitch) : NULL;
        for (int x = 0; x < w; x++) {
            int sx = out_x + x;
            int on = scr && sx >= 0 && sx < 640 && scr[sx] == tab[lo_copy[y * w + x]];
            any |= on;
            for (int yy = 0; yy < n; yy++) {
                u32 *o = layer + (size_t)(y * n + yy) * W + x * n;
                if (!on) { for (int xx = 0; xx < n; xx++) o[xx] = 0; continue; }
                const u8 *s = MEM + HR_BUF + (u32)((y * n + yy) * W + x * n);
                for (int xx = 0; xx < n; xx++) o[xx] = argb[s[xx]];
            }
        }
    }
    if (any) hires_layer = (HiresLayer){ layer, W, H, out_x, out_y, w, h, 1 };
}
